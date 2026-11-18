#ifndef __SOUND_H__
#define __SOUND_H__
#include "all_includes.h"
#include "hardware/timer.h"
#include "hardware/uart.h"

#define SOUND_PIN 20

//一時デバッグ: HSTX起動経路の追跡をUART (GP0、115200) へ出力する (問題解決後に削除)
#define MILKJAM_HSTX_DBG 1
#if MILKJAM_HSTX_DBG
static void dbg_putc(char c) { uart_putc_raw(uart0, c); }
static void dbg_str(const char* s) { while (*s) dbg_putc(*s++); }
static void dbg_hex(uint32_t v) {
    for (int i = 28; i >= 0; i -= 4) {
        uint32_t d = (v >> i) & 0xF;
        dbg_putc(d < 10 ? '0' + d : 'A' + d - 10);
    }
}
#else
static void dbg_putc(char c) { (void)c; }
static void dbg_str(const char* s) { (void)s; }
static void dbg_hex(uint32_t v) { (void)v; }
#endif

// 多声PSGモード: PWMデューティ解像度 (キャリア = sysclk/1024 ≈ 154-264kHz、可聴域外)
#define SOUND_PWM_WRAP 1023
// オーディオ割り込み周期はpsg_emu.hのPSG_EMU_TIMER_US (18µs ≈ 55.6kHz) を使用

static uint slice_num;
static uint16 prev_tone = 0;
// sound_multi_mode / sound_multi_play は psg_mml3.hで定義 (psg_mml3.hが先にincludeされるため)
// core0 (60FPS/USB/映像) が書き、core1 (オーディオ) が読むため volatile

//多声PSGが使用可能か (音声ISRはcore1のアラームプールで駆動する):
//HSTXビルド: core1の映像DMA割込みは軽量 (レジスタ書換のみ) なため全モードで使用可能
//PicoDVIビルド: DVIモード中のcore1はTMDSエンコードで占有されるためNTSC/TFTモードのみ
static bool psg_audio_allowed() {
#if MILKJAM_USE_HSTX
    return true;
#else
    return video_output_mode != VIDEO_MODE_DVI;
#endif
}

//音声タイマー割込みの可否ゲート (core1のISRが参照、core0がビデオモード切替時に更新)
//IRQ有効化はcore1側で行われるためNVICをcore0から触れず、フラグで制御する
static volatile bool psg_audio_gate = false;

void sound_set_multi_mode(bool on, bool by_play);//多声PSGモード切替 (後ろで定義)

//ビデオモードに応じて音声の使用可否を更新する (video_output_init/video_switch_modeから呼ぶ)
//使用不可モードへの切替時は多声モードを停止してPWMトーンモードへ戻してからゲートを閉じる
void sound_update_mode_gate() {
    bool on = psg_audio_allowed();
    if (!on) {
        sound_set_multi_mode(false, false);
    }
    psg_audio_gate = on;
}

// ===== オーディオ専用core1 =====
// PSGエンジンはcore1で動かす。core0の映像DMA割込み (優先度0) は毎チャンク
// CPUを占有するため、同じcore0/同優先度だと音声ISRが遅延しリピートタイマーが
// まとめて追いついて音が荒れる。core1の専用ハードウェアアラームなら完全に分離される。
// 多声OFF時 (およびPicoDVIビルドのDVIモード中) はゲートで何もせず即帰る
static int8_t psg_hw_alarm = -1;//音声用の生ハードウェアアラーム番号 (アラームプール不使用)

