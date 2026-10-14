//IchigoJam PW: WiFi接続管理 (Pico W内蔵CYW43 + lwIP)
//ichigojam_iotpad_no15e のWiFi機能をPico W内部に移植したもの (IOTPAD機能の一部)
//EJコマンド (ext_iotpad.h) から使用する。既存ファイルへの影響は
//init.hのiotpad_wifi_bootinit()/iotpad_wifi_tick()呼び出しのみ
#ifndef __EXT_WIFI_H__
#define __EXT_WIFI_H__
#include "all_includes.h"

#include "pico/cyw43_arch.h"
#include "cyw43.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/apps/sntp.h"

void iotpad_dbg_str(const char* s); //init.hで定義 (UART診断出力、ext_wifi.hより後ろでincludeされるため前方宣言)
void iotpad_ds18b20_load_flag(); //iotpad_sensors.hで定義 (ext_wifi.hより後ろでincludeされるため前方宣言)
void iotpad_oled_load_font(int idx); //iotpad_oled.hで定義 (ext_wifi.hより後ろでincludeされるため前方宣言)
void iotpad_tb_load_auto(uint16_t v); //iotpad_tb.hで定義 (ext_wifi.hより後ろでincludeされるため前方宣言)
void iotpad_sensors_load_offsets(const uint8_t* cfg); //iotpad_sensors.hで定義 (オフセット5種の起動復元)

//設定保存 (flash設定セクタ内レイアウト: [0]=KBD [1]=ビデオモード、[2]以降をIOTPAD用に使用)
#define PW_CFG_SSID_OFF    2
#define PW_CFG_SSID_LEN    32
#define PW_CFG_PASS_OFF    34
#define PW_CFG_PASS_LEN    64
#define PW_CFG_TOKEN_OFF   98
#define PW_CFG_TOKEN_LEN   64
#define PW_CFG_RECON_OFF   162 // 自動再接続間隔(分) 1byte
#define PW_CFG_DS18B20_OFF 163 // DS18B20有効フラグ (0=OFF 1=ON) 1byte
#define PW_CFG_FONT_OFF    164 // OLEDフォント番号 0-4 1byte
#define PW_CFG_TB_AUTO_OFF 165 // ThingsBoard自動送信間隔(秒) 2byte LE (0=OFF)
#define PW_CFG_OFFS_OFF    167 // センサーオフセット5種 (temp/hum/pres/ext1/ext2) 各float 4byte LE 計20byte

#define PW_WIFI_CONNECT_TIMEOUT_MS 15000 // EJ "APC"の接続待ち (DHCP含めて余裕を持たせる)
#define PW_WIFI_STABLE_MS         3000   // 接続後にNTP同期を開始するまでの安定待ち (pad準拠)
#define PW_NTP_WAIT_MS            5000   // EJ "SYNCTIME"のNTP待ち (pad準拠)

static char iotpad_ssid[PW_CFG_SSID_LEN + 1];
static char iotpad_pass[PW_CFG_PASS_LEN + 1];
static char iotpad_token[PW_CFG_TOKEN_LEN + 1];
static int  iotpad_reconnect_min;   // 自動再接続間隔(分、0=OFF)
static bool iotpad_wifi_started;   // cyw43_arch_init済み
static bool iotpad_wifi_want;      // 接続を維持すべきか (SSID保存済み or 手動APC)
static uint32_t iotpad_wifi_connect_ms; // 最終接続開始時刻(起動ms)
static bool iotpad_ntp_done;        // 今回の接続でNTP同期を開始済み
static volatile bool iotpad_sntp_synced; // SNTP受信済み (lwIP IRQが書くためvolatile)
static bool iotpad_sntp_started;    // sntp_init実行済み (2重init防止)
static uint64_t iotpad_sntp_us_base; // SNTP受信時の起動クロック(µs)
static uint32_t iotpad_sntp_sec_base; // SNTP受信時のUNIX秒(UTC)

//基板LED (GP2: io.hのLED) の制御 — 診断表示用
//Pico WではGP25(Pico無印の基板LED)がWiFi SPIバス専用のため使用禁止、代わりにIchigoJam互換基板のLED(GP2)を使う
static bool iotpad_led_state;
void iotpad_led_set(int on) {
    gpio_init(LED);
    gpio_set_dir(LED, GPIO_OUT);
    gpio_put(LED, on);
    iotpad_led_state = on;
}
void iotpad_led_toggle() {
    iotpad_led_set(!iotpad_led_state);
}

