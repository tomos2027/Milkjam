//Milkjam Web表示モード (SWITCH 3 / VIDEO_MODE_WEB)
//NTSCコンポジット出力をローカル表示として継続しつつ、WiFi AP (アクセスポイント) を起動して
//ブラウザにもNTSCと同じ画面を表示し、キー入力もブラウザから受け付ける。
//
//構成:
//  - 簡易DHCPサーバー (udp 67) — pico-sdkに同梱がないため独自実装 (192.168.4.2-9のリース)
//  - HTTP+WebSocketサーバー (tcp 80) — ブラウザへページを返し、WSで画面転送とキー入力
//  - 画面転送エンジン (sys_timeout 50ms周期) — vram/screen_attrの差分を行単位で送信
//    RUN中もメインループが塞がっている間はlwIPのタイムアウト処理 (割込みコンテキスト) から動くため
//    NTSCのTV表示と同じくブラウザもライブ更新される。キー入力はkeybuf経由でINKEY()/STOPも有効
//  - クライアントはフォント+パレット+モード (拡大/反転/カーソル) を受けてcanvasに描画する。
//    単色モードの拡大 (VIDEO 3/4) はDVIのvram_to_framebufと同じピクセル複製で再現し、
//    カラーモードの反転/カーソルはNTSC (vram_to_tvram_all) と同じ前景/背景入替で再現する
//
// ※WebサーバーとAPはSWITCH 3の間だけ動く (SWITCH 0-2へ戻すと停止し、保存済みSSIDがあれば
//   家のWiFiへ自動再接続する)。AP中はSTAを切断する (LWIP_SINGLE_NETIF=1のため併用不可)。
#ifndef __EXT_WEB_H__
#define __EXT_WEB_H__
#include "all_includes.h"
#include "pico/cyw43_arch.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/timeouts.h"

//===== 設定 =====
#define WEB_PORT              80
#define WEB_PUSH_INTERVAL_MS  50               // 画面転送周期 (20fps)
#define WEB_DHCP_POOL         8                // 同時リース数 (192.168.4.2-9)
#define WEB_DHCP_LEASE_SEC    86400            // リース時間 (24時間)
#define WEB_TCP_BUF           1024             // 受信バッファ (HTTPヘッダ/WSフレームの部分受信)
#define WEB_WS_PAYLOAD_MAX    520              // 送受信ペイロード上限 (フォントチャンク515含む)
#define WEB_IDLE_TIMEOUT_MS   10000            // ACKが10秒止まったら接続を回収 (無線切断等で死んだ接続の残存時間を短縮)

//===== 状態 =====
static struct udp_pcb* web_dhcp_pcb;
static struct tcp_pcb* web_listen_pcb;
static struct tcp_pcb* web_conn_pcb;   // 現在の接続 (1クライアント制限)
static uint8_t web_conn_ws;            // 0=HTTP 1=WebSocket確立済み
static uint8_t web_conn_closing;       // 送信完了後にcloseする
static bool web_server_running;
static char web_rbuf[WEB_TCP_BUF];     // 受信バッファ
static int web_rlen;
static int web_page_off;               // HTMLページ送信済みバイト数 (-1=対象外)
static int web_font_off;               // WS初期化: フォント送信済みバイト数
static uint8_t web_client_cm, web_client_w, web_client_h, web_client_mag; // クライアントが持つ状態
static uint8_t web_client_inv, web_client_xm, web_client_cx, web_client_cy, web_client_con;
static uint32_t web_last_ack_ms;
static uint32_t web_last_ping_ms;
static uint16_t web_ping_fail; //WS pingの連続失敗数 (転送経路の停滞診断用、UARTに出力)
static uint32_t web_tx_count; //WS送信フレーム数 (クライアントの診断表示用、統計フレームで通知)
//画面転送タイマー鎖の自己修復用 (iotpad_web_tickがメインループから監視)
//sys_timeoutの再登録が一度でも欠けると転送が永久停止するため、armedフラグと最終動作時刻を管理する
static volatile uint8_t web_push_armed;
static volatile uint32_t web_last_push_ms;
//WS送信組み立てバッファ (先頭4バイトはWSヘッダ置き場)。
//行差分を1フレームにまとめるため全画面分 (ワイド27行×87B+ヘッダ≈2351、sndbuf上限2144で分割) を格納できるサイズ
static uint8_t web_wsbuf[2200];

static void web_reset_conn_state(); //接続状態リセット (web_cleanup_stale_connから先行参照)

//送信済み差分ベース (クライアントと共有している内容の控え)
//画面データはvram/screen_attr (最大32x24) を直接読む。拡大 (VIDEO 3/4) はTVRAMでは
//グリフ断片の複製になるためブラウザ側で再現が難しく、vram+倍率を送ってクライアント側で
//ピクセル複製する (DVIのvram_to_framebufと同じ方式でNTSC表示と同じ見た目になる)
static uint8_t web_sent_char[32 * 24];
static uint8_t web_sent_attr[32 * 24];
static uint8_t web_sent_font[2048];
static uint8_t web_sent_pal[16];
static uint8_t web_sent_pal_valid; //0=パレット未送信 (接続切替/resyncで無効化、次tickで必ず再送)
static int web_sent_valid;

//===== SHA1 (WebSocketハンドシェイクのSec-WebSocket-Accept計算用) =====
static void web_sha1(const uint8_t* data, int len, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    uint8_t block[128]; // WSキー+GUIDは60バイトのため最大2ブロック
    int blocks = (len + 1 + 8 + 63) / 64;
    memset(block, 0, blocks * 64);
    memcpy(block, data, len);
    block[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) {
        block[blocks * 64 - 1 - i] = (uint8_t)(bits >> (i * 8));
    }
    for (int b = 0; b < blocks; b++) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            w[i] = ((uint32_t)block[b * 64 + i * 4] << 24) | ((uint32_t)block[b * 64 + i * 4 + 1] << 16)
                 | ((uint32_t)block[b * 64 + i * 4 + 2] << 8) | block[b * 64 + i * 4 + 3];
        }
        for (int i = 16; i < 80; i++) {
            uint32_t v = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
            w[i] = (v << 1) | (v >> 31);
        }
        uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (bb & c) | (~bb & d); k = 0x5A827999; }
            else if (i < 40) { f = bb ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (bb & c) | (bb & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = bb ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t t = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e = d; d = c; c = (bb << 30) | (bb >> 2); bb = a; a = t;
        }
        h[0] += a; h[1] += bb; h[2] += c; h[3] += d; h[4] += e;
    }
    for (int i = 0; i < 5; i++) {
        out[i * 4] = h[i] >> 24; out[i * 4 + 1] = h[i] >> 16; out[i * 4 + 2] = h[i] >> 8; out[i * 4 + 3] = h[i];
    }
}

//base64エンコード (出力はlen*4/3+1バイト以上)
static void web_b64(const uint8_t* in, int len, char* out) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int o = 0;
    for (int i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < len ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < len ? in[i + 2] : 0);
        out[o++] = t[(v >> 18) & 63];
        out[o++] = t[(v >> 12) & 63];
        out[o++] = i + 1 < len ? t[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < len ? t[v & 63] : '=';
    }
    out[o] = 0;
}

//===== 簡易DNSサーバー (udp 53) =====
//AP内の名前解決用 (DHCPでDNS=自身を通知)。すべてのAレコード問い合わせに自身 (192.168.4.1) を
//即答するため、クライアントOSの接続検証 (Windows NCSI/Android connectivitycheck等) が
//DNSタイムアウト (1回5秒×複数回) で滞留せず、AP接続直後にネットワークが即使用可能になる
static struct udp_pcb* web_dns_pcb;
static uint8_t web_dns_tx[512];