//18µs周期の音声ISR (生ハードウェアアラーム、core1で実行)
//コールバック先頭で次回発火を現在時刻+18µsに再設定するため、
//フラッシュ書込やスリープ等で大きく遅延しても遅延分の追い上げが発生しない
//(遅延は単に音が途れるだけで自動回復する)
static void __not_in_flash_func(psg_audio_alarm_cb)(uint alarm_num) {
    hardware_alarm_set_target(alarm_num, delayed_by_us(get_absolute_time(), PSG_EMU_TIMER_US));
    if (!psg_audio_gate || !sound_multi_mode) {
        return;
    }
    int8_t mixed = psg_emu_tick(); // -45..+45 (3ch × 音量±15、双極性混合)
    // 双極性をデューティへ: ×16で1声±240 (47%) ≈ 単声PWMの振幅に相当
    // 2声までフルスケール、3声同時ピークはクランプ (ソフトリミット)
    int level = 512 + mixed * 16;
    if (level < 0)
        level = 0;
    if (level > SOUND_PWM_WRAP)
        level = SOUND_PWM_WRAP;
    pwm_set_gpio_level(SOUND_PIN, level);
}

// core1のエントリから呼ぶ: 音声用の生ハードウェアアラームと常駐18µsタイマーを設定する
// (アラームのIRQはこの関数を呼んだcore = core1側で処理される)。
// アラームプール (malloc使用) を使わない生アラーム方式のため、core1でヒープ操作しない
void psg_audio_core1_setup() {
    dbg_str("[C1] audio setup\r\n");
    psg_hw_alarm = hardware_alarm_claim_unused(true);
    if (psg_hw_alarm >= 0) {
        hardware_alarm_set_callback(psg_hw_alarm, psg_audio_alarm_cb);
        hardware_alarm_set_target(psg_hw_alarm, delayed_by_us(get_absolute_time(), PSG_EMU_TIMER_US));
        dbg_str("[C1] alarm=");
        dbg_hex(psg_hw_alarm);
        dbg_str("\r\n");
    }
}

//SLEEP復帰後に呼ぶ: 音声タイマーを現在時刻から張り直す
//(生アラーム方式ではコールバックが毎回現在時刻+18µsに再設定するため通常は不要だが、
// SLEEP復帰直後のタイミングずれを確実に解消するために呼んでおく)
void psg_audio_timer_reset() {
    if (psg_hw_alarm >= 0) {
        hardware_alarm_cancel(psg_hw_alarm);
        hardware_alarm_set_target(psg_hw_alarm, delayed_by_us(get_absolute_time(), PSG_EMU_TIMER_US));
    }
}

