@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion

rem ============================================================
rem  Сборка и прошивка VELO-OS одной командой.
rem
rem    flash.bat          обычное ядро
rem    flash.bat 17       ядро с остановкой на этапе 17 (см. HALT_STAGE)
rem    flash.bat restore  вернуть заводской Android
rem
rem  Что делает сам: держит WSL живым, ждёт появления телефона,
rem  пробрасывает USB в WSL, собирает и прошивает.
rem  Что остаётся тебе: комбинация кнопок, если в телефоне наше ядро —
rem  оно висит, и adb до него не достучится.
rem ============================================================

set WSL=wsl -u root
set ROOT=/mnt/d/OS_ANDROID
set VID=0bb4:0c01

if /i "%~1"=="restore" goto :restore

rem --- 1. Сборка -------------------------------------------------
if "%~1"=="" (
    echo [1/4] Собираю ядро...
    set HALT=
) else (
    echo [1/4] Собираю ядро с остановкой на этапе %~1...
    set HALT=HALT_AT=%~1
)
rem Без конвейера: иначе код возврата будет от tail, а не от make,
rem и провалившаяся сборка проедет незамеченной.
%WSL% bash -c "cd %ROOT%/kernel && rm -rf build/merlin && make BOARD=merlin !HALT! >/dev/null 2>&1"
if errorlevel 1 (
    echo       СБОРКА НЕ ПРОШЛА. Подробности:
    %WSL% bash -c "cd %ROOT%/kernel && make BOARD=merlin !HALT! 2>&1 | grep -E 'error:' | head -5"
    goto :fail
)
%WSL% bash -c "cd %ROOT% && bash tools/mkboot.sh >/dev/null 2>&1"
if errorlevel 1 (
    echo       УПАКОВКА ОБРАЗА НЕ ПРОШЛА
    goto :fail
)
for /f %%s in ('%WSL% bash -c "stat -c%%s %ROOT%/out/velo-boot.img"') do echo       образ готов, %%s байт

:flashimg
rem --- 2. Держим WSL живым: без запущенного дистрибутива usbipd не пробросит
start "" /b %WSL% -- sleep 300 >nul 2>&1

rem --- 3. Ждём телефон в fastboot -------------------------------
echo [2/4] Пробую загнать в fastboot через adb...
adb devices 2>nul | findstr /r "device$" >nul && (
    adb reboot bootloader >nul 2>&1
    echo       команда отправлена
) || (
    echo       adb не видит телефон — жми Power 20 сек, затем Vol- + Power
)

echo [3/4] Жду появления телефона в fastboot...
set BUSID=
for /l %%i in (1,1,120) do (
    for /f "tokens=1" %%a in ('usbipd list 2^>nul ^| findstr /c:"%VID%"') do set BUSID=%%a
    if not "!BUSID!"=="" goto :found
    ping -n 2 127.0.0.1 >nul
)
echo       не дождался за две минуты
goto :fail

:found
echo       нашёл на шине !BUSID!
usbipd list | findstr /c:"%VID%" | findstr /c:"Attached" >nul
if errorlevel 1 (
    usbipd attach --wsl --busid !BUSID! >nul 2>&1
    if errorlevel 1 (
        echo.
        echo       ПРОБРОС НЕ УДАЛСЯ. Скорее всего нужна привязка,
        echo       она требует прав администратора. Один раз выполни:
        echo           usbipd bind --busid !BUSID!
        goto :fail
    )
)
ping -n 3 127.0.0.1 >nul

rem --- 4. Прошивка ----------------------------------------------
echo [4/4] Прошиваю...
%WSL% bash -c "cd %ROOT% && echo y | bash tools/flash.sh out/velo-boot.img 2>&1 | grep -E 'Sending|Writing|Rebooting|СТОП|НЕТ'"
echo.
echo Готово. Смотри на экран телефона.
goto :end

:restore
echo Возвращаю заводской Android...
start "" /b %WSL% -- sleep 300 >nul 2>&1
set BUSID=
for /l %%i in (1,1,120) do (
    for /f "tokens=1" %%a in ('usbipd list 2^>nul ^| findstr /c:"%VID%"') do set BUSID=%%a
    if not "!BUSID!"=="" goto :rfound
    ping -n 2 127.0.0.1 >nul
)
echo Не дождался телефона в fastboot
goto :fail
:rfound
usbipd attach --wsl --busid !BUSID! >nul 2>&1
ping -n 3 127.0.0.1 >nul
%WSL% bash -c "cd %ROOT% && bash tools/flash.sh --restore 2>&1 | grep -E 'Sending|Writing|Rebooting|НЕТ'"
echo Готово, заводской Android возвращён.
goto :end

:fail
echo.
echo ОШИБКА. Смотри сообщения выше.
exit /b 1

:end
endlocal
