#ifndef __DISPLAY_H__
#define __DISPLAY_H__
#include "all_includes.h"

#define CHAR_MAX_COLS 32
#define CHAR_MAX_ROWS 24
struct dvi_inst dvi0;

//ビデオ出力モード 0:DVI(HDMI)、1:NTSC(コンポジット)、2:TFT(ILI9341)、SWITCHコマンドで切り替える
#define VIDEO_MODE_DVI 0
#define VIDEO_MODE_NTSC 1
#define VIDEO_MODE_TFT 2
uint8_t video_output_mode = VIDEO_MODE_DVI;

//TFT液晶 (GP12-16) はDVI (HDMI、GP12-19) とピンを共用するため排他で使用する:
//LCDONコマンドで1にするとHDMI (SWITCH 0) への切替を禁止し、
//LCDOFFコマンドで0にするとLCD (SWITCH 2) への切替を禁止する (安全のため、flashに保存)
//NTSC (GP3) は排他ピンではないため常に切替可能
uint8_t lcd_enabled = 0;

void video_switch_mode(uint8_t mode);//init.hで定義
void ntsc_init();//init.hで定義
//IJB_lcd (SWITCHコマンド) の実体はinit.hで定義

void video_on() {
    SCREEN_W = CHAR_MAX_COLS >> _g.screen_big;
    SCREEN_H = CHAR_MAX_ROWS >> _g.screen_big;
    if (video_output_mode == VIDEO_MODE_NTSC) {
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
        dvi_start(&dvi0);
    }
}

//クロックの変更機能はつけない
void video_off(int clkdiv) {
    if (video_output_mode == VIDEO_MODE_NTSC) {
        stop_composite();
    } else if (video_output_mode == VIDEO_MODE_TFT) {
        tft_stop();
    } else {
        dvi_stop(&dvi0);
    }
}

static inline int video_active() {
    if (video_output_mode == VIDEO_MODE_NTSC) {
        return ntsc_active();
    } else if (video_output_mode == VIDEO_MODE_TFT) {
        return tft_active();
    }
    return dvi0.dvi_started;
}

void video_waitSync(uint num) {
    //NTSC/TFTモードでは1フレームごとのカウンタ増加を待つ (DVIモードでは何もしない)
    if (video_output_mode == VIDEO_MODE_NTSC) {
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
