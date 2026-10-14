//IchigoJam PW: IOTPAD「EJ」コマンドエンジン
//ichigojam_iotpad_no15e (XIAO ESP32C6版) のUARTコマンド「EJ 」を、
//BASICコマンド EJ "CMD"[,"ARG"] / 関数 EJ("CMD") として移植したもの
//フェーズ2: WiFi設定・接続・NTP時刻同期 (フェーズ3以降でセンサー/OLED/TB送信を追加)
#ifndef __EXT_IOTPAD_H__
#define __EXT_IOTPAD_H__
#include "all_includes.h"

#define IOTPAD_VERSION "1.0.0.00"
//応答バッファサイズ — HELP/GETCONF等の複数行応答のため512 (padのsend_response複数回相当)
//※command_ej/func_ejともstaticバッファ使用 (スタック使用量0)
#define IOTPADIOT_RESP_MAX 512

//トークン列から文字列リテラルを取得 (コピーしてNUL終端)
//※token_skipstr()の返り値は'"'までの生ポインタのためコピーが必要
//toupper_mode=true: 大文字化 (コマンド名 — 小文字で打っても一致させる)
//toupper_mode=false: そのまま (SSID/パスワード等の値 — CapsLockで入力した小文字を保持)
static void token_getstr_copy(char* buf, int len, bool toupper_mode) {
    char* s = token_skipstr();
    int i = 0;
    while (s[i] && s[i] != '"' && i < len - 1) {
        buf[i] = toupper_mode ? basic_toupper(s[i]) : s[i];
        i++;
    }
    buf[i] = 0;
}

//EJの引数取得: 引用符付き文字列、または引用符なしの英数字列を生テキストから読む
//(引用符忘れ対策: EJ "SETTOKEN",Abc123 / EJ "OLEDFONT",2 / EJ SYNCTIME 等。padのシリアルコマンド形式と同様に引用符を必須にしない)
//※引用符なし値の小文字はそのまま保持 (SETTOKENのトークン等)
//※token_back()はpcを引数先頭(lasttoken)へ戻すため、トークン化結果に関係なく元テキストを読み直せる
//accept_comma: true=値引数 (EJ OLEDCUR,10,20 の「10,20」等、カンマを含めて読む)
//               false=コマンド名 (カンマ=引数境界なので読み取り停止。EJ GETTEMP,A → cmd="GETTEMP")
static void token_get_ej_arg_raw(char* buf, int len, bool toupper_mode, bool accept_comma) {
    int i = 0;
    while (i < len - 1) {
        char c = pc[i];
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
            || c == '-' || c == '_' || c == '.' || c == '@' || (accept_comma && c == ',')) {
            buf[i] = toupper_mode ? basic_toupper(c) : c;
            i++;
        } else {
            break;
        }
    }
    while (i > 0 && buf[i - 1] == ',') i--;//末尾カンマは引数に含めない
    buf[i] = 0;
    pc += i;
}
static void token_get_ej_arg(char* buf, int len, bool toupper_mode, bool accept_comma) {
    Token t; Token_get(t);
    if (t.code == TOKEN_STRING) {
        token_getstr_copy(buf, len, toupper_mode);
        return;
    }
    token_back();//引数の先頭まで戻す (pc = lasttoken)
    token_get_ej_arg_raw(buf, len, toupper_mode, accept_comma);
}

//文字列をIOTPAD設定領域へ保存 (flash設定セクタ、NUL埋め)
static void iotpad_set_and_save(char* field, int fieldlen, const char* arg, int offset) {
    memset(field, 0, fieldlen + 1);
    int n = strlen(arg);
    if (n > fieldlen) n = fieldlen;
    memcpy(field, arg, n);
    iotpad_save_config(field, offset, fieldlen);
}

//16進文字列をデコードしてbufへ (小文字・記号を含むSSID/パスワード/トークンを
//大文字のみのIchigoJamキーボードから正確に入力するための経路)
//hexは2桁×Nバイト (例: "6D7950617373")。成功=true
static bool iotpad_hexdecode(char* buf, int buflen, const char* hex) {
    int n = 0;
    while (hex[n * 2] && hex[n * 2 + 1]) {
        if (n >= buflen - 1) return false;//長すぎ
        int v = 0;
        for (int k = 0; k < 2; k++) {
            char c = hex[n * 2 + k];
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else return false;//16進以外の文字
            v = (v << 4) | d;
        }
        buf[n] = (char)v;
        n++;
    }
    if (hex[n * 2]) return false;//奇数長
    buf[n] = 0;
    return true;
}

//オフセット5種をflash設定領域へ保存 (PW_CFG_OFFS_OFF、20バイト。padのsave_settings相当)
static void iotpad_save_offsets() {
    uint8_t buf[20];
    memcpy(buf + 0, &temp_offset, 4);
    memcpy(buf + 4, &hum_offset, 4);
    memcpy(buf + 8, &pres_offset, 4);
    memcpy(buf + 12, &ext1_offset, 4);
    memcpy(buf + 16, &ext2_offset, 4);
    iotpad_save_config(buf, PW_CFG_OFFS_OFF, 20);
}

//RTC(DS3231、UTC保存)からJSTの年月日時分秒を取得 (PAD.IN/GETCONF/GETTIME代入で使用)
//成功=true (rtc無し or 読み失敗はfalse=全0)
static bool iotpad_rtc_jst(uint16_t* y, uint8_t* mo, uint8_t* d, uint8_t* h, uint8_t* mi, uint8_t* s) {
    if (!rtc_ok) return false;
    uint16_t ry; uint8_t rmo, rd, rh, rmi, rs;
    ds3231_read(&ry, &rmo, &rd, &rh, &rmi, &rs);
    uint32_t jst = iotpad_ymd_to_unix(ry, rmo, rd, rh, rmi, rs) + 9 * 3600; // UTC→JST
    iotpad_unix_to_ymd(jst, y, mo, d, h, mi, s);
    return true;
}

//UNIX秒→"YYYY-MM-DDTHH:MM:SS" (JST +9h表示、padの表示形式)
static void iotpad_format_datetime_jst(char* buf, int len) {
    uint16_t y; uint8_t mo, d, h, mi, s;
    iotpad_unix_to_ymd(iotpad_get_unix_sec() + 9 * 3600, &y, &mo, &d, &h, &mi, &s); // JST
    snprintf(buf, len, "%04u-%02u-%02uT%02u:%02u:%02u", y, mo, d, h, mi, s);
}

