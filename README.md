# UzeMOD

4-channel MOD file player for Uzebox. Based on [MODPlay](https://github.com/prochazkaml/MODPlay) by Michal Procházka. Uses basically every free CPU cycle to just barely pull it off :)


See discussion on the [Uzebox forums](https://uzebox.org/forums/viewtopic.php?t=11501)
## Overview

UzeMOD is a standalone `.mod` player for the Uzebox platform. It supports 4 channels (ProTracker format), making heavy use of Uzenet expansion RAM. An SD card **and** expansion RAM are _required_ for playback.

Thanks do danboid for back and forth testing, feedback, live demonstration at Northwest Computer Museum, and this demo video:

[![Watch the demo](https://img.youtube.com/vi/CSrFbTvUJGQ/0.jpg)](https://youtu.be/CSrFbTvUJGQ)

For emulation:

- [CUzeBox](https://github.com/Jubatian/cuzebox) supports 128KB(limits what files can be loaded).  
- [CUzeBoxESP8266](https://github.com/weber21w/cuzebox-8266) supports 128KB-8MB modules.  

_Web emulator demo coming when I get a chance._

## Requirements

- Uzebox with Uzenet expansion RAM (128 KB minimum, more for larger files, possible 512K hardware revision coming soon...)
- SD card formatted FAT16/FAT32 


## Building

1. Place the UzeMOD folder under your uzebox-master/demos/ directory:
```text
uzebox-master/
└── demos/
└── UzeMOD/
├── UzeMOD.c
├── UzeMOD.h
└── default/
└── Makefile
```
3. Navigate into the demo build directory:  
   ```bash
   cd uzebox-master/demos/UzeMOD/default

4. Build with avr-gcc. This will produce a .hex file, and if have the proper packrom at ../../../bin/packrom, it will also create a .uze file

make

4. Use the .hex or .uze directly in an emulator. On hardware Flash the .hex directly with an ISP, or better, copy the .uze to your SD card and use the bootloader

