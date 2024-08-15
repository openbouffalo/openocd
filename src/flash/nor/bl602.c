// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "imp.h"
#include <jtag/jtag.h>
#include <target/algorithm.h>
#include "spi.h"
#include "../../target/riscv/opcodes.h"
#include "../../target/riscv/gdb_regs.h"


// SFlash CFG definitions
#define SFLASH_CFG_SIZE 84
#define SFLASH_CFG_JEDEC_ID_CMD_POS 0x08
#define SFLASH_CFG_JEDEC_ID_CMD_DMY_CLK_POS 0x09
#define SFLASH_CFG_PAGE_SIZE_POS 0x0E
#define SFLASH_CFG_PAGE_PROGRAM_CMD_POS 0x15
#define SFLASH_CFG_TIME_PAGE_PGM_POS 0x4E
#define SFLASH_CFG_WRITE_ENABLE_CMD_POS 0x14
#define SFLASH_CFG_WRITE_ENABLE_INDEX_POS 0x28
#define SFLASH_CFG_WRITE_ENABLE_READ_REG_LEN_POS 0x2F
#define SFLASH_CFG_WRITE_ENABLE_BIT_POS 0x2B
#define SFLASH_CFG_BUSY_INDEX_POS 0x2A
#define SFLASH_CFG_BUSY_READ_REG_LEN_POS 0x33
#define SFLASH_CFG_READ_STATUS_REG1_POS 0x34
#define SFLASH_CFG_BUSY_BIT_POS 0x2D
#define SFLASH_CFG_FAST_READ_CMD_POS 0x18
#define SFLASH_CFG_SECTOR_SIZE_POS 0x0C
#define SFLASH_CFG_SECTOR_ERASE_CMD_POS 0x11
#define SFLASH_CFG_TIME_ERASE_SECTOR_POS 0x48

struct bl602_part_info {
	const uint32_t idcode;
	const uint32_t romapi_get_jedec_id;
	const uint32_t romapi_sflash_init_gpio;
	const uint32_t romapi_sflash_init;
	const uint32_t romapi_sflash_program;
	const uint32_t romapi_sflash_read;
	const uint32_t romapi_sflash_erase_sector;
};

struct bl602_flash_bank {
	// flag indicating successful flash probe
	bool probed;
	const struct bl602_part_info *part_info;
	uint8_t sflash_cfg[SFLASH_CFG_SIZE];
	// detected model of SPI flash
	const struct flash_device *dev;
};

static const struct bl602_part_info bl602_parts[] = {
	{
		.idcode = 0x20000e05,
		.romapi_get_jedec_id = 0x210189a4,
		.romapi_sflash_init_gpio = 0x21018a78,
		.romapi_sflash_init = 0x21018960,
		.romapi_sflash_program = 0x2101899c,
		.romapi_sflash_read = 0x210189d8,
		.romapi_sflash_erase_sector = 0x2101898c,
	}
};

static int bl602_call_func(struct target *target, uint32_t func_addr,
	uint32_t arg_data[], unsigned int n_args, uint32_t *return_data, unsigned int timeout_ms)
{
	int retval;
	char *reg_names[] = { "a0", "a1", "a2", "a3", "a4", "a5" };

	assert(n_args <= ARRAY_SIZE(reg_names)); // only allow register arguments

	struct working_area *trampoline_algorithm;
	
	uint32_t trampoline_code[] = {
		jalr(GDB_REGNO_RA, GDB_REGNO_T0, 0x0),
		ebreak(),
	};

	retval = target_alloc_working_area(target, sizeof(trampoline_code),
			&trampoline_algorithm);
	if (retval != ERROR_OK) {
		LOG_WARNING("No working area available, can't do trampoline");
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}

	retval = target_write_buffer(target, trampoline_algorithm->address,
			sizeof(trampoline_code), (uint8_t*)trampoline_code);
	if (retval != ERROR_OK) {
		target_free_working_area(target, trampoline_algorithm);
		return retval;
	}

	unsigned int n_reg_params = 0;
	struct reg_param reg_params[ARRAY_SIZE(reg_names) + 1];
	// initialize a0 register, which is used both as arg and also return register.
	if (!return_data || n_args > 0) {
		init_reg_param(&reg_params[0], reg_names[0], 32, PARAM_IN);
		if (n_args > 0) {
			buf_set_u32(reg_params[0].value, 0, 32, arg_data[0]);
			reg_params[0].direction = PARAM_IN_OUT;
		}
		n_reg_params++;
	}
	// initialize rest of registers, if any
	for (unsigned int i = 1; i < n_args; ++i) {
		init_reg_param(&reg_params[i], reg_names[i], 32, PARAM_OUT);
		buf_set_u32(reg_params[i].value, 0, 32, arg_data[i]);
		n_reg_params++;
	}
	// pass function pointer to t0
	init_reg_param(&reg_params[n_reg_params], "t0", 32, PARAM_OUT);
	buf_set_u32(reg_params[n_reg_params].value, 0, 32, func_addr);
	n_reg_params++;

	retval = target_run_algorithm(target,
			0, NULL,
			n_reg_params, reg_params,
			trampoline_algorithm->address,
			trampoline_algorithm->address + sizeof(trampoline_code) - 4,
			timeout_ms, NULL);

	if (retval != ERROR_OK) {
		LOG_ERROR("Failed to execute algorithm at 0x%" TARGET_PRIxADDR ": %d",
				trampoline_algorithm->address, retval);
	}

	if (return_data) {
		*return_data = buf_get_u32(reg_params[0].value, 0, 32);
	}

	for (unsigned int i = 0; i < n_reg_params; i++)
		destroy_reg_param(&reg_params[i]);

	target_free_working_area(target, trampoline_algorithm);

	return retval;
}

