//IchigoJam PW: IOTPADセンサードライバ (BME280/BH1750/LTR390/VL53L0X/DS18B20/DS3231)
//ichigojam_iotpad_no15e のセンサー機能をPico Wに移植 (Adafruit系ライブラリの必要部分を再実装)
//I2C: GP4(SDA)/GP5(SCL)、DS18B20(OneWire): GP20
#ifndef __IOTPAD_SENSORS_H__
#define __IOTPAD_SENSORS_H__
#include "all_includes.h"

//I2Cアドレス (pad準拠)
#define BME280_ADDR     0x76
#define BH1750_ADDR     0x23
#define LTR390_ADDR     0x53
#define VL53L0X_ADDR    0x29
#define DS3231_ADDR     0x68

//ピン
#define DS18B20_PIN     27 // GP27 = IN1 (EJ "DS18B20",1 でOneWireに切替、0 でIN1に戻す)
//※GP20はサウンド出力専用 (sound.h SOUND_PIN=20) — DS18B20には使わない

//===== I2Cヘルパー =====
#define SENSOR_I2C_TIMEOUT_US 10000

static int sensor_i2c_write(uint8_t addr, const uint8_t* src, int len) {
    int res = i2c_write_timeout_us(i2c_default, addr, (uint8_t*)src, len, false, SENSOR_I2C_TIMEOUT_US);
    return (res == len) ? 0 : -1;
}
static int sensor_i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t val) {
    uint8_t buf[2] = { reg, val };
    return sensor_i2c_write(addr, buf, 2);
}
static int sensor_i2c_read(uint8_t addr, uint8_t reg, uint8_t* dst, int len) {
    int res = i2c_write_timeout_us(i2c_default, addr, &reg, 1, true, SENSOR_I2C_TIMEOUT_US);
    if (res != 1) return -1;
    res = i2c_read_timeout_us(i2c_default, addr, dst, len, false, SENSOR_I2C_TIMEOUT_US);
    return (res == len) ? 0 : -1;
}

//===== センサー状態 (pad準拠の変数名) =====
static bool bme_ok, lux_ok, ltr_ok, tof_ok, rtc_ok;
static bool ltr_current_uvs = false; // LTR390 ALS/UVS交互測定
static bool ds18b20_requested = false;
static uint64_t ds18b20_request_time = 0;
static int ds18b20_count = 0;
static uint64_t ds18b20_addr_val[2]; // OneWire ROMアドレス
static uint8_t ds18b20_crc_ok[2];

//DS18B20有効フラグ (EJ "DS18B20",1/0 で切替、flash保存)
static bool ds18b20_enabled = false;
//起動時にflashから読み込み (ext_wifi.hのiotpad_load_settingsから呼ばれる)
//※ext_wifi.hより前のinclude位置で呼ばれるため、ext_wifi.h内で前方宣言される
void iotpad_ds18b20_load_flag() {
    uint8_t flag = get_flash(get_config_offset())[PW_CFG_DS18B20_OFF];
    ds18b20_enabled = (flag == 1); // 0xFF(未設定)=OFF
}

//センサー値キャッシュ (pad準拠)
static float cur_t1 = 0, cur_t2 = 0, cur_t2b = 0; // BME280温度, DS18B20 #1, #2
static float cur_h = 0, cur_p = 0; // BME280湿度/気圧
static float cur_l = 0, cur_l2 = 0; // BH1750照度, LTR390照度2
static float cur_uv = 0, cur_d = 0; // LTR390 UV, VL53L0X距離
static unsigned long cur_uvcps = 0; // LTR390 UV counts/sec

//SET*/OFFSET*用 (pad準拠 — 一時的な値のオーバーライド)
static char tmp_kio[16] = "", tmp_ondo[16] = "", tmp_ondoext2[16] = "";
static char tmp_hum[16] = "", tmp_pres[16] = "", tmp_lux[16] = "";
static char tmp_ltrlux[16] = "", tmp_ltruv[16] = "", tmp_ltruv_cps[16] = "";
static char tmp_dist[16] = "";
static float temp_offset = 0, hum_offset = 0, pres_offset = 0;
static float ext1_offset = 0, ext2_offset = 0;