//EJコマンド実行の共通エンジン
//cmd: 大文字化済みサブコマンド ("APC"等), arg: 第2引数 (無指定="")
//resp: 応答文字列 (呼び出し側で表示), num: 数値結果 (関数形式EJ()の戻り値), num_valid: 数値結果の有無
static void iotpad_ej_exec(const char* cmd, const char* arg, char* resp, int resp_len, int16* num, bool* num_valid) {
    *num_valid = false;
    *num = 0;
    #define EJ_RESP(s) do { snprintf(resp, resp_len, "%s", (s)); } while (0)

    if (strcmp(cmd, "VER") == 0) {
        EJ_RESP(IOTPAD_VERSION);
    } else if (strcmp(cmd, "GETSSID") == 0) {
        EJ_RESP(iotpad_ssid);
    } else if (strcmp(cmd, "GETPWD") == 0) {
        EJ_RESP(iotpad_pass);
    } else if (strcmp(cmd, "GETTOKEN") == 0) {
        EJ_RESP(iotpad_token);
    } else if (strcmp(cmd, "GETIP") == 0) {
        iotpad_wifi_get_ip(resp, resp_len);
    } else if (strcmp(cmd, "GETALL") == 0) {
        //全センサー値をJSON一括取得 (pad準拠。値はGET*と同一)
        snprintf(resp, resp_len,
            "{\"temperature\":%.1f,\"exttemp1\":%.1f,\"exttemp2\":%.1f,\"humidity\":%.1f,"
            "\"pressure\":%.2f,\"lux\":%d,\"lux2\":%.1f,\"uv\":%.1f,\"uv_cps\":%lu,\"distance\":%.1f}",
            tmp_kio[0] ? atof(tmp_kio) : cur_t1 + temp_offset,
            tmp_ondo[0] ? atof(tmp_ondo) : cur_t2 + ext1_offset,
            tmp_ondoext2[0] ? atof(tmp_ondoext2) : cur_t2b + ext2_offset,
            tmp_hum[0] ? atof(tmp_hum) : cur_h + hum_offset,
            tmp_pres[0] ? atof(tmp_pres) : cur_p + pres_offset,
            (int)((tmp_lux[0] ? atof(tmp_lux) : cur_l) + 0.5f),
            tmp_ltrlux[0] ? atof(tmp_ltrlux) : cur_l2,
            tmp_ltruv[0] ? atof(tmp_ltruv) : (float)cur_uv,
            (unsigned long)(tmp_ltruv_cps[0] ? (unsigned long)atol(tmp_ltruv_cps) : (unsigned long)cur_uvcps),
            tmp_dist[0] ? atof(tmp_dist) : cur_d);
    } else if (strcmp(cmd, "GETCONF") == 0) {
        //総合状態表示 (pad準拠の複数行応答)
        char ip[24];
        iotpad_wifi_get_ip(ip, sizeof ip);
        uint16_t y; uint8_t mo, d, h, mi, s;
        bool rtcv = iotpad_rtc_jst(&y, &mo, &d, &h, &mi, &s);
        char rtcstr[32];
        if (rtcv) snprintf(rtcstr, sizeof rtcstr, "%04u-%02u-%02uT%02u:%02u:%02u", y, mo, d, h, mi, s);
        else snprintf(rtcstr, sizeof rtcstr, "N/A");
        snprintf(resp, resp_len,
            "VER:%s\nSSID:%s\nTOKEN:%s\nIP:%s\nRTC:%s\nQ:%d/%d\nQStore:RAM\n"
            "Auto:%lus\nRecon:%dmin\nFont:%d=%s\nReset:%s\nLTR390:%s\n"
            "OfsTemp:%.1f\nOfsHum:%.1f\nOfsPres:%.1f\nOfsExtTemp1:%.1f\nOfsExtTemp2:%.1f",
            IOTPAD_VERSION, iotpad_ssid, iotpad_token, ip, rtcstr,
            tb_queue_count, TB_QUEUE_CAP,
            (unsigned long)tb_auto_sec, iotpad_reconnect_min,
            oled_font_index, oled_font_names[oled_font_index],
            watchdog_caused_reboot() ? "WDT" : "POR",
            ltr_ok ? "OK" : "NG",
            temp_offset, hum_offset, pres_offset, ext1_offset, ext2_offset);
    } else if (strcmp(cmd, "GETRESET") == 0) {
        //リセット原因 (簡易版: ウォッチドッグによる再起動か否か)
        snprintf(resp, resp_len, "Reset:%s", watchdog_caused_reboot() ? "WDT" : "POR");
    }
    //===== センサーGET* (フェーズ3) =====
    //※num = 代入形式 (EJ GETTEMP,A / EJ GETTEMP,[100]) と関数形式 EJ("GETTEMP") の戻り値
    //  基本形は0.1単位×10 (PAD.INと同一スケール)、"*2"形は従来通り×100
    else if (strcmp(cmd, "GETTEMP") == 0 || strcmp(cmd, "GETTEMP2") == 0) {
        bool x = cmd[7] == '2';
        float v = tmp_kio[0] ? atof(tmp_kio) : (cur_t1 + temp_offset);
        if (x) { snprintf(resp, resp_len, "%d", (int)(v * 100 + (v >= 0 ? 0.5 : -0.5))); *num = (int16)(v * 100); }
        else { snprintf(resp, resp_len, "%.1f", v); *num = (int16)(v * 10 + (v >= 0 ? 0.5 : -0.5)); }
        *num_valid = true;
    } else if (strcmp(cmd, "GETEXTTEMP1") == 0 || strcmp(cmd, "GETEXTTEMP12") == 0) {
        bool x = cmd[11] == '2';
        float v = tmp_ondo[0] ? atof(tmp_ondo) : (cur_t2 + ext1_offset);
        if (x) { snprintf(resp, resp_len, "%d", (int)(v * 100 + (v >= 0 ? 0.5 : -0.5))); *num = (int16)(v * 100); }
        else { snprintf(resp, resp_len, "%.1f", v); *num = (int16)(v * 10 + (v >= 0 ? 0.5 : -0.5)); }
        *num_valid = true;
    } else if (strcmp(cmd, "GETEXTTEMP2") == 0 || strcmp(cmd, "GETEXTTEMP22") == 0) {
        bool x = cmd[11] == '2';
        float v = tmp_ondoext2[0] ? atof(tmp_ondoext2) : (cur_t2b + ext2_offset);
        if (x) { snprintf(resp, resp_len, "%d", (int)(v * 100 + (v >= 0 ? 0.5 : -0.5))); *num = (int16)(v * 100); }
        else { snprintf(resp, resp_len, "%.1f", v); *num = (int16)(v * 10 + (v >= 0 ? 0.5 : -0.5)); }
        *num_valid = true;
    } else if (strcmp(cmd, "GETHUM") == 0 || strcmp(cmd, "GETHUM2") == 0) {
        bool x = cmd[6] == '2';
        float v = tmp_hum[0] ? atof(tmp_hum) : (cur_h + hum_offset);
        if (x) { snprintf(resp, resp_len, "%d", (int)(v * 100 + (v >= 0 ? 0.5 : -0.5))); *num = (int16)(v * 100); }
        else { snprintf(resp, resp_len, "%.1f", v); *num = (int16)(v * 10 + (v >= 0 ? 0.5 : -0.5)); }
        *num_valid = true;
    } else if (strcmp(cmd, "GETPRESS") == 0 || strcmp(cmd, "GETPRESS2") == 0) {
        bool x = cmd[8] == '2';
        float v = tmp_pres[0] ? atof(tmp_pres) : (cur_p + pres_offset);
        if (x) { snprintf(resp, resp_len, "%d", (int)(v * 100 + (v >= 0 ? 0.5 : -0.5))); *num = (int16)(v * 100); }
        else { snprintf(resp, resp_len, "%.2f", v); *num = (int16)(v * 10 + (v >= 0 ? 0.5 : -0.5)); } // 0.1hPa単位 (int16安全)
        *num_valid = true;
    } else if (strcmp(cmd, "GETLUX") == 0 || strcmp(cmd, "GETLUX2") == 0) {
        bool x = cmd[6] == '2';
        float v = tmp_lux[0] ? atof(tmp_lux) : cur_l;
        if (x) { snprintf(resp, resp_len, "%d", (int)(v * 100 + (v >= 0 ? 0.5 : -0.5))); *num = (int16)(v * 100); }
        else { snprintf(resp, resp_len, "%d", (int)(v + 0.5)); *num = (int16)(v + 0.5); } // lux整数値
        *num_valid = true;
    } else if (strcmp(cmd, "GETLTRLUX") == 0 || strcmp(cmd, "GETLTRLUX2") == 0) {
        bool x = cmd[9] == '2';
        float v = tmp_ltrlux[0] ? atof(tmp_ltrlux) : cur_l2;
        if (x) { snprintf(resp, resp_len, "%d", (int)(v * 100 + (v >= 0 ? 0.5 : -0.5))); *num = (int16)(v * 100); }
        else { snprintf(resp, resp_len, "%.1f", v); *num = (int16)(v * 10 + (v >= 0 ? 0.5 : -0.5)); }
        *num_valid = true;
    } else if (strcmp(cmd, "GETLTRUV") == 0 || strcmp(cmd, "GETLTRUV2") == 0) {
        bool x = cmd[8] == '2';
        float v = tmp_ltruv[0] ? atof(tmp_ltruv) : cur_uv;
        if (x) { snprintf(resp, resp_len, "%d", (int)(v * 100 + (v >= 0 ? 0.5 : -0.5))); *num = (int16)(v * 100); }
        else { snprintf(resp, resp_len, "%lu", (unsigned long)v); *num = (int16)(v * 10 + (v >= 0 ? 0.5 : -0.5)); }
        *num_valid = true;
    } else if (strcmp(cmd, "GETLTRUVCPS") == 0 || strcmp(cmd, "GETLTRUVCPS2") == 0) {
        bool x = cmd[11] == '2';
        unsigned long v = tmp_ltruv_cps[0] ? (unsigned long)atol(tmp_ltruv_cps) : cur_uvcps;
        if (x) { snprintf(resp, resp_len, "%lu", v * 100UL); *num = (int16)v; }
        else { snprintf(resp, resp_len, "%lu", v); *num = (int16)v; } // cps値
        *num_valid = true;
    } else if (strcmp(cmd, "GETDISTANCE") == 0 || strcmp(cmd, "GETDISTANCE2") == 0) {
        bool x = cmd[11] == '2';
        float v = tmp_dist[0] ? atof(tmp_dist) : cur_d;
        if (v <= 0) { snprintf(resp, resp_len, "0"); *num = 0; }
        else if (x) { snprintf(resp, resp_len, "%d", (int)(v * 100 + 0.5)); *num = (int16)(v * 100); }
        else { snprintf(resp, resp_len, "%.1f", v); *num = (int16)(v * 10 + 0.5); } // 0.1cm単位
        *num_valid = true;
    }
    //===== SET* (値のオーバーライド) =====
    else if (strcmp(cmd, "SETTEMP") == 0) { strncpy(tmp_kio, arg, 15); tmp_kio[15] = 0; EJ_RESP("TEMP Set");
    } else if (strcmp(cmd, "SETEXTTEMP1") == 0) { strncpy(tmp_ondo, arg, 15); tmp_ondo[15] = 0; EJ_RESP("EXTTEMP1 Set");
    } else if (strcmp(cmd, "SETEXTTEMP2") == 0) { strncpy(tmp_ondoext2, arg, 15); tmp_ondoext2[15] = 0; EJ_RESP("EXTTEMP2 Set");
    } else if (strcmp(cmd, "SETHUM") == 0) { strncpy(tmp_hum, arg, 15); tmp_hum[15] = 0; EJ_RESP("HUM Set");
    } else if (strcmp(cmd, "SETPRESS") == 0) { strncpy(tmp_pres, arg, 15); tmp_pres[15] = 0; EJ_RESP("PRESS Set");
    } else if (strcmp(cmd, "SETLUX") == 0) { strncpy(tmp_lux, arg, 15); tmp_lux[15] = 0; EJ_RESP("LUX Set");
    } else if (strcmp(cmd, "SETLTRLUX") == 0) { strncpy(tmp_ltrlux, arg, 15); tmp_ltrlux[15] = 0; EJ_RESP("LTRLUX Set");
    } else if (strcmp(cmd, "SETLTRUV") == 0) { strncpy(tmp_ltruv, arg, 15); tmp_ltruv[15] = 0; EJ_RESP("LTRUV Set");
    } else if (strcmp(cmd, "SETLTRUVCPS") == 0) { strncpy(tmp_ltruv_cps, arg, 15); tmp_ltruv_cps[15] = 0; EJ_RESP("LTRUVCPS Set");
    } else if (strcmp(cmd, "SETDISTANCE") == 0) { strncpy(tmp_dist, arg, 15); tmp_dist[15] = 0; EJ_RESP("DIST Set");
    }
    //===== OFFSET* =====
    else if (strcmp(cmd, "OFFSETTEMP") == 0) { temp_offset = atof(arg); iotpad_save_offsets(); EJ_RESP("TmpOfs Set");
    } else if (strcmp(cmd, "OFFSETHUM") == 0) { hum_offset = atof(arg); iotpad_save_offsets(); EJ_RESP("HumOfs Set");
    } else if (strcmp(cmd, "OFFSETPRES") == 0) { pres_offset = atof(arg); iotpad_save_offsets(); EJ_RESP("PreOfs Set");
    } else if (strcmp(cmd, "OFFSETEXTTEMP1") == 0) { ext1_offset = atof(arg); iotpad_save_offsets(); EJ_RESP("Ext1Ofs Set");
    } else if (strcmp(cmd, "OFFSETEXTTEMP2") == 0) { ext2_offset = atof(arg); iotpad_save_offsets(); EJ_RESP("Ext2Ofs Set");
    } else if (strcmp(cmd, "OFFSETCLEAR") == 0) {
        temp_offset = hum_offset = pres_offset = ext1_offset = ext2_offset = 0;
        iotpad_save_offsets();
        EJ_RESP("All Offsets Cleared");
    } else if (strcmp(cmd, "GETOFFSET") == 0) {
        snprintf(resp, resp_len, "OfsTemp:%.1f\nOfsHum:%.1f\nOfsPres:%.1f\nOfsExtTemp1:%.1f\nOfsExtTemp2:%.1f",
                 temp_offset, hum_offset, pres_offset, ext1_offset, ext2_offset);
    } else if (strcmp(cmd, "CALIBEXTTEMP") == 0) {
        //EXTTEMP1を基準に本体温度(TEMP)とEXTTEMP2を自動較正 (padのdo_calib_ext_temp準拠)
        if (ds18b20_count == 0) {
            EJ_RESP("No EXTTEMP sensor connected");
        } else {
            float target = cur_t2 + ext1_offset; // EXTTEMP1を基準にする
            temp_offset = target - cur_t1;
            int n = snprintf(resp, resp_len, "TmpOfs:%.1f", temp_offset);
            if (ds18b20_count > 1) {
                ext2_offset = target - cur_t2b;
                snprintf(resp + n, resp_len - n, " Ext2Ofs:%.1f", ext2_offset);
            } else {
                snprintf(resp + n, resp_len - n, " Ext2:skip(not connected)");
            }
            iotpad_save_offsets();
        }
    }
    //===== SET* 補助 (値のオーバーライド) =====
    else if (strcmp(cmd, "SETALL") == 0) {
        //現在値をSET*キャッシュへ一括保存 (pad準拠: 以降の送信はこの値で固定)
        snprintf(tmp_kio, 16, "%.1f", cur_t1 + temp_offset);
        snprintf(tmp_ondo, 16, "%.1f", cur_t2 + ext1_offset);
        snprintf(tmp_ondoext2, 16, "%.1f", cur_t2b + ext2_offset);
        snprintf(tmp_hum, 16, "%.1f", cur_h + hum_offset);
        snprintf(tmp_pres, 16, "%.2f", cur_p + pres_offset);
        snprintf(tmp_lux, 16, "%d", (int)(cur_l + 0.5f));
        snprintf(tmp_ltrlux, 16, "%.1f", cur_l2);
        snprintf(tmp_ltruv, 16, "%lu", (unsigned long)cur_uv);
        snprintf(tmp_ltruv_cps, 16, "%lu", (unsigned long)cur_uvcps);
        snprintf(tmp_dist, 16, "%.1f", cur_d);
        EJ_RESP("All Cached");
    } else if (strcmp(cmd, "SETCLEAR") == 0) {
        tb_clear_set_values();
        EJ_RESP("SET Cleared");
    }
    //===== RTC =====
    //RTCはUTC保存 (pad規約)。表示はJST (+9h) — padのget_datetime_str準拠
    else if (strcmp(cmd, "GETTIME") == 0) {
        if (rtc_ok) {
            uint16_t y; uint8_t mo, d, h, mi, s;
            ds3231_read(&y, &mo, &d, &h, &mi, &s);
            uint32_t jst = iotpad_ymd_to_unix(y, mo, d, h, mi, s) + 9 * 3600;
            iotpad_unix_to_ymd(jst, &y, &mo, &d, &h, &mi, &s);
            snprintf(resp, resp_len, "%04u-%02u-%02uT%02u:%02u:%02u", y, mo, d, h, mi, s);
        } else {
            EJ_RESP("N/A");
        }
    } else if (strcmp(cmd, "SETTIME") == 0) {
        //形式: YYYY-MM-DDTHH:MM:SS (JST) — RTCへはUTC (-9h) で保存 (pad準拠)
        int y, mo, d, h, mi, s;
        if (sscanf(arg, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) == 6 && rtc_ok) {
            uint32_t utc = iotpad_ymd_to_unix((uint16_t)y, (uint8_t)mo, (uint8_t)d,
                                              (uint8_t)h, (uint8_t)mi, (uint8_t)s) - 9 * 3600;
            uint16_t uy; uint8_t umo, ud, uh, umi, us;
            iotpad_unix_to_ymd(utc, &uy, &umo, &ud, &uh, &umi, &us);
            ds3231_write(uy, umo, ud, uh, umi, us);
            //pad準拠: 書込後のRTCを読み戻してJST表示で応答 ("Time Set(JST):...")
            uint16_t ry; uint8_t rmo, rd, rh, rmi, rs;
            ds3231_read(&ry, &rmo, &rd, &rh, &rmi, &rs);
            uint32_t jst = iotpad_ymd_to_unix(ry, rmo, rd, rh, rmi, rs) + 9 * 3600;
            iotpad_unix_to_ymd(jst, &ry, &rmo, &rd, &rh, &rmi, &rs);
            snprintf(resp, resp_len, "Time Set(JST):%04u-%02u-%02uT%02u:%02u:%02u",
                     ry, rmo, rd, rh, rmi, rs);
        } else {
            EJ_RESP(rtc_ok ? "Format Err" : "RTC N/A");
        }
    }
    //===== DS18B20 ON/OFF (GP27=IN1をDS18B20 OneWireと共有) =====
    else if (strcmp(cmd, "DS18B20") == 0) {
        if (*arg == 0) {
            //状態表示
            snprintf(resp, resp_len, "DS18B20:%d", ds18b20_enabled ? 1 : 0);
        } else {
            int v = atoi(arg);
            bool en = (v > 0);
            if (en == ds18b20_enabled) {
                snprintf(resp, resp_len, "DS18B20:%d (no change)", ds18b20_enabled ? 1 : 0);
            } else {
                ds18b20_enabled = en;
                //flashに保存
                uint8_t flag = en ? 1 : 0;
                iotpad_save_config(&flag, PW_CFG_DS18B20_OFF, 1);
                if (en) {
                    //GP27をOneWireに切り替え
                    ds18b20_count = 0;
                    gpio_init(DS18B20_PIN);
                    gpio_pull_up(DS18B20_PIN);
                    if (ow_reset()) {
                        ds18b20_count = 1;
                        ds18b20_request_convert();
                        EJ_RESP("DS18B20:1 ON");
                    } else {
                        ds18b20_count = 0;
                        EJ_RESP("DS18B20:1 (no device)");
                    }
                } else {
                    //GP27をIN1(プルアップ入力)に戻す
                    ds18b20_count = 0;
                    ds18b20_requested = false;
                    gpio_init(DS18B20_PIN);
                    gpio_pull_up(DS18B20_PIN);
                    EJ_RESP("DS18B20:0 OFF");
                }
            }
        }
        *num = ds18b20_enabled ? 1 : 0;
        *num_valid = true;
    }
    //===== OLED制御 (フェーズ4) =====
    else if (strcmp(cmd, "OLEDON") == 0) {
        if (oled_ok) {
            oled_direct = true;
            oled_cursor_x = 0;
            oled_cursor_y = 0;
            oled_buf_clear();
            oled_buf_send();
            EJ_RESP("OLED Direct ON");
        } else { EJ_RESP("OLED Not Found"); }
    } else if (strcmp(cmd, "OLEDOFF") == 0) {
        oled_direct = false;
        oled_update_display();
        EJ_RESP("OLED Direct OFF");
    } else if (strcmp(cmd, "OLEDCLEAR") == 0) {
        if (oled_ok) {
            oled_buf_clear();
            oled_buf_send();
            oled_cursor_x = oled_cursor_y = 0;
            EJ_RESP("OLED Cleared");
        } else { EJ_RESP("OLED Not Found"); }
    } else if (strcmp(cmd, "OLEDCUR") == 0) {
        if (oled_ok) {
            int x = 0, y = 0;
            if (sscanf(arg, "%d,%d", &x, &y) == 2) {
                oled_cursor_x = x;
                oled_cursor_y = y;
                snprintf(resp, resp_len, "CUR:%d,%d", oled_cursor_x, oled_cursor_y);
            } else {
                EJ_RESP("Usage: EJ OLEDCUR x,y");
            }
        } else { EJ_RESP("OLED Not Found"); }
    } else if (strcmp(cmd, "OLEDFONT") == 0 || strcmp(cmd, "OLEDSIZE") == 0) {
        if (*arg == 0) {
            snprintf(resp, resp_len, "Font:%d=%s", oled_font_index, oled_font_names[oled_font_index]);
        } else {
            int idx = atoi(arg);
            if (idx >= 0 && idx < 5) {
                oled_font_index = idx;
                uint8_t f = (uint8_t)idx;
                iotpad_save_config(&f, PW_CFG_FONT_OFF, 1);//pad準拠: フォント番号を保存
                oled_update_display();//即座に再描画
                snprintf(resp, resp_len, "Font:%d=%s (saved)", idx, oled_font_names[idx]);
            } else {
                EJ_RESP("Usage: EJ OLEDFONT 0-4");
            }
        }
    } else if (strcmp(cmd, "GETFONT") == 0) {
        snprintf(resp, resp_len, "Font:%d=%s", oled_font_index, oled_font_names[oled_font_index]);
    } else if (strcmp(cmd, "OLEDPRINT") == 0 || strcmp(cmd, "OLED") == 0) {
        if (oled_ok && oled_direct) {
            //pad準拠: 実測の描画幅でカーソルを進める
            oled_cursor_x = oled_draw_str(oled_cursor_x, oled_cursor_y, arg);
            oled_buf_send();
            EJ_RESP("OK");
        } else { EJ_RESP("OLED Direct OFF"); }
    } else if (strcmp(cmd, "OLEDPAGE") == 0 || strcmp(cmd, "NEXTSCR") == 0) {
        if (oled_ok) {
            if (*arg == 0) {
                //次の画面へ
                oled_current_screen = (oled_current_screen % 5) + 1;
            } else {
                int p = atoi(arg);
                if (p >= 1 && p <= 5) oled_current_screen = p;
            }
            snprintf(resp, resp_len, "Page:%d", oled_current_screen);
            oled_update_display();
            *num = oled_current_screen;
            *num_valid = true;
        } else { EJ_RESP("OLED Not Found"); }
    }
    //===== SETSSID (以降WiFi設定) =====
    else if (strcmp(cmd, "SETSSID") == 0) {
        iotpad_set_and_save(iotpad_ssid, PW_CFG_SSID_LEN, arg, PW_CFG_SSID_OFF);
        EJ_RESP("SSID Saved");
    } else if (strcmp(cmd, "SETPWD") == 0) {
        iotpad_set_and_save(iotpad_pass, PW_CFG_PASS_LEN, arg, PW_CFG_PASS_OFF);
        EJ_RESP("PWD Saved");
    } else if (strcmp(cmd, "SETTOKEN") == 0) {
        iotpad_set_and_save(iotpad_token, PW_CFG_TOKEN_LEN, arg, PW_CFG_TOKEN_OFF);
        EJ_RESP("Token Saved");
    } else if (strcmp(cmd, "SETSSIDHEX") == 0) {
        char dec[PW_CFG_SSID_LEN + 1];
        if (!iotpad_hexdecode(dec, sizeof dec, arg)) {
            EJ_RESP("HEX Error");
            return;
        }
        iotpad_set_and_save(iotpad_ssid, PW_CFG_SSID_LEN, dec, PW_CFG_SSID_OFF);
        EJ_RESP("SSID Saved");
    } else if (strcmp(cmd, "SETPWDHEX") == 0) {
        char dec[PW_CFG_PASS_LEN + 1];
        if (!iotpad_hexdecode(dec, sizeof dec, arg)) {
            EJ_RESP("HEX Error");
            return;
        }
        iotpad_set_and_save(iotpad_pass, PW_CFG_PASS_LEN, dec, PW_CFG_PASS_OFF);
        EJ_RESP("PWD Saved");
    } else if (strcmp(cmd, "SETTOKENHEX") == 0) {
        char dec[PW_CFG_TOKEN_LEN + 1];
        if (!iotpad_hexdecode(dec, sizeof dec, arg)) {
            EJ_RESP("HEX Error");
            return;
        }
        iotpad_set_and_save(iotpad_token, PW_CFG_TOKEN_LEN, dec, PW_CFG_TOKEN_OFF);
        EJ_RESP("Token Saved");
    } else if (strcmp(cmd, "APS") == 0) {
        int st = iotpad_wifi_is_connected() ? 1 : 0;
        snprintf(resp, resp_len, "%d", st);
        *num = st;
        *num_valid = true;
    } else if (strcmp(cmd, "APC") == 0) {
        if (!iotpad_ssid[0]) {
            EJ_RESP("SSID Not Set");
        } else if (iotpad_wifi_connect_blocking(PW_WIFI_CONNECT_TIMEOUT_MS)) {
            char ip[24];
            iotpad_wifi_get_ip(ip, sizeof ip);
            snprintf(resp, resp_len, "Connected:%s", ip);
            *num = 1;
        } else {
            //リンク状態コード付きで失敗理由を表示 (-1=FAIL -2=SSID未発見 -3=認証失敗 1/2=接続済みだがIP未取得)
            snprintf(resp, resp_len, "Connect Failed:%d", iotpad_wifi_link_status());
            *num = 0;
        }
        *num_valid = true;
    } else if (strcmp(cmd, "APD") == 0) {
        iotpad_wifi_disconnect();
        EJ_RESP("WiFi Disconnected");
    } else if (strcmp(cmd, "RECONNECT") == 0) {
        if (*arg == 0) {
            snprintf(resp, resp_len, "Recon:%dmin", iotpad_reconnect_min);
        } else {
            int v = atoi(arg);
            iotpad_reconnect_min = v > 0 ? v : 0;
            uint8_t r = (uint8_t)iotpad_reconnect_min;
            iotpad_save_config(&r, PW_CFG_RECON_OFF, 1);
            if (r) {
                snprintf(resp, resp_len, "Recon:%dmin (saved)", iotpad_reconnect_min);
            } else {
                EJ_RESP("Recon:OFF (saved)");
            }
        }
        *num = iotpad_reconnect_min;
        *num_valid = true;
    }
    //===== ThingsBoard送信 (フェーズ5、pad準拠) =====
    else if (strcmp(cmd, "SENDTB") == 0) {
        if (*arg == 0) {
            //手動送信: SET*キャッシュ優先 (padのexecute_send_tb(silent=false, is_auto=false)準拠)
            if (!iotpad_token[0]) {
                EJ_RESP("No Token");
            } else {
                char payload[TB_PAYLOAD_MAX];
                tb_make_payload(payload, sizeof payload, true);
                if (!iotpad_wifi_is_connected()) {
                    //WiFi未接続 → 取得時刻付きJSONをキューへ (pad準拠)
                    tb_queue_push(payload);
                    tb_clear_set_values();
                    snprintf(resp, resp_len, "Queued(%d)", tb_queue_count);
                } else {
                    int flushed = tb_queue_count > 0 ? tb_queue_flush() : 0;
                    int code = tb_http_post(payload);
                    tb_clear_set_values();
                    if (flushed > 0) snprintf(resp, resp_len, "TB Code:%d Flush:%d", code, flushed);
                    else snprintf(resp, resp_len, "TB Code:%d", code);
                    *num = code;
                }
            }
        } else {
            //自動送信間隔設定してflashに保存 (pad準拠)
            long v = atol(arg);
            tb_auto_sec = v > 0 ? (uint32_t)v : 0;
            tb_last_send_ms = to_ms_since_boot(get_absolute_time());
            uint8_t cfg[2] = { (uint8_t)(tb_auto_sec & 0xFF), (uint8_t)(tb_auto_sec >> 8) };
            iotpad_save_config(cfg, PW_CFG_TB_AUTO_OFF, 2);
            if (tb_auto_sec > 0) {
                snprintf(resp, resp_len, "Auto:%lus (saved)", (unsigned long)tb_auto_sec);
            } else {
                EJ_RESP("Auto:OFF (saved)");
            }
            *num = (int16)tb_auto_sec;
        }
        *num_valid = true;
    } else if (strcmp(cmd, "GETQUEUE") == 0) {
        snprintf(resp, resp_len, "Queue:%d/%d (RAM)", tb_queue_count, TB_QUEUE_CAP);
        *num = tb_queue_count;
        *num_valid = true;
    } else if (strcmp(cmd, "FLUSHQUEUE") == 0) {
        if (iotpad_wifi_is_connected()) {
            int sent = tb_queue_flush();
            snprintf(resp, resp_len, "Flushed:%d", sent);
            *num = sent;
        } else {
            EJ_RESP("No WiFi");
            *num = 0;
        }
        *num_valid = true;
    } else if (strcmp(cmd, "CLEARQUEUE") == 0) {
        tb_queue_count = 0;
        EJ_RESP("Queue Cleared");
    } else if (strcmp(cmd, "SYNCTIME") == 0) {
        if (!iotpad_wifi_is_connected()) {
            EJ_RESP("No WiFi");
        } else {
            iotpad_dbg_str("PW: SYNCTIME\n");//診断
            //冪等なSNTP開始: 起動時同期が済んでいればsntp_startは何もせず、
            //iotpad_sntp_synced=trueのままなので即座に"Synced"を返す (padのUXと同一)
            iotpad_sntp_start();
            // 5秒の協調待機 (pad準拠) — 待機中もNTSC表示・ウォッチドッグ・WiFi管理は生きている
            uint64_t start = time_us_64();
            int last_sec = -1;
            while (!iotpad_sntp_synced && time_us_64() - start < (uint64_t)PW_NTP_WAIT_MS * 1000) {
                iotpad_wifi_tick();//padのservice_during_wait相当 (再接続・LED管理を継続)
                sleep_ms(50);
                int sec = (int)((time_us_64() - start) / 1000000);
                if (sec != last_sec) {
                    last_sec = sec;
                    iotpad_dbg_str("PW: SYNCTIME wait\n");//診断: 待機中であることを1秒ごとに出力
                }
            }
            if (iotpad_sntp_synced) {
                iotpad_dbg_str("PW: SYNCTIME ok\n");//診断
                //NTP時刻(UTC)をDS3231へ書込 (padのsync_rtc_from_ntp準拠)
                if (rtc_ok) {
                    uint16_t y; uint8_t mo, d, h, mi, s;
                    iotpad_unix_to_ymd(iotpad_get_unix_sec(), &y, &mo, &d, &h, &mi, &s);
                    ds3231_write(y, mo, d, h, mi, s);
                    iotpad_dbg_str("PW: RTC set\n");//診断
                }
                iotpad_format_datetime_jst(resp, resp_len);
                *num = 1;
            } else {
                iotpad_dbg_str("PW: SYNCTIME fail\n");//診断
                EJ_RESP("Sync Failed");
                *num = 0;
            }
            *num_valid = true;
        }
    }
    //===== キュー保存先 (互換シム) =====
    else if (strcmp(cmd, "QUEUESTORE") == 0) {
        EJ_RESP("RAM (fixed)");//PWはRAMキュー固定 (LittleFS非搭載)
    }
    //===== HELP (全コマンド一覧) =====
    //※IchigoJamフォントは1バイト文字のみ → 純ASCII。511バイト以内 (respサイズ)
    else if (strcmp(cmd, "HELP") == 0) {
        snprintf(resp, resp_len,
            "=== IchigoJam PW EJ CMD[,arg] ===\n"
            "GET> GETTEMP/EXT1/EXT2/HUM[2]\n"
            "     PRESS/LUX/LTRLUX/LTRUV[2]\n"
            "     CPS/DIST (2=x100)\n"
            "     GETALL GETTIME GETOFFSET\n"
            "     GETRESET GETCONF\n"
            "IN>  EJ GETTEMP,A / GETTEMP,[n]\n"
            "     EJ GETTIME,[n]=YMD hms\n"
            "     PAD.IN [n]=16 vals (x10)\n"
            "SET> SET*10 SETALL SETCLEAR\n"
            "OFS> OFFSET* CLEAR CALIBEXTTEMP\n"
            "WiFi> SETSSID/PWD/TOKEN(+HEX)\n"
            "     APC APD APS RECONNECT\n"
            "     SYNCTIME SETTIME\n"
            "TB>  SENDTB[,sec] (auto)\n"
            "Q>   GETQUEUE FLUSHQUEUE CLEAR\n"
            "SYS> VER GETTOKEN OLEDFONT\n"
            "     DS18B20 QUEUESTORE(RAM)");
    } else {
        snprintf(resp, resp_len, "Unknown:%s", cmd);
        return;
    }
    #undef EJ_RESP
}

