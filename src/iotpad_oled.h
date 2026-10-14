//IchigoJam PW: IOTPAD OLED表示 (SSD1306 128x64、I2C 0x3C)
//U8g2フォントを事前デコードしたビットマップで描画 (pad=ichigojam_iotpad_no15e準拠)
//行モデル: baseline = (i+1)*フォント高さ + y_offset (padのoled_line_yと同一式)
//グリフモデル (u8g2_font.c準拠): top = y - (h + gy), left = x + gx
//5画面レイアウト + WiFiアイコン表示
#ifndef __IOTPAD_OLED_H__
#define __IOTPAD_OLED_H__
#include "all_includes.h"
//フォントデータはall_includes.hでoled_fonts.hとして既にinclude済み

#define OLED_ADDR     0x3C
#define OLED_WIDTH    128
#define OLED_HEIGHT   64
#define OLED_PAGES    (OLED_HEIGHT / 8)
#define OLED_BUFSIZ   (OLED_WIDTH * OLED_PAGES)

//画面更新間隔 (padのDISPLAY_UPDATE_INTERVAL_MS準拠)
#define OLED_UPDATE_INTERVAL_MS 200

//===== SSD1306 コマンド =====
#define OLED_CMD          0x00
#define OLED_DATA         0x40

//===== 状態 =====
#define OLED_FONT_COUNT 5
#define OLED_DEFAULT_FONT 2 // 8x13標準 (padのDEFAULT_FONT_INDEX準拠)
static bool oled_ok = false;
static bool oled_direct = false;
static int  oled_font_index = OLED_DEFAULT_FONT;
static int  oled_cursor_x = 0, oled_cursor_y = 0;
static int  oled_current_screen = 1;
static uint32_t oled_last_update_ms;

//フレームバッファ (フォント描画より前の位置に定義)
static uint8_t oled_buf[OLED_BUFSIZ];
static void oled_buf_pixel(int x, int y, bool on) {
    if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) return;
    if (on) oled_buf[y / 8 * OLED_WIDTH + x] |= (1 << (y & 7));
    else    oled_buf[y / 8 * OLED_WIDTH + x] &= ~(1 << (y & 7));
}

//===== フォントテーブル (padのFONT_TABLE準拠) =====
static const OLED_Char* oled_font_tables[OLED_FONT_COUNT] = {
    oled_font_5_7,
    oled_font_6_12,
    oled_font_8_13,
    oled_font_9_15,
    oled_font_10_20,
};
static const char* oled_font_names[OLED_FONT_COUNT] = { "5x7", "6x12", "8x13", "9x15", "10x20" };
static const uint8_t oled_font_h[OLED_FONT_COUNT] = { 7, 12, 13, 15, 20 };       // フォント高さ
static const int8_t oled_font_yoff[OLED_FONT_COUNT] = { 0, 0, -1, 0, 0 };       // pad準拠のy補正 (8x13は上に1px)

//起動時のフォント番号設定 (flash設定。ext_wifi.hのiotpad_load_settingsから呼ばれる)
void iotpad_oled_load_font(int idx) {
    if (idx >= 0 && idx < OLED_FONT_COUNT) oled_font_index = idx;
}

//===== SSD1306 I2Cドライバ =====
static int oled_i2c_cmd(uint8_t cmd) {
    uint8_t buf[2] = { OLED_CMD, cmd };
    return sensor_i2c_write(OLED_ADDR, buf, 2);
}
static int oled_i2c_data(const uint8_t* data, int len) {
    for (int i = 0; i < len; i += 32) {
        int chunk = (i + 32 < len) ? 32 : len - i;
        uint8_t buf[33];
        buf[0] = OLED_DATA;
        memcpy(buf + 1, data + i, chunk);
        if (sensor_i2c_write(OLED_ADDR, buf, chunk + 1) != 0) return -1;
    }
    return 0;
}
static bool oled_init_hw() {
    int r = 0;
    r |= oled_i2c_cmd(0xAE); // display off
    r |= oled_i2c_cmd(0xD5); r |= oled_i2c_cmd(0x80); // clk div
    r |= oled_i2c_cmd(0xA8); r |= oled_i2c_cmd(0x3F); // multiplex 63
    r |= oled_i2c_cmd(0xD3); r |= oled_i2c_cmd(0x00); // display offset
    r |= oled_i2c_cmd(0x40); // start line 0
    r |= oled_i2c_cmd(0x8D); r |= oled_i2c_cmd(0x14); // charge pump on
    r |= oled_i2c_cmd(0x20); r |= oled_i2c_cmd(0x00); // horizontal addressing
    r |= oled_i2c_cmd(0xA1); // segment remap
    r |= oled_i2c_cmd(0xC8); // COM scan direction
    r |= oled_i2c_cmd(0xDA); r |= oled_i2c_cmd(0x12); // COM pins
    r |= oled_i2c_cmd(0x81); r |= oled_i2c_cmd(0xCF); // contrast
    r |= oled_i2c_cmd(0xD9); r |= oled_i2c_cmd(0xF1); // precharge
    r |= oled_i2c_cmd(0xDB); r |= oled_i2c_cmd(0x40); // VCOMH
    r |= oled_i2c_cmd(0xA4); // display from RAM
    r |= oled_i2c_cmd(0xA6); // normal display
    r |= oled_i2c_cmd(0xAF); // display on
    return r == 0;
}