//flash設定領域からオフセット5種を復元 (ext_wifi.hのiotpad_load_settingsから呼ばれる)
//cfg: PW_CFG_OFFS_OFFを指す20バイト (temp/hum/pres/ext1/ext2 各float LE)
//※消去状態(全0xFF)はNaN回避のため0のままにする (padのsave_settings相当の起動復元)
void iotpad_sensors_load_offsets(const uint8_t* cfg) {
    float vals[5];
    memcpy(vals, cfg, 20);
    bool erased = true;
    const uint8_t* p = cfg;
    for (int i = 0; i < 20; i++) {
        if (p[i] != 0xFF) { erased = false; break; }
    }
    if (erased) return;//未保存
    //異常値ガード (NaNや極端な値は無効化: floatが非正規化/NaNなら0)
    for (int i = 0; i < 5; i++) {
        if (vals[i] != vals[i] || vals[i] > 1e6f || vals[i] < -1e6f) vals[i] = 0; // NaN比較は!=で検出
    }
    temp_offset = vals[0];
    hum_offset = vals[1];
    pres_offset = vals[2];
    ext1_offset = vals[3];
    ext2_offset = vals[4];
}

//===== BME280 =====
//キャリブレーションデータ
static uint16_t bme_dig_T1;
static int16_t bme_dig_T2, bme_dig_T3;
static uint16_t bme_dig_P1;
static int16_t bme_dig_P2, bme_dig_P3, bme_dig_P4, bme_dig_P5;
static int16_t bme_dig_P6, bme_dig_P7, bme_dig_P8, bme_dig_P9;
static uint8_t bme_dig_H1;
static int16_t bme_dig_H2, bme_dig_H3;
static int16_t bme_dig_H4, bme_dig_H5;
static int8_t bme_dig_H6;
static int32_t bme_t_fine;

static bool bme280_init() {
    uint8_t id;
    if (sensor_i2c_read(BME280_ADDR, 0xD0, &id, 1) || id != 0x60) return false;
    //キャリブレーション読み込み (0x88: 26bytes)
    uint8_t cal1[26];
    if (sensor_i2c_read(BME280_ADDR, 0x88, cal1, 26)) return false;
    bme_dig_T1 = (uint16_t)(cal1[0] | (cal1[1] << 8));
    bme_dig_T2 = (int16_t)(cal1[2] | (cal1[3] << 8));
    bme_dig_T3 = (int16_t)(cal1[4] | (cal1[5] << 8));
    bme_dig_P1 = (uint16_t)(cal1[6] | (cal1[7] << 8));
    bme_dig_P2 = (int16_t)(cal1[8] | (cal1[9] << 8));
    bme_dig_P3 = (int16_t)(cal1[10] | (cal1[11] << 8));
    bme_dig_P4 = (int16_t)(cal1[12] | (cal1[13] << 8));
    bme_dig_P5 = (int16_t)(cal1[14] | (cal1[15] << 8));
    bme_dig_P6 = (int16_t)(cal1[16] | (cal1[17] << 8));
    bme_dig_P7 = (int16_t)(cal1[18] | (cal1[19] << 8));
    bme_dig_P8 = (int16_t)(cal1[20] | (cal1[21] << 8));
    bme_dig_P9 = (int16_t)(cal1[22] | (cal1[23] << 8));
    bme_dig_H1 = cal1[25];
    // (0xE1: 7bytes)
    uint8_t cal2[7];
    if (sensor_i2c_read(BME280_ADDR, 0xE1, cal2, 7)) return false;
    bme_dig_H2 = (int16_t)(cal2[0] | (cal2[1] << 8));
    bme_dig_H3 = (uint8_t)cal2[2];
    bme_dig_H4 = (int16_t)(((int16_t)cal2[3] << 4) | (cal2[4] & 0x0F));
    bme_dig_H5 = (int16_t)(((int16_t)cal2[5] << 4) | (cal2[4] >> 4));
    bme_dig_H6 = (int8_t)cal2[6];
    //設定: 湿度オーバーサンプリング x1
    sensor_i2c_write_reg(BME280_ADDR, 0xF2, 0x01);
    //設定: フィルタ無効、SPI無効
    sensor_i2c_write_reg(BME280_ADDR, 0xF5, 0x00);
    return true;
}

