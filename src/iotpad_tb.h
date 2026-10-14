//IchigoJam PW: IOTPAD ThingsBoard送信 (フェーズ5)
//ichigojam_iotpad_no15e のThingsBoard機能をPico W内部に移植したもの
//  - 送信先: http://15sat.jp:8080/api/v1/<TOKEN>/telemetry (pad準拠・固定)
//  - ペイロード: {"ts":<UNIXms>,"values":{...}} (ThingsBoard時系列API形式)
//  - WiFi未接続時はRAMキューに貯めて接続後にFLUSH (pad準拠、満杯時は最古を破棄)
//  - EJ "SENDTB"[,<秒>] で手動送信/自動送信間隔設定
//lwIP生APIでHTTPクライアント実装 (HTTPClient相当、NO_SYS環境)
//※メインスレッドからのlwIP呼び出しはcyw43_arch_lwip_begin/endでロック、
//  コールバック(lwIP IRQ内)からの呼び出しはロック不要 (pico-sdk仕様)
#ifndef __IOTPAD_TB_H__
#define __IOTPAD_TB_H__
#include "all_includes.h"
#include "lwip/tcp.h"
#include "lwip/dns.h"
#include "lwip/pbuf.h"

//===== 設定 (pad準拠) =====
#define TB_HOST           "15sat.jp"  // padのhttp.beginと同一
#define TB_PORT           8080
#define TB_TIMEOUT_MS     5000        // 接続+応答の上限 (pad: 接続2s+読取3s)
#define TB_DNS_TIMEOUT_MS 4000
#define TB_QUEUE_CAP      8           // RAMキュー容量
#define TB_PAYLOAD_MAX    256         // 1テレメトリの最大長
#define TB_REQ_MAX        480         // HTTPリクエスト最大長
#define TB_SEND_GAP_MS    100         // キュー一括送信の間隔 (pad: 1秒あたり最大10件)
#define TB_MAX_CONSEC_FAIL 3          // 連続失敗でフラッシュ中断 (pad準拠)

//===== 状態 =====
static char tb_queue[TB_QUEUE_CAP][TB_PAYLOAD_MAX]; // RAMオフラインキュー
static uint8_t tb_queue_count;
static uint32_t tb_auto_sec;      // 自動送信間隔 (秒、0=OFF、flash保存)
static uint32_t tb_last_send_ms;

//HTTPクライアント状態 (IRQ⇔メイン間共有はvolatile)
static struct tcp_pcb* tb_pcb;
static volatile bool tb_done;
static volatile int tb_http_code; // -1=未受信 0=失敗 200等
static volatile int tb_dns_state; // 0=待機 1=解決 -1=失敗
static ip_addr_t tb_ip;
static char tb_req[TB_REQ_MAX];  // connectedコールバック(lwIP IRQ)で使用

//ThingsBoard用UNIXミリ秒: NTP優先、未同期ならRTC (どちらもUTC)
static uint64_t iotpad_tb_unix_ms() {
    if (iotpad_sntp_synced) return iotpad_get_unix_ms();
    if (rtc_ok) {
        uint16_t y; uint8_t mo, d, h, mi, s;
        ds3231_read(&y, &mo, &d, &h, &mi, &s);
        return (uint64_t)iotpad_ymd_to_unix(y, mo, d, h, mi, s) * 1000ULL;
    }
    return 0;
}

//===== ペイロード組み立て (padのmake_payload/make_live_values_body準拠) =====
//use_set=true: SET*キャッシュ優先 (手動送信)、false: ライブ値 (自動送信)
static void tb_make_payload(char* out, int outlen, bool use_set) {
    char body[224];
    int n = 0;
    #define TB_V(...) do { if (n < (int)sizeof body - 1) n += snprintf(body + n, sizeof body - n, __VA_ARGS__); } while (0)
    TB_V("\"temperature\":%.1f", (use_set && tmp_kio[0]) ? atof(tmp_kio) : (cur_t1 + temp_offset));
    TB_V(",\"exttemp1\":%.1f", (use_set && tmp_ondo[0]) ? atof(tmp_ondo) : (cur_t2 + ext1_offset));
    TB_V(",\"exttemp2\":%.1f", (use_set && tmp_ondoext2[0]) ? atof(tmp_ondoext2) : (cur_t2b + ext2_offset));
    TB_V(",\"humidity\":%.1f", (use_set && tmp_hum[0]) ? atof(tmp_hum) : (cur_h + hum_offset));
    TB_V(",\"pressure\":%.2f", (use_set && tmp_pres[0]) ? atof(tmp_pres) : (cur_p + pres_offset));
    TB_V(",\"lux\":%d", (int)((use_set && tmp_lux[0]) ? atof(tmp_lux) : cur_l + 0.5f));
    TB_V(",\"lux2\":%.1f", (use_set && tmp_ltrlux[0]) ? atof(tmp_ltrlux) : cur_l2);
    TB_V(",\"uv_cps\":%lu", (use_set && tmp_ltruv_cps[0]) ? (unsigned long)atol(tmp_ltruv_cps) : (unsigned long)cur_uvcps);
    TB_V(",\"distance\":%.1f", (use_set && tmp_dist[0]) ? atof(tmp_dist) : cur_d);
    #undef TB_V
    snprintf(out, outlen, "{\"ts\":%llu,\"values\":{%s}}",
             (unsigned long long)iotpad_tb_unix_ms(), body);
}

