# Milkjam

An **Raspberry Pi Pico 2 W (wireless)** version based on IchigoJam BASIC 1.6.1 (4K edition).
A BASIC for IoT starter kits that integrates WiFi, sensors, ThingsBoard telemetry, OLED display and SD card storage on a single Pico 2 W.

📖 **See [MANUAL.md](MANUAL.md) for detailed usage instructions**

## Hardware
- **Raspberry Pi Pico 2 W** (RP2350A + CYW43439 WiFi, 4MB flash)
- On Pico 2 W, **GPIO 23/24/25/29 are used by the on-board WiFi chip (CYW43)** and are not available for other purposes (the on-board LED is driven via the WiFi chip; the GP2 LED mapping still works)
- I2C sensors connect to GP4 (SDA) / GP5 (SCL)
- External temperature sensor DS18B20: GP27 (shared with IN1, toggle with `EJ "DS18B20",1`)
- OLED (SSD1306 128x64): shared I2C bus on GP4/GP5 (address 0x3C)
- RTC (DS3231): shared I2C bus on GP4/GP5
- **SD card** (FAT32): GP6/8/10/11 + GP7 (card detect) — time-shared with IN3/IN4/OUT1/OUT3/OUT4 (toggle with `SDON`/`SDOFF`)
- All other pin assignments are identical to IchigoJam_P (GP26-28 usable as ADC)

Supported sensors (all optional; only connected ones are detected):
BME280 (temp/humidity/pressure) / BH1750 (lux) / LTR390 (lux2/UV) / VL53L0X (distance) / DS18B20 ×2 (waterproof temp) / DS3231 (RTC)

## Features

### Display (3 modes, switch with the `SWITCH` command)
| Mode | Output | Pins | Clock |
|---|---|---|---|
| `SWITCH 0` | **DVI (HDMI)** 720x480p60 (TV-compatible 480p) | GP12-19 (Pico DVI sock) | 270MHz |
| `SWITCH 1` | **NTSC composite** | GP3 (75Ω+560pF external circuit) | 157.5MHz |
| `SWITCH 2` | **TFT LCD ILI9341** | GP12-16 (**exclusive with HDMI**) | 270MHz |

- `SWITCH 2,1` rotates the TFT by 180°. The mode and rotation are saved to flash and restored on the next boot
- **LCDON / LCDOFF commands** (new): the TFT LCD (GP12-16) shares pins with the HDMI sock (GP12-19), so they are used **exclusively**
  - `LCDON` — enable the LCD and switch to LCD mode. `SWITCH 0` (HDMI) is blocked afterwards (message only)
  - `LCDOFF` — disable the LCD (switches to HDMI if in LCD mode). `SWITCH 2` (LCD) is blocked afterwards
  - NTSC (GP3) is always switchable (not a shared pin). The lock state is saved to flash
- **WS.LED** (WS2812B): ports 1-7 (OUT1-6 + LED/GP2). **Driven by PIO**, so long LED tapes do not disturb the display or USB keyboard. Blocked on shared pins (ports 1/3/4) while SDON

### SD card (`SDON` / `SDOFF`, slots 200-299)
- `SDON` — initialize the card (FAT32). I/O commands on the shared pins (IN3/IN4/OUT1/OUT3/OUT4) are blocked
- `SDOFF` — release the card and restore the I/O pins
- `SAVE 200` → P200.IJB (binary) / `SAVE 200,1` → **P200.BAS (ASCII text, UTF-8, PC-friendly)**
- `LOAD 200` / `LRUN 200` — prefers .IJB, falls back to .BAS automatically
- `FILES` — lists the SD card while SDON
- Shares SPI1 with the TFT LCD; the display update pauses only during SD read/write
- Card detect: GP7 (CD pin of the SD module, or tie to GND for "always inserted")

### BASIC (based on the IchigoJam_P 4K edition)
- 4096-byte program area, extended array variables [0]-[357], `IR.IN` / `ENV.IN`, etc.
- Storage: internal flash 25 slots (0-24) / external EEPROM (100-131) / **SD card (200-299)**
- `VER()` = 16130 / `VER(1)` = 9 (RP2350)

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
- Apply the PicoDVI patch (required for stable DVI/NTSC/TFT switching with the SWITCH command)
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

Write the `milkjam.uf2` file to the Pico 2 W (hold BOOT while plugging in, then drag & drop onto the RPI-RP2 drive).

## License

### IchigoJam core (IchigoJam_BASIC, src/)
Copyright 2014-2025 the IchigoJam authors. All rights reserved. MIT license.

### 4K version extensions (based on GivetakeJam_P)
The 4K edition extensions (4096-byte program area, extended array variables, `IR.IN`, `ENV.IN`, etc.) are implemented with reference to [GivetakeJam_P](https://github.com/Canon-boy4/ichigojam-firm) and follow the same license as GivetakeJam_P (MIT license, Copyright 2014-2025 the IchigoJam authors).

### SD card driver (`src/sd_card.h`)
Adapted from [Canon-boy4's GivetakeJam_P](https://github.com/Canon-boy4/ichigojam-firm/tree/rp2350-16130) (rp2350-16130 branch, MIT license), via [IchigoJam-RP2040Zero](../IchigoJam-RP2040Zero).

### NTSC output code (`src/ntsc/`)
The NTSC composite video output code (`src/ntsc/`) is copyrighted by KenKen.
- Free to use for non-commercial purposes. When publishing works using it, please mention the usage somewhere (website etc.)
- Commercial use requires individual judgment (contact via the BBS etc.)
- Author: [KenKen](http://www.ze.em-net.ne.jp/~kenken/profile/index.html) — Copyright (C) 2024, KenKen, all right reserved.

### OLED font data (`src/oled_fonts.h`)
The OLED font data (`src/oled_fonts.h`) is converted from the [U8g2](https://github.com/olikraus/u8g2) library (olikraus, BSD-3-Clause license) fonts using `tools/gen_oled_fonts.py`.

### WS2812 PIO program (`src/ws2812.pio`)
Includes the ws2812 program from pico-examples (Copyright Raspberry Pi Ltd, BSD-3-Clause license).

### Notice
- When using this code, check each license and follow each respective license.
- The contents of this repository are provided "AS IS", without warranty of any kind. The authors are not liable for any damages or disadvantages arising from the use of them.

## Trademark
- IchigoJam is a registered trademark of jig.jp co., ltd.
- Raspberry Pi is a registered trademark of the Raspberry Pi Foundation
