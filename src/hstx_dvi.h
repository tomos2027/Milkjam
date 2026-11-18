#ifndef __HSTX_DVI_H__
#define __HSTX_DVI_H__

//RP2350 HSTXによるDVI (HDMI) 出力ドライバ (4bppグレースケール720x480p60)
//PicoDVI (PIO) の代替実装。ピン配置はPico DVI sock準拠 (GP12-19)
//参考: pico-examples/hstx/dvi_out_hstx_encoder
//core1がDMA完了割込み (DMA_IRQ_0) と開始/停止処理を担当し、core0とはSIO FIFOで
//コマンドをやり取りする (flash書込中のフリーズ対策としてPicoDVI版と同じ分離方式)

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/resets.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"

//DVIのTMDSコントロールシンボル (RAWコマンドで同期信号区間に出力する)
#define TMDS_CTRL_00 0x354u
#define TMDS_CTRL_01 0x0abu
#define TMDS_CTRL_10 0x154u
#define TMDS_CTRL_11 0x2abu

#define SYNC_V0_H0 (TMDS_CTRL_00 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V0_H1 (TMDS_CTRL_01 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H0 (TMDS_CTRL_10 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H1 (TMDS_CTRL_11 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))

//CEA-861 720x480p60のタイミング (PicoDVIのdvi_timing_720x480p_60hzと同一)
#define MODE_H_FRONT_PORCH   16
#define MODE_H_SYNC_WIDTH    62
#define MODE_H_BACK_PORCH    60
#define MODE_H_ACTIVE_PIXELS 720

#define MODE_V_FRONT_PORCH   9
#define MODE_V_SYNC_WIDTH    6
#define MODE_V_BACK_PORCH    30
#define MODE_V_ACTIVE_LINES  480

#define MODE_V_TOTAL_LINES ( \
    MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + \
    MODE_V_BACK_PORCH  + MODE_V_ACTIVE_LINES \
)

//HSTXのピクセルクロック135MHz (シリアル出力は2bit/クロックで270Mbps=TMDS bit clock、
//CSRのCLKDIV=5でCLKピンには27MHzのピクセルクロックが出力される)
#define HSTX_PIXEL_CLOCK_HZ 135000000

//HSTXコマンド語 (SDKには定義がないためpico-examplesと同一の値を定義)
#define HSTX_CMD_RAW         (0x0u << 12)
#define HSTX_CMD_RAW_REPEAT  (0x1u << 12)
#define HSTX_CMD_TMDS        (0x2u << 12)
#define HSTX_CMD_TMDS_REPEAT (0x3u << 12)
#define HSTX_CMD_NOP         (0xfu << 12)

//HSTXコマンドリスト (垂直ブランキング区間/映像区間の1走査線分)
//リストの末尾のNOPはHSTX FIFOの深さ分の語数を確保するためのパディング
static uint32_t hstx_vblank_line_vsync_off[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
    SYNC_V1_H0,
    HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS),
    SYNC_V1_H1,
    HSTX_CMD_NOP
};

static uint32_t hstx_vblank_line_vsync_on[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
    SYNC_V0_H1,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
    SYNC_V0_H0,
    HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS),
    SYNC_V0_H1,
    HSTX_CMD_NOP
};

//映像区間はコマンドリスト (9語) とピクセルデータ (90語) の2回のDMA転送で1走査線分を供給する
//TMDSコマンド (720ピクセル) が続くFIFOデータを4bppピクセルデータとして消費する
static uint32_t hstx_vactive_line[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
    SYNC_V1_H0,
    HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | MODE_H_BACK_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_TMDS       | MODE_H_ACTIVE_PIXELS
};

//ピンポン用DMAチャンネル (起動時にhstx_claim_dma_channelsで確保する)
#define HSTX_DMACH_PING 0
#define HSTX_DMACH_PONG 1

//core1へのコマンド (SIO FIFOで送信、core1は処理後にコマンド値をackとして返す)
#define HSTX_CORE_CMD_START 1
#define HSTX_CORE_CMD_STOP 2

static bool hstx_dma_pong = false;
static uint hstx_v_scanline = SCANLINE_INIT;
static bool hstx_vactive_cmdlist_posted = false;
static volatile bool hstx_running = false;
static bool hstx_dvi_initialized = false;