static int bl602_call_romapi_func(struct target *target, uint32_t romapi_func_addr,
	uint32_t arg_data[], unsigned int n_args, uint32_t *return_data, unsigned int timeout_ms)
{
	uint32_t func_addr;
	int retval;

	retval = target_read_u32(target, romapi_func_addr, &func_addr);
	if (retval != ERROR_OK) {
		return retval;
	}

	return bl602_call_func(target, func_addr, arg_data, n_args, return_data, timeout_ms);
}

static int bl602_alloc_bounce_buffer(struct flash_bank *bank,
		struct working_area** working_area, uint32_t count) {
	int retval = ERROR_OK;
	struct bl602_flash_bank *priv = bank->driver_priv;
	struct target *target = bank->target;
	
	unsigned int avail_pages = target_get_working_area_avail(target) / priv->dev->pagesize;
	/* We try to allocate working area rounded down to device page size,
	 * at least 1 page, at most the write data size */
	unsigned int chunk_size = MIN(MAX(avail_pages - 1, 1) * priv->dev->pagesize, count);
	retval = target_alloc_working_area(target, chunk_size, working_area);
	if (retval != ERROR_OK) {
		LOG_ERROR("Could not allocate bounce buffer for flash manipulation. Can't continue");
		return retval;
	}

	LOG_DEBUG("Allocated flash bounce buffer @" TARGET_ADDR_FMT, (*working_area)->address);

	return ERROR_OK;
}

static int bl602_alloc_sflash_cfg(struct flash_bank *bank,
		struct working_area** working_area)
{
	struct bl602_flash_bank *priv = bank->driver_priv;
	struct target *target = bank->target;

	int retval = target_alloc_working_area(target, sizeof(priv->sflash_cfg),
			working_area);
	if (retval != ERROR_OK) {
		LOG_WARNING("No working area available, can't alloc sflash cfg");
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}

	retval = target_write_buffer(target, (*working_area)->address,
			sizeof(priv->sflash_cfg), priv->sflash_cfg);
	if (retval != ERROR_OK) {
		target_free_working_area(target, *working_area);
	}
	return retval;
}

static int bl602_flash_init(struct flash_bank *bank)
{
	int retval = ERROR_OK;
	struct bl602_flash_bank *priv = bank->driver_priv;
	struct target *target = bank->target;
	const struct bl602_part_info *part_info = priv->part_info;

	// TODO: eFuse check, pSRAM etc!!!
	uint32_t flash_pin_cfg = 0;

	// initialize flash GPIOs
	{
		uint32_t args[] = {
			flash_pin_cfg,
			1,	// restoreDefault
		};

		retval = bl602_call_romapi_func(target, part_info->romapi_sflash_init_gpio,
				args, ARRAY_SIZE(args), NULL, 3000);
		if (retval != ERROR_OK) {
			LOG_ERROR("Failed to invoke spi flash gpio init function");
			return retval;
		}
	}

	// initialize SFlash peripheral
	{
		// structure for this can be found in official SDK by name SF_Ctrl_Cfg_Type
		const uint8_t sflash_ctrl_cfg[] = {
			0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00,
		};
		struct working_area *sflash_ctrl_cfg_area;
		retval = target_alloc_working_area(target, ARRAY_SIZE(sflash_ctrl_cfg),
				&sflash_ctrl_cfg_area);
		if (retval != ERROR_OK) {
			LOG_WARNING("No working area available, can't init SFlash");
			return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		}

		target_write_buffer(target, sflash_ctrl_cfg_area->address,
			ARRAY_SIZE(sflash_ctrl_cfg), sflash_ctrl_cfg);

		uint32_t args[] = {
			sflash_ctrl_cfg_area->address,
		};

		retval = bl602_call_romapi_func(target, part_info->romapi_sflash_init,
				args, ARRAY_SIZE(args), NULL, 3000);
		if (retval != ERROR_OK) {
			LOG_ERROR("Failed to invoke spi flash init function");
		}
		target_free_working_area(target, sflash_ctrl_cfg_area);
	}

	
	return retval;
}