//SET*キャッシュのクリア (手動送信後、padのclear_set_values準拠)
static void tb_clear_set_values() {
    tmp_kio[0] = tmp_ondo[0] = tmp_ondoext2[0] = tmp_hum[0] = tmp_pres[0] = 0;
    tmp_lux[0] = tmp_ltrlux[0] = tmp_ltruv[0] = tmp_ltruv_cps[0] = tmp_dist[0] = 0;
}

//===== lwIPコールバック (async context = lwIP IRQ内で実行、ロック不要) =====
static void tb_dns_found(const char* name, const ip_addr_t* ipaddr, void* arg) {
    (void)name; (void)arg;
    if (ipaddr != NULL) { tb_ip = *ipaddr; tb_dns_state = 1; }
    else tb_dns_state = -1;
}
static err_t tb_connected(void* arg, struct tcp_pcb* pcb, err_t err) {
    (void)arg;
    if (err != ERR_OK) {
        tb_http_code = 0;
        tb_pcb = NULL;
        tb_done = true;
        return ERR_OK;
    }
    err_t e = tcp_write(pcb, tb_req, (u16_t)strlen(tb_req), TCP_WRITE_FLAG_COPY);
    if (e == ERR_OK) {
        tcp_output(pcb);
    } else {
        tb_http_code = 0;
        tb_pcb = NULL;
        tb_done = true;
        tcp_abort(pcb);
    }
    return ERR_OK;
}
static err_t tb_recv(void* arg, struct tcp_pcb* pcb, struct pbuf* p, err_t err) {
    (void)arg; (void)err;
    if (p == NULL) {//相手が切断 (Connection: close)
        tb_pcb = NULL;
        tb_done = true;
        return ERR_OK;
    }
    if (tb_http_code < 0) {
        //ステータス行 "HTTP/1.1 200 OK" のコードを解析
        char head[24];
        u16_t n = p->len < 23 ? p->len : 23;
        pbuf_copy_partial(p, head, n, 0);
        head[n] = 0;
        int code = 0;
        for (char* q = head; *q; q++) {
            if (*q == ' ') { code = atoi(q + 1); break; }
        }
        tb_http_code = code;
    }
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    if (tb_http_code > 0) {
        tb_pcb = NULL;
        tb_done = true;
        if (tcp_close(pcb) != ERR_OK) tcp_abort(pcb);
    }
    return ERR_OK;
}
static void tb_err(void* arg, err_t err) {
    (void)arg; (void)err;
    if (tb_done) return;//完了後の遅延コールバック (自close/abort後のERR_ABRT等) は無視
    tb_pcb = NULL;
    tb_http_code = 0;
    tb_done = true;
}

