# OpenBouffalo's OpenOCD work repository

This repository contains all OpenOCD patches, which are work in progress, or in process of upstreaming.

# Matrix table

- ✅ - Available in upstream OpenOCD
- 🌊 - In the process of upstreaming
- 🛠️ - Work in progress
- ❌ - No support

|                      | BL602 | BL702 | BL702L | BL808 | BL616 |
|----------------------|-------|-------|--------|-------|-------|
| Basic support        |   🌊   |   ✅   |    🌊   |   ❌   |   ❌   |
| Proper reset         |   🌊   |   ✅   |    🌊   |   ❌   |   ❌   |
| Flash driver support |   🌊   |   🌊   |    🌊   |   ❌   |   ❌   |

# Pending patches for upstreaming

- [flash/nor/bl602: add bl602 flash driver](https://review.openocd.org/c/openocd/+/8527) - branch: `feature/bl602-flash-driver`
- [tcl/target: add Bouffalo Lab BL602 and BL702L chip series support](https://review.openocd.org/c/openocd/+/8593) - branch: `feature/bl602-bl702-bl702l-tcl`