//DNS受信 (lwIP = 割込みコンテキスト): A→自身のアドレス、AAAA→空応答 (NODATA)、その他は無視
static void web_dns_recv(void* arg, struct udp_pcb* pcb, struct pbuf* p, const ip_addr_t* addr, u16_t port) {
    (void)arg; (void)pcb;
    uint8_t q[512];
    int len = p && p->tot_len <= (int)sizeof q ? pbuf_copy_partial(p, q, sizeof q, 0) : 0;
    pbuf_free(p);
    if (len < 17) return; //ヘッダ12 + 最小の問い合わせ
    u16_t flags = ((u16_t)q[2] << 8) | q[3];
    if (flags & 0x8000) return; //応答は無視 (クエリのみ)
    if (((u16_t)q[4] << 8 | q[5]) < 1) return; //QDCOUNT
    //質問部の終端を探す (QNAMEの0終端 + QTYPE2 + QCLASS2)
    int i = 12;
    while (i < len && q[i] != 0) i += q[i] + 1;
    if (i + 5 > len) return;
    int qtype = (q[i + 1] << 8) | q[i + 2];
    if (qtype != 1 && qtype != 28) return; //A/AAAA以外は無視
    int qlen = i + 5 - 12; //質問部の長さ (12起点)
    uint8_t* t = web_dns_tx;
    memcpy(t, q, 12); //ID等をコピー
    t[2] = 0x84; t[3] = 0x00; //QR=1 AA=1 RCODE=0
    memcpy(t + 12, q + 12, qlen);
    int o = 12 + qlen;
    if (qtype == 1) {
        //回答: NAME (0xC00C圧縮ポインタ) TYPE=A CLASS=IN TTL=10 RDLENGTH=4 RDATA=192.168.4.1
        t[o++] = 0xC0; t[o++] = 0x0C;
        t[o++] = 0; t[o++] = 1;
        t[o++] = 0; t[o++] = 1;
        t[o++] = 0; t[o++] = 0; t[o++] = 0; t[o++] = 10;
        t[o++] = 0; t[o++] = 4;
        t[o++] = 192; t[o++] = 168; t[o++] = 4; t[o++] = 1;
        t[6] = 0; t[7] = 1; //ANCOUNT=1
    } else {
        t[6] = 0; t[7] = 0; //AAAA: NODATA (即フォールバックでAを問いさせる)
    }
    struct pbuf* r = pbuf_alloc(PBUF_TRANSPORT, o, PBUF_RAM);
    if (!r) return;
    memcpy(r->payload, t, o);
    udp_sendto(web_dns_pcb, r, addr, port);
    pbuf_free(r);
}

//===== 簡易DHCPサーバー (udp 67) =====
struct web_dhcp_lease {
    uint8_t mac[6];
    uint32_t ip;       // ネットワークバイトオーダー
    uint32_t expiry;   // 起動ms
};
static struct web_dhcp_lease web_dhcp_leases[WEB_DHCP_POOL];
static uint8_t web_dhcp_tx[300];

#define WEB_IP_BASE    0xC0A80401u // 192.168.4.1 (ネットワークバイトオーダー)
#define WEB_IP_CLIENT0 0xC0A80402u // 192.168.4.2

//MACに対応するリースIPを返す (無ければ空き/期限切れスロットへ割り当て、満席なら0)
static uint32_t web_dhcp_assign(const uint8_t* mac) {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    for (int i = 0; i < WEB_DHCP_POOL; i++) {
        if (web_dhcp_leases[i].ip && memcmp(web_dhcp_leases[i].mac, mac, 6) == 0) {
            web_dhcp_leases[i].expiry = now + WEB_DHCP_LEASE_SEC * 1000;
            return web_dhcp_leases[i].ip;
        }
    }
    for (int i = 0; i < WEB_DHCP_POOL; i++) {
        if (!web_dhcp_leases[i].ip || web_dhcp_leases[i].expiry < now) {
            memcpy(web_dhcp_leases[i].mac, mac, 6);
            web_dhcp_leases[i].ip = WEB_IP_CLIENT0 + i;
            web_dhcp_leases[i].expiry = now + WEB_DHCP_LEASE_SEC * 1000;
            return web_dhcp_leases[i].ip;
        }
    }
    return 0;
}

//DHCPクライアントが(再)接続してきた際の強制クリーンアップ: 同一クライアント (IP一致) の
//TCP接続が残っていれば即座に破棄する。無線切断等で死んだ接続がIdle監視 (10秒) を待たず、
//再接続直後のリロードで即クリーンな状態にできる
static void web_cleanup_stale_conn(uint32_t yiaddr) {
    struct tcp_pcb* pcb = web_conn_pcb;
    if (!pcb) return;
    if (pcb->remote_ip.addr != yiaddr) return; //別クライアント (2台目) の場合は触らない
    iotpad_dbg_str("WEB: dhcp cleanup\n");
    web_conn_pcb = NULL;
    web_reset_conn_state();
    tcp_abort(pcb);
}

static void web_dhcp_reply(const uint8_t* req, int reqlen, int msgtype, uint32_t yiaddr) {
    (void)reqlen;
    uint8_t* t = web_dhcp_tx;
    memset(t, 0, sizeof web_dhcp_tx);
    t[0] = 2;               // BOOTREPLY
    t[1] = 1;               // htype=ethernet
    t[2] = 6;               // hlen
    t[3] = 0;               // hops
    memcpy(t + 4, req + 4, 4);      // xid
    t[10] = 0; t[11] = 0x80;        // flags: ブロードキャスト応答
    t[16] = yiaddr >> 24; t[17] = yiaddr >> 16; t[18] = yiaddr >> 8; t[19] = yiaddr; // yiaddr
    t[20] = (WEB_IP_BASE >> 24) & 0xFF; t[21] = (WEB_IP_BASE >> 16) & 0xFF; t[22] = (WEB_IP_BASE >> 8) & 0xFF; t[23] = WEB_IP_BASE & 0xFF; // siaddr
    memcpy(t + 28, req + 28, 16);   // chaddr
    t[236] = 0x63; t[237] = 0x82; t[238] = 0x53; t[239] = 0x63; // magic cookie
    int o = 240;
    t[o++] = 53; t[o++] = 1; t[o++] = msgtype;                    // DHCPメッセージタイプ
    t[o++] = 54; t[o++] = 4;                                      // サーバーID
    t[o++] = (WEB_IP_BASE >> 24) & 0xFF; t[o++] = (WEB_IP_BASE >> 16) & 0xFF; t[o++] = (WEB_IP_BASE >> 8) & 0xFF; t[o++] = WEB_IP_BASE & 0xFF;
    t[o++] = 51; t[o++] = 4;                                      // リース時間
    t[o++] = WEB_DHCP_LEASE_SEC >> 24; t[o++] = WEB_DHCP_LEASE_SEC >> 16; t[o++] = (WEB_DHCP_LEASE_SEC >> 8) & 0xFF; t[o++] = WEB_DHCP_LEASE_SEC & 0xFF;
    t[o++] = 1; t[o++] = 4; t[o++] = 255; t[o++] = 255; t[o++] = 255; t[o++] = 0; // サブネットマスク
    t[o++] = 3; t[o++] = 4;                                       // ルーター
    t[o++] = (WEB_IP_BASE >> 24) & 0xFF; t[o++] = (WEB_IP_BASE >> 16) & 0xFF; t[o++] = (WEB_IP_BASE >> 8) & 0xFF; t[o++] = WEB_IP_BASE & 0xFF;
    t[o++] = 6; t[o++] = 4;                                       // DNSサーバー (簡易DNS=自身)
    t[o++] = (WEB_IP_BASE >> 24) & 0xFF; t[o++] = (WEB_IP_BASE >> 16) & 0xFF; t[o++] = (WEB_IP_BASE >> 8) & 0xFF; t[o++] = WEB_IP_BASE & 0xFF;
    t[o++] = 255; // end
    struct pbuf* p = pbuf_alloc(PBUF_TRANSPORT, o, PBUF_RAM);
    if (!p) return;
    memcpy(p->payload, t, o);
    udp_sendto(web_dhcp_pcb, p, IP_ADDR_BROADCAST, 68);
    pbuf_free(p);
}

