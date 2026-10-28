# Milkjam

IchigoJam BASIC 1.6.1 (4K版) をベースにした **Raspberry Pi Pico 2 W (RP2350 + CYW43439 WiFi)** 版です。
WiFi接続・センサー・ThingsBoard送信・OLED表示・SDカードを1枚のPico 2 Wに統合した、IoTスターターキット向けBASICです。

📖 **使い方の詳細は [MANUAL_JA.md](MANUAL_JA.md) (マニュアル) を参照してください**

## ハードウェア
- **Raspberry Pi Pico 2 W** (RP2350A + CYW43439 WiFi、フラッシュ4MB)
- Pico 2 W では **GPIO 23/24/25/29 が内蔵WiFiチップ (CYW43) の接続に使用される**ため、他の用途には使えません (基板LEDもWiFiチップ経由。GP2のLEDマップはそのまま使用可能)
- I2Cセンサー接続: GP4 (SDA) / GP5 (SCL)
- 外部温度センサー DS18B20: GP27 (IN1と共有、`EJ "DS18B20",1` で切替)
- OLED (SSD1306 128x64): GP4/GP5のI2Cバスに共有接続 (アドレス 0x3C)
- RTC (DS3231): GP4/GP5のI2Cバスに共有接続
- **SDカード** (FAT32): GP6/8/10/11 + GP7 (検出) — IN3/IN4/OUT1/OUT3/OUT4と時分割共有 (`SDON`/`SDOFF`で切替)
- その他のピン割り当ては IchigoJam_P と同一 (GP26-28はADC使用可)

対応センサー (すべて任意。接続したものだけ自動認識):
BME280 (温度/湿度/気圧) / BH1750 (照度) / LTR390 (照度2/UV) / VL53L0X (距離) / DS18B20 ×2 (防水温度計) / DS3231 (RTC)

## 主な機能

### 表示 (3モード、`SWITCH`コマンドで切替)
| モード | 出力 | ピン | クロック |
|---|---|---|---|
| `SWITCH 0` | **DVI (HDMI)** 720x480p60 (TV互換480p) | GP12-19 (Pico DVI sock) | 270MHz |
| `SWITCH 1` | **NTSCコンポジット** | GP3 (75Ω+560pF外部回路) | 157.5MHz |
| `SWITCH 2` | **TFT液晶 ILI9341** | GP12-16 (**HDMIピンと排他**) | 270MHz |

- `SWITCH 2,1` でTFTを180°回転。モードと回転設定はflashに保存され、次回起動時にそのモードで起動する
- **LCDON / LCDOFFコマンド** (新設): TFT液晶 (GP12-16) はHDMI sock (GP12-19) とピンを共用するため**排他で使用**します
  - `LCDON` — LCDを有効化してLCDモードへ切替。以後 `SWITCH 0` (HDMI) はブロック (メッセージ表示のみ)
  - `LCDOFF` — LCDを無効化 (LCDモード中ならHDMIへ切替)。以後 `SWITCH 2` (LCD) はブロック
  - NTSC (GP3) は排他ピンではないため常に切替可能。排他状態はflashに保存
- **WS.LED** (WS2812B): ポート1-7 (OUT1-6＋LED/GP2)。**PIO送信**のため長いLEDテープでも表示やUSBキーボードが乱れない。SDON中は共有ピン (ポート1/3/4) の送信をブロック

### SDカード (`SDON` / `SDOFF`、スロット200-299)
- `SDON` — SDカードを初期化 (FAT32)。共有ピン (IN3/IN4/OUT1/OUT3/OUT4) のI/O命令はブロックされる
- `SDOFF` — SDを解放してI/Oピンを復帰
- `SAVE 200` → P200.IJB (バイナリ) / `SAVE 200,1` → **P200.BAS (アスキー形式・UTF-8、PCと連携可)**
- `LOAD 200` / `LRUN 200` — .IJB優先、なければ.BASを自動判読
- `FILES` — SDON中はSDカードの一覧
- TFT液晶とSPI1を共用するため、SD読み書き中のみ画面更新が一時停止する
- カード検出: GP7 (SDモジュールのCD端子、またはGND直結で常時挿入扱い)

### BASIC (IchigoJam_P 4K版ベース)
- プログラム領域4096バイト、拡張配列変数 [0]-[357]、`IR.IN` / `ENV.IN` 等
- 保存: 内蔵フラッシュ25本 (0-24) / 外部EEPROM (100-131) / **SDカード (200-299)**
- `VER()` = 16130 / `VER(1)` = 9 (RP2350)

### IOTPAD機能 (`EJ`コマンド)
- **WiFi接続**: `EJ "SETSSID","SSID"` / `EJ "SETPWD","PASS"` / `EJ "APC"` (接続) / `EJ "SYNCTIME"` (NTP時刻同期+RTCへ自動書込)
- **センサー値取得**: `EJ "GETTEMP"` 等 (引用符なし `EJ GETTEMP` も可)
- **変数/配列への代入**: `EJ GETTEMP,A` (A=23.5℃なら235) / `EJ GETTIME,[200]` ([200]〜[205]=年/月/日/時/分/秒)
- **PAD.IN**: `PAD.IN [100]` — 全センサー値+RTC時刻を16値の配列へ一括格納
- **ThingsBoard送信**: `EJ "SETTOKEN","<トークン>"` → `EJ "SENDTB"` (即時) / `EJ "SENDTB",60` (60秒ごと自動送信)。WiFi未接続時はRAMキューに貯めて接続後に送信
- **OLED表示**: 5画面 (環境/実験/較正/WiFi状況/バージョン) × 5フォント (`EJ "OLEDFONT",0`〜`4`)、`EJ "NEXTSCR"` で画面切替
- **較正**: `EJ "OFFSETTEMP",1.5` 等 (flashに保存され再起動後も維持) / `EJ "CALIBEXTTEMP"` (自動較正)
- **その他**: `EJ "HELP"` (全コマンド一覧) / `EJ "GETCONF"` (総合状態) / `EJ "GETALL"` (JSON一括取得) / `EJ "GETTIME"` / `EJ "SETTIME","2026-10-14T12:00:00"` (JST)