static void bme280_read(float* t, float* h, float* p) {
    //forced mode: 温度/気圧/湿度 各x1
    sensor_i2c_write_reg(BME280_ADDR, 0xF4, 0x25);
    sleep_ms(10); // 変換待ち
    uint8_t data[8];
    if (sensor_i2c_read(BME280_ADDR, 0xF7, data, 8)) {
        *t = *h = *p = 0;
        return;
    }
    int32_t adc_P = ((int32_t)data[0] << 12) | ((int32_t)data[1] << 4) | (data[2] >> 4);
    int32_t adc_T = ((int32_t)data[3] << 12) | ((int32_t)data[4] << 4) | (data[5] >> 4);
    int32_t adc_H = ((int32_t)data[6] << 8) | data[7];
    //温度補償 (Bosch標準)
    int32_t var1, var2;
    var1 = (int32_t)((adc_T / 8) - ((int32_t)bme_dig_T1 * 2));
    var1 = (var1 * ((int32_t)bme_dig_T2)) / 2048;
    var2 = (int32_t)((adc_T / 16) - ((int32_t)bme_dig_T1));
    var2 = (((var2 * var2) / 4096) * ((int32_t)bme_dig_T3)) / 16384;
    bme_t_fine = var1 + var2;
    int32_t T = (bme_t_fine * 5 + 128) / 256;
    *t = (float)T / 100.0f;
    //気圧補償
    int64_t v1, v2, v3, v4;
    v1 = ((int64_t)bme_t_fine) - 128000;
    v2 = v1 * v1 * (int64_t)bme_dig_P6;
    v2 = v2 + ((v1 * (int64_t)bme_dig_P5) * 131072);
    v2 = v2 + (((int64_t)bme_dig_P4) * 34359738368LL);
    v1 = ((v1 * v1 * (int64_t)bme_dig_P3) / 256) + ((v1 * ((int64_t)bme_dig_P2) * 4096));
    v3 = ((int64_t)1) * 140737488355328LL;
    v1 = (v3 + v1) * ((int64_t)bme_dig_P1) / 8589934592LL;
    if (v1 == 0) { *p = 0; } else {
        v4 = 1048576 - adc_P;
        v4 = (((v4 * 2147483648LL) - v2) * 3125) / v1;
        v1 = (((int64_t)bme_dig_P9) * (v4 / 8192) * (v4 / 8192)) / 33554432;
        v2 = (((int64_t)bme_dig_P8) * v4) / 524288;
        v4 = ((v4 + v1 + v2) / 256) + (((int64_t)bme_dig_P7) * 16);
        *p = (float)v4 / 256.0f / 100.0f; //Pa→hPa
    }
    //湿度補償
    int32_t hv1, hv2, hv3, hv4, hv5;
    hv1 = bme_t_fine - ((int32_t)76800);
    hv2 = (int32_t)(adc_H * 16384);
    hv3 = (int32_t)(((int32_t)bme_dig_H4) * 1048576);
    hv4 = ((int32_t)bme_dig_H5) * hv1;
    hv5 = (((hv2 - hv3) - hv4) + (int32_t)16384) / 32768;
    hv2 = (hv1 * ((int32_t)bme_dig_H6)) / 1024;
    hv3 = (hv1 * ((int32_t)bme_dig_H3)) / 2048;
    hv4 = ((hv2 * (hv3 + (int32_t)32768)) / 1024) + (int32_t)2097152;
    hv2 = ((hv4 * ((int32_t)bme_dig_H2)) + 8192) / 16384;
    hv3 = hv5 * hv2;
    hv4 = ((hv3 / 32768) * (hv3 / 32768)) / 128;
    hv5 = hv3 - ((hv4 * ((int32_t)bme_dig_H1)) / 16);
    if (hv5 < 0) hv5 = 0;
    if (hv5 > 419430400) hv5 = 419430400;
    *h = (float)((uint32_t)hv5 / 4096) / 1024.0f;
}

