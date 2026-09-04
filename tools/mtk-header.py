#!/usr/bin/env python3
"""Добавляет к ядру 512-байтовый заголовок MediaTek.

LK на MTK не принимает голый arm64 Image: он ищет перед ядром свой заголовок
с магией 0x88168858 и именем KERNEL. Без него загрузчик не опознаёт содержимое
boot.img как ядро и молча уходит в штатную загрузку — именно это мы и увидели
на merlin: образ уходил (OKAY), а телефон грузил Android.

Формат взят из repack-MTK.pl (bgcngm/mtk-tools):
    pack('a4 L a32 a472', "\x88\x16\x88\x58", $length, $type, "\xFF"x472)

Запуск: tools/mtk-header.py <вход> <выход> [ТИП]
"""
import struct
import sys

MAGIC = b"\x88\x16\x88\x58"
HEADER_LEN = 512


def add_header(payload: bytes, kind: str = "KERNEL") -> bytes:
    name = kind.encode("ascii")
    if len(name) > 32:
        raise ValueError(f"имя типа длиннее 32 байт: {kind}")
    header = MAGIC + struct.pack("<I", len(payload)) + name.ljust(32, b"\x00")
    return header.ljust(HEADER_LEN, b"\xff") + payload


def main() -> int:
    if not 3 <= len(sys.argv) <= 4:
        print(__doc__, file=sys.stderr)
        return 2
    src, dst = sys.argv[1], sys.argv[2]
    kind = sys.argv[3] if len(sys.argv) == 4 else "KERNEL"

    payload = open(src, "rb").read()
    if payload[:4] == MAGIC:
        print(f"{src}: заголовок MTK уже есть, ничего не делаю", file=sys.stderr)
        return 1

    out = add_header(payload, kind)
    open(dst, "wb").write(out)
    print(f"{dst}: {kind}, ядро {len(payload)} байт + заголовок {HEADER_LEN} = {len(out)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
