//IchigoJam PW: lwIP設定 (Pico W内蔵WiFi用)
//RAM節約チューニング (DVI framebuf 150KBとの共存のため、lwIP全体を~25KBに収める)
//※lwIPの各.cからincludeされる (src/がincludeパスに含まれているため)
#ifndef _LWIPOPTS_H
#define _LWIPOPTS_H

#include <stdint.h> // SNTP_SET_SYSTEM_TIMEのフック関数宣言のため

#define NO_SYS                      1
#define SYS_LIGHTWEIGHT_PROT         1
#define LWIP_ALTCP                  0

//※必須: CPUのアライメント。未定義だとlwIPデフォルトの1になり、
//  プール領域が非整列位置に置かれてCortex-M0+の非整列アクセスでハードフォルトする
#define MEM_ALIGNMENT               4

// プロトコル (必要最小構成)
#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define LWIP_ARP                    1
#define LWIP_ICMP                   1
#define LWIP_RAW                    0
#define LWIP_UDP                    1
#define LWIP_TCP                    1
#define LWIP_DHCP                   1
#define DHCP_DOES_ARP_CHECK         0
#define LWIP_DNS                    1

// ソケット/Netconnは不使用 (生APIのみ使用)
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0

// RAM節約 (デフォルトはMEM_SIZE等が大きい)
#define MEM_LIBC_MALLOC             0
#define MEM_SIZE                    (16 * 1024)
#define MEMP_NUM_TCP_PCB            4
#define MEMP_NUM_TCP_PCB_LISTEN     1
#define MEMP_NUM_TCP_SEG            24
#define MEMP_NUM_UDP_PCB            4
#define MEMP_NUM_PBUF               8
#define MEMP_NUM_NETBUF             0
//sys_timeout管理プール (同時タイムアウト数)
//※lwIP既定の計算式 (TCP+ARP+2*DHCP+DNS=5) はSNTPを数に入れないため、
//  SNTPタイマー(1-2件)+TCP接続時のTCPタイマー(1件)が加わると枯渇し
//  「pool MEMP_SYS_TIMEOUT is empty」パニック→ハードフォルトする (SENDTBで実機確認)
//  必要数: ARP(1)+DHCP(2)+DNS(1)+SNTP(2)+TCP(1)+余裕(1) = 8
#define MEMP_NUM_SYS_TIMEOUT        8
#define PBUF_POOL_SIZE              8
#define TCP_MSS                     536
#define TCP_WND                     (2 * TCP_MSS)
#define TCP_SND_BUF                 (2 * TCP_MSS)
#define TCP_SND_QUEUELEN            8
#define TCP_QUEUE_OOSEQ             0
#define TCP_LISTEN_BACKLOG           0
#define LWIP_SINGLE_NETIF           1

// SNTP (NTP時刻同期) — IOTPAD機能 (サーバはichigojam_iotpad_no15e準拠)
#define SNTP_SERVER_DNS             1
#define SNTP_UPDATE_DELAY           (60 * 60 * 1000)
#define SNTP_RETRY_TIMEOUT          3000
void iotpad_sntp_set_time(uint32_t sec); // ext_wifi.h で実装 (SNTP受信時刻を記録)
#define SNTP_SET_SYSTEM_TIME(sec)   iotpad_sntp_set_time((uint32_t)(sec))

// 統計・デバッグ無効
#define LWIP_STATS                  0
#define LWIP_DEBUG                  0

#endif
