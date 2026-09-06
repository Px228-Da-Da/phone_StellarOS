#!/usr/bin/env python3
"""
Превратить снимок кадра (PPM) в PNG.

Нужен только затем, чтобы на кадр можно было посмотреть чем угодно: PPM
пишет наш просмотрщик, потому что это три строки кода, а открывает его
почти ничто.
"""
import struct
import sys
import zlib

NL = bytes([10])


def main():
    if len(sys.argv) < 3:
        raise SystemExit("как пользоваться: ppm2png.py кадр.ppm кадр.png")

    d = open(sys.argv[1], "rb").read()
    parts = d.split(NL, 3)
    if parts[0] != b"P6":
        raise SystemExit("это не PPM")
    w, h = map(int, parts[1].split())
    px = parts[3]

    raw = b"".join(bytes([0]) + px[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c))

    out = bytes([137, 80, 78, 71, 13, 10, 26, 10])
    out += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    out += chunk(b"IDAT", zlib.compress(raw, 9))
    out += chunk(b"IEND", b"")
    open(sys.argv[2], "wb").write(out)
    print("%s: %dx%d" % (sys.argv[2], w, h))


main()