//EJ "CMD"[,"ARG"] — コマンド形式
//引数なし/文字列引数 → 応答を画面に表示 (従来動作)
//カンマ後に変数/配列 → 代入形式: コマンドの数値結果を代入し画面出力なし (例: EJ GETTEMP,A / EJ GETTEMP,[100])
//※EJ GETTIME,[n]は配列6スロット (年月日時分秒、JST) に代入 (PAD.INと同一形式)
S_INLINE void command_ej() {
    //スタックオーバーフロー防止: 大容量バッファはstatic化 (SYNCTIME待機中のtimer割り込みとのスタック競合対策)
    static char cmd[48];
    //コマンド名: 引用符付き/引用符なし両対応 (引用符なしは大文字化して読む。カンマで停止)
    token_get_ej_arg(cmd, sizeof cmd, true, false);
    if (!cmd[0]) {
        command_error(ERR_SYNTAX_ERROR);
        return;
    }
    static char arg[PW_CFG_PASS_LEN * 2 + 16];
    arg[0] = 0;
    bool has_target = false;
    bool target_is_array = false;
    int target_idx = 0;//VAR: var[]インデックス(102-127) / ARRAY: 配列インデックス
    Token t; Token_get(t);
    if (t.code == TOKEN_COMMA) {
        Token_get(t);
        //代入形式の対象コマンド判定 (数値結果を返す系のみ。
        //  SETTOKEN/SETSSID等の値引数コマンドが代入と誤認されないための防御)
        bool value_cmd = strncmp(cmd, "GET", 3) == 0
            || strcmp(cmd, "SYNCTIME") == 0
            || strcmp(cmd, "SENDTB") == 0
            || strcmp(cmd, "OLEDPAGE") == 0
            || strcmp(cmd, "NEXTSCR") == 0
            || strcmp(cmd, "RECONNECT") == 0
            || strcmp(cmd, "FLUSHQUEUE") == 0
            || strcmp(cmd, "DS18B20") == 0;
        if (value_cmd && t.code == TOKEN_VAR) {
            //変数代入: 「1文字で行末」のみ代入先とする (EJ GETTEMP,A)
            //※それ以外 (EJ SETTOKEN,G8vPq... 等) はraw文字列引数として読み直す
            char* arg_start = lasttoken;
            Token t2; Token_get(t2);
            if (t2.code == TOKEN_NULL || t2.code == TOKEN_ELSE) {
                token_back();//先読みした行末トークンを戻す
                has_target = true;
                target_is_array = false;
                target_idx = t.value;// A-Z → var[102..127]
            } else {
                pc = arg_start;//変数文字を含む識別子 → raw文字列引数
                token_get_ej_arg_raw(arg, sizeof arg, false, true);
            }
        } else if (value_cmd && t.code == TOKEN_ARRAY) {
            //Token_get(TOKEN_ARRAY)は'['を消費済みでpcは括弧内 → そのままindex解析 (command_letと同一パターン)
            target_idx = token_getArrayIndex();
            IJB_ERR_CHK();
            has_target = true;
            target_is_array = true;
        } else {
            token_back();
            token_get_ej_arg(arg, sizeof arg, false, true);//値引数: 小文字を保持・カンマ可
        }
    } else {
        token_back();
    }
    token_end();
    IJB_ERR_CHK();
    static char resp[IOTPADIOT_RESP_MAX];
    int16 num;
    bool num_valid;
    iotpad_ej_exec(cmd, arg, resp, sizeof resp, &num, &num_valid);
    if (has_target && (target_is_array && strcmp(cmd, "GETTIME") == 0)) {
        //GETTIME配列代入: [n]〜[n+5]=年/月/日/時/分/秒 (JST)
        if (target_idx + 5 >= IJB_SIZEOF_ARRAY_MAX) {
            command_error(ERR_INDEX_OUT_OF_RANGE);
            return;
        }
        uint16_t y; uint8_t mo, d, h, mi, s;
        if (!iotpad_rtc_jst(&y, &mo, &d, &h, &mi, &s)) {
            command_error(ERR_ILLEGAL_ARGUMENT);// RTCなし
            return;
        }
        *basic_getArrayPtr(target_idx + 0) = (int16)y;
        *basic_getArrayPtr(target_idx + 1) = (int16)mo;
        *basic_getArrayPtr(target_idx + 2) = (int16)d;
        *basic_getArrayPtr(target_idx + 3) = (int16)h;
        *basic_getArrayPtr(target_idx + 4) = (int16)mi;
        *basic_getArrayPtr(target_idx + 5) = (int16)s;
    } else if (has_target && num_valid) {
        //代入形式 (数値結果を代入): 画面出力なし
        if (target_is_array) *basic_getArrayPtr(target_idx) = num;
        else var[target_idx] = num;
    } else {
        //従来動作: 応答表示 (ターゲット指定でも数値結果がないコマンドは表示にフォールバック)
        put_str(resp);
        put_chr('\n');
    }
}