//===== BH1750 =====
//※BH1750はレジスタアドレスを持たない — 単バイトコマンドで制御する (レジスタ書き込み方式は不可)
static bool bh1750_init() {
    uint8_t cmd = 0x10; // 連続高解像度モード (1 lux resolution, ~120ms測定)
    return i2c_write_timeout_us(i2c_default, BH1750_ADDR, &cmd, 1, false, SENSOR_I2C_TIMEOUT_US) == 1;
}
static float bh1750_read() {
    //レジスタ指定なし — 直接2バイト読み取り
    uint8_t data[2];
    int res = i2c_read_timeout_us(i2c_default, BH1750_ADDR, data, 2, false, SENSOR_I2C_TIMEOUT_US);
    if (res != 2) return 0;
    uint16_t raw = ((uint16_t)data[0] << 8) | data[1];
    return (float)raw / 1.2f; // 1.2 = 測定精度係数 (高解像度モード)
}

//===== LTR390 =====
#define LTR390_REG_MAIN_CTRL   0x00
#define LTR390_REG_MEAS_RATE   0x04
#define LTR390_REG_GAIN        0x05
#define LTR390_REG_PART_ID     0x06
#define LTR390_REG_MAIN_STATUS 0x07
#define LTR390_REG_ALSDATA      0x0D
#define LTR390_REG_UVSDATA      0x10
#define LTR390_MODE_ALS         0x00
#define LTR390_MODE_UVS         0x08
#define LTR390_GAIN_3          0x02
#define LTR390_RESOLUTION_18BIT 0x20

static bool ltr390_init() {
    uint8_t id;
    if (sensor_i2c_read(LTR390_ADDR, LTR390_REG_PART_ID, &id, 1)) return false;
    if ((id & 0xF0) != 0xB0) return false;
    sensor_i2c_write_reg(LTR390_ADDR, LTR390_REG_GAIN, LTR390_GAIN_3);
    sensor_i2c_write_reg(LTR390_ADDR, LTR390_REG_MEAS_RATE, LTR390_RESOLUTION_18BIT);
    sensor_i2c_write_reg(LTR390_ADDR, LTR390_REG_MAIN_CTRL, LTR390_MODE_ALS | 0x02); //ALS有効
    ltr_current_uvs = false;
    return true;
}
static uint32_t ltr390_read_raw(uint8_t reg) {
    uint8_t data[3];
    if (sensor_i2c_read(LTR390_ADDR, reg, data, 3)) return 0;
    return ((uint32_t)data[0] << 16) | ((uint32_t)data[1] << 8) | data[2];
}
static bool ltr390_new_data() {
    uint8_t st;
    if (sensor_i2c_read(LTR390_ADDR, LTR390_REG_MAIN_STATUS, &st, 1)) return false;
    return (st & 0x08) != 0;
}
//LTR390変換式 (pad準拠、Gain=3/18bit)
static float ltr390_raw_to_lux(uint32_t raw) {
    return 0.6f * (float)raw / (3.0f * 1.0f) * 1.0f;
}
static float ltr390_raw_to_uvi(uint32_t raw) {
    float gain_factor = 3.0f / 18.0f;
    float res_factor = 1.0f / 4.0f;
    return (float)raw / (gain_factor * res_factor * 2300.0f) * 1.0f;
}
static unsigned long ltr390_raw_to_cps(uint32_t raw) {
    return (unsigned long)raw * 10UL;
}