//WiFi状態をPico Wモジュールの内蔵LED (CYW43 WL_GPIO0) に表示: 接続済み=点灯
static bool iotpad_wl_led_on;
static void iotpad_wifi_update_status_led(bool connected) {
    if (!iotpad_wifi_started || connected == iotpad_wl_led_on) return;
    iotpad_wl_led_on = connected;
    cyw43_arch_lwip_begin();
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, connected);
    cyw43_arch_lwip_end();
}

//NTP同期済みのUNIXミリ秒 (未同期=0) — ThingsBoard送信のtsに使用
//※uint64必須: UNIX秒×1000はuint32を溢れる (1,791,000,000×1000が折り返して1970年表示になる実測バグ)
static uint64_t iotpad_get_unix_ms() {
    if (!iotpad_sntp_synced) return 0;
    return (uint64_t)iotpad_sntp_sec_base * 1000ULL + (time_us_64() - iotpad_sntp_us_base) / 1000;
}
static uint32_t iotpad_get_unix_sec() {
    if (!iotpad_sntp_synced) return 0;
    return iotpad_sntp_sec_base + (uint32_t)((time_us_64() - iotpad_sntp_us_base) / 1000000);
}

//UNIX秒→年月日時分秒 (Howard Hinnantのcivil_from_days)
static void iotpad_unix_to_ymd(uint32_t sec, uint16_t* y, uint8_t* mo, uint8_t* d, uint8_t* h, uint8_t* mi, uint8_t* s) {
    uint32_t days = sec / 86400;
    uint32_t t = sec % 86400;
    days += 719468;
    uint32_t era = days / 146097;
    uint32_t doe = days - era * 146097;
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    uint32_t yy = yoe + era * 400;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint32_t mp = (5 * doy + 2) / 153;
    *d = (uint8_t)(doy - (153 * mp + 2) / 5 + 1);
    *mo = (uint8_t)(mp < 10 ? mp + 3 : mp - 9);
    *y = (uint16_t)(yy + (*mo <= 2));
    *h = (uint8_t)(t / 3600);
    *mi = (uint8_t)((t / 60) % 60);
    *s = (uint8_t)(t % 60);
}
//年月日時分秒→UNIX秒 (Howard Hinnantのdays_from_civil)
static uint32_t iotpad_ymd_to_unix(uint16_t y, uint8_t mo, uint8_t d, uint8_t h, uint8_t mi, uint8_t s) {
    y -= (mo <= 2);
    uint32_t era = y / 400;
    uint32_t yoe = y - era * 400;
    //3月=0起点への変換 (mo-3は負になり得るためuint8_t cast禁止 — int算術で計算する)
    uint32_t m0 = (mo > 2) ? (uint32_t)mo - 3 : (uint32_t)mo + 9;
    uint32_t doy = (153 * m0 + 2) / 5 + d - 1;
    uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int32_t days = (int32_t)(era * 146097 + doe) - 719468;
    return (uint32_t)days * 86400 + h * 3600 + mi * 60 + s;
}

//SNTP受信時にlwIPから呼ばれる (lwipopts.hのSNTP_SET_SYSTEM_TIME、lwIP IRQコンテキストで動作)
void iotpad_sntp_set_time(uint32_t sec) {
    iotpad_sntp_synced = true;
    iotpad_sntp_us_base = time_us_64();
    iotpad_sntp_sec_base = sec;
    iotpad_dbg_str("PW: NTP recv\n");//診断: NTP応答の受信を確認 (UARTポーリング書きなのでIRQ内でも安全)
}

//接続済みか (IPアドレス取得済みで判定)
static bool iotpad_wifi_is_connected() {
    if (!iotpad_wifi_started) return false;
    struct netif* n = netif_default;
    return n && netif_is_up(n) && !ip4_addr_isany(netif_ip4_addr(n));
}

//WiFiリンク状態 (CYW43_LINK_*: 0=DOWN 1=JOIN 2=NOIP 3=UP / -1=FAIL -2=NONET -3=BADAUTH)
static int iotpad_wifi_link_status() {
    if (!iotpad_wifi_started) return -100;
    return cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA);
}

//IPアドレスの文字列表現
static void iotpad_wifi_get_ip(char* buf, int len) {
    if (iotpad_wifi_is_connected()) {
        snprintf(buf, len, "%s", ip4addr_ntoa(netif_ip4_addr(netif_default)));
    } else {
        snprintf(buf, len, "0.0.0.0");
    }
}

