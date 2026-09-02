@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion

rem ============================================================
rem  VELO-OS - build and run in QEMU.
rem
rem  Usage:
rem     run.bat            build + run with screen window
rem     run.bat text       no graphics, UART log in this window
rem     run.bat phone      phone config: Cortex-A55, 8 cores, 4 GB
rem     run.bat shot       capture screen to PNG and open it
rem     run.bat clean      rebuild from scratch
rem
rem  Compiler lives in WSL, the screen window is opened by the
rem  Windows build of QEMU - so this file uses both.
rem
rem  NOTE: this file is deliberately ASCII-only. cmd parses a .bat
rem  in the codepage active when it opens the file, so a "chcp"
rem  inside the file cannot fix its own non-ASCII text - Cyrillic
rem  comments here would break parsing. The chcp above is for the
rem  OUTPUT: compiler messages and the kernel log are UTF-8, and
rem  they are the part that actually matters.
rem ============================================================

set "MODE=%~1"
if "%MODE%"=="" set "MODE=screen"

rem --- Paths are derived from this file, so the folder can be moved
set "PROJ=%~dp0"
if "%PROJ:~-1%"=="\" set "PROJ=%PROJ:~0,-1%"

for /f "delims=" %%i in ('wsl wslpath -a "%PROJ%" 2^>nul') do set "WPROJ=%%i"
if not defined WPROJ (
    echo [ERROR] WSL does not answer. It is required: the compiler lives there.
    goto :end
)

rem --- QEMU: look in PATH first, then in the usual install location
set "QEMU="
for /f "delims=" %%i in ('where qemu-system-aarch64.exe 2^>nul') do set "QEMU=%%i"
if not defined QEMU if exist "%ProgramFiles%\qemu\qemu-system-aarch64.exe" (
    set "QEMU=%ProgramFiles%\qemu\qemu-system-aarch64.exe"
)

rem --- phone build differs by RAM size, hence a separate board
set "BOARD=qemu"
if /i "%MODE%"=="phone" set "BOARD=phone"

if /i "%MODE%"=="clean" (
    echo === make clean ===
    wsl -u root bash -lc "cd '%WPROJ%/kernel' && make clean"
    set "MODE=screen"
)

echo === build (BOARD=%BOARD%) ===
wsl -u root bash -lc "cd '%WPROJ%/kernel' && make BOARD=%BOARD%" || goto :buildfail
echo.

set "IMG=%PROJ%\kernel\build\%BOARD%\Image"
if not exist "%IMG%" (
    echo [ERROR] image not found: %IMG%
    goto :end
)

rem --- Screenshot mode: no window needed, the WSL script does everything
if /i "%MODE%"=="shot" (
    echo === screenshot ===
    wsl -u root bash -lc "cd '%WPROJ%' && python3 tools/screenshot.py 3.5 kernel/build/velo-screen.png"
    if exist "%PROJ%\kernel\build\velo-screen.png" (
        start "" "%PROJ%\kernel\build\velo-screen.png"
    )
    goto :end
)

rem --- Text mode: no graphics, everything in this window
if /i "%MODE%"=="text" (
    echo === no graphics. exit: Ctrl+A then X ===
    wsl -u root bash -lc "cd '%WPROJ%/kernel' && make run"
    goto :end
)

rem --- Normal run: a window with the kernel's own screen
if not defined QEMU (
    echo [ERROR] qemu-system-aarch64.exe for Windows not found.
    echo Install:  winget install SoftwareFreedomConservancy.QEMU
    echo Or run without graphics:  run.bat text
    goto :end
)

set "CPU=cortex-a53"
set "RAM=1G"
if /i "%BOARD%"=="phone" (
    set "CPU=cortex-a55"
    set "RAM=4G"
)

echo === QEMU: %CPU%, 8 cores, %RAM% ===
echo     A separate window will show the kernel screen.
echo     The UART log stays here. Close the QEMU window to stop.
echo.

rem -device ramfb is the screen itself; without it the kernel
rem falls back to text-only, exactly as it would on a phone with no display
"%QEMU%" -M virt,gic-version=3 -cpu %CPU% -smp 8 -m %RAM% ^
    -device ramfb -serial stdio -kernel "%IMG%"

goto :end

:buildfail
echo.
echo [ERROR] build failed - compiler output above.

:end
echo.
pause
endlocal