//===== HTTP POST (メインスレッドから、応答コード or 0を返す) =====
static int tb_http_post(const char* payload) {
    if (!iotpad_token[0]) return 0;
    //--- DNS (lwIP内蔵キャッシュ、2回目以降は即時) ---
    tb_dns_state = 0;
    cyw43_arch_lwip_begin();
    err_t e = dns_gethostbyname(TB_HOST, &tb_ip, tb_dns_found, NULL);
    cyw43_arch_lwip_end();
    if (e == ERR_INPROGRESS) {
        uint64_t start = time_us_64();
        while (tb_dns_state == 0 && time_us_64() - start < (uint64_t)TB_DNS_TIMEOUT_MS * 1000) {
            iotpad_wifi_tick();//待機中もWiFi管理を継続 (padのservice_during_wait相当)
            sleep_ms(25);
        }
        if (tb_dns_state != 1) return 0;
    } else if (e != ERR_OK) return 0;
    //--- HTTPリクエスト組み立て ---
    int reqlen = snprintf(tb_req, sizeof tb_req,
        "POST /api/v1/%s/telemetry HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        iotpad_token, TB_HOST, (int)strlen(payload), payload);
    if (reqlen < 0 || reqlen >= (int)sizeof tb_req) return 0;
    //--- TCP接続+送受信 ---
    tb_done = false;
    tb_http_code = -1;
    cyw43_arch_lwip_begin();
    tb_pcb = tcp_new();
    if (!tb_pcb) { cyw43_arch_lwip_end(); return 0; }
    tcp_err(tb_pcb, tb_err);
    tcp_recv(tb_pcb, tb_recv);
    err_t ce = tcp_connect(tb_pcb, &tb_ip, TB_PORT, tb_connected);
    cyw43_arch_lwip_end();
    if (ce != ERR_OK) {
        cyw43_arch_lwip_begin();
        tcp_abort(tb_pcb);
        tb_pcb = NULL;
        cyw43_arch_lwip_end();
        return 0;
    }
    uint64_t start = time_us_64();
    while (!tb_done && time_us_64() - start < (uint64_t)TB_TIMEOUT_MS * 1000) {
        iotpad_wifi_tick();
        sleep_ms(25);
    }
    if (!tb_done) {//タイムアウト: 通信中断
        tb_done = true;
        cyw43_arch_lwip_begin();
        if (tb_pcb) { tcp_abort(tb_pcb); tb_pcb = NULL; }
        cyw43_arch_lwip_end();
    }
    return tb_http_code > 0 ? tb_http_code : 0;
}

//===== RAMオフラインキュー (padのqueue_push_ram/queue_flush_ram準拠) =====
static void tb_queue_push(const char* json) {
    if (tb_queue_count >= TB_QUEUE_CAP) {//満杯→最古を破棄してシフト (pad準拠)
        for (int i = 0; i < TB_QUEUE_CAP - 1; i++) strcpy(tb_queue[i], tb_queue[i + 1]);
        strcpy(tb_queue[TB_QUEUE_CAP - 1], json);
    } else {
        strcpy(tb_queue[tb_queue_count++], json);
    }
}
//キュー全件送信。成功分だけキューから消し、連続失敗3回で中断 (pad準拠)。送信成功数を返す
static int tb_queue_flush() {
    int sent = 0;
    int consec_fail = 0;
    int i = 0;
    while (i < tb_queue_count) {
        if (consec_fail >= TB_MAX_CONSEC_FAIL || !iotpad_wifi_is_connected()) break;
        int code = tb_http_post(tb_queue[i]);
        if (code == 200) {
            for (int j = i; j < tb_queue_count - 1; j++) strcpy(tb_queue[j], tb_queue[j + 1]);
            tb_queue_count--;
            sent++;
            consec_fail = 0;
        } else {
            i++;
            consec_fail++;
        }
        sleep_ms(TB_SEND_GAP_MS);
    }
    return sent;
}

//===== 自動送信 (padのservice_auto_send/execute_send_tb(silent,is_auto)準拠) =====
//ライブ値・サイレント・WiFi未接続時はキューへ
static void iotpad_tb_auto_send() {
    char payload[TB_PAYLOAD_MAX];
    tb_make_payload(payload, sizeof payload, false);
    if (!iotpad_wifi_is_connected()) {
        tb_queue_push(payload);
        return;
    }
    if (tb_queue_count > 0) tb_queue_flush();
    int code = tb_http_post(payload);
    iotpad_dbg_str(code == 200 ? "PW: TB auto OK\n" : "PW: TB auto err\n");
}
//mainループから呼ぶ (EJ "SENDTB",<秒> で設定した間隔で自動送信)
void iotpad_tb_tick() {
    if (!tb_auto_sec) return;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - tb_last_send_ms < tb_auto_sec * 1000UL) return;
    tb_last_send_ms = now;
    iotpad_tb_auto_send();
}

//flash設定の自動送信間隔を読み込む (ext_wifi.hのiotpad_load_settingsから呼ばれる)
void iotpad_tb_load_auto(uint16_t v) {
    tb_auto_sec = v;
    tb_last_send_ms = to_ms_since_boot(get_absolute_time());
}

#endif
