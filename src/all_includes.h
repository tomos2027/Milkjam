//main.c以外のファイルにもincludeしているのは、IDEに認識させるため
#ifndef __ALL_INCLUDES_H__
#define __ALL_INCLUDES_H__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef IJB_PICO_NO_PRINT
#define printf(...)
#endif

//pico-sdk
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/flash.h"
#include "hardware/irq.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"
#include "hardware/adc.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "hardware/structs/scb.h"
#include "hardware/i2c.h"
// SDK定義の後、BASIC命令定義の前にSDドライバーを読み込む (SDON/SDOFF、SAVE/LOAD 200-299)
// ※tft.hがsd_pins_reservedを参照するため、tft.hより前である必要がある
#include "sd_card.h"

//pico-extras
#include "pico/sleep.h"
#include "hardware/rosc.h"

//PicoDVI
#include "dvi.h"
#include "tmds_encode.h"
#include "common_dvi_pin_configs.h"
#include "sprite.h"

//NTSCコンポジット出力 (rp2040_pwm_ntsc_textgraph)
#include "rp2040_pwm_ntsc_textgraph.h"

#include "tusb.h"
#include "hid.h"

//TV互換のCEA-861 720x480p60 (480p)。640x480p60 (VGA) はTVが認識しないことがある
#define DVI_TIMING dvi_timing_720x480p_60hz

// for IchigoJam BASIC
#define NO_MEMCPY
#define uart_putc ichigojam_uart_putc //picoのものと名前衝突しているのを解決
#include "config.h"
#include "IchigoJam_BASIC/ichigojam-stddef.h"
#include "IchigoJam_BASIC/random.h"
#include "IchigoJam_BASIC/lang.h"
#include "IchigoJam_BASIC/vars.h"
#include "IchigoJam_BASIC/ram.h"
#include "IchigoJam_BASIC/screen.h"
#include "IchigoJam_BASIC/error.h"
#include "IchigoJam_BASIC/ichigojam-jp.fnt.h"
#include "IchigoJam_BASIC/psg.h"
#include "IchigoJam_BASIC/basic.h"
//---
#include "i2c.h"
#include "i2ceeprom.h"
#include "tft.h"//TFT (ILI9341) ドライバ、display.hより先 (video_on/off/activeが参照するため)
#include "display.h"//storage.h/keyboard.hがvideo_active等を使うため、それより先にincludeする
#include "storage.h"
#include "keyboard.h"
#include "hid_app.c"
#include "io.h"
#include "system.h"
#include "sound.h"
#include "ext_ws.h"
#include "ext_wifi.h" // Pico W: WiFi (CYW43+lwIP) 接続管理 (IOTPAD)
#include "iotpad_sensors.h" // Pico W: IOTPADセンサードライバ (BME280/BH1750/LTR390/VL53L0X/DS18B20/DS3231)
#include "iotpad_tb.h" // Pico W: IOTPAD ThingsBoard送信 (lwIP HTTP、フェーズ5) ※iotpad_oled.hより前 (画面4がキュー数等を参照)
#include "oled_fonts.h" // Pico W: 事前デコード済みU8g2フォントデータ (OLED用)
#include "iotpad_oled.h" // Pico W: IOTPAD OLED表示 (SSD1306 128x64)
#include "ext_iotpad.h" // Pico W: IOTPAD「EJ」コマンド
#include "mainc_functions.h"
#include "IchigoJam_BASIC/ext_iot.h"
#include "usr.h"
#include "init.h"

#endif