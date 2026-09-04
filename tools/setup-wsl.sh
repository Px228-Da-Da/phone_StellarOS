#!/usr/bin/env bash
# Установка инструментов разработки в WSL Ubuntu 24.04.
# Запуск:  wsl bash /mnt/d/OS_ANDROID/tools/setup-wsl.sh
set -e

echo "==> Обновление списка пакетов"
sudo apt-get update

echo "==> Кросс-компилятор AArch64 и сборка"
sudo apt-get install -y \
    gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu \
    build-essential make git

echo "==> QEMU (наша песочница вместо телефона)"
sudo apt-get install -y qemu-system-arm gdb-multiarch

# В Ubuntu 24.04 пакеты называются adb и fastboot: старые android-tools-*
# из репозитория убрали, и apt на них падал, обрывая скрипт через set -e.
echo "==> Работа с Android: adb, fastboot, распаковка boot.img"
sudo apt-get install -y android-sdk-libsparse-utils adb fastboot
sudo apt-get install -y device-tree-compiler   # dtc: читать DTB устройства
sudo apt-get install -y python3-pip python3-venv

echo "==> mkbootimg из AOSP: упаковка и разбор boot.img"
# Пакета mkbootimg на PyPI не существует (pip его не найдёт), поэтому берём
# официальный инструмент AOSP — это два python-файла — и делаем из них команды.
if [ ! -d /opt/mkbootimg ]; then
    sudo git clone --depth=1 \
        https://android.googlesource.com/platform/system/tools/mkbootimg \
        /opt/mkbootimg
fi
for t in mkbootimg unpack_bootimg; do
    sudo tee "/usr/local/bin/$t" >/dev/null <<WRAPPER
#!/bin/sh
exec python3 /opt/mkbootimg/$t.py "\$@"
WRAPPER
    sudo chmod +x "/usr/local/bin/$t"
done

echo "==> mtkclient: страховка от кирпича + бэкап разделов"
sudo apt-get install -y libusb-1.0-0 python3-usb
if [ ! -d "$HOME/mtkclient" ]; then
    git clone --depth=1 https://github.com/bkerler/mtkclient "$HOME/mtkclient"
    python3 -m venv "$HOME/mtkclient/.venv"
    "$HOME/mtkclient/.venv/bin/pip" install -r "$HOME/mtkclient/requirements.txt"
    sudo cp "$HOME/mtkclient/mtkclient/Setup/Linux/"*.rules /etc/udev/rules.d/ || true
    sudo udevadm control --reload-rules || true
fi

echo
echo "Проверка:"
aarch64-linux-gnu-gcc --version | head -1
qemu-system-aarch64 --version | head -1
echo
echo "Готово. Дальше:  cd /mnt/d/OS_ANDROID/kernel && make run"
