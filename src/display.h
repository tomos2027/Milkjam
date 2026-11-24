#ifndef __DISPLAY_H__
#define __DISPLAY_H__
#include "all_includes.h"

#define CHAR_MAX_COLS 32
#define CHAR_MAX_ROWS 24
#if !MILKJAM_USE_HSTX
struct dvi_inst dvi0;//PicoDVIフォールバックビルドのみ (HSTXビルドではsrc/hstx_dvi.hを使用)
#endif

//ビデオ出力モード 0:DVI(HDMI)、1:NTSC(コンポジット)、2:TFT(ILI9341)、3:WEB(NTSC+ブラウザ)、SWITCHコマンドで切り替える
//WEBモードはNTSC出力をローカル表示として継続しつつ、WiFi AP+Webサーバーを起動し
//ブラウザにもNTSCと同じ画面を表示してキー入力も受け付ける (src/ext_web.h)
#define VIDEO_MODE_DVI 0
#define VIDEO_MODE_NTSC 1
#define VIDEO_MODE_TFT 2
#define VIDEO_MODE_WEB 3
uint8_t video_output_mode = VIDEO_MODE_DVI;

//ローカル映像バックエンドがNTSC (157.5MHzクロック) のモードか (NTSC/WEB共通)
//クロック設定・UART/PWM/SPIの再設定・WS.LEDのbit-bangタイミング等で参照する
static inline int video_clock_is_ntsc() {
    return video_output_mode == VIDEO_MODE_NTSC || video_output_mode == VIDEO_MODE_WEB;
}

//TFT液晶 (GP12-16) はDVI (HDMI、GP12-19) とピンを共用するため排他で使用する:
//LCDONコマンドで1にするとHDMI (SWITCH 0) への切替を禁止し、
//LCDOFFコマンドで0にするとLCD (SWITCH 2) への切替を禁止する (安全のため、flashに保存)
//NTSC (GP3) は排他ピンではないため常に切替可能
uint8_t lcd_enabled = 0;

void video_switch_mode(uint8_t mode);//init.hで定義
void ntsc_init();//init.hで定義
//HDMI (DVI) バックエンドの初期化・開始・停止・状態取得 (init.hで定義、HSTX/PicoDVIを切替える)
void dvi_backend_init();
void dvi_backend_start();
void dvi_backend_stop();
int dvi_backend_active();
//IJB_lcd (SWITCHコマンド) の実体はinit.hで定義

void video_on() {
    SCREEN_W = CHAR_MAX_COLS >> _g.screen_big;
    SCREEN_H = CHAR_MAX_ROWS >> _g.screen_big;
    if (video_clock_is_ntsc()) {
        if (ntsc_active()) {
            return;//動作中なら再初期化不要 (VIDEOコマンドの反転/拡大は内容の変更だけのため)
        }
        //NTSCはflash書込等で割り込みが長時間止まると走査線カウンタが表示位置からずれるため、
        //開始するときは毎回チェーンを再初期化して位相を合わせ直す
        ntsc_release();
        ntsc_init();
    } else if (video_output_mode == VIDEO_MODE_TFT) {
        tft_resume();//video_offからの再開 (初回の開始はvideo_switch_mode/video_output_initが行う)
    } else {
        dvi_backend_start();
    }
}

//クロックの変更機能はつけない
void video_off(int clkdiv) {
    if (video_clock_is_ntsc()) {
        stop_composite();
    } else if (video_output_mode == VIDEO_MODE_TFT) {
        tft_stop();
    } else {
        dvi_backend_stop();
    }
}

static inline int video_active() {
    if (video_clock_is_ntsc()) {
        return ntsc_active();
    } else if (video_output_mode == VIDEO_MODE_TFT) {
        return tft_active();
    }
    return dvi_backend_active();
}

void video_waitSync(uint num) {
    //NTSC/TFTモードでは1フレームごとのカウンタ増加を待つ (DVIモードでは何もしない)
    if (video_clock_is_ntsc()) {
        for (uint i = 0; i < num; i++) {
            uint16_t count = drawcount;
            uint64_t start = time_us_64();
            while (drawcount == count) {
                if (time_us_64() - start > 100000) {
                    return;//100msで諦める (映像停止中などに起動が固まるのを防ぐ)
                }
                __wfi();
            }
        }
    } else if (video_output_mode == VIDEO_MODE_TFT) {
        for (uint i = 0; i < num; i++) {
            uint16_t count = frames;
            uint64_t start = time_us_64();
            while (frames == count) {
                if (time_us_64() - start > 100000) {
                    return;
                }
                __wfi();
            }
        }
    }
}

#endif