static int bl602_flash_read_id(struct flash_bank *bank,
		uint32_t *jedec_id)
{
	struct bl602_flash_bank *priv = bank->driver_priv;
	struct target *target = bank->target;
	const struct bl602_part_info *part_info = priv->part_info;
	int retval = ERROR_OK;

	struct working_area *data_area;
	retval = target_alloc_working_area(target, sizeof(uint32_t) + 84, &data_area);
	if (retval != ERROR_OK) {
		LOG_WARNING("No working area available, can't read flash id");
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}

	target_write_buffer(target, data_area->address + 4, sizeof(priv->sflash_cfg),
			priv->sflash_cfg);

	uint32_t args[2] = {
		data_area->address + 4,	// SFlash CFG
		data_area->address,		// JEDEC ID pointer
	};

	retval = bl602_call_romapi_func(target, part_info->romapi_get_jedec_id,
			args, ARRAY_SIZE(args), NULL, 3000);
	if (retval != ERROR_OK) {
		LOG_ERROR("Failed to invoke get jedec id function");
		goto cleanup;
	}

	retval = target_read_u32(target, data_area->address, jedec_id);
	if (retval != ERROR_OK) {
		LOG_ERROR("Failed to read flash id from target");
	}

	*jedec_id &= 0x00FFFFFF;

cleanup:
	target_free_working_area(target, data_area);

	return retval;
}

static int bl602_flash_erase(struct flash_bank *bank, unsigned int first,
		unsigned int last)
{
	int retval = ERROR_OK;
	struct bl602_flash_bank *priv = bank->driver_priv;
	const struct bl602_part_info *part_info = priv->part_info;
	struct target *target = bank->target;
	struct working_area *sflash_cfg_area = NULL;
	
	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	retval = bl602_alloc_sflash_cfg(bank, &sflash_cfg_area);
	if (retval != ERROR_OK) {
		return retval;
	}

	uint32_t return_value;
	LOG_INFO("F: %d, L: %d", first, last);

	for (unsigned int sector = first; sector <= last; sector++) {
		LOG_INFO("Erasing sector %d", sector);
		uint32_t args[] = {
			sflash_cfg_area->address,
			sector,
		};
		retval = bl602_call_romapi_func(target, part_info->romapi_sflash_erase_sector,
				args, ARRAY_SIZE(args), &return_value, 20000);
		if (retval != ERROR_OK) {
			LOG_ERROR("Failed to invoke flash erase code on target");
			break;
		}
		LOG_INFO("RETVAL: %08X", return_value);
	}

	target_free_working_area(target, sflash_cfg_area);
	return retval;
}