//オプションコードの値を探す (見つからなければnullptr)
static const uint8_t* web_dhcp_opt(const uint8_t* p, int len, int code, int* vlen) {
    int o = 240;
    while (o + 1 < len) {
        int c = p[o];
        if (c == 0) { o++; continue; }       // pad
        if (c == 255) break;                 // end
        int l = p[o + 1];
        if (o + 2 + l > len) break;
        if (c == code) {
            if (vlen) *vlen = l;
            return p + o + 2;
        }
        o += 2 + l;
    }
    return NULL;
}

//DHCP受信 (lwIP = 割込みコンテキスト)
static void web_dhcp_recv(void* arg, struct udp_pcb* pcb, struct pbuf* p, const ip_addr_t* addr, u16_t port) {
    (void)arg; (void)addr; (void)port;
    uint8_t req[320];
    int len = p && p->tot_len <= (int)sizeof req ? pbuf_copy_partial(p, req, sizeof req, 0) : 0;
    pbuf_free(p);
    if (len < 240 || req[0] != 1) return; // BOOTREQUEST以外は無視
    if (req[236] != 0x63 || req[237] != 0x82 || req[238] != 0x53 || req[239] != 0x63) return;
    int mtlen;
    const uint8_t* mt = web_dhcp_opt(req, len, 53, &mtlen);
    if (!mt || !mtlen) return;
    if (mt[0] == 1) { // DISCOVER → OFFER
        uint32_t ip = web_dhcp_assign(req + 28);
        web_cleanup_stale_conn(ip); //クライアント再接続時: 残留TCP接続を即破棄
        if (ip) web_dhcp_reply(req, len, 2, ip);
    } else if (mt[0] == 3) { // REQUEST → ACK (要求IPが範囲外ならNAK)
        int rlen;
        const uint8_t* r = web_dhcp_opt(req, len, 50, &rlen);
        uint32_t ip = web_dhcp_assign(req + 28);
        web_cleanup_stale_conn(ip); //クライアント再接続時: 残留TCP接続を即破棄
        if (ip && (!r || rlen != 4
            || (((uint32_t)r[0] << 24 | (uint32_t)r[1] << 16 | (uint32_t)r[2] << 8 | r[3]) == ip))) {
            web_dhcp_reply(req, len, 5, ip);
        } else {
            web_dhcp_reply(req, len, 6, 0);
        }
    }
    //RELEASE/INFORM等は無視
}

//===== 診断ログ (UART GP0 115200bps、1秒に1回まで間引く) =====
static uint32_t web_diag_last_ms;
static void web_diag(const char* msg, int v1, int v2) {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - web_diag_last_ms < 1000) return;
    web_diag_last_ms = now;
    char buf[96];
    snprintf(buf, sizeof buf, msg, v1, v2);
    iotpad_dbg_str(buf);
}

//===== WebSocket送受信 =====
//WSバイナリフレーム送信。ペイロードはweb_wsbuf+4に組み立てておく。
//ヘッダ+本体を1回のtcp_writeで送る (2回に分けると本体側のtcp_writeが失敗したとき
//孤立ヘッダがストリームに混入してフレーム境界が壊れるため)。送信できたら1を返す
static int web_ws_tx(struct tcp_pcb* pcb, int opcode, int len) {
    int hl;
    if (len < 126) {
        web_wsbuf[0] = 0x80 | opcode;
        web_wsbuf[1] = len;
        hl = 2;
        if (len) memmove(web_wsbuf + 2, web_wsbuf + 4, len); //ヘッダ直後に本体を連結
    } else {
        web_wsbuf[0] = 0x80 | opcode;
        web_wsbuf[1] = 126;
        web_wsbuf[2] = len >> 8;
        web_wsbuf[3] = len & 0xFF;
        hl = 4; //本体はweb_wsbuf+4にそのまま連結
    }
    if (hl + len > (int)sizeof web_wsbuf) {
        web_diag("WEB: tx ovf op=%d len=%d\n", opcode, len);
        return 0;
    }
    if (tcp_sndbuf(pcb) < hl + len) {
        web_diag("WEB: tx full snd=%d qlen=%d\n", tcp_sndbuf(pcb), pcb->snd_queuelen);
        return 0;
    }
    if (tcp_write(pcb, web_wsbuf, hl + len, TCP_WRITE_FLAG_COPY) != ERR_OK) {
        web_diag("WEB: tx err snd=%d qlen=%d\n", tcp_sndbuf(pcb), pcb->snd_queuelen);
        return 0;
    }
    tcp_output(pcb);
    web_tx_count++;
    return 1;
}

//受信したキーデータをBASICのキーバッファへ (ESCはSTOPフラグも立てる。hid_app.cと同じ経路)
//paste=1: コードペースト (WSテキストフレーム) — かな変換とESC停止フラグをバイパスし、
//         プログラムコードをそのままデータとして入力する
static void web_keys_in(const uint8_t* data, int len, int paste) {
    for (int i = 0; i < len; i++) {
        char ch = (char)data[i];
        if (!paste && key_flg.kana) {
            ch = romajikana_input(ch); // ローマ字かな変換 (USBキーボードと同じ経路)
        }
        if (!paste && ch == ESC) {
            _g.key_flg_esc = 1;
        }
        key_pushc(ch);
    }
}

static void web_reset_conn_state() {
    web_conn_ws = 0;
    web_conn_closing = 0;
    web_rlen = 0;
    web_page_off = -1;
    web_font_off = 0;
    web_sent_valid = 0;
    web_sent_pal_valid = 0; //新クライアントはパレットを持っていないため必ず再送する
    web_ping_fail = 0;
    web_client_cm = 0xFF; //クライアントはまだ何も知らない (モード再送を促す)
    web_client_w = 0;
    web_client_h = 0;
    web_client_mag = 0;
    web_client_inv = 0;
    web_client_xm = 0;
    web_client_cx = 0;
    web_client_cy = 0;
    web_client_con = 0;
}

static void web_conn_close(struct tcp_pcb* pcb) {
    iotpad_dbg_str("WEB: close\n");
    if (web_conn_pcb == pcb) {
        web_conn_pcb = NULL;
    }
    web_reset_conn_state();
    if (tcp_close(pcb) != ERR_OK) tcp_abort(pcb);
}

static err_t web_recv(void* arg, struct tcp_pcb* pcb, struct pbuf* p, err_t err);
static err_t web_sent_cb(void* arg, struct tcp_pcb* pcb, u16_t len);
static void web_err(void* arg, err_t err);
static err_t web_poll(void* arg, struct tcp_pcb* pcb);
static void web_http_process(struct tcp_pcb* pcb);
static void web_ws_parse(struct tcp_pcb* pcb);

