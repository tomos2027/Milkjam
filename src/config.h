// created by Taisuke Fukuno, @taisukef, http://fukuno.jig.jp/
// license CC BY-NC-SA 4.0 http://creativecommons.org/licenses/by-nc-sa/4.0/deed.ja
#ifndef __CONFIG_H__
#define __CONFIG_H__
#include "all_includes.h"

#define ICHIGOJAM

//#define IJB_BETA // beta or not

// #define PSG_TRUE_TONE_UNIT (109 * 1)
// #define PSG_TICK_PER_SEC 60

#define PLATFORM_RP2350 9               //### ここに書くべきか、それが問題だ
#define VER_PLATFORM PLATFORM_RP2350

#define IJB_VER_STR 1.6.1

#ifdef IJB_BETA
#define IJB_TITLE "IchigoJam BASIC 1.5b rv jig.jp\n"
#else
//#define IJB_TITLE "Milkjam " STRING2(IJB_VER_STR) " (IchigoJam BASIC by jig.jp)\n"
#define IJB_TITLE "Milkjam Basic " STRING2(IJB_VER_STR) " by PICO 2W \nforked by NEXTDAY\n"
#endif

#define ENABLE_I2C_BPS

//映像出力: DVI (HDMI) / NTSCコンポジット / TFT液晶 (ILI9341) の3モード (SWITCHコマンドで切替)
//NTSC出力ピン: IchigoJam_Pと同一のGP3 (I2CをGP4/5に揃えたためGP3は未使用に戻った)
//(75Ω+560pFの外部回路を介してテレビ等の映像入力へ接続)
#define NTSC_PIN 3
#define NTSC_CLOCK_KHZ 157500//NTSC動作時のシステムクロック (157.5MHz/11=14.318181MHzでサンプリング、色副搬送波の4倍)

//4K版: 1プログラム4096バイト化に伴い保存本数を100→25に (1本=フラッシュ1セクタ4096バイト、GivetakeJam_P準拠)
//Pico 2 Wはフラッシュ4MB。保存域はフラッシュ末尾 (storage.h)。SDカード (スロット200-299) にも保存可
#define N_FLASH_STORAGE 25

#define VERSION15

#define EXT_IOT
// #define KEY_INIT_DELAY
#endif