//DMAチャンネル (0,1) を起動時に確保しておく
//WiFi (CYW43) とNTSCがdma_claim_unused_channelで動的確保するため、
//NTSCモードで起動して後からSWITCH 0するケースでもHSTX用に確保しておく必要がある
void hstx_claim_dma_channels() {
    dma_channel_claim(HSTX_DMACH_PING);
    dma_channel_claim(HSTX_DMACH_PONG);
}

//1走査線ごとにDMA完了割込みで次の走査線分の転送を組み立てる (core1で実行)
//pico-examplesのdvi_out_hstx_encoder.cと同じ方式:
//映像区間ではコマンドリスト→ピクセルデータの順に2回の割込みで1走査線分を供給する
void hstx_dma_irq() {
    //dma_pongは完了した方のチャンネル (これから組み立て直す方) を示す
    uint ch_num = hstx_dma_pong ? HSTX_DMACH_PONG : HSTX_DMACH_PING;
    dma_channel_hw_t* ch = &dma_hw->ch[ch_num];
    dma_hw->intr = 1u << ch_num;
    hstx_dma_pong = !hstx_dma_pong;

    if (hstx_v_scanline >= MODE_V_FRONT_PORCH && hstx_v_scanline < (MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH)) {
        ch->read_addr = (uintptr_t)hstx_vblank_line_vsync_on;
        ch->transfer_count = count_of(hstx_vblank_line_vsync_on);
    } else if (hstx_v_scanline < MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + MODE_V_BACK_PORCH) {
        ch->read_addr = (uintptr_t)hstx_vblank_line_vsync_off;
        ch->transfer_count = count_of(hstx_vblank_line_vsync_off);
    } else if (!hstx_vactive_cmdlist_posted) {
        ch->read_addr = (uintptr_t)hstx_vactive_line;
        ch->transfer_count = count_of(hstx_vactive_line);
        hstx_vactive_cmdlist_posted = true;
    } else {
        //ピクセルデータ (framebufの1走査線分=90語)。物理480ラインを2ラインおきに参照し、
        //240論理ラインのうち1回だけTICK用カウンタを進める (PicoDVI版core1_scanline_callbackと同じ)
        uint active_line = hstx_v_scanline - (MODE_V_TOTAL_LINES - MODE_V_ACTIVE_LINES);
        ch->read_addr = (uintptr_t)&framebuf[(active_line >> 1) * FRAME_BYTES_PER_LINE];
        ch->transfer_count = FRAME_BYTES_PER_LINE / 4;
        hstx_vactive_cmdlist_posted = false;
        if ((active_line & 1) == 0) {
            _g.linecnt++;
        }
    }

    if (!hstx_vactive_cmdlist_posted) {
        hstx_v_scanline = (hstx_v_scanline + 1) % MODE_V_TOTAL_LINES;
    }
}

