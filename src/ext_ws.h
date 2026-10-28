//LED(WS2812B)の仕様は https://akizukidenshi.com/goodsaffix/WS2812B_20200225.pdf 参照

#ifndef __EXT_WS_H__
#define __EXT_WS_H__
#include "all_includes.h"
#include "hardware/pio.h"
#include "ws2812.pio.h"

//===== WS.LED送信 (PIO) =====
//通常はPIOステートマシンでWS2812波形を生成する (pico-examples公式プログラム、1bit=1.25µs)。
//CPUは送信FIFOを埋めるだけで、割込みを止めないためDVI/NTSC/TFTの表示やUSBキーボードが乱れない。
//
//PIOはpio1固定で使用する (pio_claim_free_sm_and_add_programの全PIO走査は不可):
//DVIモードはpio0のSM0-2を占有しており、pio0の空きSM (SM3) にプログラムを置くと
//DVIのPIOプログラム領域と衝突する。pio1は全モードで未使用のため、
//全ポートピン (GP2/8/9/10/11/22/21) が使用できる。

static PIO ws_pio;       //pio1固定
static uint ws_pio_sm;
static uint ws_pio_offset;
static int ws_pio_state; //0=未初期化、1=使用可、-1=確保失敗

static bool ws_pio_init(void) {
    if (ws_pio_state != 0) {
        return ws_pio_state > 0;
    }
    ws_pio = pio1;
    ws_pio_sm = pio_claim_unused_sm(ws_pio, false);
    if (ws_pio_sm < 0 || !pio_can_add_program(ws_pio, &ws2812_program)) {
        ws_pio_state = -1;
        return false;
    }
    ws_pio_offset = pio_add_program(ws_pio, &ws2812_program);
    ws_pio_state = 1;
    return true;
}

//ピンをPIOに接続してSMを起動し、Lowを保持したままリセット時間を作る
static void ws_pio_configure(uint8 pin) {
    pio_sm_set_enabled(ws_pio, ws_pio_sm, false);
    pio_sm_clear_fifos(ws_pio, ws_pio_sm);
    pio_sm_restart(ws_pio, ws_pio_sm);
    ws2812_program_init(ws_pio, ws_pio_sm, ws_pio_offset, pin, 800000.0f, false);
    gpio_set_drive_strength(pin, GPIO_DRIVE_STRENGTH_12MA);//5Vテープ向けに信号マージンを確保
    gpio_set_slew_rate(pin, GPIO_SLEW_RATE_FAST);
    sleep_us(300); //リセット信号
}

static void ws_pio_release(uint8 pin) {
    pio_sm_set_enabled(ws_pio, ws_pio_sm, false);
    pio_sm_clear_fifos(ws_pio, ws_pio_sm);
    gpio_init(pin);//通常GPIO (OUT/LEDコマンド) に戻す
    gpio_set_dir(pin, GPIO_OUT);
    gpio_put(pin, false);
}

//===== フォールバック (PIO確保失敗時のみ) のbit-bangタイミング =====
//1クロックの時間は動作モードにより異なる (DVI/TFT:270MHz=約3.7ns、NTSC:157.5MHz=約6.3ns)
//100nsあたりのNOP数: 270MHz→27個、157.5MHz→16個
#define NOP5_270 __asm volatile("nop");__asm volatile("nop");__asm volatile("nop");__asm volatile("nop");__asm volatile("nop");
#define NOP2_270 __asm volatile("nop");__asm volatile("nop");
#define NOP4_1575 __asm volatile("nop");__asm volatile("nop");__asm volatile("nop");__asm volatile("nop");
#define SLEEP_100NS_DVI NOP5_270 NOP5_270 NOP5_270 NOP5_270 NOP5_270 NOP2_270 //27 nop=約100ns (270MHz)
#define SLEEP_100NS_NTSC NOP4_1575 NOP4_1575 NOP4_1575 NOP4_1575 //16 nop=約102ns (157.5MHz)

//WS2812B (800kHz) の1ビット分のNOPグループ (1グループ=約100ns)
//規格: T0H 0.40µs / T1H 0.80µs / T0L 0.85µs / T1L 0.45µs (各±0.15µs)
//関数呼び出し・ループのオーバーヘッドはLow側に加算されるため、Lowは短めに設定する
//(IchigoJam-RP2040Zeroのフォールバック実装と同一の波形)
#define WS_HIGH_0_DVI SLEEP_100NS_DVI SLEEP_100NS_DVI SLEEP_100NS_DVI SLEEP_100NS_DVI
#define WS_HIGH_1_DVI SLEEP_100NS_DVI SLEEP_100NS_DVI SLEEP_100NS_DVI SLEEP_100NS_DVI SLEEP_100NS_DVI SLEEP_100NS_DVI SLEEP_100NS_DVI SLEEP_100NS_DVI
#define WS_LOW_0_DVI SLEEP_100NS_DVI SLEEP_100NS_DVI SLEEP_100NS_DVI SLEEP_100NS_DVI SLEEP_100NS_DVI SLEEP_100NS_DVI SLEEP_100NS_DVI
#define WS_LOW_1_DVI SLEEP_100NS_DVI SLEEP_100NS_DVI SLEEP_100NS_DVI
#define WS_HIGH_0_NTSC SLEEP_100NS_NTSC SLEEP_100NS_NTSC SLEEP_100NS_NTSC SLEEP_100NS_NTSC
#define WS_HIGH_1_NTSC SLEEP_100NS_NTSC SLEEP_100NS_NTSC SLEEP_100NS_NTSC SLEEP_100NS_NTSC SLEEP_100NS_NTSC SLEEP_100NS_NTSC SLEEP_100NS_NTSC SLEEP_100NS_NTSC
#define WS_LOW_0_NTSC SLEEP_100NS_NTSC SLEEP_100NS_NTSC SLEEP_100NS_NTSC SLEEP_100NS_NTSC SLEEP_100NS_NTSC SLEEP_100NS_NTSC SLEEP_100NS_NTSC
#define WS_LOW_1_NTSC SLEEP_100NS_NTSC SLEEP_100NS_NTSC SLEEP_100NS_NTSC

