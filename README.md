# Milkjam

An **Raspberry Pi Pico W (wireless)** version based on IchigoJam BASIC 1.6.1 (NTSC composite output + 4K edition).
A BASIC for IoT starter kits that integrates WiFi, sensors, ThingsBoard telemetry and OLED display on a single Pico W.

📖 **See [MANUAL.md](MANUAL.md) for detailed usage instructions**

## Hardware
- **Raspberry Pi Pico W** (RP2040 + CYW43439 WiFi)
- On Pico W, **GPIO 23/24/25/29 are used by the on-board WiFi chip (CYW43)** and are not available for other purposes (the on-board LED is driven via the WiFi chip; the GP2 LED mapping still works)
- I2C sensors connect to GP4 (SDA) / GP5 (SCL)
- External temperature sensor DS18B20: GP27 (shared with IN1, toggle with `EJ "DS18B20",1`)
- OLED (SSD1306 128x64): shared I2C bus on GP4/GP5 (address 0x3C)
- RTC (DS3231): shared I2C bus on GP4/GP5
- NTSC composite output: GP3 — same as IchigoJam_P
- All other pin assignments are identical to IchigoJam_P

Supported sensors (all optional; only connected ones are detected):
BME280 (temp/humidity/pressure) / BH1750 (lux) / LTR390 (lux2/UV) / VL53L0X (distance) / DS18B20 ×2 (waterproof temp) / DS3231 (RTC)

## Features

### BASIC (based on the IchigoJam_P 4K edition)
- NTSC composite output + DVI (HDMI) output (switch with the `SWITCH` command)
- 4096-byte program area, extended array variables [0]-[357], `IR.IN` / `ENV.IN`, etc.

### IOTPAD features (`EJ` commands)
- **WiFi**: `EJ "SETSSID","SSID"` / `EJ "SETPWD","PASS"` / `EJ "APC"` (connect) / `EJ "SYNCTIME"` (NTP sync + write to RTC)
- **Sensor values**: `EJ "GETTEMP"` etc. (quotes optional: `EJ GETTEMP`)
- **Assignment to variables/arrays**: `EJ GETTEMP,A` (A=235 for 23.5°C) / `EJ GETTIME,[200]` ([200]-[205]=year/month/day/hour/min/sec)
- **PAD.IN**: `PAD.IN [100]` — stores all sensor values + RTC time into a 16-value array
- **ThingsBoard**: `EJ "SETTOKEN","<token>"` then `EJ "SENDTB"` (immediate) / `EJ "SENDTB",60` (auto every 60s). Queued in RAM while WiFi is disconnected
- **OLED**: 5 screens (environment/experiment/calibration/WiFi/version) × 5 fonts (`EJ "OLEDFONT",0`-`4`), switch screens with `EJ "NEXTSCR"`
- **Calibration**: `EJ "OFFSETTEMP",1.5` etc. (saved to flash) / `EJ "CALIBEXTTEMP"` (auto calibration)
- **Others**: `EJ "HELP"` (command list) / `EJ "GETCONF"` (status) / `EJ "GETALL"` (JSON) / `EJ "GETTIME"` / `EJ "SETTIME","2026-10-14T12:00:00"` (JST)

### Quick start
```
EJ "SETSSID","<SSID>"
EJ "SETPWD","<password>"
EJ "APC"            ... connect (IP address shown)
EJ "SYNCTIME"       ... NTP sync + RTC (time kept even without WiFi)
EJ "SETTOKEN","<ThingsBoard device access token>"
EJ "SENDTB"         ... send telemetry -> TB Code:200
EJ "SENDTB",60      ... auto send every 60s (0=OFF)
PAD.IN [100]
? [100],[101],[102],[110],[113]   ... temp/humidity pressure/year/hour
```

## Setup
- Install **CMake** and **GCC** (arm-none-eabi)
- **IchigoJam_BASIC** is already included (no copy needed)
- Place the following libraries directly under the `Milkjam` directory
    - **pico-sdk**: https://github.com/raspberrypi/pico-sdk.git 2.3.1 (commit `079c6f3`), update submodules with `git submodule update --init --recursive`
    - **pico-extras**: https://github.com/raspberrypi/pico-extras.git master (commit `52fd7a7`)
    - **PicoDVI**: https://github.com/mlorenzati/PicoDVI.git master (tested: commit `9eca921`)
- Apply the PicoDVI patch (required for stable DVI/NTSC switching with the SWITCH command)
    ```sh
    cd PicoDVI
    git apply ../patches/picodvi_dvi_stop_race_fix.patch
    ```
- Apply the TinyUSB patch (required for USB keyboards): replace `osal_task_delay()` in `pico-sdk/lib/tinyusb/src/osal/osal_pico.h` with a busy loop
    ```c
    TU_ATTR_ALWAYS_INLINE static inline void osal_task_delay(uint32_t msec) {
        // sleep_ms(msec);
        uint64_t start = time_us_64();
        while (time_us_64() - start < msec * 1000) {
            ;
        }
    }
    ```
- Set the following paths
    ```sh
    PICO_SDK_PATH=foo/Milkjam/pico-sdk
    PICO_EXTRAS_PATH=foo/Milkjam/pico-extras
    ```

## Build
```sh
cd Milkjam
mkdir build
cd build
cmake ..
make
```

Write the `milkjam.uf2` file to the Pico W.

## License

### IchigoJam core (IchigoJam_BASIC, src/)
Copyright 2014-2025 the IchigoJam authors. All rights reserved. MIT license.

### 4K version extensions (based on GivetakeJam_P)
The 4K edition extensions (4096-byte program area, extended array variables, `IR.IN`, `ENV.IN`, etc.) are implemented with reference to [GivetakeJam_P](https://github.com/Canon-boy4/ichigojam-firm) and follow the same license as GivetakeJam_P (MIT license, Copyright 2014-2025 the IchigoJam authors).

### NTSC output code (`src/ntsc/`)
The NTSC composite video output code (`src/ntsc/`) is copyrighted by KenKen.
- Free to use for non-commercial purposes. When publishing works using it, please mention the usage somewhere (website etc.)
- Commercial use requires individual judgment (contact via the BBS etc.)
- Author: [KenKen](http://www.ze.em-net.ne.jp/~kenken/profile/index.html) — Copyright (C) 2024, KenKen, all right reserved.

### OLED font data (`src/oled_fonts.h`)
The OLED font data (`src/oled_fonts.h`) is converted from the [U8g2](https://github.com/olikraus/u8g2) library (olikraus, BSD-3-Clause license) fonts using `tools/gen_oled_fonts.py`.

### Notice
- When using this code, check each license and follow each respective license.
- The contents of this repository are provided "AS IS", without warranty of any kind. The authors are not liable for any damages or disadvantages arising from the use of them.

## Trademark
- IchigoJam is a registered trademark of jig.jp co., ltd.
- Raspberry Pi is a registered trademark of the Raspberry Pi Foundation