//===== VL53L0X =====
static uint8_t vl53_stop_variable;
static bool vl53l0x_init() {
    uint8_t id;
    if (sensor_i2c_read(VL53L0X_ADDR, 0xC0, &id, 1) || id != 0xEE) return false;
    //簡易初期化シーケンス (Adafruit/Pololu互換)
    uint8_t r;
    sensor_i2c_write_reg(VL53L0X_ADDR, 0xFF, 0x01);
    sensor_i2c_write_reg(VL53L0X_ADDR, 0x00, 0x00);
    if (sensor_i2c_read(VL53L0X_ADDR, 0x04, &r, 1)) return false;
    vl53_stop_variable = r;
    sensor_i2c_write_reg(VL53L0X_ADDR, 0xFF, 0x00);
    sensor_i2c_write_reg(VL53L0X_ADDR, 0x91, vl53_stop_variable);
    sensor_i2c_write_reg(VL53L0X_ADDR, 0x00, 0x01);
    sensor_i2c_write_reg(VL53L0X_ADDR, 0xFF, 0x00);
    sensor_i2c_write_reg(VL53L0X_ADDR, 0x80, 0x01);
    sensor_i2c_write_reg(VL53L0X_ADDR, 0xFF, 0x07);
    sensor_i2c_write_reg(VL53L0X_ADDR, 0xFF, 0x01);
    sensor_i2c_write_reg(VL53L0X_ADDR, 0x00, 0x01);
    sensor_i2c_write_reg(VL53L0X_ADDR, 0xFF, 0x06);
    sensor_i2c_write_reg(VL53L0X_ADDR, 0xFF, 0x00);
    sensor_i2c_write_reg(VL53L0X_ADDR, 0x09, 0x00);
    sensor_i2c_write_reg(VL53L0X_ADDR, 0x10, 0x00);
    sensor_i2c_write_reg(VL53L0X_ADDR, 0x11, 0x00);
    sensor_i2c_write_reg(VL53L0X_ADDR, 0x24, 0x01);
    sensor_i2c_write_reg(VL53L0X_ADDR, 0x25, 0xFF);
    sensor_i2c_write_reg(VL53L0X_ADDR, 0x75, 0x00);
    sensor_i2c_write_reg(VL53L0X_ADDR, 0xFF, 0x01);
    sensor_i2c_write_reg(VL53L0X_ADDR, 0x4E, 0x2C);
    sensor_i2c_write_reg(VL53L0X_ADDR, 0x48, 0x00);
    sensor_i2c_write_reg(VL53L0X_ADDR, 0x30, 0x20);
    sensor_i2c_write_reg(VL53L0X_ADDR, 0xFF, 0x00);
    sensor_i2c_write_reg(VL53L0X_ADDR, 0x30, 0x09);
    sensor_i2c_write_reg(VL53L0X_ADDR, 0x54, 0x00);
    sensor_i2c_write_reg(VL53L0X_ADDR, 0x31, 0x04);
    sensor_i2c_write_reg(VL53L0X_ADDR, 0x10, 0x00);
    sensor_i2c_write_reg(VL53L0X_ADDR, 0x10, 0x00);
    sensor_i2c_write_reg(VL53L0X_ADDR, 0x07, 0x01);
    sensor_i2c_write_reg(VL53L0X_ADDR, 0xFF, 0x01);
    sensor_i2c_write_reg(VL53L0X_ADDR, 0x00, 0x00);
    return true;
}
static float vl53l0x_read_mm() {
    //single-shot測距
    sensor_i2c_write_reg(VL53L0X_ADDR, 0x00, 0x01);
    //完了待ち (最大100ms)
    for (int i = 0; i < 100; i++) {
        uint8_t st;
        if (sensor_i2c_read(VL53L0X_ADDR, 0x13, &st, 1) == 0 && (st & 0x07) != 0) {
            //距離読み取り (12bytes from 0x14)
            uint8_t data[12];
            if (sensor_i2c_read(VL53L0X_ADDR, 0x14, data, 12) == 0) {
                sensor_i2c_write_reg(VL53L0X_ADDR, 0x0B, 0x07); //割り込みクリア
                if (data[0] == 0) { //ステータス0=正常
                    return (float)(((uint16_t)data[10] << 8) | data[11]);
                }
            }
            return 0; //測定失敗/範囲外
        }
        sleep_ms(1);
    }
    return 0;
}

