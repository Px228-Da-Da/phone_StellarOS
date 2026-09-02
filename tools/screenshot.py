#!/usr/bin/env python3
"""
Снимок экрана ядра в QEMU.

Запускает эмулятор с ramfb, ждёт, пока ядро нарисует экран, и просит QEMU
сохранить кадр. Нужно, потому что графику иначе никак не проверить: у нас
нет ни монитора у эмулятора, ни телефона до разблокировки, а код рисования
(шрифт, консоль, переносы строк) обязан быть проверен до первой прошивки.

Общение с QEMU идёт по QMP — это его управляющий протокол, обычный JSON
построчно через сокет.

    python3 tools/screenshot.py [секунд_до_снимка] [выходной.png]
"""
import json
import os
import socket
import subprocess
import sys
import time

KERNEL = "kernel/build/qemu/Image"
SOCK = "/tmp/velo-qmp.sock"
PPM = "/tmp/velo-shot.ppm"

delay = float(sys.argv[1]) if len(sys.argv) > 1 else 6.0
out_png = sys.argv[2] if len(sys.argv) > 2 else "/tmp/velo-shot.png"

if not os.path.exists(KERNEL):
    sys.exit(f"нет ядра {KERNEL} — сначала make BOARD=qemu")

for path in (SOCK, PPM, out_png):
    if os.path.exists(path):
        os.remove(path)

qemu = subprocess.Popen([
    "qemu-system-aarch64",
    "-M", "virt,gic-version=3",
    "-cpu", "cortex-a53",
    "-smp", "8",
    "-m", "1G",
    "-device", "ramfb",            # тот самый экран
    "-display", "none",            # окна нет, кадр забираем через QMP
    "-serial", "file:/tmp/velo-serial.log",
    "-qmp", f"unix:{SOCK},server,nowait",
    "-kernel", KERNEL,
], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

try:
    # Ждём появления сокета, потом даём ядру дорисовать экран
    for _ in range(50):
        if os.path.exists(SOCK):
            break
        time.sleep(0.1)
    else:
        sys.exit("QEMU не открыл QMP-сокет")

    time.sleep(delay)

    s = socket.socket(socket.AF_UNIX)
    s.connect(SOCK)
    s.settimeout(5)

    def cmd(obj):
        s.sendall((json.dumps(obj) + "\n").encode())
        time.sleep(0.4)
        return s.recv(65536).decode(errors="replace")

    s.recv(65536)                                   # приветствие QMP
    cmd({"execute": "qmp_capabilities"})
    reply = cmd({"execute": "screendump", "arguments": {"filename": PPM}})
    s.close()

    if "error" in reply:
        sys.exit(f"screendump не удался: {reply.strip()}")
finally:
    qemu.terminate()
    try:
        qemu.wait(timeout=5)
    except subprocess.TimeoutExpired:
        qemu.kill()

if not os.path.exists(PPM):
    sys.exit("QEMU не сохранил кадр")

# PPM читается кем угодно, но PNG удобнее пересылать
png_ok = subprocess.run(
    ["ffmpeg", "-y", "-loglevel", "error", "-i", PPM, out_png],
    capture_output=True,
).returncode == 0

size = os.path.getsize(PPM)
print(f"кадр снят: {PPM} ({size} байт)")
if png_ok:
    print(f"PNG: {out_png} ({os.path.getsize(out_png)} байт)")
else:
    print("ffmpeg не сработал — остался только PPM")