### クイックスタート
```
EJ "SETSSID","<SSID>"
EJ "SETPWD","<パスワード>"
EJ "APC"            … 接続 (IPアドレス表示)
EJ "SYNCTIME"       … NTP同期 + RTCへ書込 (以後WiFiなしでも時刻保持)
EJ "SETTOKEN","<ThingsBoardデバイスのアクセストークン>"
EJ "SENDTB"         … テレメトリ送信 → TB Code:200
EJ "SENDTB",60      … 60秒ごと自動送信 (0=OFF)
PAD.IN [100]
? [100],[101],[102],[110],[113]   … 温度/湿度/気圧/年/時
```

## 環境構築
- **CMake**と**GCC** (arm-none-eabi) をインストールする
- **IchigoJam_BASIC** は同梱済みです (コピー不要)
- 以下のライブラリを`Milkjam`ディレクトリ直下に置く
    - **pico-sdk**: https://github.com/raspberrypi/pico-sdk.git 2.3.1 (コミット `079c6f3`) `git submodule update --init --recursive` でサブモジュールも更新する
    - **pico-extras**: https://github.com/raspberrypi/pico-extras.git master (コミット `52fd7a7`)
    - **PicoDVI**: https://github.com/mlorenzati/PicoDVI.git master (動作確認: コミット `9eca921`)
- PicoDVIパッチを当てる (SWITCHコマンドによるDVI/NTSC/TFT切替の安定化に必須)
    `PicoDVI`ディレクトリで`patches/picodvi_dvi_stop_race_fix.patch`を適用する
    ```sh
    cd PicoDVI
    git apply ../patches/picodvi_dvi_stop_race_fix.patch
    ```
- TinyUSBパッチを当てる (USBキーボード利用に必須)
    `pico-sdk/lib/tinyusb/src/osal/osal_pico.h`の`osal_task_delay()`をbusy loopに置き換える
    ```c
    TU_ATTR_ALWAYS_INLINE static inline void osal_task_delay(uint32_t msec) {
        // sleep_ms(msec);
        uint64_t start = time_us_64();
        while (time_us_64() - start < msec * 1000) {
            ;
        }
    }
    ```
- 以下のパスを通す
    ```sh
    PICO_SDK_PATH=foo/Milkjam/pico-sdk
    PICO_EXTRAS_PATH=foo/Milkjam/pico-extras
    ```

## ビルド
```sh
cd Milkjam
mkdir build
cd build
cmake ..
make
```

`milkjam.uf2`ファイルが作成できたらPico 2 Wに書き込む (BOOTボタンを押しながら接続し、RPI-RP2ドライブへドラッグ＆ドロップ)

## ライセンス / license

### IchigoJam本体 / IchigoJam core (IchigoJam_BASIC, src/)
Copyright 2014-2025 the IchigoJam authors. All rights reserved. MIT license.

### 4K版拡張 / 4K version extensions (GivetakeJam_P準拠 / based on GivetakeJam_P)
4K版の拡張機能 (プログラム領域4096バイト化、拡張配列変数、`IR.IN`、`ENV.IN`等) は [GivetakeJam_P](https://github.com/Canon-boy4/ichigojam-firm) を参考にした実装であり、この拡張分は GivetakeJam_P と同じライセンス (MIT license, Copyright 2014-2025 the IchigoJam authors) に従います。

### SDカードドライバ / SD card driver (`src/sd_card.h`)
[Canon-boy4氏 GivetakeJam_P](https://github.com/Canon-boy4/ichigojam-firm/tree/rp2350-16130) (rp2350-16130ブランチ、MIT license) の実装を [IchigoJam-RP2040Zero](../IchigoJam-RP2040Zero) 経由で改変して収録しています。

### NTSC出力コード / NTSC output code (`src/ntsc/`)
NTSCコンポジット映像出力コード (`src/ntsc/`) は KenKen 氏による著作物です。
- 営利目的でなければ自由に利用可能。利用したものを公開する場合は、利用している旨をどこか (Webサイト等) に記載すること
- 営利利用については個別判断 (掲示板等で連絡のこと)
- 著作者: [KenKen](http://www.ze.em-net.ne.jp/~kenken/profile/index.html) — Copyright (C) 2024, KenKen, all right reserved.

### OLEDフォントデータ / OLED font data (`src/oled_fonts.h`)
OLED表示用フォントデータ (`src/oled_fonts.h`) は [U8g2](https://github.com/olikraus/u8g2) ライブラリ (olikraus, BSD-3-Clause license) のフォントデータを`tools/gen_oled_fonts.py`で変換したものです。

### WS2812 PIOプログラム (`src/ws2812.pio`)
pico-examples の ws2812 プログラム (Copyright Raspberry Pi Ltd, BSD-3-Clause license) を収録しています。

### 利用時の注意 / Notice
- このコードを利用する場合は、それぞれのライセンスを確認して、それぞれのライセンスに従ってください。
- 本リポジトリの内容は、いかなる保証も行うものではありません。利用によって生じたいかなる損害・不利益についても、一切の責任を負いません。

## 商標 / trademark
- IchigoJamは、株式会社jig.jpの登録商標です  (IchigoJam is a registered trademark of jig.jp co., ltd.)
- Raspberry Piは、Raspberry Pi財団の登録商標です  (Raspberry Pi is a registered trademark of Raspberry Pi Foundation)
