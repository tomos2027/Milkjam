//TFT液晶 (ILI9341) ドライバ — Milkjam (Raspberry Pi Pico 2 W)
//接続ピン (GP12-16) はDVI (HDMI sock、GP12-19) と共用のため排他で使用する:
//  LCDON中はSWITCH 0 (HDMI) への切替を禁止、LCDOFF中はSWITCH 2 (TFT) への切替を禁止 (SWITCHは排他を保証)
//  SCK=GP14 (18番ピン)、MOSI=GP15 (19番ピン)、CS=GP13 (16番ピン)、DC=GP16 (20番ピン)、RESET=GP12 (15番ピン)
//  VCC=3.3V、LEDA=3.3V (抵抗経由で明るさ調整可)、GND=GND、SDO (MISO) は未接続 (書き込み専用)
//SPI1はSDカード (SDON〜SDOFF、共有ピンGP6/8/10/11) と共用のため、SD使用中は
//そのフレームの画面更新をスキップし、SDOFF後に自動で再開する
//描画はvramとシャドウ (前回表示内容) の差分があるセルのみをSPIで書き込む (白/黒の2色、DVIと同じ表示)
#ifndef __TFT_H__
#define __TFT_H__
#include "all_includes.h"
#include "hardware/spi.h"

//接続ピン (DVI sockピンGP12-19の内の5本 — HDMIと排他で使用)
#define TFT_SPI spi1
#define TFT_PIN_SCK 14
#define TFT_PIN_MOSI 15
#define TFT_PIN_CS 13
#define TFT_PIN_DC 16
#define TFT_PIN_RESET 12
//SPIクロック: 31.5MHzを要求 (270MHzからは27MHzに分周される。ILI9341の規格値は10MHzだが大半のモジュールは動作する。
//表示が化ける・白画面のままの場合は15.75MHzに落とす)
#define TFT_SPI_BAUD 31500000
//#define TFT_SPI_BAUD 15750000

#define TFT_WIDTH 320
#define TFT_HEIGHT 240
#define TFT_COLS 32
#define TFT_ROWS 24
//文字領域 (256x192) の画面内オフセット (DVIのMARGINと同じ位置)
#define TFT_OFFS_X ((TFT_WIDTH - TFT_COLS * 8) / 2)
#define TFT_OFFS_Y ((TFT_HEIGHT - TFT_ROWS * 8) / 2)

static bool tft_initialized = false;//GPIO・ILI9341の初期化済み
static volatile bool tft_stopped = true;//video_off中
static bool tft_force_redraw = false;//次回更新時に全セル再描画する
static uint8_t tft_shadow_vram[TFT_COLS * TFT_ROWS];//最後に表示したVRAMのコピー
static int16_t tft_shadow_cx = -1, tft_shadow_cy = -1;//最後に表示したカーソル位置
static uint8_t tft_shadow_invert = 0;
static uint8_t tft_shadow_mag = 0;
static bool tft_flip = false;//SWITCH 2,1: 画面を180°回転 (MADCTL 0xE8=MV|MX|MY)
static bool tft_madctl_pending = false;//tft_set_flipで設定、tft_update内でMADCTL書換+全画面消去

static int tft_active() {
    return tft_initialized && !tft_stopped;
}

void tft_stop() {
    tft_stopped = true;
}

static void tft_cs(bool select) {
    gpio_put(TFT_PIN_CS, !select);
}

static void tft_write_cmd(uint8_t cmd) {
    gpio_put(TFT_PIN_DC, 0);
    spi_write_blocking(TFT_SPI, &cmd, 1);
}

static void tft_write_data(const uint8_t* data, int len) {
    gpio_put(TFT_PIN_DC, 1);
    spi_write_blocking(TFT_SPI, data, len);
}

//コマンド+データを1トランザクション (CS確立) で書き込む
static void tft_write_reg(uint8_t cmd, const uint8_t* data, int len) {
    tft_cs(true);
    tft_write_cmd(cmd);
    if (len > 0) {
        tft_write_data(data, len);
    }
    tft_cs(false);
}

//書き込みウィンドウ設定 (CSはデータ書き込みが終わるまで維持する、呼び出し側でtft_cs(false)する)
static void tft_set_window(int x0, int y0, int x1, int y1) {
    uint8_t d[4];
    d[0] = x0 >> 8; d[1] = x0; d[2] = x1 >> 8; d[3] = x1;
    tft_write_cmd(0x2A);//CASET
    tft_write_data(d, 4);
    d[0] = y0 >> 8; d[1] = y0; d[2] = y1 >> 8; d[3] = y1;
    tft_write_cmd(0x2B);//PASET
    tft_write_data(d, 4);
    tft_write_cmd(0x2C);//RAMWR
}

//全画面黒クリア (320x240x16bpp、27MHzで約45ms)
static void tft_fill_black() {
    static uint8_t zeros[512];//0初期化 (RGB565の黒=0x0000)
    tft_cs(true);
    tft_set_window(0, 0, TFT_WIDTH - 1, TFT_HEIGHT - 1);
    for (int i = 0; i < TFT_WIDTH * TFT_HEIGHT * 2 / (int)sizeof(zeros); i++) {
        tft_write_data(zeros, sizeof(zeros));
    }
    tft_cs(false);
}