//HSTXを起動する (core1から呼ばれる)
static void hstx_do_start() {
    dbg_str("[C1] do_start enter\r\n");
    //clk_hstxをPLL_SYSソースから135MHzで生成する (sysクロック270MHzと同じPLL出力の1/2)
    //NTSCからの復帰等でPLLが設定し直されていても、そのつど組み立て直すので正しくなる
    clock_configure(clk_hstx,
                    0,
                    CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                    clock_get_hz(clk_sys),
                    HSTX_PIXEL_CLOCK_HZ);
    //HSTXブロックをリセットして既知の状態から設定し直す (停止後の再開始・SLEEP復帰対策)
    reset_block_num(RESET_HSTX);
    unreset_block_num_wait_blocking(RESET_HSTX);

    //TMDSエンコーダを4bppグレースケール用に設定 (全レーン4bit入力、NBITS=3/ROT=28で
    //1ワード=8ピクセルを下位ビットから消費する)。白 (0xF) はTMDS 0xF0 (94%白)、黒 (0x0) はTMDS 0x00
    hstx_ctrl_hw->expand_tmds =
        3 << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB | 28 << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB |
        3 << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB | 28 << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB |
        3 << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB | 28 << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;
    //TMDSデータは4bit (1ピクセル) ずつ8チャンクに分割して消費する。RAWは1語で1シンボル
    hstx_ctrl_hw->expand_shift =
        8 << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB |
        4 << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB |
        1 << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB |
        0 << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB;
    //シリアル出力設定: 生成クロックの周期5クロック、コマンドエキスパンダは5クロックごとに
    //フェッチ、出力シフトレジスタは毎クロック2bitシフト (135MHz×2=270Mbps=TMDS bit clock)
    hstx_ctrl_hw->csr = 0;
    hstx_ctrl_hw->csr =
        HSTX_CTRL_CSR_EXPAND_EN_BITS |
        5u << HSTX_CTRL_CSR_CLKDIV_LSB |
        5u << HSTX_CTRL_CSR_N_SHIFTS_LSB |
        2u << HSTX_CTRL_CSR_SHIFT_LSB |
        HSTX_CTRL_CSR_EN_BITS;

    //HSTX出力ビット0-7はGP12-19に対応する。Pico DVI sockのピン配置に合わせる:
    //  GP12 D0+  GP13 D0-   (レーン0)
    //  GP14 CK+  GP15 CK-
    //  GP16 D2+  GP17 D2-   (レーン2)
    //  GP18 D1+  GP19 D1-   (レーン1)
    hstx_ctrl_hw->bit[2] = HSTX_CTRL_BIT0_CLK_BITS;
    hstx_ctrl_hw->bit[3] = HSTX_CTRL_BIT0_CLK_BITS | HSTX_CTRL_BIT0_INV_BITS;
    for (uint lane = 0; lane < 3; ++lane) {
        //各TMDSレーン (10bit出力) を対応するGPIOペアに割り当てる
        static const int lane_to_output_bit[3] = {0, 6, 4};
        int bit = lane_to_output_bit[lane];
        //偶数ビットを前半、奇数ビットを後半のタイミングで出す (シフタは毎クロック2bit進む)
        uint32_t lane_data_sel_bits =
            (lane * 10    ) << HSTX_CTRL_BIT0_SEL_P_LSB |
            (lane * 10 + 1) << HSTX_CTRL_BIT0_SEL_N_LSB;
        //差動ペアの片方は反転して出力する
        hstx_ctrl_hw->bit[bit    ] = lane_data_sel_bits;
        hstx_ctrl_hw->bit[bit + 1] = lane_data_sel_bits | HSTX_CTRL_BIT0_INV_BITS;
    }
    for (int i = 12; i <= 19; ++i) {
        gpio_set_function(i, GPIO_FUNC_HSTX);
    }

    //DMA: ピンポン2チャンネルとも同一設定で、1走査線分転送するごとに相手チャンネルへチェーンする
    //完了したチャンネルはhstx_dma_irqで組み立て直す (相手が転送中の間に次の1走査線分を準備できる)
    dma_channel_config c;
    c = dma_channel_get_default_config(HSTX_DMACH_PING);
    channel_config_set_chain_to(&c, HSTX_DMACH_PONG);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(
        HSTX_DMACH_PING,
        &c,
        &hstx_fifo_hw->fifo,
        hstx_vblank_line_vsync_off,
        count_of(hstx_vblank_line_vsync_off),
        false
    );
    c = dma_channel_get_default_config(HSTX_DMACH_PONG);
    channel_config_set_chain_to(&c, HSTX_DMACH_PING);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(
        HSTX_DMACH_PONG,
        &c,
        &hstx_fifo_hw->fifo,
        hstx_vblank_line_vsync_off,
        count_of(hstx_vblank_line_vsync_off),
        false
    );

    dma_hw->intr = (1u << HSTX_DMACH_PING) | (1u << HSTX_DMACH_PONG);
    dma_hw->inte0 = (1u << HSTX_DMACH_PING) | (1u << HSTX_DMACH_PONG);

    //DMAのバス優先度を上げる (フレームバッファ読み出しの取りこぼし防止)
    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;

    //走査線カウンタを初期化してping側から開始する (最初の2走査線分は上で組み込み済み)
    hstx_v_scanline = SCANLINE_INIT;
    hstx_vactive_cmdlist_posted = false;
    hstx_dma_pong = false;
    dma_channel_start(HSTX_DMACH_PING);
    hstx_running = true;
    dbg_str("[C1] do_start done clk_hstx=");
    dbg_hex(clock_get_hz(clk_hstx));
    dbg_str("\r\n");
}