void sound_init() {
    _g.psgratio = 1;
    gpio_set_function(SOUND_PIN, GPIO_FUNC_PWM);
    // 音声出力パッドを最大駆動(12mA)+高速スルーレートに設定する (既定は4mA)
    gpio_set_drive_strength(SOUND_PIN, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_slew_rate(SOUND_PIN, GPIO_SLEW_RATE_FAST);
    slice_num = pwm_gpio_to_slice_num(SOUND_PIN);
    pwm_set_clkdiv(slice_num, PWM_CLKDIV_MHZ()); // 1MHzカウンタ (PWMトーンモード)
    pwm_set_wrap(slice_num, 0);
    pwm_set_gpio_level(SOUND_PIN, 0);
    pwm_set_enabled(slice_num, true);
    prev_tone = 0;
    psg_emu_init();
    sound_multi_mode = false;
    sound_multi_play = false;
    // NOTE: core1の起動とpsg_audio_core1_setup()の呼び出しはビデオバックエンド側
    // (HSTXビルド: hstx_core1_launch / PicoDVIビルド: core1_psg_dvi_entry) が行う
}

// 多声PSGモード切替 (psg.h / basic.hのSOUNDコマンドから呼ばれる)
// on=true:  PWMをデューティモード (wrap=1023) にしPSGエミュ出力を有効化
// on=false: PWMをトーンモード (1MHzカウンタ) に戻す
// 音声タイマーはcore1で常駐しているため、ここのON/OFFはゲート判定とフラグ・PWM設定のみ
// by_play=true: 多声PLAYによるモードON (psg_tickが全声終了を検出して自動OFF)
void sound_set_multi_mode(bool on, bool by_play) {
    if (on && !psg_audio_allowed()) {
        //このモードでは多声PSG不可 (PicoDVIビルドのDVIモード) → PWMトーンモードを維持する
        //(PLAY 2-3声のうちvoice0のみPWMトーンで鳴る。voice1/2は無音)
        on = false;
        sound_multi_play = false;
    }
    if (on) {
        if (!sound_multi_mode) {
            // 先にPWMをデューティモードへ (クロック無分周: キャリア = sysclk/1024)。
            // sound_multi_modeは最後に立てる (core1が未設定のPWMへ書かないように)
            pwm_set_enabled(slice_num, false);
            pwm_set_clkdiv(slice_num, 1);
            pwm_set_wrap(slice_num, SOUND_PWM_WRAP);
            pwm_set_counter(slice_num, 0);
            pwm_set_gpio_level(SOUND_PIN, 0);
            pwm_set_enabled(slice_num, true);
            psg_emu_init();
            sound_multi_mode = true;
        }
        if (by_play) {
            sound_multi_play = true;
        }
    } else {
        if (!sound_multi_mode && !sound_multi_play)
            return; // 既にPWMトーンモード
        sound_multi_mode = false; // 先にOFF (core1のPWMデューティ書き込みを止める)
        sound_multi_play = false;
        _g.psgtone = 0; // 停止トーンの残存でPWMが再発音するのを防止
        prev_tone = 0;  // set_tone()に再設定させる
        // PWMをトーンモードへ戻す (1MHzカウンタ)
        pwm_set_enabled(slice_num, false);
        pwm_set_clkdiv(slice_num, PWM_CLKDIV_MHZ());
        pwm_set_wrap(slice_num, 0);
        pwm_set_counter(slice_num, 0);
        pwm_set_gpio_level(SOUND_PIN, 0);
        pwm_set_enabled(slice_num, true);
    }
}

// 60FPSタイマーから呼ばれる: _g.psgtone → PWM周波数設定 (旧IchigoJam_P方式)
// PWMハードウェアが正確な方形波を生成するため、タイマージッタがなくクリアな音が出る
// 多声モード中は音声ISRがPWMデューティを制御するため何もしない
void set_tone() {
    if (sound_multi_mode) {
        return;
    }
    if (prev_tone != _g.psgtone) {
        if (_g.psgtone) {
            int freq = 60 * 261 / 2 / _g.psgtone;
            int wrap = 1000000 / freq - 1;
            if (wrap < 0) {
                wrap = 0;
            } else if (wrap > 0xffff) {
                wrap = 0xffff;
            }
            pwm_set_wrap(slice_num, wrap);
            pwm_set_gpio_level(SOUND_PIN, wrap / 2); // 50%デューティ = 方形波
        } else {
            pwm_set_counter(slice_num, 0);
            pwm_set_gpio_level(SOUND_PIN, 0); // 消音
        }
    }
    prev_tone = _g.psgtone;
}

//クロックを変更したときにPWMの分周比を再設定する
void sound_reconfig() {
    // スリープ復帰/クロック変更後に音声出力パッド設定を再適用
    gpio_set_drive_strength(SOUND_PIN, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_slew_rate(SOUND_PIN, GPIO_SLEW_RATE_FAST);
    if (sound_multi_mode) {
        pwm_set_clkdiv(slice_num, 1); // デューティモード: キャリア = sysclk/1024
    } else {
        pwm_set_clkdiv(slice_num, PWM_CLKDIV_MHZ());
        prev_tone = 0; //次のset_toneで再設定させる
    }
}

static void sound_switch(int on) {
    if (sound_multi_mode) {
        return; // 多声モード中は音声ISRが制御 (PWMトーンの開始/停止はしない)
    }
    if (on) {
        // set_tone()がPWM周波数を設定する (ここではPWMを有効化のみ)
    } else {
        pwm_set_counter(slice_num, 0);
        pwm_set_gpio_level(SOUND_PIN, 0); // 消音
    }
}

#endif