//SPI1をTFT用に設定する (SD側のspi_deinitやクロック切替に追従できるよう毎フレーム呼ぶ)
static void tft_spi_init() {
    spi_init(TFT_SPI, TFT_SPI_BAUD);
    spi_set_format(TFT_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(TFT_PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(TFT_PIN_MOSI, GPIO_FUNC_SPI);
}

//ILI9341の初期化シーケンス (Adafruit ILI9341準拠の標準的な設定)
static void tft_hw_init() {
    gpio_put(TFT_PIN_CS, 1);
    //ハードウェアリセット
    gpio_put(TFT_PIN_RESET, 1);
    sleep_ms(50);
    gpio_put(TFT_PIN_RESET, 0);
    sleep_ms(20);
    gpio_put(TFT_PIN_RESET, 1);
    sleep_ms(150);

    tft_write_reg(0x01, NULL, 0);//SWRESET
    sleep_ms(150);
    static const uint8_t pwctrla[] = {0x00, 0xC1, 0x30};
    tft_write_reg(0xCF, pwctrla, 3);
    static const uint8_t prctrl[] = {0x64, 0x03, 0x12, 0x81};
    tft_write_reg(0xED, prctrl, 4);
    static const uint8_t dtctrl[] = {0x85, 0x00, 0x78};
    tft_write_reg(0xE8, dtctrl, 3);
    static const uint8_t pwctrlb[] = {0x39, 0x2C, 0x00, 0x34, 0x02};
    tft_write_reg(0xCB, pwctrlb, 5);
    static const uint8_t pwctrl1[] = {0x23};
    tft_write_reg(0xC0, pwctrl1, 1);
    static const uint8_t pwctrl2[] = {0x10};
    tft_write_reg(0xC1, pwctrl2, 1);
    static const uint8_t vmctrl1[] = {0x3E, 0x28};
    tft_write_reg(0xC5, vmctrl1, 2);
    static const uint8_t vmctrl2[] = {0x86};
    tft_write_reg(0xC7, vmctrl2, 1);
    static const uint8_t frmctr1[] = {0x00, 0x18};
    tft_write_reg(0xB1, frmctr1, 2);
    static const uint8_t enable3g[] = {0x00};
    tft_write_reg(0xF2, enable3g, 1);//3Gガンマ無効
    static const uint8_t gammaset[] = {0x01};
    tft_write_reg(0x26, gammaset, 1);
    uint8_t madctl = tft_flip ? 0xE8 : 0x28;//横長 (MV|BGR、180°回転時はMV|MX|MY。白黒2色なので色順序は影響しない)
    tft_write_reg(0x36, &madctl, 1);
    static const uint8_t colmod[] = {0x55};//16bpp
    tft_write_reg(0x3A, colmod, 1);
    static const uint8_t gmctrp[] = {0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1, 0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00};
    tft_write_reg(0xE0, gmctrp, 15);
    static const uint8_t gmctrn[] = {0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1, 0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F};
    tft_write_reg(0xE1, gmctrn, 15);
    tft_write_reg(0x11, NULL, 0);//SLPOUT
    sleep_ms(120);
    tft_write_reg(0x29, NULL, 0);//DISPON
    sleep_ms(50);
    tft_fill_black();
}

//1セル (文字1文字分、8xmag x 8xmagピクセル) を描画する
//PCG (POKE #700-#7FF)・VIDEO反転・カーソルXORの描画ロジックはDVIのvram_to_framebuf_scanlineと同一
static void tft_push_cell(int x, int y, int ccx, int ccy) {
    uint8_t c = vram[y * SCREEN_W + x];
    int mag = 1 << _g.screen_big;
    int px = TFT_OFFS_X + x * 8 * mag;
    int py = TFT_OFFS_Y + y * 8 * mag;
    bool cursor_here = x == ccx && y == ccy;
    uint8_t cursor_mask = key_flg.insert ? 0xff : 0xf0;//挿入モードは文字全体、上書きモードは左半分を反転
    static uint8_t line[8 * 8 * 2];//1走査線分のRGB565バイト列 (mag最大8で128バイト)
    tft_cs(true);
    tft_set_window(px, py, px + 8 * mag - 1, py + 8 * mag - 1);
    for (int r = 0; r < 8; r++) {//フォントの8行
        uint8_t bits = CHAR_PATTERN[c * 8 + r];
        if (c >= 0xE0) {
            bits = screen_pcg[(c - 0xE0) * 8 + r];//POKEで書き換えられた文字はscreen_pcgを参照する
        }
        bits ^= 0xff * _g.screen_invert;//VIDEOコマンドでの画面の反転
        if (cursor_here) {
            bits ^= cursor_mask;//カーソル位置の文字をマスクXOR
        }
        int len = 0;
        for (int b = 0; b < 8; b++) {//1フォント行をmag倍に引き伸ばしたRGB565列を構築
            uint8_t byte = (bits & (0x80 >> b)) ? 0xFF : 0x00;//白=0xFFFF、黒=0x0000
            for (int k = 0; k < mag; k++) {
                line[len++] = byte;
                line[len++] = byte;
            }
        }
        for (int v = 0; v < mag; v++) {//1フォント行をmag走査線に引き伸ばす
            tft_write_data(line, len);
        }
    }
    tft_cs(false);
}

//60Hzタイマーから呼ばれる: vramとシャドウの差分があるセルのみILI9341へ書き込む
static void tft_update(bool visible_cursor) {
    if (!tft_active() || !vram) {
        return;
    }
    bool blink = ((frames >> 4) & 1) != 0;
    int ccx = -1, ccy = -1;
    if (blink && visible_cursor && _g.cursory >= 0 && _g.cursorx >= 0 && _g.cursorx < SCREEN_W && _g.cursory < SCREEN_H) {
        ccx = _g.cursorx;
        ccy = _g.cursory;
    }
    int ncells = SCREEN_W * SCREEN_H;
    //まず差分があるか確認する (変化がなければSPIバスに触れない)
    bool any = tft_force_redraw || tft_madctl_pending || tft_shadow_invert != _g.screen_invert || tft_shadow_mag != _g.screen_big;
    if (!any) {
        for (int i = 0; i < ncells; i++) {
            if (tft_shadow_vram[i] != vram[i]) {
                any = true;
                break;
            }
        }
    }
    if (!any && ccx == tft_shadow_cx && ccy == tft_shadow_cy) {
        return;
    }
    if (sd_pins_reserved) {
        return;//SDカードがSPI1を使用中 → このフレームは更新しない (SDOFF後の次フレームで反映される)
    }
    tft_spi_init();
    if (tft_madctl_pending) {
        //180°回転の反映: MADCTL書換後に全画面を消去する (座標系が反転するため、旧座標系に書かれた
        //GRAMは差分更新では消せない。消去後はforce_redrawで現在のvramを全再描画)
        uint8_t madctl = tft_flip ? 0xE8 : 0x28;
        tft_write_reg(0x36, &madctl, 1);
        tft_fill_black();
        tft_madctl_pending = false;
        tft_force_redraw = true;
    }
    for (int y = 0; y < SCREEN_H; y++) {
        for (int x = 0; x < SCREEN_W; x++) {
            int i = y * SCREEN_W + x;
            if (tft_force_redraw || tft_shadow_vram[i] != vram[i] || (x == tft_shadow_cx && y == tft_shadow_cy) || (x == ccx && y == ccy)) {
                tft_push_cell(x, y, ccx, ccy);
            }
        }
    }
    memcpy(tft_shadow_vram, vram, ncells);
    tft_shadow_cx = ccx;
    tft_shadow_cy = ccy;
    tft_shadow_invert = _g.screen_invert;
    tft_shadow_mag = _g.screen_big;
    tft_force_redraw = false;
}

//TFT表示を開始する (video_switch_mode・video_output_initから呼ばれる)
//SDON中 (SDがSPI1占有中) のときはSDを一旦オフにしてTFTを優先する (再度SDONでSDは使用可)
//SWITCH 2,1用: 画面の180°回転を設定する
//MADCTL (MX/MY) はILI9341の描画座標系ごと反転させるため、反転前に書かれたGRAMは
//新しい座標系の差分更新では上書きされず残留する。そのため反映はtft_update (60Hzタイマー割り込み内) で
//「MADCTL書換 → 全画面消去 → 全セル再描画」の順で行う。
//ここではフラグの設定のみ行い、メインラインからSPIを直接触らない (tft_updateとのSPIトランザクション競合の回避)
void tft_set_flip(bool flip) {
    tft_flip = flip;
    tft_madctl_pending = true;
}

void tft_start() {
    if (sd_pins_reserved) {
        sd_card_off();
    }
    if (!tft_initialized) {
        gpio_init(TFT_PIN_CS);
        gpio_set_dir(TFT_PIN_CS, GPIO_OUT);
        gpio_put(TFT_PIN_CS, 1);
        gpio_init(TFT_PIN_DC);
        gpio_set_dir(TFT_PIN_DC, GPIO_OUT);
        gpio_init(TFT_PIN_RESET);
        gpio_set_dir(TFT_PIN_RESET, GPIO_OUT);
        gpio_put(TFT_PIN_RESET, 1);
        tft_spi_init();
        tft_hw_init();
        tft_initialized = true;
        tft_madctl_pending = false;//hw_initで現在のtft_flipどおりのMADCTLと全画面消去が済んでいる
    }
    tft_stopped = false;
    tft_force_redraw = true;//起動・再開時はシャドウを信頼せず全セル再描画する
}

//video_offからの再開 (video_onから呼ばれる)
//ハードウェアはそのまま (SDセッションも維持)、SD使用中は画面更新がスキップされ続けるだけ
void tft_resume() {
    tft_stopped = false;
    tft_force_redraw = true;
}

#endif
