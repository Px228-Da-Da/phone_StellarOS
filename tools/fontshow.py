#!/usr/bin/env python3
"""
Посмотреть на растровый шрифт .stf глазами — картинкой, без телефона.

Растеризатор легко написать так, что он «почти работает»: контуры
разъезжаются на паре букв, дырки в О заливаются, у Й теряется краткая.
Числа этого не покажут, а картинка показывает сразу. Поэтому проверка
шрифта — не прошивка, а один PNG.

    python3 tools/fontshow.py шрифт.stf 32 "Привет, StellarOS 123" вид.png
"""
import struct
import sys
import zlib


def load(path):
    d = open(path, 'rb').read()
    if d[:4] != b'STF1':
        raise SystemExit('это не шрифт StellarOS')
    nface = struct.unpack('<I', d[4:8])[0]
    faces = []
    for i in range(nface):
        f = struct.unpack('<8I', d[8 + i * 32:8 + i * 32 + 32])
        px, line, base, count, codes_off, glyphs_off, bits_off, bits_len = f
        codes = struct.unpack('<%dI' % count, d[codes_off:codes_off + count * 4])
        glyphs = {}
        for k in range(count):
            r = d[glyphs_off + k * 8:glyphs_off + k * 8 + 8]
            off = r[0] | (r[1] << 8) | (r[2] << 16)
            w, h, adv = r[3], r[4], r[5]
            left = r[6] - 256 if r[6] > 127 else r[6]
            top = r[7] - 256 if r[7] > 127 else r[7]
            glyphs[codes[k]] = (off, w, h, adv, left, top)
        faces.append({'px': px, 'line': line, 'base': base,
                      'glyphs': glyphs,
                      'bits': d[bits_off:bits_off + bits_len]})
    return faces


def png(path, w, h, gray):
    raw = b''.join(b'\x00' + bytes(gray[y * w:(y + 1) * w]) for y in range(h))

    def chunk(tag, data):
        c = tag + data
        return struct.pack('>I', len(data)) + c + struct.pack('>I', zlib.crc32(c))

    out = b'\x89PNG\r\n\x1a\n'
    out += chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 0, 0, 0, 0))
    out += chunk(b'IDAT', zlib.compress(raw, 9))
    out += chunk(b'IEND', b'')
    open(path, 'wb').write(out)


def main():
    if len(sys.argv) < 5:
        raise SystemExit(__doc__)
    faces = load(sys.argv[1])
    want = int(sys.argv[2])
    text = sys.argv[3]
    face = min(faces, key=lambda f: abs(f['px'] - want))

    W, H = 1400, face['line'] * 2 + 20
    img = [16] * (W * H)
    x, y = 10, 10 + face['base']

    for ch in text:
        g = face['glyphs'].get(ord(ch))
        if not g:
            x += face['px'] // 2
            continue
        off, w, h, adv, left, top = g
        for row in range(h):
            for col in range(w):
                a = face['bits'][off + row * w + col]
                px, py = x + left + col, y + top + row
                if 0 <= px < W and 0 <= py < H:
                    i = py * W + px
                    img[i] = max(img[i], a)
        x += adv

    png(sys.argv[4], W, H, img)
    print('кегль %d, строка %d, база %d, знаков %d -> %s'
          % (face['px'], face['line'], face['base'], len(face['glyphs']),
             sys.argv[4]))


main()