//===== フレームバッファ操作 =====
static void oled_buf_clear() { memset(oled_buf, 0, sizeof oled_buf); }
static void oled_buf_send() {
    oled_i2c_cmd(0x20); oled_i2c_cmd(0x02); // page addressing mode
    for (int page = 0; page < OLED_PAGES; page++) {
        oled_i2c_cmd(0xB0 | page);
        oled_i2c_cmd(0x00); // col low
        oled_i2c_cmd(0x10); // col high
        oled_i2c_data(oled_buf + page * OLED_WIDTH, OLED_WIDTH);
    }
}

//===== 文字描画 (事前デコード済みフォント、u8g2位置モデル) =====
//y_baseline=ベースライン位置 (u8g2のsetFontPosBaseline相当)
//戻り値=描画後のx位置 (OLEDPRINTのカーソル進行に使用)
static int oled_draw_str(int x, int y_baseline, const char* s) {
    const OLED_Char* tbl = oled_font_tables[oled_font_index];
    while (*s) {
        uint8_t c = (uint8_t)*s++;
        if (c < 0x20 || c > 0x7E) c = 0x3F; // '?'
        const OLED_Char* ch = &tbl[c - 0x20];
        int top = y_baseline - (ch->h + ch->gy); // u8g2: target_y -= (h + gy)
        int left = x + ch->gx;                    // u8g2: target_x += gx
        for (int col = 0; col < ch->w; col++) {
            uint16_t bits = ch->data[col];
            for (int row = 0; row < ch->h; row++) {
                if (bits & (1 << row)) {
                    oled_buf_pixel(left + col, top + row, true);
                }
            }
        }
        x += ch->adv;
    }
    return x;
}
//行位置: baseline = (行+1)×フォント高さ + y補正 (padのoled_line_yと同一式)
static int oled_line_y(int i) {
    return (i + 1) * oled_font_h[oled_font_index] + oled_font_yoff[oled_font_index];
}
static void oled_line(int i, const char* s) {
    oled_draw_str(0, oled_line_y(i), s);
}
static void oled_linef(int i, const char* fmt, ...) {
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    oled_line(i, buf);
}

//===== WiFi/キューアイコン (8x13px、右上コーナー、pad準拠の配置) =====
#define WIFI_ICON_W 8
#define WIFI_ICON_H 13
static const uint8_t wifi_icon_conn[]  = {0x3C,0x42,0x81,0x3C,0x42,0x00,0x18,0x24,0x00,0x18,0x18,0x18,0x18};
static const uint8_t wifi_icon_disc[] = {0x00,0x81,0x42,0x24,0x18,0x24,0x42,0x81,0x00,0x18,0x18,0x18,0x18};
static void oled_draw_icon(const uint8_t* icon, int x, int y) {
    for (int col = 0; col < WIFI_ICON_W; col++) {
        for (int row = 0; row < WIFI_ICON_H; row++) {
            if (icon[col] & (1 << row)) oled_buf_pixel(x + col, y + row, true);
        }
    }
}
static void oled_draw_status_icons() {
    //pad準拠: テキスト領域の右端から2px余白を空け、y=フォントy補正位置に描く
    int x = OLED_WIDTH - WIFI_ICON_W - 2;
    if (iotpad_wifi_is_connected()) oled_draw_icon(wifi_icon_conn, x, oled_font_yoff[oled_font_index]);
    else if (iotpad_wifi_started) oled_draw_icon(wifi_icon_disc, x, oled_font_yoff[oled_font_index]);
}

//===== 画面レイアウト (5画面、pad準拠) =====
//曜日短縮名 (日曜=0、padのWEEKDAY_JP3準拠)
static const char* WEEKDAY_JP3[] = { "sun","mon","tue","wed","thu","fri","sat" };