//EJ("CMD") / EJ(CMD) — 関数形式 (数値結果を返す。数値を返さないコマンドはエラー)
S_INLINE int func_ej() {
    static char cmd[48];
    token_get_ej_arg(cmd, sizeof cmd, true, false);//引用符付き/引用符なし両対応 (カンマで停止)
    if (!cmd[0]) {
        command_error(ERR_SYNTAX_ERROR);
        return 0;
    }
    Token t; Token_get(t);
    if (t.code != TOKEN_PAREN_E) {
        command_error(ERR_SYNTAX_ERROR);
        return 0;
    }
    static char resp[IOTPADIOT_RESP_MAX];//スタック使用量削減のためstatic化
    int16 num;
    bool num_valid;
    iotpad_ej_exec(cmd, "", resp, sizeof resp, &num, &num_valid);
    if (!num_valid) {
        command_error(ERR_ILLEGAL_ARGUMENT);
        return 0;
    }
    return num;
}

//PAD.IN [n] — IOTPADセンサー一括読み取り (ENV.IN形式)
//EJ GET*と同一の値 (SET*キャッシュ・オフセット適用済み) を16値配列へ格納
//  [n+0]=温度0.1℃  [n+1]=湿度0.1%RH  [n+2]=気圧0.1hPa   (BME280)
//  [n+3]=外気温1 0.1℃ [n+4]=外気温2 0.1℃                (DS18B20)
//  [n+5]=照度lux  [n+6]=照度2 0.1lux [n+7]=UVcps          (BH1750/LTR390)
//  [n+8]=距離0.1cm                                        (VL53L0X、無効=0)
//  [n+9]=状態フラグ (bit0:BME280 bit1:BH1750 bit2:LTR390 bit3:VL53L0X bit4:DS18B20 bit5:RTC)
//  [n+10]=年 [n+11]=月 [n+12]=日 [n+13]=時 [n+14]=分 [n+15]=秒 (RTC→JST、RTCなし=0)
//※値は1秒周期のバックグラウンド読み取り値 (EJ GET*・ThingsBoard送信と同一)
S_INLINE void command_pad_in() {
    Token t; Token_get(t);
    if (t.code != TOKEN_ARRAY) {
        command_error(ERR_SYNTAX_ERROR);
        return;
    }
    int base = token_getArrayIndex();
    IJB_ERR_CHK();
    token_end();
    IJB_ERR_CHK();
    //[base] .. [base+15] を使用
    if (base < 0 || base + 15 >= IJB_SIZEOF_ARRAY_MAX) {
        command_error(ERR_INDEX_OUT_OF_RANGE);
        return;
    }
    //EJ GET*と同一の値 (SET*キャッシュ優先、オフセット適用)
    float temp = tmp_kio[0] ? atof(tmp_kio) : cur_t1 + temp_offset;
    float hum = tmp_hum[0] ? atof(tmp_hum) : cur_h + hum_offset;
    float pres = tmp_pres[0] ? atof(tmp_pres) : cur_p + pres_offset;
    float ext1 = tmp_ondo[0] ? atof(tmp_ondo) : cur_t2 + ext1_offset;
    float ext2 = tmp_ondoext2[0] ? atof(tmp_ondoext2) : cur_t2b + ext2_offset;
    float lux = tmp_lux[0] ? atof(tmp_lux) : cur_l;
    float lux2 = tmp_ltrlux[0] ? atof(tmp_ltrlux) : cur_l2;
    unsigned long cps = tmp_ltruv_cps[0] ? (unsigned long)atol(tmp_ltruv_cps) : (unsigned long)cur_uvcps;
    float dist = tmp_dist[0] ? atof(tmp_dist) : cur_d;
    #define PAD_R10(v) ((int16)((v) * 10 + ((v) >= 0 ? 0.5f : -0.5f)))//0.1単位四捨五入
    *basic_getArrayPtr(base + 0) = PAD_R10(temp);       // 温度 0.1℃
    *basic_getArrayPtr(base + 1) = PAD_R10(hum);        // 湿度 0.1%RH
    *basic_getArrayPtr(base + 2) = PAD_R10(pres);       // 気圧 0.1hPa
    *basic_getArrayPtr(base + 3) = PAD_R10(ext1);       // 外気温1 0.1℃
    *basic_getArrayPtr(base + 4) = PAD_R10(ext2);       // 外気温2 0.1℃
    *basic_getArrayPtr(base + 5) = (int16)(lux + 0.5f); // 照度 lux
    *basic_getArrayPtr(base + 6) = PAD_R10(lux2);       // 照度2 0.1lux
    *basic_getArrayPtr(base + 7) = (int16)cps;          // UV cps
    *basic_getArrayPtr(base + 8) = dist > 0 ? PAD_R10(dist) : 0; // 距離 0.1cm
    *basic_getArrayPtr(base + 9) = (int16)((bme_ok ? 1 : 0) | (lux_ok ? 2 : 0) | (ltr_ok ? 4 : 0)
                                            | (tof_ok ? 8 : 0) | (ds18b20_count > 0 ? 16 : 0) | (rtc_ok ? 32 : 0));
    #undef PAD_R10
    uint16_t y; uint8_t mo, d, h, mi, s;
    if (iotpad_rtc_jst(&y, &mo, &d, &h, &mi, &s)) {
        *basic_getArrayPtr(base + 10) = (int16)y;
        *basic_getArrayPtr(base + 11) = (int16)mo;
        *basic_getArrayPtr(base + 12) = (int16)d;
        *basic_getArrayPtr(base + 13) = (int16)h;
        *basic_getArrayPtr(base + 14) = (int16)mi;
        *basic_getArrayPtr(base + 15) = (int16)s;
    } else {
        for (int i = 10; i <= 15; i++) *basic_getArrayPtr(base + i) = 0;
    }
}

#endif
