# Milkjam マニュアル / Manual

Milkjam (IchigoJam BASIC 1.6.1 4K版ベース / Raspberry Pi Pico 2 W) の使用マニュアルです。
セットアップ方法は [README_JA.md](README_JA.md) を参照してください。

## 目次
1. [ハードウェア](#1-ハードウェア)
2. [起動と診断](#2-起動と診断)
3. [表示出力 — SWITCH / LCDON / LCDOFF](#3-表示出力--switch--lcdon--lcdoff)
4. [SDカード — SDON / SDOFF](#4-sdカード--sdon--sdoff)
5. [EJコマンドリファレンス](#5-ejコマンドリファレンス)
6. [PAD.IN — センサー値の一括取得](#6-padin--センサー値の一括取得)
7. [EJ代入形式](#7-ej代入形式)
8. [OLED画面リファレンス](#8-oled画面リファレンス)
9. [ThingsBoard連携ガイド](#9-thingsboard連携ガイド)
10. [BASICプログラムサンプル](#10-basicプログラムサンプル)
11. [トラブルシューティング](#11-トラブルシューティング)

---

## 1. ハードウェア

### ピン割当 (Raspberry Pi Pico 2 W)

| 機能 | GP | 備考 |
|---|---|---|
| **DVI (HDMI) 出力** | **GP12-19** | Pico DVI sock (SWITCH 0、720x480p60) |
| **TFT液晶 ILI9341** | **GP12-16** | SCK=GP14、MOSI=GP15、CS=GP13、DC=GP16、RESET=GP12 (SWITCH 2、**HDMIピンと排他** → LCDON/LCDOFFで管理) |
| NTSCコンポジット出力 | GP3 | IchigoJam_Pと同一 (75Ω+560pFの回路でビデオ端子へ) |
| **SDカード (FAT32)** | **GP6/8/10/11 + GP7** | CS=GP6、検出=GP7、MISO=GP8、CLK=GP10、MOSI=GP11 (IN3/IN4/OUT1/OUT3/OUT4と時分割共有、SDON/SDOFFで切替) |
| I2Cバス (センサー・OLED・RTC) | GP4 (SDA) / GP5 (SCL) | 3.3V、プルアップは基板側または内蔵 |
| 外部温度センサー DS18B20 | GP27 | IN1入力と共有 (`EJ "DS18B20",1` で切替、要4.7kΩプルアップ) |
| 基板LED | GP2 | 心拍表示 (約1Hz点滅=正常稼働、約8Hz高速点滅=ハードフォルト) |
| **WiFiチップ (CYW43) 専用** | **GP23/24/25/29** | **他の用途には使用不可** (Pico 2 W固有の制約) |
| USBキーボード | Type-Cポート直結 | USBホスト |
| UART (診断出力) | GP0 (TX) | 115200bps (書込・診断用) |

WS.LED (WS2812B): ポート1-7 (OUT1-6＋LED/GP2)。PIO送信のため表示やUSBキーボードは乱れません。SDON中は共有ピン (ポート1/3/4) への送信はブロックされます。

### 対応センサーとI2Cアドレス

すべて任意接続。起動時に自動認識し、UARTに`PW: xxx OK/FAIL`を出力します。

| センサー | アドレス | 測定値 | 備考 |
|---|---|---|---|
| BME280 | 0x76 | 温度 / 湿度 / 気圧 | |
| BH1750 | 0x23 | 照度 (lux) | |
| LTR390 | 0x53 | 照度2 (lux2) / UV (cps) | |
| VL53L0X | 0x29 | 距離 (cm) | ToFレーザー測距 |
| DS3231 | 0x68 | RTC (時刻) | **電池バックアップ推奨** (NTP同期後、WiFiなしでも時刻保持) |
| SSD1306 OLED | 0x3C | 表示 (128x64) | 同一I2Cバスに共有接続 |
| DS18B20 ×2 | — (OneWire) | 外気温1 / 外気温2 | GP27、防水プローブ、4.7kΩプルアップ |

### 基板固有の注意
- Pico 2 Wの基板LED (GP25) はWiFiチップ経由のため直接GPIO操作禁止。GP2のIchigoJam互換LEDを使用
- WiFi使用時はCYW43ファームウェア(~226KB)がフラッシュを消費、WiFi用RAMとして~28KB追加使用
- Pico 2 Wのフラッシュは4MB。BASIC保存領域 (25本) と設定はフラッシュ末尾に配置
- GP26-28はADC (ANA()命令) として使用可

---

## 2. 起動と診断

### 起動シーケンス
1. タイトル表示: `Milkjam 1.6.1 (IchigoJam BASIC by jig.jp)`
2. WiFi初期化 (`PW: cyw43 init OK` / 約1〜2秒、表示は維持)
3. SSID保存済みなら自動接続開始 (安定後約3秒でNTP同期、成功でRTCへ書込)
4. センサー/OLED初期化 (`PW: BME280 OK` 等、1秒ごとに定期読み取り開始)

### UART診断 (GP0 TX、115200bps)

| メッセージ | 意味 |
|---|---|
| `PW: cyw43 init OK` / `FAIL` | WiFiチップ初期化 |
| `PW: link=3 (Ns)` | 接続待機中の状態 (3=IP取得済み、-2=SSID未発見、-3=パスワード誤り) |
| `PW: sntp start` / `PW: NTP recv` | NTP同期の開始/応答受信 |
| `PW: SYNCTIME ok` / `fail` | EJ "SYNCTIME"の結果 |
| `PW: RTC set` | RTC (DS3231) への時刻書込 |
| `PW: OLED OK` / `FAIL` | OLED認識 |
| `PW: BME280 OK` 等 | 各センサー認識 (BH1750/LTR390/VL53L0X/DS3231/DS18B20) |
| `PW: TB auto OK` / `err` | ThingsBoard自動送信結果 |
| `HF core=0 pc=... cfsr=...` | ハードフォルト発生時のレジスタダンプ (詳細はトラブルシュート) |

### LED診断
| 動作 | 意味 |
|---|---|
| 約1Hzの点滅 | 正常稼働 (NTSCモード時) |
| 約8Hzの高速点滅 | ハードフォルト (UARTダンプを確認) |
| 点灯しない | core0停止または起動失敗 |

---

## 3. 表示出力 — SWITCH / LCDON / LCDOFF

### 3モード映像出力 (`SWITCH`)

| コマンド | 出力 | 接続先 | 動作クロック |
|---|---|---|---|
| `SWITCH 0` | DVI (HDMI) 720x480p60 (TV互換480p) | GP12-19 (Pico DVI sock) | 270MHz |
| `SWITCH 1` | NTSCコンポジット | GP3 (75Ω直列+560pF対地GND) | 157.5MHz |
| `SWITCH 2` | TFT液晶 ILI9341 (SPI) | GP12-16 (**HDMIと排他**) | 270MHz |

- `SWITCH` (引数なし) — 次のモードへトグル (DVI→NTSC→TFT→DVI。ブロック中のモードはスキップ)
- `SWITCH 2,1` — TFTを180°回転 (`SWITCH 2,0` で通常向きに戻る)
- モードと回転設定は**flashに保存**され、次回起動時にそのモードで起動する
- NTSCモードはCPUが157.5MHz駆動のため、BASICの実行速度は270MHzモードより遅い
- TFTモードは差分更新のため、静止画中はSPIバスに触れない (表示はDVIと同じ白黒2色)
- **TFTとSDカードはSPI1を共用**するため、SDON中 (SD読み書き中) は画面更新が一時停止する

### HDMI/LCDピン排他 (`LCDON` / `LCDOFF`)

TFT液晶 (GP12-16) はDVI (HDMI sock、GP12-19) と**ピンを共用**するため、誤ったモードに切り替えて
信号を流さないよう**排他で使用**します (安全担保)。排他状態はflashに保存され、再起動後も維持されます。

| コマンド | 動作 |
|---|---|
| `LCDON` | **LCDを有効化**してLCD (TFT) モードへ切り替える。以後 `SWITCH 0` (HDMI) への切替を**ブロック** |
| `LCDOFF` | **LCDを無効化**する (LCDモード中はHDMI (DVI) へ切り替わる)。以後 `SWITCH 2` (LCD) への切替を**ブロック** |

- ブロックされた切替を実行すると、画面にメッセージを表示して切替を行いません (プログラムは継続):
  - LCDON中の`SWITCH 0` → `HDMI disabled (LCDOFF to use)`
  - LCDOFF中の`SWITCH 2` → `LCD disabled (LCDON to use)`
- **NTSC (GP3) は排他ピンではないため、どちらの状態でも切替可能**
- NTSCモード中に`LCDON`/`LCDOFF`すると、排他設定だけ変更してモードはそのまま (LCDON後に`SWITCH 2`でLCDへ)

### TFT液晶 (ILI9341) の配線

| TFT端子 | GP | Pico 2 W物理ピン |
|---|---|---|
| SCK | GP14 | 18番 |
| MOSI (SDA) | GP15 | 19番 |
| CS | GP13 | 16番 |
| DC | GP16 | 20番 |
| RESET | GP12 | 15番 |
| VCC / LEDA | 3.3V | 3V3 (OUT) |
| GND | GND | GND |
| SDO (MISO) | 未接続可 | — (書き込み専用動作) |

※GP12-16はHDMI sockピンと共用のため、**LCDとHDMI sockを同時に接続して使用しないでください**
(排他制御により同時表示はできませんが、物理的に一方のみの接続を推奨)

---

## 4. SDカード — SDON / SDOFF

FAT32形式のSDカード (≤32GB推奨、SDXCはFAT32で再フォーマット) にプログラムを保存できます (スロット200-299)。

### 配線 (IchigoJamのI/Oポートと時分割共有 — IchigoJam-RP2040Zero/GivetakeJam_P準拠)

| SDモジュール端子 | GP | 共有するI/O |
|---|---|---|
| DAT3 / CS | GP6 | IN3 |
| DAT0 / MISO | GP8 | OUT1 |
| CLK / SCK | GP10 | OUT3 |
| CMD / MOSI | GP11 | OUT4 |
| CD (カード検出、LOW=挿入) | GP7 | IN4 (CD端子がない場合はGP7をGNDに直結=常時挿入扱い) |

- **SDON中は共有ピン (IN3/IN4/OUT1/OUT3/OUT4) のI/O命令がブロック**されます (`IN`/`OUT`/`PWM`/`WS.LED`/`IR.IN`)
- `SDOFF`でSDを解放すると、ピンはI/O状態に戻ります
- ポーリング方式のためSD読み書き中もDVI/NTSC/TFTの表示は維持されます (TFTはSDとSPI1共用のため更新のみ一時停止)

### コマンド

| コマンド | 説明 |
|---|---|
| `SDON` | カード初期化+FAT32マウント → `SD ready (FAT32, 1000kHz)` |
| `SDOFF` | SD解放 (I/Oピンを復帰) |
| `SAVE 200` | P200.IJB (バイナリ、BASIC内部形式) |
| `SAVE 200,1` | **P200.BAS (アスキー形式・UTF-8)** — PCのエディタでそのまま閲覧・編集可 |
| `LOAD 200` / `LRUN 200` | .IJB優先、なければ.BASを自動判読 (LOADは現在のプログラムを消さずに読める) |
| `FILES` | SDON中はSDカード一覧 (200-299。番号/範囲明示指定は従来どおり) |

- `.BAS`は**UTF-8**で保存 (半角カナ1文字=3バイト。かな中心の長いプログラムは4096バイト制限に注意。**漢字は非対応、かなで記述**)
- LOADはUTF-8 (半角カナ/全角カナ→半角カナ、全角英数→半角の自動変換、BOM対応) と旧形式の生バイトを自動判別
- 読み取りCRC検査・書き込み後の全バイト照合を行います。失敗時は`SD read/write error`の詳細を表示
- ルートディレクトリのみ対応 (サブフォルダ不可、8.3形式ファイル名)

---

## 5. EJコマンドリファレンス

**基本形式**: `EJ "CMD"[,"ARG"]` — 引用符は省略可能 (`EJ CMD` / `EJ CMD,ARG`)。応答は画面に表示されます。

### WiFi / 時刻
| コマンド | 説明 |
|---|---|
| `EJ "SETSSID","<ssid>"` | SSID保存 (flash不揮発) |
| `EJ "SETPWD","<pass>"` | パスワード保存 |
| `EJ "SETTOKEN","<token>"` | ThingsBoardアクセストークン保存 |
| `EJ "SETSSIDHEX"/"SETPWDHEX"/"SETTOKENHEX","<hex>"` | 16進入力 (小文字・記号混在を正確に入力: 2桁×Nバイト) |
| `EJ "APC"` | 接続開始 (最大15秒待機、link状態を表示) |
| `EJ "APD"` | 切断 |
| `EJ "APS"` | 接続状態表示 (1/0) |
| `EJ "RECONNECT"` / `EJ "RECONNECT",<min>` | 自動再接続間隔 (0=OFF、分) |
| `EJ "SYNCTIME"` | NTP時刻同期 + **RTCへ自動書込** (同期済みなら即座に現在時刻を返す) |
| `EJ "GETTIME"` | RTCからJST時刻 `YYYY-MM-DDTHH:MM:SS` |
| `EJ "SETTIME","2026-10-14T12:00:00"` | JST指定でRTCへ書込 |
| `EJ "GETIP"` / `GETSSID` / `GETPWD` / `GETTOKEN` | 設定値表示 |

### センサー値取得 (GET系)
| コマンド | 表示 | 代入値 (×10系) |
|---|---|---|
| `EJ "GETTEMP"` | `23.5` | 235 (0.1℃単位) |
| `EJ "GETEXTTEMP1"` / `"GETEXTTEMP2"` | `22.1` 等 | ×10 (DS18B20 1本目/2本目) |
| `EJ "GETHUM"` | `45.2` | ×10 (0.1%RH) |
| `EJ "GETPRESS"` | `1013.25` | ×10 (**0.1hPa単位**=10132) |
| `EJ "GETLUX"` | `120` | lux整数値 (BH1750) |
| `EJ "GETLTRLUX"` | `45.3` | ×10 (LTR390照度2) |
| `EJ "GETLTRUV"` | UV指数相当 | ×10 |
| `EJ "GETLTRUVCPS"` | `123` | cps値 (LTR390 UVカウント) |
| `EJ "GETDISTANCE"` | `45.6` | ×10 (0.1cm、無効=0) |

- `GETxxx2` 形は×100の数値 (例: `GETTEMP2` → 2350)。関数形式 `?EJ("GETTEMP2")` も可 (引用符なし `?EJ(GETTEMP2)` 動作)
- **SET*値キャッシュ**: `EJ "SETTEMP","25"` 等で値を固定、`EJ "SETCLEAR"` で解除、`EJ "SETALL"` で現在値を一括キャッシュ
- **オフセット較正**: `EJ "OFFSETTEMP",1.5` 等5種 (flash不揮発)、`EJ "GETOFFSET"` 表示、`EJ "OFFSETCLEAR"` 全消去、`EJ "CALIBEXTTEMP"` で外気温1を基準に本体温度と外気温2を自動較正

### ThingsBoard
| コマンド | 説明 |
|---|---|
| `EJ "SENDTB"` | 即時送信 → `TB Code:200` (SET*キャッシュ優先、送信後キャッシュクリア) |
| `EJ "SENDTB",<sec>` | 自動送信間隔設定 (flash不揮発、`0`=OFF) |
| `EJ "GETQUEUE"` | `Queue:n/8 (RAM)` オフラインキュー状況 |
| `EJ "FLUSHQUEUE"` | キュー全送信 → `Flushed:n` |
| `EJ "CLEARQUEUE"` | キュー全消去 |

WiFi未接続時の`SENDTB`は取得時刻付きJSONをキューへ (`Queued(n)`)。最大8件、満杯時は最古を破棄。

### OLED制御
| コマンド | 説明 |
|---|---|
| `EJ "OLEDFONT",0〜4` | フォント切替 5x7 / 6x12 / **8x13 (標準)** / 9x15 / 10x20 (flash保存) |
| `EJ "OLEDPAGE",1〜5` / `EJ "NEXTSCR"` | 画面切替 |
| `EJ "OLEDON"` / `"OLEDOFF"` | 直接描画モード (5画面表示を停止/再開) |
| `EJ "OLEDCUR",x,y` / `"OLEDCLEAR"` / `"OLEDPRINT","text"` | 直接描画 ( baseline座標系) |

### その他
| コマンド | 説明 |
|---|---|
| `EJ "DS18B20",1` / `0` | GP27をDS18B20用/IN1用に切替 (flash保存) |
| `EJ "GETALL"` | 全センサー値JSON一括 |
| `EJ "GETCONF"` | 総合状態 (VER/SSID/TOKEN/IP/RTC/キュー/Auto/Recon/Font/Reset/オフセット) |
| `EJ "GETRESET"` | リセット原因 (`POR`=電源投入 / `WDT`=ウォッチドッグ) |
| `EJ "HELP"` | 全コマンド一覧 |
| `EJ "VER"` | バージョン |

---

## 6. PAD.IN — センサー値の一括取得

```
PAD.IN [n]
```
全センサー値とRTC時刻を**16値の配列へ一括格納**します (IchigoJam標準の`ENV.IN`と同じ形式。値は1秒周期の定期読み取り値)。

| 配列 | 内容 | 単位 |
|---|---|---|
| [n+0] | 温度 (BME280) | 0.1℃ |
| [n+1] | 湿度 (BME280) | 0.1%RH |
| [n+2] | 気圧 (BME280) | 0.1hPa |
| [n+3] | 外気温1 (DS18B20) | 0.1℃ |
| [n+4] | 外気温2 (DS18B20) | 0.1℃ |
| [n+5] | 照度 (BH1750) | lux |
| [n+6] | 照度2 (LTR390) | 0.1lux |
| [n+7] | UVカウント (LTR390) | cps |
| [n+8] | 距離 (VL53L0X) | 0.1cm (無効=0) |
| [n+9] | 状態フラグ | bit0:BME280 bit1:BH1750 bit2:LTR390 bit3:VL53L0X bit4:DS18B20 bit5:RTC (1=認識済) |
| [n+10]〜[n+15] | 年 / 月 / 日 / 時 / 分 / 秒 | JST (RTCなし=0) |

配列番号に変数・式も使えます: `PAD.IN [100+N]`。範囲は [n+15] ≤ [357]。

## 7. EJ代入形式

`EJ`の後ろに**変数や配列を書くと、数値結果を代入して画面出力なし**になります (引用符不要):

```
EJ GETTEMP,A      → Aに温度×10を代入 (23.5℃なら A=235)
EJ GETTEMP,[100]  → [100]に代入
EJ GETTIME,[200]  → [200]〜[205]に年/月/日/時/分/秒 (JST) を代入
EJ SYNCTIME,A    → A=1 (同期成功) / 0
?EJ(GETTEMP)     → 関数形式 (式の中で使用可)
```
- 代入対象は数値を返すコマンド (GET系/SYNCTIME/SENDTB等) で、**変数は1文字+行末**の形式です
- ターゲットを書かない場合は従来どおり応答を画面表示します
- スケールはPAD.INと同一 (0.1単位系)

## 8. OLED画面リファレンス

`EJ "OLEDPAGE",n` / `EJ "NEXTSCR"` で5画面を巡回します。

| 画面 | 内容 |
|---|---|
| 1 環境 | 日付 / `JST hh:mm (曜日)` / 温度+外気温 / 湿度+気圧 / 照度+UV |
| 2 実験 | 外気温1 / 外気温2 / 照度 / 照度2 / `CPS:n D:n.n` |
| 3 較正 | 5種のオフセット値 (`EJ "OFFSET*"`で変更、画面を見ながら較正) |
| 4 WiFi | 接続状態 / 自動送信間隔 / SSID / IP / キュー数 |
| 5 バージョン | バージョン / センサー認識状況 |

右上のWiFiアイコン: 接続済み=波型、未接続=×型。フォントは5種 (`EJ "OLEDFONT",0〜4`、8x13標準)。

## 9. ThingsBoard連携ガイド

1. ThingsBoardでデバイスを作成し、**アクセストークン**をコピー
2. Milkjam側の設定:
```
EJ "SETTOKEN","<アクセストークン>"
EJ "SENDTB"          → TB Code:200
```
3. 送信先: `http://15sat.jp:8080/api/v1/<トークン>/telemetry` (ichigojam_iotpad_no15e準拠)
4. ペイロード形式 (ThingsBoard時系列API。`ts`はNTP/RTCの正確なUNIXミリ秒):
```json
{"ts":1791800000000,"values":{"temperature":23.5,"exttemp1":22.1,"exttemp2":-3.2,
 "humidity":45.2,"pressure":1013.25,"lux":120,"lux2":45.3,"uv_cps":123,"distance":45.6}}
```
5. 自動送信: `EJ "SENDTB",60` (60秒ごと、flash保存で再起動後も継続)
6. **オフラインキュー**: WiFi未接続時の送信は自動でキューに入り、接続後の送信時に`ts`付きでまとめて送信 (最大8件)

## 10. BASICプログラムサンプル

### センサーロガー (1分ごとThingsBoard送信)
```
10 EJ "SENDTB",60
20 PAD.IN [100]
30 ? "T:";[100]/10;"C H:";[101]/10;"%"
40 WAIT 300
50 GOTO 20
```

### 時刻表示 (RTC、WiFiなしで動作)
```
10 EJ GETTIME,[200]
20 ? [200];"/";[201];"/";[202];" ";[203];":";[204]
30 WAIT 600
40 GOTO 10
```

### 温度がしきい値を超えたら通知
```
10 EJ GETTEMP,A
20 IF A>300 ? "HOT!":EJ "SENDTB"
30 WAIT 60
40 GOTO 10
```

## 11. トラブルシューティング

| 症状 | 対処 |
|---|---|
| 起動しない / 画面が出ない | UF2を書き直す。NTSC受信機の入力切替確認。HDMIはTVが480pに対応しているか確認。`SWITCH`でモード切替 (UART: `PW: cyw43 init OK`より前に出ない=起動失敗) |
| HDMIに何も表示されない | `SWITCH 0`を確認。TV側が640x480 (VGA) を認識しないことがあるため本機は720x480p60 (TV互換480p) 出力。DVI sockの配線 (GP12-19) を確認 |
| TFTに何も表示されない | `LCDON`を実行してから`SWITCH 2` (LCD無効中はブロックされる)。配線 (SCK=GP14/MOSI=GP15/CS=GP13/DC=GP16/RESET=GP12) と3.3V電源を確認。白画面のままの場合は`src/tft.h`の`TFT_SPI_BAUD`を15.75MHzに落とす |
| `HDMI disabled` / `LCD disabled` と表示される | HDMI/LCDピン排他が働いています。HDMIに戻すには`LCDOFF`、LCDを使うには`LCDON` |
| `SD init error` / `SD FS error` | カード挿入と配線 (CS=GP6/MISO=GP8/CLK=GP10/MOSI=GP11/検出=GP7) を確認。**FAT32必須** (SDXC/exFATは再フォーマット)。CD端子がないモジュールはGP7をGNDへ |
| SD保存が失敗する | `SD read/write error`の詳細表示を確認。品質の悪いカードは`SD FALLBACK`で100kHzに自動低下します。書込み後の照合に失敗するカードは使用を避ける |
| WiFi接続しない | SSID/パスワード再設定 (`GETSSID`/`GETPWD`で確認)。UARTの`link=-2`=SSID未発見、`link=-3`=パスワード誤り。2.4GHz帯のみ対応 |
| `Sync Failed` | NTPサーバへ到達できず。`APC`で再接続後`SYNCTIME`再実行。DNSはWiFiルータ側設定も確認 |
| 時刻が9時間ずれる | RTCは**UTC保存・JST表示**仕様。RTCが古い場合は`SYNCTIME`で更新、または`SETTIME`で手動設定 |
| `TB Code`が200以外 | トークン再確認 (`GETTOKEN`)。ThingsBoard側デバイスの有効確認。`401`=トークン不正 |
| OLEDに何も表示されない | 接続 (SDA=GP4/SCL=GP5)、アドレス0x3C。UARTの`PW: OLED FAIL`=認識失敗 |
| LEDが高速点滅 (約8Hz) | ハードフォルト。UART (`HF core=... pc=... cfsr=...`) のダンプを記録すると原因特定できる |
| `EJ xxx`が`Unknown:xxx` | コマンド名確認 (`EJ "HELP"`)。引用符なし入力時はカンマ区切りに注意 |
| キーボードが効かない | Type-Cポート直結のUSBキーボードを使用 (PC経由のUSBハブは不可の場合あり) |
| DS18B20の値が出ない | `EJ "DS18B20",1` (GP27をOneWireに切替、初期化後に有効)。4.7kΩプルアップを確認 |

---

ライセンス・商標については [README_JA.md](README_JA.md) を参照してください。
