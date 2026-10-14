#!/usr/bin/env python3
#IchigoJam PW: IOTPAD OLED用フォント事前デコードジェネレーター
#U8g2フォント (RLE圧縮) を固定幅ビットマップC配列へ変換する (Pico W側はデコード不要のルックアップのみ)
#
#出力: src/oled_fonts.h
#  typedef struct { uint8_t w, h, adv; int8_t gx, gy; uint16_t data[10]; } OLED_Char;
#  - w,h: グリフボックス幅/高さ、adv: 進行幅 (delta_x)
#  - gx,gy: u8g2のper-glyph符号付きオフセット
#    描画公式 (u8g2_font.c準拠): top = y - (h + gy), left = x + gx
#    ピクセル(r,c) = (left + c, top + r)  (bit0=最上段)
#
#使い方: python3 tools/gen_oled_fonts.py [u8g2_fonts.cへのパス]
import sys
import os

U8G2_FONTS_C = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser(
    '~/Arduino/libraries/U8g2/src/clib/u8g2_fonts.c')
OUT_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'src', 'oled_fonts.h')

#pad (ichigojam_iotpad_no15e) のFONT_TABLE準拠: (u8g2配列名, サイズ, 短縮名)
FONTS = [
    ('u8g2_font_5x7_tf',    1612, '5x7'),
    ('u8g2_font_6x12_tf',   1989, '6x12'),
    ('u8g2_font_8x13_tf',   2376, '8x13'),
    ('u8g2_font_9x15_tf',   2606, '9x15'),
    ('u8g2_font_10x20_tf', 2990, '10x20'),
]


def extract_font(src_lines, name, expected_size):
    '''u8g2_fonts.cからフォントのバイト列を抽出する (行ベース: 文字列リテラル+8進エスケープ)'''
    start_line = -1
    for i, l in enumerate(src_lines):
        if name + '[' + str(expected_size) + ']' in l:
            start_line = i
            break
    if start_line < 0:
        raise RuntimeError('font not found: ' + name)
    values = []
    for i in range(start_line + 1, len(src_lines)):
        line = src_lines[i].strip()
        if not line:
            continue
        if line.startswith('"') or (line.endswith('"') and '"' in line):
            for part in line.split('"'):
                if not part:
                    continue
                j = 0
                while j < len(part):
                    ch = part[j]
                    if ch == '\\':
                        j += 1
                        od = ''
                        while j < len(part) and len(od) < 3 and part[j] in '01234567':
                            od += part[j]
                            j += 1
                        if od:
                            values.append(int(od, 8))
                    else:
                        values.append(ord(ch) & 0xFF)
                        j += 1
            if len(values) >= expected_size:
                return values[:expected_size]
        elif ';' in line and not line.startswith('"'):
            break
        elif line.startswith('const uint8_t'):
            break
    return values[:expected_size]


class BitReader:
    '''u8g2フォントのビットストリーム読み取り (u8g2_font_decode_get_bits準拠)'''

    def __init__(self, data):
        self.d = data
        self.p = 0
        self.bp = 0

    def bits(self, c):
        if c == 0:
            return 0
        v = self.d[self.p] >> self.bp
        np = self.bp + c
        if np >= 8:
            self.p += 1
            if self.p < len(self.d):
                v |= self.d[self.p] << (8 - self.bp)
            np -= 8
        v &= (1 << c) - 1
        self.bp = np
        return v

    def signed(self, c):
        if c == 0:
            return 0
        return self.bits(c) - (1 << (c - 1))