//===== 画面転送 (50ms周期、lwIPのsys_timeout = 割込みコンテキストから駆動) =====
//BASICのRUNでメインループが塞がっていても動く (タイマー割込みがvramを更新し続けるため)
static void web_push_tick(void* arg) {
    (void)arg;
    web_push_armed = 0;
    web_last_push_ms = to_ms_since_boot(get_absolute_time());
    if (web_server_running) {
        web_push_armed = 1;
        sys_timeout(WEB_PUSH_INTERVAL_MS, web_push_tick, NULL);
    }
    struct tcp_pcb* pcb = web_conn_pcb;
    if (!pcb || !web_conn_ws || web_conn_closing) return;
    uint32_t now = web_last_push_ms;
    //状態診断ログ (2秒ごと): ntsc_activeゲートの前に出力し、転送が止まっている原因
    //(NTSC停止中か/WS未確立か/送信キュー詰まりか) をUARTで判別できるようにする
    static uint32_t web_diag_tick_ms;
    if (now - web_diag_tick_ms > 2000) {
        web_diag_tick_ms = now;
        char buf[120];
        snprintf(buf, sizeof buf, "WEB: tick ntsc=%d ws=%d fo=%d sv=%d snd=%d ql=%d ack=%dms\n",
            ntsc_active(), (int)web_conn_ws, (int)web_font_off, web_sent_valid,
            (int)tcp_sndbuf(pcb), (int)pcb->snd_queuelen, (int)(now - web_last_ack_ms));
        iotpad_dbg_str(buf);
    }
    if (!ntsc_active()) return; //VIDEO 0等で映像停止中は転送しない
    //WS ping (3秒ごと): 画面静止中は差分ゼロで無通信が正常なため、定期的にACKを強制して
    //アイドル監視の誤発動とクライアント側WiFiのアイドル切断を防ぐ (pongはブラウザが自動返信)。
    //連続失敗数はUARTに出力し、転送経路 (クライアント側) の停滞診断に使う
    if (now - web_last_ping_ms > 3000) {
        web_last_ping_ms = now;
        if (web_ws_tx(pcb, 0x9, 0)) {
            web_ping_fail = 0;
        } else {
            web_ping_fail++;
            if (web_ping_fail % 5 == 0) {
                char buf[64];
                snprintf(buf, sizeof buf, "WEB: ping fail x%d snd=%d ql=%d\n",
                    web_ping_fail, (int)tcp_sndbuf(pcb), (int)pcb->snd_queuelen);
                iotpad_dbg_str(buf);
            }
        }
    }
    //送信統計フレーム (1秒ごと): [5][送信フレーム数2byte][デバイス起動時間2byte] —
    //クライアントの診断表示 (tx=N up=XXs) 用。push tickが動いていれば転送が詰まっていても
    //1秒ごとに届くため、tx/upの推移で「tick死」「転送詰まり」「クライアント解析問題」に加え、
    //upがリセットされたらデバイス再起動 (ウォッチドッグ) であることがブラウザだけで判別できる
    static uint32_t web_stat_ms;
    if (now - web_stat_ms > 1000) {
        web_stat_ms = now;
        uint32_t upsec = now / 1000; //デバイス起動時間 (秒、16bitで約18時間まで)
        web_wsbuf[4] = 5; //種別=統計
        web_wsbuf[5] = web_tx_count & 0xFF;
        web_wsbuf[6] = (web_tx_count >> 8) & 0xFF;
        web_wsbuf[7] = upsec & 0xFF;
        web_wsbuf[8] = (upsec >> 8) & 0xFF;
        web_ws_tx(pcb, 2, 5);
    }
    // 描画モード (NTSCのvideomodeに追従): 0=単色 (拡大あり) 1=カラー32x24 2=カラー42x27
    int cm = videomode == VMODE_IJCOLOR ? 1 : videomode == VMODE_WIDETEXT ? 2 : 0;
    int w = SCREEN_W;
    int h = SCREEN_H;
    int mag = 1 << _g.screen_big;
    // 1) フォント未送信チャンク (接続直後。ntsc_fontram = IchigoJamフォント+PCG 2048バイト)
    //メッセージ形式: [1][オフセット2byte][長さ2byte][データ...] (先頭1バイト=メッセージ種別、オフセット/長さはLE)
    while (web_font_off < 2048) {
        if (tcp_sndbuf(pcb) < 4 + 517) return;
        int len = 2048 - web_font_off;
        if (len > 512) len = 512;
        uint8* pay = web_wsbuf + 4;
        pay[0] = 1; //種別=フォント
        pay[1] = web_font_off & 0xFF;
        pay[2] = (web_font_off >> 8) & 0xFF;
        pay[3] = len & 0xFF;
        pay[4] = len >> 8;
        memcpy(pay + 5, ntsc_fontram + web_font_off, len);
        if (!web_ws_tx(pcb, 2, 5 + len)) return; //opcode 2=バイナリ (テキスト1はUTF-8検証で切断される)
        memcpy(web_sent_font + web_font_off, ntsc_fontram + web_font_off, len);
        web_font_off += len;
    }
    // 2) モード (描画モード・グリッド・拡大・反転・カーソル)
    //メッセージ形式: [2][cm][w][h][mag][inv][xm][cx][cy][con]
    //xm=カーソルXORマスク (挿入0xF0/上書き0xFF)、con=カーソル表示中
    int inv = _g.screen_invert ? 1 : 0;
    int xm = key_flg.insert ? 0xFF : 0xF0;
    int cx = _g.cursorx;
    int cy = _g.cursory;
    int con = _g.cursorflg && cy >= 0 && cx < w && cy < h;
    if (cm != web_client_cm || w != web_client_w || h != web_client_h || mag != web_client_mag
        || inv != web_client_inv || xm != web_client_xm || cx != web_client_cx || cy != web_client_cy
        || con != web_client_con) {
        if (tcp_sndbuf(pcb) < 4 + 11) return;
        uint8* pay = web_wsbuf + 4;
        pay[0] = 2; //種別=モード
        pay[1] = cm;
        pay[2] = w;
        pay[3] = h;
        pay[4] = mag;
        pay[5] = inv;
        pay[6] = xm;
        pay[7] = cx;
        pay[8] = cy;
        pay[9] = con;
        if (!web_ws_tx(pcb, 2, 10)) return;
        int grid_changed = cm != web_client_cm || w != web_client_w || h != web_client_h || mag != web_client_mag;
        web_client_cm = cm;
        web_client_w = w;
        web_client_h = h;
        web_client_mag = mag;
        web_client_inv = inv;
        web_client_xm = xm;
        web_client_cx = cx;
        web_client_cy = cy;
        web_client_con = con;
        if (grid_changed) web_sent_valid = 0; //グリッドサイズが変わるため全行再送
    }
    // 3) パレット (RGB332 16色、PALコマンドの変更を反映)
    //未送信フラグまたは内容変化で送る (web_sent_palの状態は接続切替/resyncで無効化する)
    if (!web_sent_pal_valid || memcmp(screen_pal, web_sent_pal, 16) != 0) {
        if (tcp_sndbuf(pcb) < 4 + 17) return;
        web_wsbuf[4] = 3; //種別=パレット
        memcpy(web_wsbuf + 5, screen_pal, 16);
        if (!web_ws_tx(pcb, 2, 17)) return; //opcode 2=バイナリ (3-7は予約オペコードで切断される)
        memcpy(web_sent_pal, screen_pal, 16);
        web_sent_pal_valid = 1;
    }
    // 4) 行差分 (全差分行を1フレームにまとめて送る: 小フレームの連続送信は
    //    送信キュー/ACK粒度との絡みで送信が停滞するため)
    //メッセージ形式: [4][行数n][(y)(w)(文字w)(属性w)]×n — sndbufに収まる分だけ送り、残りは次周期
    int batch = 0;
    int cap = tcp_sndbuf(pcb) - 6; //WSヘッダ+行数バイト分の余裕
    int y = 0;
    for (; y < h; y++) {
        uint8* cl = vram + y * w;
        uint8* al = screen_attr + y * w;
        if (web_sent_valid && memcmp(cl, web_sent_char + y * w, w) == 0
            && memcmp(al, web_sent_attr + y * w, w) == 0) {
            continue;
        }
        if (cap < 2 + 2 * w) break; //この周期で送れるのはここまで
        uint8* pay = web_wsbuf + 6 + batch * (2 + 2 * w);
        pay[0] = y;
        pay[1] = w;
        memcpy(pay + 2, cl, w);
        memcpy(pay + 2 + w, al, w);
        memcpy(web_sent_char + y * w, cl, w); //送信分だけ差分ベースへ反映
        memcpy(web_sent_attr + y * w, al, w);
        batch++;
        cap -= 2 + 2 * w;
    }
    if (batch > 0) {
        web_wsbuf[4] = 4;  //種別=行
        web_wsbuf[5] = batch;
        web_ws_tx(pcb, 2, 2 + batch * (2 + 2 * w)); //opcode 2=バイナリ
    }
    if (y == h) {
        web_sent_valid = 1; //全行を処理しきった
    }
    // 5) フォント差分 (PCG: POKE #700-#7FFでの書き換えを検知して再送)
    for (int off = 0; off < 2048; off += 512) {
        if (memcmp(ntsc_fontram + off, web_sent_font + off, 512) == 0) continue;
        if (tcp_sndbuf(pcb) < 4 + 517) return;
        uint8* pay = web_wsbuf + 4;
        pay[0] = 1; //種別=フォント
        pay[1] = off & 0xFF;
        pay[2] = (off >> 8) & 0xFF;
        pay[3] = 512 & 0xFF;
        pay[4] = 512 >> 8;
        memcpy(pay + 5, ntsc_fontram + off, 512);
        if (!web_ws_tx(pcb, 2, 5 + 512)) return; //opcode 2=バイナリ
        memcpy(web_sent_font + off, ntsc_fontram + off, 512);
    }
}