static int bl602_flash_write(struct flash_bank *bank, const uint8_t *buffer,
		uint32_t offset, uint32_t count)
{
	int retval = ERROR_OK;
	struct bl602_flash_bank *priv = bank->driver_priv;
	const struct bl602_part_info *part_info = priv->part_info;
	struct target *target = bank->target;
	struct working_area *bounce_area = NULL;
	struct working_area *sflash_cfg_area = NULL;

	LOG_DEBUG("bank->size=0x%x offset=0x%08" PRIx32 " count=0x%08" PRIx32,
			bank->size, offset, count);

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (offset + count > priv->dev->size_in_bytes) {
		LOG_WARNING("Write past end of flash. Extra data discarded.");
		count = priv->dev->size_in_bytes - offset;
	}

	retval = bl602_alloc_sflash_cfg(bank, &sflash_cfg_area);
	if (retval != ERROR_OK) {
		return retval;
	}

	retval = bl602_alloc_bounce_buffer(bank, &bounce_area, count);
	if (retval != ERROR_OK) {
		goto cleanup;
	}
	unsigned int chunk_size = bounce_area->size;
	uint32_t return_value;

	while (count > 0) {
		uint32_t write_size = count > chunk_size ? chunk_size : count;
		LOG_INFO("Writing %d bytes to offset 0x%" PRIx32, write_size, offset);
		retval = target_write_buffer(target, bounce_area->address, write_size, buffer);
		if (retval != ERROR_OK) {
			LOG_ERROR("Could not load data into target bounce buffer");
			break;
		}
		uint32_t args[] = {
			sflash_cfg_area->address,
			0x0, // io_mode
			offset,
			bounce_area->address,
			write_size,
		};
		retval = bl602_call_romapi_func(target, part_info->romapi_sflash_program,
				args, ARRAY_SIZE(args), &return_value, 3000);
		if (retval != ERROR_OK) {
			LOG_ERROR("Failed to invoke flash programming code on target");
			break;
		}
		if (return_value != 0) {
			LOG_ERROR("Write flash function returned wrong value: %02X", return_value);
			retval = ERROR_FAIL;
			break;
		}

		buffer += write_size;
		offset += write_size;
		count -= write_size;
	}


cleanup:
	target_free_working_area(target, bounce_area);
	target_free_working_area(target, sflash_cfg_area);
	return retval;
}

static int bl602_flash_read(struct flash_bank *bank,
	uint8_t *buffer, uint32_t offset, uint32_t count)
{
	int retval;
	struct bl602_flash_bank *priv = bank->driver_priv;
	const struct bl602_part_info *part_info = priv->part_info;
	struct target *target = bank->target;
	struct working_area *bounce_area = NULL;
	struct working_area *sflash_cfg_area = NULL;
	
	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (offset + count > priv->dev->size_in_bytes) {
		LOG_WARNING("Read past end of flash.");
		count = priv->dev->size_in_bytes - offset;
	}

	retval = bl602_alloc_sflash_cfg(bank, &sflash_cfg_area);
	if (retval != ERROR_OK) {
		return retval;
	}

	retval = bl602_alloc_bounce_buffer(bank, &bounce_area, count);
	if (retval != ERROR_OK) {
		goto cleanup;
	}
	unsigned int chunk_size = bounce_area->size;
	uint32_t return_value;

	while (count > 0) {
		uint32_t read_size = count > chunk_size ? chunk_size : count;
		LOG_DEBUG("Read %d bytes from offset 0x%" PRIx32, read_size, offset);
		uint32_t args[] = {
			sflash_cfg_area->address,
			0x0, // io_mode
			false, // continous_read
			offset,
			bounce_area->address,
			read_size,
		};
		retval = bl602_call_romapi_func(target, part_info->romapi_sflash_read,
				args, ARRAY_SIZE(args), &return_value, 3000);
		if (retval != ERROR_OK) {
			LOG_ERROR("Failed to invoke flash read code on target");
			break;
		}
		if (return_value != 0) {
			LOG_ERROR("Read flash function returned wrong value: %02X", return_value);
			retval = ERROR_FAIL;
			break;
		}

		retval = target_read_buffer(target, bounce_area->address, read_size, buffer);
		if (retval != ERROR_OK) {
			LOG_ERROR("Could not load data from target bounce buffer");
			break;
		}

		buffer += read_size;
		offset += read_size;
		count -= read_size;
	}

cleanup:
	target_free_working_area(target, bounce_area);
	target_free_working_area(target, sflash_cfg_area);

	return retval;
}