//flash設定セクタの部分更新 (読み出し→変更→消去/書込)
//NTSC稼働中は画面を止めずに書き込む (IJB_saveと同じ方式)
static void iotpad_save_config(const void* data, int offset, int len) {
    uint32_t off = get_config_offset();
    memcpy(tflash, get_flash(off), FLASH_SECTOR_SIZE);
    if (data) {
        memcpy(tflash + offset, data, len);
    }
    if (ntsc_active()) {
        int save = save_and_disable_interrupts();
        flash_range_erase(off, FLASH_SECTOR_SIZE);
        flash_range_program(off, tflash, FLASH_SECTOR_SIZE);
        restore_interrupts(save);
        ntsc_release();
        ntsc_init();
    } else {
        bool active = video_active();
        video_off(0);
        int save = save_and_disable_interrupts();
        flash_range_erase(off, FLASH_SECTOR_SIZE);
        flash_range_program(off, tflash, FLASH_SECTOR_SIZE);
        if (active) {
            video_on();
        }
        restore_interrupts(save);
    }
}

//設定の読み込み (flash→RAM、起動時に呼ぶ)
static void iotpad_load_settings() {
    const uint8_t* cfg = get_flash(get_config_offset());
    memcpy(iotpad_ssid, cfg + PW_CFG_SSID_OFF, PW_CFG_SSID_LEN);
    iotpad_ssid[PW_CFG_SSID_LEN] = 0;
    memcpy(iotpad_pass, cfg + PW_CFG_PASS_OFF, PW_CFG_PASS_LEN);
    iotpad_pass[PW_CFG_PASS_LEN] = 0;
    memcpy(iotpad_token, cfg + PW_CFG_TOKEN_OFF, PW_CFG_TOKEN_LEN);
    iotpad_token[PW_CFG_TOKEN_LEN] = 0;
    iotpad_reconnect_min = cfg[PW_CFG_RECON_OFF];
    //未設定(消去状態=0xFF)を空文字/0として扱う (wifi初期化を実行しないようにする)
    if ((uint8)iotpad_ssid[0] == 0xFF) iotpad_ssid[0] = 0;
    if ((uint8)iotpad_pass[0] == 0xFF) iotpad_pass[0] = 0;
    if ((uint8)iotpad_token[0] == 0xFF) iotpad_token[0] = 0;
    if ((uint8)iotpad_reconnect_min == 0xFF) iotpad_reconnect_min = 0;
    //DS18B20有効フラグ読み込み (IOTPAD、iotpad_sensors.h)
    iotpad_ds18b20_load_flag();
    //OLEDフォント番号読み込み (IOTPAD、iotpad_oled_h。未設定=0xFFは範囲外なのでiotpad_oled_load_font内で拒否=規定値2=8x13維持)
    iotpad_oled_load_font((int)(uint8)cfg[PW_CFG_FONT_OFF]);
    //ThingsBoard自動送信間隔読み込み (IOTPAD、iotpad_tb.h。未設定=0xFFFFは0=OFFとして扱う)
    uint16_t tb_auto = (uint16_t)(cfg[PW_CFG_TB_AUTO_OFF] | (cfg[PW_CFG_TB_AUTO_OFF + 1] << 8));
    iotpad_tb_load_auto(tb_auto == 0xFFFF ? 0 : tb_auto);
    //センサーオフセット5種読み込み (IOTPAD、iotpad_sensors.h)
    iotpad_sensors_load_offsets(cfg + PW_CFG_OFFS_OFF);
}

//SNTP開始 (NTPサーバはpad準拠)
//※lwIPのsntp_init()は「既に初期化済みなら何もしない」仕様のため、冪等化して2回呼ばない
//  (sntp_stop()はDNS解決保留中のコールバックがNULLのpcbへアクセスする危険があるため使用しない)
//※メインスレッドからlwIP関数を呼ぶためcyw43_arch_lwip_begin/endでロック必須
static void iotpad_sntp_start() {
    if (iotpad_sntp_started) return;//開始済みなら何もしない (sntp_initのno-op仕様対策)
    iotpad_sntp_started = true;
    cyw43_arch_lwip_begin();
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "ntp.nict.jp");
    sntp_setservername(1, "pool.ntp.org");
    sntp_init();
    cyw43_arch_lwip_end();
    iotpad_dbg_str("PW: sntp start\n");//診断
}

//使用時の遅延初期化 (起動時の初期化に失敗していた場合の再試行)
static bool iotpad_wifi_ensure_init() {
    if (iotpad_wifi_started) return true;
    if (cyw43_arch_init()) return false;
    iotpad_wifi_started = true;
    iotpad_wifi_update_status_led(false);
    return true;
}

//非同期接続開始 (結果はtick/APSで確認)
static void iotpad_wifi_join_async() {
    cyw43_arch_enable_sta_mode();
    if (cyw43_arch_wifi_connect_async(iotpad_ssid, iotpad_pass, CYW43_AUTH_WPA2_MIXED_PSK) == 0) {
        iotpad_wifi_connect_ms = to_ms_since_boot(get_absolute_time());
        iotpad_wifi_want = true;
    }
}