//===== DS18B20 (OneWire) =====
//1-Wireタイミング (157.5MHz動作、busy_wait_us使用)
//※NTSC走査線周期63.5µs内に収めるため、割り込み禁止はWrite1/Readの短時間(6-12µs)のみ
//  Reset(480µs)とWrite0(60µs)はOneWire規格の許容範囲が広いため割り込み禁止なし(NTSC IRQが挿入されても問題ない)
static void ow_pin_low() { gpio_set_dir(DS18B20_PIN, GPIO_OUT); gpio_put(DS18B20_PIN, 0); }
static void ow_pin_release() { gpio_set_dir(DS18B20_PIN, GPIO_IN); } //プルアップで解放
static bool ow_read_bit() {
    //Read: 6µs Low+9µs待ち+サンプル = 約12µs → NTSC走査線の1/5、影響なし
    int save = save_and_disable_interrupts();
    ow_pin_low();
    busy_wait_us(3);
    ow_pin_release();
    busy_wait_us(9);
    bool b = gpio_get(DS18B20_PIN);
    restore_interrupts(save);
    busy_wait_us(50);
    return b;
}
static void ow_write_bit(bool b) {
    if (b) {
        //Write 1: 15µs以内の厳しいタイミング → 6µsのみ割り込み禁止
        int save = save_and_disable_interrupts();
        ow_pin_low();
        busy_wait_us(6);
        ow_pin_release();
        restore_interrupts(save);
        busy_wait_us(60);//回復待ち (割り込み許可中)
    } else {
        //Write 0: 60-120µsの許容範囲 → 割り込み禁止なし (NTSC IRQ挿入されても100µs程度で問題なし)
        ow_pin_low();
        busy_wait_us(60);
        ow_pin_release();
        busy_wait_us(8);
    }
}
static bool ow_reset() {
    //Reset: 480µs以上で上限が緩い → 割り込み禁止なし (NTSC IRQ挿入されても520µs程度で問題なし)
    ow_pin_low();
    busy_wait_us(480);
    ow_pin_release();
    busy_wait_us(70);
    bool present = !gpio_get(DS18B20_PIN); //LOW=存在
    busy_wait_us(410);
    return present;
}
static void ow_write_byte(uint8_t b) {
    for (int i = 0; i < 8; i++) {
        ow_write_bit(b & 1);
        b >>= 1;
    }
}
static uint8_t ow_read_byte() {
    uint8_t b = 0;
    for (int i = 0; i < 8; i++) {
        if (ow_read_bit()) b |= (1 << i);
    }
    return b;
}
static bool ds18b20_init() {
    gpio_init(DS18B20_PIN);
    gpio_pull_up(DS18B20_PIN);
    if (!ow_reset()) return false;
    //ROMサーチで2個のデバイスを検出 (簡易: Search ROM)
    uint64_t addr[2];
    ds18b20_count = 0;
    //最初のデバイス
    ow_reset();
    ow_write_byte(0xF0); // Search ROM
    uint64_t rom = 0;
    for (int i = 0; i < 64; i++) {
        bool bit = ow_read_bit();
        bool nbit = ow_read_bit();
        if (!bit && !nbit) { //衝突 — 0方向を選択
            ow_write_bit(false);
        } else if (bit && !nbit) { //1のみ
            ow_write_bit(true);
            rom |= (1ULL << i);
        } else if (!bit && nbit) { //0のみ
            ow_write_bit(false);
        } else { //両方1 = デバイスなし
            return false;
        }
    }
    addr[0] = rom;
    ds18b20_count = 1;
    //2個目 (簡易: Skip ROMを使い、1個のみ対応で十分なら省略)
    //padは2個対応だが、OneWireのSearchは複雑 — まず1個のみ検出
    //TODO: 2個目のSearch ROM実装
    //skip ROM方式で1個のみ扱う場合:
    ds18b20_addr_val[0] = addr[0];
    return true;
}
static void ds18b20_request_convert() {
    ow_reset();
    ow_write_byte(0xCC); // Skip ROM
    ow_write_byte(0x44); // Convert T
    ds18b20_requested = true;
    ds18b20_request_time = time_us_64();
}
static float ds18b20_read_temp(int index) {
    //index 0: 1個目 (ROMアドレス指定)、index 1: 2個目
    if (ds18b20_count == 0) return 0;
    if (ds18b20_count == 1 && index > 0) return 0;
    ow_reset();
    if (ds18b20_count == 1) {
        ow_write_byte(0xCC); // Skip ROM (1個のみ)
    } else {
        ow_write_byte(0x55); // Match ROM
        uint64_t addr = ds18b20_addr_val[index];
        for (int i = 0; i < 8; i++) {
            ow_write_byte((addr >> (i * 8)) & 0xFF);
        }
    }
    ow_write_byte(0xBE); // Read Scratchpad
    uint8_t sp[9];
    for (int i = 0; i < 9; i++) sp[i] = ow_read_byte();
    int16_t raw = ((int16_t)sp[1] << 8) | sp[0];
    return (float)raw / 16.0f;
}