static void oled_update_display() {
    if (!oled_ok) return;
    oled_buf_clear();
    oled_draw_status_icons();
    switch (oled_current_screen) {
    case 1: // 環境表示 (pad準拠)
        if (rtc_ok) {
            uint16_t yy; uint8_t mo, d, h, mi, s;
            ds3231_read(&yy, &mo, &d, &h, &mi, &s);
            //RTCはUTC保存 → JST (+9h) 表示 (pad準拠)。曜日: 1970-01-01=木(4)、日曜=0
            uint32_t jst = iotpad_ymd_to_unix(yy, mo, d, h, mi, s) + 9 * 3600;
            iotpad_unix_to_ymd(jst, &yy, &mo, &d, &h, &mi, &s);
            oled_linef(0, "%04u-%02u-%02u", yy, mo, d);
            oled_linef(1, "JST  %02u:%02u (%s)", h, mi, WEEKDAY_JP3[(jst / 86400 + 4) % 7]);
        } else {
            oled_line(0, "RTC N/A");
        }
        //pad:1374-1378準拠のT/EXTフォールバック
        if (bme_ok && ds18b20_count > 0) oled_linef(2, "T:%.1f EXT:%.1f", cur_t1 + temp_offset, cur_t2 + ext1_offset);
        else if (bme_ok)                 oled_linef(2, "T:%.1f EXT:--", cur_t1 + temp_offset);
        else if (ds18b20_count > 0)      oled_linef(2, "T:-- EXT:%.1f", cur_t2 + ext1_offset);
        else                             oled_line(2, "T:-- EXT:--");
        if (bme_ok) oled_linef(3, "H:%.1f P:%.2f", cur_h + hum_offset, cur_p + pres_offset);
        else        oled_line(3, "H:-- P:--");
        if (ltr_ok)     oled_linef(4, "LUX:%.0f UV:%.0f", cur_l2, cur_uv);
        else if (lux_ok) oled_linef(4, "LUX:%.0f UV:--", cur_l);
        else            oled_line(4, "LUX:-- UV:--");
        break;
    case 2: // 実験表示 (pad:1390-1400準拠)
        if (ds18b20_count > 0) oled_linef(0, "EXT1:%.1f", cur_t2 + ext1_offset);
        else                   oled_line(0, "EXT1:--");
        if (ds18b20_count > 1) oled_linef(1, "EXT2:%.1f", cur_t2b + ext2_offset);
        else                   oled_line(1, "EXT2:--");
        if (lux_ok) oled_linef(2, "LUX:%.0f", cur_l);
        else        oled_line(2, "LUX:--");
        if (ltr_ok) oled_linef(3, "LX2:%.0f", cur_l2);
        else        oled_line(3, "LX2:--");
        if (ltr_ok && tof_ok)       oled_linef(4, "CPS:%lu D:%.1f", cur_uvcps, cur_d);
        else if (ltr_ok)            oled_linef(4, "CPS:%lu D:--", cur_uvcps);
        else if (tof_ok)            oled_linef(4, "CPS:-- D:%.1f", cur_d);
        else                        oled_line(4, "CPS:-- D:--");
        break;
    case 3: // センサー較正
        oled_linef(0, "TOFFSET:%.1f", temp_offset);
        oled_linef(1, "HOFFSET:%.1f", hum_offset);
        oled_linef(2, "POFFSET:%.1f", pres_offset);
        oled_linef(3, "EXT1OFS:%.1f", ext1_offset);
        oled_linef(4, "EXT2OFS:%.1f", ext2_offset);
        break;
    case 4: // WiFi状況 (pad:1413-1419準拠)
        if (iotpad_wifi_is_connected()) oled_line(0, "WIFI:CON");
        else if (iotpad_wifi_started)  oled_line(0, "WIFI:DISC");
        else                           oled_line(0, "WIFI:OFF");
        if (tb_auto_sec > 0) oled_linef(1, "AUTO:%lus", (unsigned long)tb_auto_sec);
        else                 oled_line(1, "AUTO:OFF");
        oled_line(2, "SSID:");
        oled_draw_str(30, oled_line_y(2), iotpad_ssid);
        {
            char ip[24];
            iotpad_wifi_get_ip(ip, sizeof ip);
            oled_line(3, ip);
        }
        oled_linef(4, "QUEU:%d/%d", tb_queue_count, TB_QUEUE_CAP);
        break;
    case 5: // バージョン情報
        oled_line(0, "IOTPADIOT");
        oled_line(1, "Ver 1.0.0.00");
        oled_linef(2, "BME:%d BH:%d", bme_ok ? 1 : 0, lux_ok ? 1 : 0);
        oled_linef(3, "LTR:%d VL:%d", ltr_ok ? 1 : 0, tof_ok ? 1 : 0);
        oled_linef(4, "RTC:%d DS:%d", rtc_ok ? 1 : 0, ds18b20_enabled ? 1 : 0);
        break;
    }
    oled_buf_send();
}

//===== OLED全体初期化 =====
static void iotpad_oled_init() {
    oled_ok = oled_init_hw();
    iotpad_dbg_str(oled_ok ? "PW: OLED OK\n" : "PW: OLED FAIL\n");
    if (oled_ok) {
        oled_buf_clear();
        oled_update_display();
    }
}

//===== OLED定期更新 (mainループから、padのmaybe_update_display相当の200msスロットル) =====
static void iotpad_oled_update() {
    if (!oled_ok || oled_direct) return;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - oled_last_update_ms < OLED_UPDATE_INTERVAL_MS) return;
    oled_last_update_ms = now;
    oled_update_display();
}

#endif