static int bl602_flash_probe(struct flash_bank *bank)
{
	int retval = ERROR_OK;
	struct target *target = bank->target;
	struct bl602_flash_bank *priv = bank->driver_priv;
	
	if (!target_was_examined(target)) {
		LOG_ERROR("Target not examined yet");
		return ERROR_TARGET_NOT_EXAMINED;
	}

	priv->probed = false;

	for (unsigned int n = 0; n < ARRAY_SIZE(bl602_parts); n++) {
		const struct bl602_part_info* part_info = &bl602_parts[n];
		if (target->tap->idcode == part_info->idcode) {
			priv->part_info = part_info;
			break;
		}
	}

	if (!priv->part_info) {
		LOG_ERROR("Cannot identify target as an BL702 family device.");
		return ERROR_FAIL;
	}

	return ERROR_OK;

	retval = bl602_flash_init(bank);
	if (retval != ERROR_OK) {
		LOG_ERROR("Initialization of flash failed.");
		return retval;
	}

	uint32_t jedec_id;
	retval = bl602_flash_read_id(bank, &jedec_id);
	if (retval != ERROR_OK) {
		LOG_ERROR("Cannot identify flash JEDEC ID.");
		return retval;
	}
	
	priv->dev = NULL;
	for (const struct flash_device *p = flash_devices; p->name ; p++) {
		if (p->device_id == jedec_id) {
			priv->dev = p;
			break;
		}
	}
	// TODO: Implement internal Flashes

	if (!priv->dev) {
		LOG_ERROR("Unknown flash device (ID 0x%08" PRIx32 ")", jedec_id);
		return ERROR_FAIL;
	}
	LOG_INFO("Found flash device '%s' (ID 0x%08" PRIx32 ")", priv->dev->name,
			priv->dev->device_id);

	// set correct size value
	bank->size = priv->dev->size_in_bytes;
	bank->num_sectors = bank->size / priv->dev->sectorsize;
	bank->write_start_alignment = 8;
	bank->write_end_alignment = 8;

	bank->sectors = alloc_block_array(0, priv->dev->sectorsize, bank->num_sectors);
	if (!bank->sectors)
		return ERROR_FAIL;

	priv->sflash_cfg[SFLASH_CFG_PAGE_PROGRAM_CMD_POS] = priv->dev->pprog_cmd;
	priv->sflash_cfg[SFLASH_CFG_PAGE_SIZE_POS] = (priv->dev->pagesize & 0xFF);
	priv->sflash_cfg[SFLASH_CFG_PAGE_SIZE_POS + 1] = ((priv->dev->pagesize >> 8) & 0xFF);
	priv->sflash_cfg[SFLASH_CFG_FAST_READ_CMD_POS] = priv->dev->read_cmd;
	priv->sflash_cfg[SFLASH_CFG_SECTOR_SIZE_POS] = priv->dev->sectorsize / 1024;
	priv->sflash_cfg[SFLASH_CFG_SECTOR_ERASE_CMD_POS] = priv->dev->erase_cmd;

	priv->probed = true;

	return retval;
}

static int bl602_flash_auto_probe(struct flash_bank *bank)
{
	struct bl602_flash_bank *priv = bank->driver_priv;

	if (priv->probed) {
		return ERROR_OK;
	}

	return bl602_flash_probe(bank);
}

static void bl602_flash_free_driver_priv(struct flash_bank *bank)
{
	free(bank->driver_priv);
	bank->driver_priv = NULL;
}