//ブロック付き接続 (EJ "APC") — 待機中もNTSC表示・ウォッチドッグは生きている
//待機中のリンク状態をUARTに出力する (診断用: -3=BADAUTH=認証失敗、-2=NONET=SSID未発見等)
static bool iotpad_wifi_connect_blocking(int timeout_ms) {
    if (!iotpad_ssid[0]) return false;
    if (!iotpad_wifi_ensure_init()) return false;
    iotpad_wifi_join_async();
    uint64_t start = time_us_64();
    int last_status = -100;
    uint64_t last_print = 0;
    while (time_us_64() - start < (uint64_t)timeout_ms * 1000) {
        if (iotpad_wifi_is_connected()) break;
        int st = iotpad_wifi_link_status();
        uint64_t now = time_us_64();
        if (st != last_status || now - last_print >= 1000000) {
            last_status = st;
            last_print = now;
            char dbg[40];
            snprintf(dbg, sizeof dbg, "PW: link=%d (%llus)\n", st, (unsigned long long)((now - start) / 1000000));
            iotpad_dbg_str(dbg);
        }
        sleep_ms(100);
    }
    iotpad_dbg_str("PW: link wait end\n");
    return iotpad_wifi_is_connected();
}

static void iotpad_wifi_disconnect() {
    if (iotpad_wifi_started) {
        //sntp_stopはlwIP内部状態を操作するためロック必須
        cyw43_arch_lwip_begin();
        sntp_stop();
        cyw43_arch_lwip_end();
        iotpad_sntp_started = false;//次回接続時にsntp_initをやり直せるようにする
        iotpad_sntp_synced = false;
        cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
        iotpad_wifi_want = false;
        iotpad_ntp_done = false;
    }
}

//起動時: WiFi(CYW43)を常に初期化し、SSID保存済みなら自動接続する
//※ichigojam_init(タイトル+OK表示)の後にmain()から呼ぶ:
//  - コールスタックが浅く安定した環境で初期化するため (BASICコマンド実行中の深いスタックから呼ぶと安定しない)
//  - cyw43_arch_initのファームウェアロード(~440KB、1〜2秒)はNTSC割り込みより優先度が低いため表示は維持される
//  - 失敗時は画面に診断を表示して継続 (BASICはWiFiなしでも動作可、EJ "APC"で再試行)
void iotpad_wifi_bootinit() {
    iotpad_load_settings();
    iotpad_dbg_str("\r\nPW: cyw43 init...\r\n");//診断: WiFi初期化開始 (UART 115200 GP0)
    if (cyw43_arch_init()) {
        //CYW43初期化失敗 (EJ "APC"時に再試行する)
        iotpad_dbg_str("PW: cyw43 init FAIL\r\n");
        put_str("W:WIFI INIT FAIL\n");
        return;
    }
    iotpad_dbg_str("PW: cyw43 init OK\r\n");
    iotpad_wifi_started = true;
    iotpad_wifi_update_status_led(false);//未接続=消灯
    if (iotpad_ssid[0]) {
        iotpad_wifi_join_async();
    }
}

//WiFi管理の定期処理 (main()のループから呼ぶ — メインスレッド限定!)
//※状態LED(cyw43_arch_gpio_put)や今後のThingsBoard送信はasync contextと同期する処理のため、
//  割り込みコールバック(timer()等)から呼ぶとデッドロック/未定義動作になる
static uint32_t iotpad_wifi_tick_last_ms;
void iotpad_wifi_tick() {
    if (!iotpad_wifi_started) return;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - iotpad_wifi_tick_last_ms < 10) return; //ループから高頻度で呼ばれるため10msに間引く
    iotpad_wifi_tick_last_ms = now;
    bool connected = iotpad_wifi_is_connected();
    iotpad_wifi_update_status_led(connected);
    if (connected) {
        if (!iotpad_ntp_done && now - iotpad_wifi_connect_ms >= PW_WIFI_STABLE_MS) {
            iotpad_ntp_done = true;
            iotpad_sntp_start(); // 接続安定後にNTP同期開始 (pad準拠)
        }
    } else {
        iotpad_ntp_done = false;
        if (iotpad_wifi_want && iotpad_reconnect_min > 0
            && now - iotpad_wifi_connect_ms > (uint32_t)iotpad_reconnect_min * 60000) {
            iotpad_wifi_join_async(); // 定期再接続 (padのRECONNECT間隔)
        }
    }
}

#endif