static uint8 ws_pin;

//1ビット送信 (フォールバック用)
//bit=1: High 約810ns + Low 約450ns、bit=0: High 約400ns + Low 約800ns
static void __noinline __not_in_flash_func(wsled_send_one_signal_270) (bool bit) { //DVI/TFTモード (270MHz)
    gpio_put(ws_pin, true);
    if (bit) { WS_HIGH_1_DVI } else { WS_HIGH_0_DVI }
    gpio_put(ws_pin, false);
    if (bit) { WS_LOW_1_DVI } else { WS_LOW_0_DVI }
}

static void __noinline __not_in_flash_func(wsled_send_one_signal_1575) (bool bit) { //NTSCモード (157.5MHz)
    gpio_put(ws_pin, true);
    if (bit) { WS_HIGH_1_NTSC } else { WS_HIGH_0_NTSC }
    gpio_put(ws_pin, false);
    if (bit) { WS_LOW_1_NTSC } else { WS_LOW_0_NTSC }
}

//PIO確保失敗時のみ使う従来方式 (割込み禁止区間がある)
static void __noinline __not_in_flash_func(ws_bitbang_out) (int nled, int repeat) {
    bool is_ntsc = (video_output_mode == VIDEO_MODE_NTSC);
    gpio_init(ws_pin);
    gpio_set_dir(ws_pin, GPIO_OUT);
    gpio_set_drive_strength(ws_pin, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_slew_rate(ws_pin, GPIO_SLEW_RATE_FAST);
    gpio_put(ws_pin, false);
    sleep_us(300); //リセット信号
    for (int i = 0; i < repeat; i++) {
        int save = save_and_disable_interrupts();
        for (int j = 0; j < nled * 3; j++) {//1つのLEDにつき、3つの変数が必要
            uint16 v = *basic_getArrayPtr(j); //拡張配列変数対応 (4K版)
            for (int k = 7; k >= 0; k--) {
                bool bit = v & (0x01 << k);
                if (is_ntsc) {
                    wsled_send_one_signal_1575(bit);
                } else {
                    wsled_send_one_signal_270(bit);
                }
            }
        }
        restore_interrupts(save);
        sleep_us(300); //フレーム間/終端のリセット (repeat時もラッチされるようにする)
    }
}

//WS.LED port,n[,repeat] (4K版: ポート選択式+拡張配列変数対応)
//port: 1-7 (OUT1-6、7=LED/GP2)、n: LED数、repeat: 送信回数
//色データは配列変数[0]〜[n*3-1]から読み込む (1LEDにつき3変数、[102]-[357]の拡張領域に対応し最大119LED)
static void __noinline ws_out(int port, int nled, int repeat) {
    if (port < 1 || port > 7 || nled < 1 || repeat < 1) {
        return;
    }
    if (nled * 3 > IJB_SIZEOF_ARRAY_MAX) { //配列変数の範囲内に収める (最大119LED)
        nled = IJB_SIZEOF_ARRAY_MAX / 3;
    }
    ws_pin = out_pins[port - 1];
    if (sd_pin_reserved(ws_pin)) {
        return;//SD動作中の共有ピン (OUT1/3/4) はWS2812出力禁止
    }
    if (!ws_pio_init()) { //PIOを確保できない場合のみ従来方式
        ws_bitbang_out(nled, repeat);
        return;
    }
    ws_pio_configure(ws_pin);//この中で300usのリセットを出す
    for (int i = 0; i < repeat; i++) {
        for (int j = 0; j < nled * 3; j += 3) {//1つのLEDにつき、3つの変数が必要
            uint8 g = *basic_getArrayPtr(j);
            uint8 r = *basic_getArrayPtr(j + 1);
            uint8 b = *basic_getArrayPtr(j + 2);
            uint32_t v = ((uint32_t)g << 24) | ((uint32_t)r << 16) | ((uint32_t)b << 8);
            pio_sm_put_blocking(ws_pio, ws_pio_sm, v); //GRB順 (配列[0]=G/[1]=R/[2]=Bの従来互換)、下位8bitは無視される
        }
        while (!pio_sm_is_tx_fifo_empty(ws_pio, ws_pio_sm)) {
            tight_loop_contents();
        }
        sleep_us(330); //最後の1ワード (約30us) のドレイン + リセット (300us)
    }
    ws_pio_release(ws_pin);
}

#endif