def decode_font(fb):
    '''フォント全体をデコードし {文字コード: (w,h,gx,gy,adv,bitmap)} を返す
    bitmapはNoneの場合 (w==0、例えばspace) ピクセル描画不要'''
    bp_0, bp_1 = fb[2], fb[3]
    bp_cw, bp_ch = fb[4], fb[5]
    bp_cx, bp_cy, bp_dx = fb[6], fb[7], fb[8]

    def decode_glyph(gd):
        br = BitReader(gd)
        gw = br.bits(bp_cw)
        gh = br.bits(bp_ch)
        gx = br.signed(bp_cx)
        gy = br.signed(bp_cy)
        dl = br.signed(bp_dx)
        if gw == 0:
            return gw, gh, gx, gy, dl, None  # space等: 進行幅のみ有意味
        bm = [[0] * gw for _ in range(gh)]
        px, py = 0, 0
        for _ in range(5000):
            a = br.bits(bp_0)
            b = br.bits(bp_1)
            while True:
                for _ in range(a):  # 0-run
                    px += 1
                    if px >= gw:
                        px = 0
                        py += 1
                for _ in range(b):  # 1-run
                    if py < gh and px < gw:
                        bm[py][px] = 1
                    px += 1
                    if px >= gw:
                        px = 0
                        py += 1
                if br.bits(1) == 0:
                    break
            if py >= gh:
                break
        return gw, gh, gx, gy, dl, bm

    glyphs = {}
    pos = 23
    while pos + 1 < len(fb):
        if fb[pos + 1] == 0:
            break
        enc = fb[pos]
        sz = fb[pos + 1]
        if sz < 3:
            break
        if pos + sz <= len(fb):
            glyphs[enc] = decode_glyph(fb[pos + 2:pos + sz])
        pos += sz
    return glyphs


def main():
    with open(U8G2_FONTS_C, 'rb') as f:
        src_lines = f.read().decode('latin-1').split('\n')

    out = []
    out.append('//IchigoJam PW: IOTPAD用事前デコード済みフォント (U8g2フォントから変換)')
    out.append('//出典: U8g2 library by olikraus, BSD-3-Clause license')
    out.append('//生成: tools/gen_oled_fonts.py により生成 (手編集しない)')
    out.append('//描画公式 (u8g2_font.c準拠): top = y - (h + gy), left = x + gx')
    out.append('//data[i]=第i列のビットマップ (bit0=最上段)')
    out.append('')
    out.append('#ifndef __OLED_FONTS_H__')
    out.append('#define __OLED_FONTS_H__')
    out.append('')
    out.append('typedef struct { uint8_t w, h, adv; int8_t gx, gy; uint16_t data[10]; } OLED_Char;')
    out.append('')

    for name, size, short in FONTS:
        fb = extract_font(src_lines, name, size)
        if len(fb) != size:
            raise RuntimeError('%s: got %d/%d bytes' % (name, len(fb), size))
        glyphs = decode_font(fb)
        print('%s: %d glyphs' % (short, len(glyphs)))
        sn = short.replace('x', '_')
        out.append('// ' + short)
        out.append('static const OLED_Char oled_font_%s[95] = {' % sn)
        for enc in range(0x20, 0x7F):
            if enc in glyphs:
                gw, gh, gx, gy, dl, bm = glyphs[enc]
                data = [0] * 10
                if bm is not None:
                    for col in range(min(gw, 10)):
                        v = 0
                        for row in range(min(gh, 16)):
                            if bm[row][col]:
                                v |= (1 << row)
                        data[col] = v
                    adv = dl
                else:
                    #w==0 (space等): 進行幅のみ有意味、位置オフセットは無意味なので0に正規化
                    gx = 0
                    gy = 0
                    adv = dl
            else:
                #未収録文字: 幅0の空白として扱う (進行幅は概算)
                gw, gh, gx, gy, adv = 0, 0, 0, 0, int(short.split('x')[0]) + 1
                data = [0] * 10
            ds = ','.join('0x%04X' % v for v in data)
            out.append('    {%d,%d,%d,%d,%d,{%s}},// 0x%02X'
                       % (gw, gh, adv, gx, gy, ds, enc))
        out.append('};')
        out.append('')

    out.append('#endif')
    out.append('')

    with open(OUT_PATH, 'w') as f:
        f.write('\n'.join(out))
    print('Saved: %s (%d bytes)' % (os.path.abspath(OUT_PATH), os.path.getsize(OUT_PATH)))


if __name__ == '__main__':
    main()