//===== DS3231 RTC =====
static uint8_t bcd2bin(uint8_t bcd) { return (bcd & 0x0F) + ((bcd >> 4) * 10); }
static uint8_t bin2bcd(uint8_t bin) { return ((bin / 10) << 4) | (bin % 10); }
static bool ds3231_init() {
    uint8_t sec;
    if (sensor_i2c_read(DS3231_ADDR, 0x00, &sec, 1)) return false;
    return true;
}
static void ds3231_read(uint16_t* y, uint8_t* mo, uint8_t* d, uint8_t* h, uint8_t* mi, uint8_t* s) {
    uint8_t regs[7];
    if (sensor_i2c_read(DS3231_ADDR, 0x00, regs, 7)) return;
    *s = bcd2bin(regs[0]);
    *mi = bcd2bin(regs[1]);
    *h = bcd2bin(regs[2] & 0x3F); //24hモード
    uint8_t dow = bcd2bin(regs[3]);
    *d = bcd2bin(regs[4]);
    *mo = bcd2bin(regs[5] & 0x1F);
    *y = 2000 + bcd2bin(regs[6]); //2000年代決め打ち (uint16必須: 2026等はuint8に収まらない)
    (void)dow;
}
static void ds3231_write(uint16_t y, uint8_t mo, uint8_t d, uint8_t h, uint8_t mi, uint8_t s) {
    uint8_t regs[7];
    regs[0] = bin2bcd(s);
    regs[1] = bin2bcd(mi);
    regs[2] = bin2bcd(h); //24h
    regs[3] = bin2bcd(1); //日曜=1
    regs[4] = bin2bcd(d);
    regs[5] = bin2bcd(mo);
    regs[6] = bin2bcd((uint8_t)(y % 100));
    for (int i = 0; i < 7; i++) {
        sensor_i2c_write_reg(DS3231_ADDR, i, regs[i]);
    }
}

