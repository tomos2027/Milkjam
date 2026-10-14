# Milkjam Manual

User manual for Milkjam (based on IchigoJam BASIC 1.6.1 4K edition / Raspberry Pi Pico W).
For setup instructions, see [README.md](README.md).

## Table of contents
1. [Hardware](#1-hardware)
2. [Boot sequence and diagnostics](#2-boot-sequence-and-diagnostics)
3. [EJ command reference](#3-ej-command-reference)
4. [PAD.IN — bulk sensor reading](#4-padin--bulk-sensor-reading)
5. [EJ assignment form](#5-ej-assignment-form)
6. [OLED screen reference](#6-oled-screen-reference)
7. [ThingsBoard integration guide](#7-thingsboard-integration-guide)
8. [BASIC program samples](#8-basic-program-samples)
9. [Troubleshooting](#9-troubleshooting)

---

## 1. Hardware

### Pin assignment (Raspberry Pi Pico W)

| Function | GP | Notes |
|---|---|---|
| NTSC composite output | GP3 | Same as IchigoJam_P (video out via 75Ω+G circuit) |
| I2C bus (sensors/OLED/RTC) | GP4 (SDA) / GP5 (SCL) | 3.3V, pull-ups required on module side |
| External temp sensor DS18B20 | GP27 | Shared with IN1 input (toggle with `EJ "DS18B20",1`, needs 4.7kΩ pull-up) |
| On-board LED | GP2 | Heartbeat (approx. 1Hz blink = running, approx. 8Hz = hardfault) |
| **WiFi chip (CYW43) reserved** | **GP23/24/25/29** | **Not usable for other purposes** (Pico W hardware constraint) |
| USB keyboard | Type-C port | USB host |
| UART (diagnostics) | GP0 (TX) | 115200bps |

### Supported sensors and I2C addresses

All sensors are optional. Detected at boot; `PW: xxx OK/FAIL` is printed on UART.

| Sensor | Address | Measurement | Notes |
|---|---|---|---|
| BME280 | 0x76 | Temperature / humidity / pressure | |
| BH1750 | 0x23 | Lux | |
| LTR390 | 0x53 | Lux2 / UV (cps) | |
| VL53L0X | 0x29 | Distance (cm) | ToF laser ranging |
| DS3231 | 0x68 | RTC (time) | **Battery backup recommended** (keeps time without WiFi after NTP sync) |
| SSD1306 OLED | 0x3C | Display (128x64) | Shared I2C bus |
| DS18B20 ×2 | — (OneWire) | Ext temp 1 / 2 | GP27, waterproof probes, 4.7kΩ pull-up |

### Board-specific notes
- The Pico W on-board LED (GP25) is driven via the WiFi chip; direct GPIO access is forbidden. Use the GP2 IchigoJam-compatible LED.
- WiFi uses CYW43 firmware (~226KB flash) and ~28KB additional RAM.

---

## 2. Boot sequence and diagnostics

### Boot sequence
1. Title: `Milkjam 1.6.1 (IchigoJam BASIC by jig.jp)`
2. WiFi init (`PW: cyw43 init OK` / takes 1-2s, NTSC display stays alive)
3. Auto-connect if SSID is saved (NTP sync starts ~3s after connect, writes to RTC on success)
4. Sensor/OLED init (`PW: BME280 OK` etc.), periodic reading every 1 second

### UART diagnostics (GP0 TX, 115200bps)

| Message | Meaning |
|---|---|
| `PW: cyw43 init OK` / `FAIL` | WiFi chip init |
| `PW: link=3 (Ns)` | Connection state (3=IP acquired, -2=SSID not found, -3=wrong password) |
| `PW: sntp start` / `PW: NTP recv` | NTP sync started / response received |
| `PW: SYNCTIME ok` / `fail` | Result of EJ "SYNCTIME" |
| `PW: RTC set` | Time written to RTC (DS3231) |
| `PW: OLED OK` / `FAIL` | OLED detected |
| `PW: BME280 OK` etc. | Each sensor (BH1750/LTR390/VL53L0X/DS3231/DS18B20) |
| `PW: TB auto OK` / `err` | ThingsBoard auto-send result |
| `HF core=0 pc=... cfsr=...` | Register dump on hardfault (see Troubleshooting) |

### LED diagnostics
| Behavior | Meaning |
|---|---|
| Blinks at ~1Hz | Running normally (NTSC mode) |
| Fast blink ~8Hz | Hardfault (check the UART dump) |
| No blink | core0 stopped or boot failure |

---

## 3. EJ command reference

**Basic form**: `EJ "CMD"[,"ARG"]` — quotes are optional (`EJ CMD` / `EJ CMD,ARG`). Responses are printed on screen.

### WiFi / time
| Command | Description |
|---|---|
| `EJ "SETSSID","<ssid>"` | Save SSID (flash) |
| `EJ "SETPWD","<pass>"` | Save password |
| `EJ "SETTOKEN","<token>"` | Save ThingsBoard access token |
| `EJ "SETSSIDHEX"/"SETPWDHEX"/"SETTOKENHEX","<hex>"` | Hex input (for mixed-case/symbols: 2 digits × N bytes) |
| `EJ "APC"` | Connect (waits up to 15s, shows link state) |
| `EJ "APD"` | Disconnect |
| `EJ "APS"` | Connection state (1/0) |
| `EJ "RECONNECT"` / `EJ "RECONNECT",<min>` | Auto-reconnect interval (0=OFF, minutes) |
| `EJ "SYNCTIME"` | NTP sync + **write to RTC** (returns current time instantly if already synced) |
| `EJ "GETTIME"` | RTC time as JST `YYYY-MM-DDTHH:MM:SS` |
| `EJ "SETTIME","2026-10-14T12:00:00"` | Write to RTC (JST input) |
| `EJ "GETIP"` / `GETSSID` / `GETPWD` / `GETTOKEN` | Show settings |

### Sensor values (GET series)
| Command | Display | Assignment value (x10 series) |
|---|---|---|
| `EJ "GETTEMP"` | `23.5` | 235 (0.1°C units) |
| `EJ "GETEXTTEMP1"` / `"GETEXTTEMP2"` | `22.1` etc. | x10 (DS18B20 #1/#2) |
| `EJ "GETHUM"` | `45.2` | x10 (0.1%RH) |
| `EJ "GETPRESS"` | `1013.25` | x10 (**0.1hPa units** = 10132) |
| `EJ "GETLUX"` | `120` | lux integer (BH1750) |
| `EJ "GETLTRLUX"` | `45.3` | x10 (LTR390 lux2) |
| `EJ "GETLTRUV"` | UV index value | x10 |
| `EJ "GETLTRUVCPS"` | `123` | cps value (LTR390 UV count) |
| `EJ "GETDISTANCE"` | `45.6` | x10 (0.1cm, 0 if invalid) |

- `GETxxx2` forms return x100 numbers (e.g. `GETTEMP2` → 2350). Function form `?EJ("GETTEMP2")` also works (quotes optional: `?EJ(GETTEMP2)`)
- **SET* value cache**: `EJ "SETTEMP","25"` etc. pins a value; `EJ "SETCLEAR"` releases; `EJ "SETALL"` caches all current values
- **Offset calibration**: `EJ "OFFSETTEMP",1.5` etc. (5 types, saved to flash), `EJ "GETOFFSET"` shows, `EJ "OFFSETCLEAR"` resets all, `EJ "CALIBEXTTEMP"` auto-calibrates body temp and ext temp 2 against ext temp 1

### ThingsBoard
| Command | Description |
|---|---|
| `EJ "SENDTB"` | Send now → `TB Code:200` (SET* cache takes priority; cache cleared after send) |
| `EJ "SENDTB",<sec>` | Set auto-send interval (flash, `0`=OFF) |
| `EJ "GETQUEUE"` | `Queue:n/8 (RAM)` offline queue status |
| `EJ "FLUSHQUEUE"` | Flush queue → `Flushed:n` |
| `EJ "CLEARQUEUE"` | Clear queue |

`SENDTB` while WiFi is disconnected stores JSON with the measurement time into the queue (`Queued(n)`). Max 8 entries; oldest is dropped when full.

### OLED control
| Command | Description |
|---|---|
| `EJ "OLEDFONT",0-4` | Font: 5x7 / 6x12 / **8x13 (default)** / 9x15 / 10x20 (saved to flash) |
| `EJ "OLEDPAGE",1-5` / `EJ "NEXTSCR"` | Switch screens |
| `EJ "OLEDON"` / `"OLEDOFF"` | Direct draw mode (stops/resumes the 5-screen display) |
| `EJ "OLEDCUR",x,y` / `"OLEDCLEAR"` / `"OLEDPRINT","text"` | Direct drawing (baseline coordinates) |

### Others
| Command | Description |
|---|---|
| `EJ "DS18B20",1` / `0` | Switch GP27 between DS18B20 / IN1 (saved to flash) |
| `EJ "GETALL"` | All sensor values as JSON |
| `EJ "GETCONF"` | Full status (VER/SSID/TOKEN/IP/RTC/queue/Auto/Recon/Font/Reset/offsets) |
| `EJ "GETRESET"` | Reset cause (`POR`=power-on / `WDT`=watchdog) |
| `EJ "HELP"` | Command list |
| `EJ "VER"` | Version |

---

## 4. PAD.IN — bulk sensor reading

```
PAD.IN [n]
```
Stores all sensor values and the RTC time into a **16-value array** (same style as the standard `ENV.IN`; values are from the 1-second periodic reading).

| Array | Content | Units |
|---|---|---|
| [n+0] | Temperature (BME280) | 0.1°C |
| [n+1] | Humidity (BME280) | 0.1%RH |
| [n+2] | Pressure (BME280) | 0.1hPa |
| [n+3] | Ext temp 1 (DS18B20) | 0.1°C |
| [n+4] | Ext temp 2 (DS18B20) | 0.1°C |
| [n+5] | Lux (BH1750) | lux |
| [n+6] | Lux2 (LTR390) | 0.1lux |
| [n+7] | UV count (LTR390) | cps |
| [n+8] | Distance (VL53L0X) | 0.1cm (0 if invalid) |
| [n+9] | Status flags | bit0:BME280 bit1:BH1750 bit2:LTR390 bit3:VL53L0X bit4:DS18B20 bit5:RTC (1=detected) |
| [n+10]-[n+15] | Year / month / day / hour / min / sec | JST (0 if no RTC) |

Variables and expressions work for the array index: `PAD.IN [100+N]`. Range: [n+15] <= [357].

## 5. EJ assignment form

Writing a **variable or array after `EJ`** assigns the numeric result silently (no quotes needed):

```
EJ GETTEMP,A      → A = temperature x10 (A=235 for 23.5°C)
EJ GETTEMP,[100]  → assigned to [100]
EJ GETTIME,[200]  → [200]-[205] = year/month/day/hour/min/sec (JST)
EJ SYNCTIME,A    → A=1 (synced) / 0
?EJ(GETTEMP)     → function form (usable inside expressions)
```
- Works with numeric-result commands (GET series/SYNCTIME/SENDTB etc.); the variable must be **a single letter at end of line**
- Without a target, the response is printed as usual
- Scaling is identical to PAD.IN (0.1-unit series)

## 6. OLED screen reference

Cycle through the 5 screens with `EJ "OLEDPAGE",n` / `EJ "NEXTSCR"`.

| Screen | Content |
|---|---|
| 1 Environment | Date / `JST hh:mm (weekday)` / temp + ext temp / humidity + pressure / lux + UV |
| 2 Experiment | Ext temp 1 / ext temp 2 / lux / lux2 / `CPS:n D:n.n` |
| 3 Calibration | The 5 offset values (change with `EJ "OFFSET*"`, calibrate while watching) |
| 4 WiFi | Connection / auto-send interval / SSID / IP / queue count |
| 5 Version | Version / sensor detection status |

Top-right WiFi icon: connected = waves, disconnected = X. 5 fonts available (`EJ "OLEDFONT",0-4`, 8x13 default).

## 7. ThingsBoard integration guide

1. Create a device in ThingsBoard and copy its **access token**
2. Configure Milkjam:
```
EJ "SETTOKEN","<access token>"
EJ "SENDTB"          -> TB Code:200
```
3. Endpoint: `http://15sat.jp:8080/api/v1/<token>/telemetry` (same as ichigojam_iotpad_no15e)
4. Payload (ThingsBoard timeseries API; `ts` is the exact UNIX milliseconds from NTP/RTC):
```json
{"ts":1791800000000,"values":{"temperature":23.5,"exttemp1":22.1,"exttemp2":-3.2,
 "humidity":45.2,"pressure":1013.25,"lux":120,"lux2":45.3,"uv_cps":123,"distance":45.6}}
```
5. Auto-send: `EJ "SENDTB",60` (every 60s, survives reboot)
6. **Offline queue**: sends while disconnected are queued automatically and flushed with their original `ts` when reconnected (max 8)

## 8. BASIC program samples

### Sensor logger (ThingsBoard every 60s)
```
10 EJ "SENDTB",60
20 PAD.IN [100]
30 ? "T:";[100]/10;"C H:";[101]/10;"%"
40 WAIT 300
50 GOTO 20
```

### Clock display (RTC, works without WiFi)
```
10 EJ GETTIME,[200]
20 ? [200];"/";[201];"/";[202];" ";[203];":";[204]
30 WAIT 600
40 GOTO 10
```

### Alert when temperature exceeds a threshold
```
10 EJ GETTEMP,A
20 IF A>300 ? "HOT!":EJ "SENDTB"
30 WAIT 60
40 GOTO 10
```

## 9. Troubleshooting

| Symptom | Fix |
|---|---|
| Does not boot / no display | Reflash the UF2. Check the TV input. If flashed in DVI mode, switch with `SWITCH` (no `PW: cyw43 init OK` on UART = boot failure) |
| WiFi does not connect | Reset SSID/password (`GETSSID`/`GETPWD` to verify). UART `link=-2`=SSID not found, `link=-3`=wrong password. 2.4GHz band only |
| `Sync Failed` | NTP unreachable. Reconnect with `APC` then retry `SYNCTIME`. Check DNS on the router |
| Time off by 9 hours | RTC stores **UTC, displays JST** by design. Update with `SYNCTIME` or set manually with `SETTIME` |
| `TB Code` other than 200 | Check token (`GETTOKEN`) and the device in ThingsBoard. `401`=invalid token |
| OLED blank | Check wiring (SDA=GP4/SCL=GP5), address 0x3C. `PW: OLED FAIL` on UART = not detected |
| LED fast blink (~8Hz) | Hardfault. Record the UART dump (`HF core=... pc=... cfsr=...`) to identify the cause |
| `EJ xxx` returns `Unknown:xxx` | Check the command name (`EJ "HELP"`). Watch comma separation when omitting quotes |
| Keyboard not working | Connect the keyboard directly to the Type-C port (some PC USB hubs don't work) |
| No DS18B20 values | Run `EJ "DS18B20",1` (switches GP27 to OneWire; takes effect after init). Check the 4.7kΩ pull-up |

---

For licenses and trademarks, see [README.md](README.md).