//HSTXを停止する (core1から呼ばれる)
static void hstx_do_stop() {
    hstx_running = false;
    //先にDMA割込みを無効化してからチャンネルを中止する (停止処理中の再割込みを防ぐ)
    dma_hw->inte0 = 0;
    dma_channel_abort(HSTX_DMACH_PING);
    dma_channel_abort(HSTX_DMACH_PONG);
    dma_hw->intr = (1u << HSTX_DMACH_PING) | (1u << HSTX_DMACH_PONG);
    //HSTXを停止する (clk_hstx自体は動いたまま。GPIOはHSTX機能のまま出力が止まるだけ。
    //TFTモードへ切替える場合はtft_startがGP12-16をSPIに再設定する)
    hstx_ctrl_hw->csr = 0;
}

//core1のメインループ: DMA IRQハンドラと音声アラームプールを登録し、
//SIO FIFOでcore0からの開始/停止コマンドを待つ
//処理後にコマンド値をackとして返す (core0はackを受信するまでブロックするため競合しない)
//映像の開始はdvi_backend_init (video_output_init/video_switch_mode) がSTARTコマンドで指示する
//NTSC/TFTモードで起動した場合もcore1はFIFO待機し、PSG音声タイマーだけが動き続ける
void hstx_core1_main() {
    dbg_str("[C1] enter\r\n");
    irq_set_exclusive_handler(DMA_IRQ_0, hstx_dma_irq);
    irq_set_enabled(DMA_IRQ_0, true);
    psg_audio_core1_setup();//音声用の生ハードウェアアラーム+18µs常駐タイマー (core1上で動く)
    dbg_str("[C1] fifo wait\r\n");
    while (true) {
        uint32_t cmd = multicore_fifo_pop_blocking();
        dbg_str("[C1] cmd=");
        dbg_hex(cmd);
        dbg_str("\r\n");
        if (cmd == HSTX_CORE_CMD_START) {
            hstx_do_start();
        } else if (cmd == HSTX_CORE_CMD_STOP) {
            hstx_do_stop();
        }
        multicore_fifo_push_blocking(cmd);
    }
}

//DMAチャンネル (0,1) を確保してcore1を起動する (video_output_initから、ビデオモードに関係なく呼ぶ)
//core1はHSTX映像のDMA割込みとPSG音声タイマーの両方をホストする。
//NTSCモードで起動した場合も先に確保しておかないと、ntsc_init (dma_claim_unused_channel) や
//WiFi (CYW43) の動的確保と衝突する
void hstx_core1_launch() {
    hstx_claim_dma_channels();
    hstx_dvi_initialized = true;
    multicore_launch_core1(hstx_core1_main);
}

//フレームバッファを黒で初期化する (dvi_backend_initから呼ぶ)
void hstx_dvi_prepare() {
    memset(framebuf, 0, sizeof(framebuf));
}

//映像出力開始 (core1へSTARTコマンドを送り、処理完了まで待つ)
void hstx_dvi_start() {
    dbg_str("[C0] dvi_start enter\r\n");
    if (!hstx_dvi_initialized || hstx_running) {
        dbg_str("[C0] dvi_start skip\r\n");
        return;
    }
    multicore_fifo_push_blocking(HSTX_CORE_CMD_START);
    multicore_fifo_pop_blocking();
    dbg_str("[C0] dvi_start ack\r\n");
}

//映像出力停止 (core1へSTOPコマンドを送り、処理完了まで待つ)
void hstx_dvi_stop() {
    dbg_str("[C0] dvi_stop enter\r\n");
    if (!hstx_dvi_initialized || !hstx_running) {
        dbg_str("[C0] dvi_stop skip\r\n");
        return;
    }
    multicore_fifo_push_blocking(HSTX_CORE_CMD_STOP);
    multicore_fifo_pop_blocking();
    dbg_str("[C0] dvi_stop ack\r\n");
}

bool hstx_dvi_active() {
    return hstx_dvi_initialized && hstx_running;
}

#endif