//===== センサー全体初期化 =====
static void iotpad_sensors_init() {
    bme_ok = bme280_init();
    iotpad_dbg_str(bme_ok ? "PW: BME280 OK\n" : "PW: BME280 FAIL\n");
    lux_ok = bh1750_init();
    iotpad_dbg_str(lux_ok ? "PW: BH1750 OK\n" : "PW: BH1750 FAIL\n");
    ltr_ok = ltr390_init();
    iotpad_dbg_str(ltr_ok ? "PW: LTR390 OK\n" : "PW: LTR390 FAIL\n");
    tof_ok = vl53l0x_init();
    iotpad_dbg_str(tof_ok ? "PW: VL53L0X OK\n" : "PW: VL53L0X FAIL\n");
    rtc_ok = ds3231_init();
    iotpad_dbg_str(rtc_ok ? "PW: DS3231 OK\n" : "PW: DS3231 FAIL\n");
    //DS18B20 (ds18b20_enabledがtrueの場合のみGP27をOneWireとして初期化、falseならIN1として維持)
    if (ds18b20_enabled) {
        ds18b20_count = 0;
        gpio_init(DS18B20_PIN);
        gpio_pull_up(DS18B20_PIN);
        if (ow_reset()) {
            ds18b20_count = 1;
            ds18b20_request_convert();
        }
        iotpad_dbg_str(ds18b20_count > 0 ? "PW: DS18B20 OK\n" : "PW: DS18B20 FAIL\n");
    } else {
        ds18b20_count = 0;
        iotpad_dbg_str("PW: DS18B20 OFF\n");
    }
}

//===== センサー定期読み取り (1秒ごと、main()のループから呼ぶ) =====
static uint64_t iotpad_sensors_last_ms = 0;

static void iotpad_sensors_read() {
    uint64_t now = to_ms_since_boot(get_absolute_time());
    if (now - iotpad_sensors_last_ms < 1000) return;
    iotpad_sensors_last_ms = now;
    //BME280
    if (bme_ok) {
        bme280_read(&cur_t1, &cur_h, &cur_p);
    }
    //BH1750
    if (lux_ok) {
        cur_l = bh1750_read();
    }
    //DS18B20 (ds18b20_enabledがtrueの場合のみ読み取り、ノンブロッキング)
    if (ds18b20_enabled && ds18b20_count > 0) {
        if (!ds18b20_requested) {
            ds18b20_request_convert();
        } else if (now - (ds18b20_request_time / 1000) >= 750) {
            cur_t2 = ds18b20_read_temp(0);
            if (ds18b20_count > 1) {
                cur_t2b = ds18b20_read_temp(1);
            }
            ds18b20_requested = false;
            //次の変換を即座に開始
            ds18b20_request_convert();
        }
    }
    //VL53L0X
    if (tof_ok) {
        cur_d = vl53l0x_read_mm() / 10.0f; // mm→cm (pad準拠)
    }
    //LTR390 (ALS/UVS交互測定、pad準拠)
    if (ltr_ok) {
        if (ltr_current_uvs) {
            if (ltr390_new_data()) {
                uint32_t raw = ltr390_read_raw(LTR390_REG_UVSDATA);
                cur_uv = (float)raw;
                cur_uvcps = ltr390_raw_to_cps(raw);
                //ALSモードに戻す
                sensor_i2c_write_reg(LTR390_ADDR, LTR390_REG_MAIN_CTRL, LTR390_MODE_ALS | 0x02);
                ltr_current_uvs = false;
            }
        } else {
            if (ltr390_new_data()) {
                uint32_t raw = ltr390_read_raw(LTR390_REG_ALSDATA);
                cur_l2 = ltr390_raw_to_lux(raw);
                //UVSモードに切り替え
                sensor_i2c_write_reg(LTR390_ADDR, LTR390_REG_MAIN_CTRL, LTR390_MODE_UVS | 0x02);
                ltr_current_uvs = true;
            }
        }
    }
}

#endif