FLASH_BANK_COMMAND_HANDLER(bl602_flash_bank_command)
{
	struct bl602_flash_bank *priv;
	priv = malloc(sizeof(struct bl602_flash_bank));
	priv->probed = false;
	priv->part_info = NULL;
#if 0
	const uint8_t data[84] = {
		0x04, 0x01, 0x01, 0x3F, 0x66, 0x99, 0xFF, 0x03, 0x9F, 0x00, 0x9F, 0x00, 0x04, 0xC8, 0x00, 0x01, 
		0xC7, 0x20, 0x52, 0xD8, 0x06, 0x02, 0x32, 0x00, 0x0B, 0x01, 0x0B, 0x01, 0x3B, 0x01, 0xBB, 0x00, 
		0x6B, 0x01, 0xEB, 0x02, 0xEB, 0x02, 0x02, 0x50, 0x00, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 
		0x02, 0x01, 0xAB, 0x01, 0x05, 0x35, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x38, 0xFF, 0xA0, 0xFF, 
		0x77, 0x03, 0x02, 0x40, 0x77, 0x03, 0x02, 0xF0, 0x2C, 0x01, 0xB0, 0x04, 0xB0, 0x04, 0x05, 0x00, 
		0x20, 0x4E, 0x14, 0x00, 
	};
	memcpy(priv->sflash_cfg, data, sizeof(priv->sflash_cfg));
#else
// 	memset(priv->sflash_cfg, 0x00, sizeof(priv->sflash_cfg));
	// set up default values in SFlash cfg
	// for (int i = 0; i < (int)sizeof(priv->sflash_cfg); i++) {
	// 	if (i > 51) {
	// 		priv->sflash_cfg[i] = 0x00;
	// 	}
	// }
	priv->sflash_cfg[SFLASH_CFG_JEDEC_ID_CMD_POS] = 0x9F;
	priv->sflash_cfg[SFLASH_CFG_JEDEC_ID_CMD_DMY_CLK_POS] = 0x0;
	priv->sflash_cfg[SFLASH_CFG_TIME_PAGE_PGM_POS] = 200;
	priv->sflash_cfg[SFLASH_CFG_WRITE_ENABLE_CMD_POS] = 0x06;
	priv->sflash_cfg[SFLASH_CFG_WRITE_ENABLE_INDEX_POS] = 0;
	priv->sflash_cfg[SFLASH_CFG_WRITE_ENABLE_READ_REG_LEN_POS] = 1;
	priv->sflash_cfg[SFLASH_CFG_WRITE_ENABLE_BIT_POS] = 1;
	priv->sflash_cfg[SFLASH_CFG_READ_STATUS_REG1_POS] = 0x05;
	priv->sflash_cfg[SFLASH_CFG_BUSY_INDEX_POS] = 0;
	priv->sflash_cfg[SFLASH_CFG_BUSY_READ_REG_LEN_POS] = 1;
	priv->sflash_cfg[SFLASH_CFG_BUSY_BIT_POS] = 0;
	priv->sflash_cfg[SFLASH_CFG_TIME_ERASE_SECTOR_POS] = (100 & 0xFF);
	priv->sflash_cfg[SFLASH_CFG_TIME_ERASE_SECTOR_POS + 1] = (100 & 0xFF);
#endif

	// for (int i = 0; i < (int)sizeof(priv->sflash_cfg); i++) {
	// 	printf("%02X ", priv->sflash_cfg[i]);
	// }
	// printf("\n");

	// set up driver_priv
	bank->driver_priv = priv;

	return ERROR_OK;
}

COMMAND_HANDLER(bl602_test)
{
	if (CMD_ARGC != 1)
		return ERROR_COMMAND_SYNTAX_ERROR;

	struct flash_bank *bank;
	int retval = CALL_COMMAND_HANDLER(flash_command_get_bank, 0, &bank);
	if (retval != ERROR_OK)
		return retval;
	// struct bl602_flash_bank *bl602_info = bank->driver_priv;

	retval = bl602_flash_init(bank);

	uint32_t jedec_id;
	retval = bl602_flash_read_id(bank, &jedec_id);
	if (retval != ERROR_OK)
		return retval;
	printf("JEDEC: %08X\n", jedec_id);
	return ERROR_OK;
}

COMMAND_HANDLER(bl602_call)
{
	if (CMD_ARGC < 2)
		return ERROR_COMMAND_SYNTAX_ERROR;

	struct flash_bank *bank;
	int retval = CALL_COMMAND_HANDLER(flash_command_get_bank, 0, &bank);
	if (retval != ERROR_OK)
		return retval;
	// struct bl602_flash_bank *priv = bank->driver_priv;
	struct target *target = bank->target;

	uint32_t addr, return_data;
	uint32_t args[5];
	int n_args = CMD_ARGC - 2;
	COMMAND_PARSE_NUMBER(u32, CMD_ARGV[1], addr);
	for (int i = 0; i < n_args; i++) {
		COMMAND_PARSE_NUMBER(u32, CMD_ARGV[2 + i], args[i]);
	}

	retval = bl602_call_func(target, addr, args, n_args, &return_data, 5000);
	printf("Return data: %08X\n", return_data);
	return retval;
}

static const struct command_registration bl602_command_handlers[] = {
	{
		.name = "bl602_test",
		.handler = bl602_test,
		.mode = COMMAND_EXEC,
		.help = "bl602 test",
		.usage = "",
	},
	{
		.name = "bl602_call",
		.handler = bl602_call,
		.mode = COMMAND_EXEC,
		.help = "bruh",
		.usage = "",
	},
	COMMAND_REGISTRATION_DONE
};

const struct flash_driver bl602_flash = {
	.name = "bl602_flash",
	.commands = bl602_command_handlers,
	.flash_bank_command = bl602_flash_bank_command,
	.erase = bl602_flash_erase,
	.write = bl602_flash_write,
	.read = bl602_flash_read,
	.probe = bl602_flash_probe,
	.auto_probe = bl602_flash_auto_probe,
	.erase_check = default_flash_blank_check,
	// .info = bl602_flash_info,
	.free_driver_priv = bl602_flash_free_driver_priv
};