//メインループから呼ぶ (main.c): 画面転送タイマー鎖の自己修復。
//sys_timeoutの再登録はpush tickの先頭で行うが、再登録に一度でも失敗/欠落すると
//armedフラグが1のまま転送だけが永久停止する。そこでarmedフラグは信頼せず、
//200ms以上pushが動いていなければsys_untimeout→sys_timeoutのペアで無条件に再登録する
//(残留/二重エントリもここで清掃される)
void iotpad_web_tick() {
    if (!web_server_running) return;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - web_last_push_ms < 200) return; //鎖が生きている (tickが最終動作時刻を更新し続ける)
    web_push_armed = 1;
    web_last_push_ms = now;
    cyw43_arch_lwip_begin();
    sys_untimeout(web_push_tick, NULL);
    sys_timeout(WEB_PUSH_INTERVAL_MS, web_push_tick, NULL);
    cyw43_arch_lwip_end();
    iotpad_dbg_str("WEB: push rearm\n");
}

//===== ブラウザページ (flash定数。フォントはWSで受信するためページは小さい) =====
static const char WEB_PAGE[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<link rel='icon' href='data:,'>"
"<title>Milkjam</title><style>"
"body{background:#000;color:#999;font:13px monospace;margin:0;text-align:center}"
"#c{image-rendering:pixelated;background:#000;margin:8px auto 4px;display:block;touch-action:none}"
"#st{color:#a66}button{background:#181818;color:#ccc;border:1px solid #444;border-radius:5px;"
"padding:10px 13px;margin:2px;font:15px monospace}#k{opacity:0;position:absolute;left:-999px}"
"</style></head><body><canvas id=c width=256 height=192></canvas><div id=st>connecting...</div>"
"<div><button onmousedown='kp([27])'>ESC</button><button onmousedown='kp([28])'>&#8592;</button>"
"<button onmousedown='kp([30])'>&#8593;</button><button onmousedown='kp([31])'>&#8595;</button>"
"<button onmousedown='kp([29])'>&#8594;</button><button onmousedown='kp([10])'>ENTER</button></div>"
"<div><button onmousedown='szm(-1)'>－</button><button onmousedown='szm(1)'>＋</button>"
"<button onmousedown='szf()'>FIT</button><button onmousedown='popen()'>PASTE</button></div>"
"<div><button onmousedown='rec()'>再接続</button><button onmousedown='location.reload()'>再読み込み</button></div>"
"<div id=p style='display:none;position:fixed;left:0;top:0;width:100%;height:100%;"
"background:rgba(0,0,0,.85);z-index:9;padding-top:20px'>"
"<textarea id=t rows=8 style='width:92%;background:#111;color:#ccc;border:1px solid #555;"
"font:13px monospace'></textarea><br>"
"<button onmousedown='psend()' id=pb>送信</button><button onmousedown='pclose()'>閉じる</button></div>"
"<input id=k autocomplete='off' autocapitalize='off'><script>"
"var F=new Uint8Array(2048),V=null,A=null,W=32,H=24,M=1,CM=0,inv=0,XM=240,cx=0,cy=0,con=0,P=null,ws=null,dr=0,bl=1;"
"var cv=document.getElementById('c'),g=cv.getContext('2d'),img=null;"
"var st=document.getElementById('st'),kb=document.getElementById('k');"
"var WF=[255,255,255],BK=[0,0,0];"
"function sz(){cv.width=CM==2?336:256;cv.height=CM==2?216:192;"
"V=new Uint8Array(W*H);A=new Uint8Array(W*H);img=g.createImageData(cv.width,cv.height);apply()}"
"var SC=0,PSC=[1,2,3,4,6,8];"
"try{var ls=parseInt(localStorage.getItem('mjscale'));if(PSC.indexOf(ls)>=0)SC=ls}catch(e){}"
"function apply(){if(SC){cv.style.width=cv.width*SC+'px';cv.style.height=cv.height*SC+'px'}else fit()}"
"function fit(){var d=Math.max(1,Math.min(8,Math.floor((innerWidth-10)/cv.width)));"
"cv.style.width=cv.width*d+'px';cv.style.height=cv.height*d+'px'}"
"function szm(d){var i=PSC.indexOf(SC);i=i<0?(d>0?0:PSC.length-1):Math.max(0,Math.min(PSC.length-1,i+d));"
"SC=PSC[i];try{localStorage.setItem('mjscale',SC)}catch(e){};apply()}"
"function szf(){SC=0;try{localStorage.setItem('mjscale',0)}catch(e){};apply()}"
"addEventListener('resize',function(){apply()});"
"setInterval(function(){bl^=1;dr=1},533);"
"function draw(){var d=img.data,w2=cv.width,W2=CM==2?42:32,H2=CM==2?27:24;"
"for(var y=0;y<H2;y++)for(var x=0;x<W2;x++){"
"var vx=x,vy=y,sx=0,sy=0,f=WF,b=BK,cu=0;"
"if(CM==2){if(x>=32||y>=24){for(var qy=0;qy<8;qy++){var ob=((y*8+qy)*w2+x*8)*4;"
"for(var qx=0;qx<8;qx++){d[ob+qx*4]=0;d[ob+qx*4+1]=0;d[ob+qx*4+2]=0;d[ob+qx*4+3]=255}}continue}}else if(CM==0){vx=(x/M)|0;vy=(y/M)|0;sx=x%M;sy=y%M}"
"var ch=V[vy*W+vx],cur=con&&bl&&cx==vx&&cy==vy;"
"if(CM){var a=A[vy*W+vx];f=P[a&15];b=P[a>>4];if(inv||cur){var t2=f;f=b;b=t2}}"
"else{if(cur)cu=1;if(inv){f=BK;b=WF}}"
"for(var qy=0;qy<8;qy++){var rb=F[ch*8+(CM==0?((sy*8+qy)/M|0):qy)];"
"if(cu)rb^=XM;var o=((y*8+qy)*w2+x*8)*4;"
"for(var qx=0;qx<8;qx++){var on=CM==0?(rb>>(7-((sx*8+qx)/M|0)))&1:(rb>>(7-qx))&1;"
"var c=on?f:b;d[o+qx*4]=c[0];d[o+qx*4+1]=c[1];d[o+qx*4+2]=c[2];d[o+qx*4+3]=255}}}"
"g.putImageData(img,0,0)}"
"var rd=0,rt=0,ot=0,om=0,rx=0,lastsync=0,f1=0,m1=0,p1=0,r1=0,ec=0,exm='',txc=-1,stupd=0,t0=0,up=0;"
"function conn(){if(!t0)t0=Date.now();st.textContent='connecting...';st.style.color='#a66';"
"ws=new WebSocket('ws://'+location.host+'/');ws.binaryType='arraybuffer';"
"ws.onopen=function(){rd=0;ot=Date.now();om=0;st.textContent='connected';st.style.color='#6c6'};"
"ws.onclose=function(e){ws=null;rd++;var g=rt;st.textContent='retry... c='+(e&&e.code!==undefined?e.code:'?')+(e&&e.wasClean?' clean':'');"
"setTimeout(function(){if(g===rt)conn()},Math.min(8000,1500*rd))};"
"ws.onmessage=function(e){om=Date.now();rx++;try{var b=new Uint8Array(e.data),t=b[0];"
"if(t==1){f1++;F.set(b.subarray(5,5+(b[3]|(b[4]<<8))),b[1]|(b[2]<<8));dr=1}"
"else if(t==2){m1++;var rsz=CM!=b[1]||W!=b[2]||H!=b[3];CM=b[1];W=b[2];H=b[3];M=b[4];inv=b[5];XM=b[6];cx=b[7];cy=b[8];con=b[9];"
"if(rsz||!V)sz();dr=1}"
"else if(t==3){p1++;P=[];for(var i=0;i<16;i++)P.push([(b[i+1]>>5&7)*255/7|0,(b[i+1]>>2&7)*255/7|0,(b[i+1]&3)*255/3|0]);dr=1}"
"else if(t==4){r1++;var n=b[1],p=2;while(n--){var y=b[p++],w=b[p++];V.set(b.subarray(p,p+w),y*W);p+=w;A.set(b.subarray(p,p+w),y*W);p+=w}dr=1}"
"else if(t==5){txc=b[1]|(b[2]<<8);up=b[3]|(b[4]<<8)}"
"}catch(ex){ec++;exm=ex.message||'err';st.textContent='rx err: '+exm}}}"
"conn();setInterval(function(){if(dr&&V&&P){dr=0;draw()}"
"if(ws&&ws.readyState==1){"
"if(Date.now()-stupd>1000&&!pasting){stupd=Date.now();"
"st.textContent='connected t='+Math.floor((Date.now()-t0)/1000)+'s rx='+rx+' f='+f1+' m='+m1+' p='+p1+' r='+r1+' e='+ec+(exm?' '+exm:'')+(txc>=0?' tx='+txc:'')+(up>0?' up='+up:'')}"
"var need=(om===0&&Date.now()-ot>2000)||((!V||!P)&&rx>0&&Date.now()-lastsync>2500)"
"||(r1===0&&Date.now()-ot>3000&&Date.now()-lastsync>2500);"
"if(need&&Date.now()-lastsync>2500){lastsync=Date.now();st.textContent='resync...';"
"ws.send(new Uint8Array([255]))}}"
"},33);"
"var KM={Enter:[10],Backspace:[8],Tab:[9],Escape:[27],ArrowLeft:[28],ArrowRight:[29],"
"ArrowUp:[30],ArrowDown:[31],Home:[18],End:[23],PageUp:[19],PageDown:[20],Insert:[17],Delete:[127]};"
"var FK={F1:[19,12],F2:[24,76,79,65,68],F3:[24,83,65,86,69],F4:[24,76,73,83,84,10],"
"F5:[24,82,85,78,10],F6:[24,63,70,82,69,69,40,41,10],F7:[24,79,85,84,48,10],"
"F8:[24,86,73,68,69,79,49,10],F9:[24,70,73,76,69,83],F10:[24,83,87,73,84,67,72,10]};"
"function kp(c){if(ws&&ws.readyState==1)ws.send(new Uint8Array(c))}"
"function rec(){rt++;pasting=0;if(ws){ws.onclose=function(){};try{ws.close()}catch(e){};ws=null}rd=0;conn()}"
"document.addEventListener('keydown',function(e){if(pv.style.display=='block')return;"
"if(!ws||ws.readyState!=1)return;var k=FK[e.key]||KM[e.key];"
"if(!k&&e.key.length==1){var c=e.key.charCodeAt(0);"
"if(e.ctrlKey){if(c>96&&c<123)c-=96;else if(c>64&&c<91)c-=64;else if(c!=32)return;}k=[c]}"
"if(!k)return;e.preventDefault();kp(k)});"
"cv.addEventListener('touchend',function(e){e.preventDefault();kb.focus()});"
"kb.addEventListener('input',function(){var v=kb.value;kb.value='';"
"if(v.length==1)kp([v.charCodeAt(0)])});"
"var pv=document.getElementById('p'),ta=document.getElementById('t'),pb=document.getElementById('pb'),pasting=0;"
"function popen(){pv.style.display='block';ta.focus()}"
"function pclose(){pv.style.display='none';pasting=0;pb.textContent='送信'}"
"function psend(){if(pasting){pasting=0;pb.textContent='送信';return}"
"if(!ws||ws.readyState!=1){st.textContent='not connected';return}"
"var s=ta.value.split('\\r').join(''),raw=s.split('\\n'),L=[],skip=0,i,j,line,out;"
"for(i=0;i<raw.length;i++){line=raw[i].split('\\t').join(' ');out='';"
"for(j=0;j<line.length;j++){var c=line.charCodeAt(j);if(c>=32&&c<127)out+=line[j];else skip++}"
"if(out.length)L.push(out)}"
"if(!L.length){st.textContent='PASTE: no lines';return}"
"pasting=1;pb.textContent='停止';psloop(L,0,skip)}"
"function psloop(L,i,skip){"
"if(!pasting){pb.textContent='送信';return}"
"if(!ws||ws.readyState!=1){pasting=0;pb.textContent='送信';st.textContent='PASTE aborted';return}"
"if(i>=L.length){pasting=0;pb.textContent='送信';st.textContent='PASTE done'+(skip?' (skip '+skip+')':'');return}"
"ws.send(L[i]+'\\n');st.textContent='PASTE '+(i+1)+'/'+L.length+(skip?' skip:'+skip:'');"
"setTimeout(function(){psloop(L,i+1,skip)},120)}"
"document.addEventListener('paste',function(e){if(pv.style.display=='block')return;"
"var t=e.clipboardData?e.clipboardData.getData('text'):null;"
"if(t&&ws&&ws.readyState==1){ta.value=t;popen();e.preventDefault()}});"
"</script></body></html>\n";
#define WEB_PAGE_LEN ((int)(sizeof WEB_PAGE - 1))

//大文字小文字を無視してbuf内から文字列を探す (見つかった位置を返す、なければ-1)
static int web_strifind(const char* needle, int limit) {
    int nl = strlen(needle);
    for (int i = 0; i + nl <= limit; i++) {
        int j = 0;
        while (j < nl) {
            char a = web_rbuf[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
            j++;
        }
        if (j == nl) return i;
    }
    return -1;
}

//Sec-WebSocket-Keyの値を取り出す (24文字のbase64)
static int web_ws_key(char* key, int keylen) {
    int i = web_strifind("sec-websocket-key:", web_rlen);
    if (i < 0) return 0;
    i += 18;
    while (i < web_rlen && web_rbuf[i] == ' ') i++;
    int n = 0;
    while (i < web_rlen && web_rbuf[i] != '\r' && web_rbuf[i] != '\n' && n < keylen - 1) {
        key[n++] = web_rbuf[i++];
    }
    key[n] = 0;
    return n >= 20;
}

static void web_send_page_start(struct tcp_pcb* pcb) {
    char hdr[160]; // ヘッダ全体は約125バイト (Cache-Control追加)
    int n = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
        WEB_PAGE_LEN);
    web_page_off = 0;
    if (tcp_write(pcb, hdr, n, TCP_WRITE_FLAG_COPY) == ERR_OK) {
        u16_t avail = tcp_sndbuf(pcb);
        int c = WEB_PAGE_LEN < avail ? WEB_PAGE_LEN : avail;
        if (tcp_write(pcb, WEB_PAGE, c, 0) == ERR_OK) { //flash定数なので無コピー
            web_page_off = c;
        }
        tcp_output(pcb);
    }
    if (web_page_off >= WEB_PAGE_LEN) {
        web_page_off = -1;
        web_conn_closing = 1;
    }
}

//HTTPリクエスト処理 (ヘッドが揃ったら1回だけ呼ばれる)
static void web_http_process(struct tcp_pcb* pcb) {
    int end = -1;
    for (int i = 0; i + 3 < web_rlen; i++) {
        if (web_rbuf[i] == '\r' && web_rbuf[i + 1] == '\n' && web_rbuf[i + 2] == '\r' && web_rbuf[i + 3] == '\n') {
            end = i;
            break;
        }
    }
    if (end < 0) {
        if (web_rlen >= WEB_TCP_BUF) web_conn_close(pcb); //ヘッダが長すぎる
        return; //続きを受信待ち
    }
    char key[32];
    if (web_strifind("upgrade: websocket", end) >= 0 && web_ws_key(key, sizeof key)) {
        //WebSocketハンドシェイク (RFC6455)
        static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        char cat[64];
        snprintf(cat, sizeof cat, "%s%s", key, guid);
        uint8_t sha[20];
        web_sha1((const uint8_t*)cat, strlen(cat), sha);
        char acc[32];
        web_b64(sha, 20, acc);
        char rsp[160];
        int n = snprintf(rsp, sizeof rsp,
            "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n",
            acc);
        if (tcp_write(pcb, rsp, n, TCP_WRITE_FLAG_COPY) == ERR_OK) {
            tcp_output(pcb);
            web_conn_ws = 1;
            web_rlen = 0;
            iotpad_dbg_str("WEB: ws connected\n");
        }
        return;
    }
    if (web_strifind("GET / ", end) >= 0) { //大文字小文字無視の検索なので小文字GETも一致する
        web_send_page_start(pcb);
        web_rlen = 0; //同一ヘッダの再処理防止
        return;
    }
    //それ以外 (favicon除くため/favicon.ico等、captive portal検証の/connecttest.txtや/generate_204等) は
    //204 No Content — クライアントOSの「インターネット接続検証」が即完了し、AP接続直後の待ち時間を解消する
    static const char nf[] = "HTTP/1.1 204 No Content\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
    if (tcp_write(pcb, nf, sizeof nf - 1, TCP_WRITE_FLAG_COPY) == ERR_OK) {
        tcp_output(pcb);
        web_conn_closing = 1;
    } else {
        web_conn_close(pcb);
    }
    web_rlen = 0;
}

//WSフレームの解析 (rbufから完了フレームを順に処理する)
static void web_ws_parse(struct tcp_pcb* pcb) {
    int pos = 0;
    while (pos + 2 <= web_rlen) {
        int opcode = web_rbuf[pos] & 0x0F;
        int masked = web_rbuf[pos + 1] & 0x80;
        int len = web_rbuf[pos + 1] & 0x7F;
        int hl = 2;
        if (len == 126) {
            if (pos + 4 > web_rlen) break;
            len = (web_rbuf[pos + 2] << 8) | web_rbuf[pos + 3];
            hl = 4;
        } else if (len == 127) { //巨大フレームは対応しない
            web_conn_close(pcb);
            return;
        }
        if (masked) hl += 4;
        if (len > WEB_WS_PAYLOAD_MAX || pos + hl + len > web_rlen) {
            if (len > WEB_WS_PAYLOAD_MAX) {
                web_conn_close(pcb);
                return;
            }
            break; //フレーム未受信
        }
        uint8_t* pay = (uint8_t*)web_rbuf + pos + hl;
        if (masked) {
            uint8_t* mk = pay - 4;
            for (int i = 0; i < len; i++) {
                pay[i] ^= mk[i & 3];
            }
        }
        if (opcode == 0x8) { //close
            web_conn_close(pcb);
            return;
        } else if (opcode == 0x9) { //ping → pong
            memcpy(web_wsbuf + 4, pay, len);
            web_ws_tx(pcb, 0xA, len);
        } else if (opcode == 0x1) { //テキストフレーム = コードペースト (かな変換/ESC停止をバイパス)
            web_keys_in(pay, len, 1);
        } else if (opcode == 0x2 || opcode == 0x0) { //バイナリ/継続 = キー入力
            if (len >= 1 && pay[0] == 0xFF) {
                //再同期要求: 差分ベースを全リセットし、次のpush tickから
                //フォント/モード/パレット/全行をフル再送する (0xFFはキー入力では使わない)
                web_sent_valid = 0;
                web_sent_pal_valid = 0; //パレットも必ず再送する
                web_font_off = 0;
                web_client_cm = 0xFF;
                web_client_w = 0;
                web_client_h = 0;
                web_client_mag = 0;
                web_client_inv = 0;
                web_client_xm = 0;
                web_client_cx = 0;
                web_client_cy = 0;
                web_client_con = 0;
                iotpad_dbg_str("WEB: resync\n");
            } else {
                web_keys_in(pay, len, 0);
            }
        }
        pos += hl + len;
    }
    if (pos > 0) {
        memmove(web_rbuf, web_rbuf + pos, web_rlen - pos);
        web_rlen -= pos;
    }
}

//===== TCPコールバック =====
static err_t web_recv(void* arg, struct tcp_pcb* pcb, struct pbuf* p, err_t err) {
    (void)arg;
    if (pcb != web_conn_pcb) {
        if (p) pbuf_free(p);
        return ERR_OK;
    }
    if (p == NULL) { //相手から切断
        web_conn_close(pcb);
        return ERR_OK;
    }
    if (err != ERR_OK) {
        pbuf_free(p);
        return err;
    }
    tcp_recved(pcb, p->tot_len);
    for (struct pbuf* q = p; q; q = q->next) {
        if (web_rlen + q->len > WEB_TCP_BUF) { //バッファ溢れ (異常な大量送信)
            pbuf_free(p);
            web_conn_close(pcb);
            return ERR_OK;
        }
        memcpy(web_rbuf + web_rlen, q->payload, q->len);
        web_rlen += q->len;
    }
    pbuf_free(p);
    if (web_conn_ws) {
        web_ws_parse(pcb);
    } else {
        web_http_process(pcb);
    }
    return ERR_OK;
}

static err_t web_sent_cb(void* arg, struct tcp_pcb* pcb, u16_t len) {
    (void)arg; (void)len;
    if (pcb != web_conn_pcb) return ERR_OK;
    web_last_ack_ms = to_ms_since_boot(get_absolute_time());
    if (web_page_off >= 0) { //ページの続きを送る (TCP_SND_BUF分割)
        u16_t avail = tcp_sndbuf(pcb);
        int rem = WEB_PAGE_LEN - web_page_off;
        if (rem > 0 && avail > 0) {
            int c = rem < (int)avail ? rem : (int)avail;
            if (tcp_write(pcb, WEB_PAGE + web_page_off, c, 0) == ERR_OK) {
                web_page_off += c;
                tcp_output(pcb);
            }
        }
        if (web_page_off >= WEB_PAGE_LEN) {
            web_page_off = -1;
            web_conn_closing = 1;
        }
    }
    if (web_conn_closing && web_page_off < 0) {
        web_conn_close(pcb);
    }
    return ERR_OK;
}

static void web_err(void* arg, err_t err) {
    (void)arg; (void)err;
    iotpad_dbg_str("WEB: err\n");
    //tcp_errにはpcbが渡されないため、単一接続前提で現在の接続を破棄扱いにする
    //(pcbはlwIPが解放済みのためここでは触らない)
    web_conn_pcb = NULL;
    web_reset_conn_state();
}

static err_t web_poll(void* arg, struct tcp_pcb* pcb) {
    (void)arg;
    if (pcb != web_conn_pcb) return ERR_OK;
    if (web_conn_closing
        || (web_last_ack_ms && to_ms_since_boot(get_absolute_time()) - web_last_ack_ms > WEB_IDLE_TIMEOUT_MS)) {
        iotpad_dbg_str("WEB: poll abort\n");
        web_conn_pcb = NULL;
        web_reset_conn_state();
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    return ERR_OK;
}

static err_t web_accept(void* arg, struct tcp_pcb* pcb, err_t err) {
    (void)arg;
    if (err != ERR_OK || !pcb) return err;
    iotpad_dbg_str("WEB: accept\n");
    if (web_conn_pcb) {
        //既存接続がある場合は中断して新しい接続を優先する (最新接続優先)。
        //ブラウザの再読み込みでは旧WS接続のFIN処理より先に新しいページGETが届くため、
        //旧接続を503で拒否すると再読み込み後に画面が表示されなくなる
        iotpad_dbg_str("WEB: replace\n");
        struct tcp_pcb* old = web_conn_pcb;
        web_conn_pcb = NULL;
        web_reset_conn_state();
        tcp_abort(old);
    }
    web_conn_pcb = pcb;
    web_reset_conn_state();
    web_last_ack_ms = to_ms_since_boot(get_absolute_time());
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, web_recv);
    tcp_sent(pcb, web_sent_cb);
    tcp_err(pcb, web_err);
    tcp_poll(pcb, web_poll, 8); //4秒ごと: 送信詰まりの回収
    tcp_nagle_disable(pcb); //小フレーム (画面差分/キー) の即時送信 (Nagle/遅延ACKの送信滞留回避)
    return ERR_OK;
}

//===== AP制御 =====
//既定のAP SSID (Milkjam-xxxx: フラッシュID由来の4桁、複数台で衝突しない)
void iotpad_web_ap_default_ssid(char* buf, int len) {
    uint8_t id[8] = {0};
    flash_get_unique_id(id);
    snprintf(buf, len, "Milkjam-%02X%02X", id[6], id[7]);
}

//Web表示モード用のAP+Webサーバーを起動する (SWITCH 3の実体、起動時とvideo_switch_modeから呼ぶ)
//メインスレッドからのみ呼ぶこと (CYW43操作を含む)
void iotpad_web_ap_start() {
    if (!iotpad_wifi_started || web_server_running) return;
    iotpad_dbg_str("WEB: ap start\n");
    //STAを切断してAP単独へ (LWIP_SINGLE_NETIF=1のためAPとSTAは併用できない)
    if (iotpad_sntp_started) {
        cyw43_arch_lwip_begin();
        sntp_stop();
        cyw43_arch_lwip_end();
        iotpad_sntp_started = false;
    }
    iotpad_sntp_synced = false;
    iotpad_ntp_done = false;
    iotpad_wifi_want = false;
    cyw43_arch_disable_sta_mode();
    //SSID/パスワード (未設定なら既定値)
    char ssid[PW_CFG_SSID_LEN + 1];
    char pass[PW_CFG_PASS_LEN + 1];
    if (iotpad_ap_ssid[0]) {
        snprintf(ssid, sizeof ssid, "%s", iotpad_ap_ssid);
    } else {
        iotpad_web_ap_default_ssid(ssid, sizeof ssid);
    }
    if (iotpad_ap_pass[0]) {
        snprintf(pass, sizeof pass, "%s", iotpad_ap_pass);
    } else {
        snprintf(pass, sizeof pass, WEB_AP_DEFAULT_PASS);
    }
    cyw43_arch_enable_ap_mode(ssid, pass, CYW43_AUTH_WPA2_AES_PSK);
    iotpad_ap_active = true;
    //DHCPサーバー (udp 67) 開始
    cyw43_arch_lwip_begin();
    memset(web_dhcp_leases, 0, sizeof web_dhcp_leases);
    web_dhcp_pcb = udp_new();
    if (web_dhcp_pcb) {
        if (udp_bind(web_dhcp_pcb, IP_ADDR_ANY, 67) != ERR_OK) {
            udp_remove(web_dhcp_pcb);
            web_dhcp_pcb = NULL;
        } else {
            udp_recv(web_dhcp_pcb, web_dhcp_recv, NULL);
        }
    }
    //簡易DNSサーバー (udp 53) 開始
    web_dns_pcb = udp_new();
    if (web_dns_pcb) {
        if (udp_bind(web_dns_pcb, IP_ADDR_ANY, 53) != ERR_OK) {
            udp_remove(web_dns_pcb);
            web_dns_pcb = NULL;
        } else {
            udp_recv(web_dns_pcb, web_dns_recv, NULL);
        }
    }
    //Webサーバー (tcp 80) 開始
    struct tcp_pcb* l = tcp_new();
    if (l) {
        if (tcp_bind(l, IP_ADDR_ANY, WEB_PORT) != ERR_OK) {
            tcp_close(l);
            l = NULL;
        } else {
            web_listen_pcb = tcp_listen(l);
            tcp_accept(web_listen_pcb, web_accept);
        }
    }
    //画面転送周期タイマー開始 (lwIPのsys_timeout → RUN中も割込みで駆動)
    web_reset_conn_state();
    web_conn_pcb = NULL;
    web_push_armed = 1;
    web_last_push_ms = to_ms_since_boot(get_absolute_time());
    sys_timeout(WEB_PUSH_INTERVAL_MS, web_push_tick, NULL);
    cyw43_arch_lwip_end();
    web_server_running = web_dhcp_pcb != NULL && web_listen_pcb != NULL;
    char msg[80];
    snprintf(msg, sizeof msg, "WEB AP:%s http://192.168.4.1\n", ssid);
    put_str(msg);
    iotpad_dbg_str(msg);
}

//WebサーバーとAPを停止し、保存済みSSIDがあれば家のWiFiへ自動再接続する
//メインスレッドからのみ呼ぶこと (SWITCH 0-2への切替時にvideo_switch_modeから呼ぶ)
void iotpad_web_ap_stop() {
    if (!web_server_running) return;
    iotpad_dbg_str("WEB: ap stop\n");
    cyw43_arch_lwip_begin();
    sys_untimeout(web_push_tick, NULL); //タイマーリスト操作はロック内で行う (メインスレッドからの競合防止)
    web_push_armed = 0;
    if (web_conn_pcb) {
        tcp_abort(web_conn_pcb);
        web_conn_pcb = NULL;
    }
    if (web_listen_pcb) {
        tcp_close(web_listen_pcb);
        web_listen_pcb = NULL;
    }
    if (web_dhcp_pcb) {
        udp_remove(web_dhcp_pcb);
        web_dhcp_pcb = NULL;
    }
    if (web_dns_pcb) {
        udp_remove(web_dns_pcb);
        web_dns_pcb = NULL;
    }
    cyw43_arch_lwip_end();
    web_reset_conn_state();
    web_server_running = false;
    cyw43_arch_disable_ap_mode();
    iotpad_ap_active = false;
    put_str("WEB AP stopped\n");
    if (iotpad_ssid[0]) {
        iotpad_wifi_join_async(); //家のWiFiへ再接続 (非同期、ThingsBoard/NTPもiotpad_wifi_tickで再開)
    }
}

#endif
