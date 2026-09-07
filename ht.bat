@echo off
rem ============================================================
rem  ht - prilozheniya Hittis.
rem
rem    ht ide   [apps\demo.ht]  sreda razrabotki v brauzere
rem    ht send  apps\demo.ht    otpravit v telefon po provodu, bez proshivki
rem    ht run   apps\demo.ht    okno na kompyutere, mysh kak palets
rem    ht build apps\demo.ht    sobrat .slt ryadom s ishodnikom
rem    ht shot  apps\demo.ht    snyat pervyy kadr v kartinku
rem    ht check                 progon vseh prilozheniy iz apps
rem
rem  Vsya rabota v tools/ht.sh: odno i to zhe rabotaet i tut, i v WSL.
rem ============================================================
chcp 65001 >nul
setlocal enabledelayedexpansion
set APP=%~2
if defined APP set APP=%APP:\=/%

rem --- send: sobiraem v WSL, otpravlyaem iz Windows.
rem     Posledovatelnyy port telefona viden tolko iz Windows, a
rem     kompilyator zhivet v WSL - poetomu komanda iz dvuh polovin.
if /i "%~1"=="send" (
    if "%APP%"=="" (
        echo ukazhi prilozhenie: ht send apps\demo.ht
        goto :eof
    )
    wsl -e sh -lc "/mnt/d/OS_ANDROID/tools/ht.sh build '%APP%'"
    if errorlevel 1 goto :eof
    set "SLT=%~2"
    set "SLT=!SLT:.ht=.slt!"
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\send.ps1" "%~dp0!SLT!"
    goto :eof
)

wsl -e sh -lc "/mnt/d/OS_ANDROID/tools/ht.sh %1 '%APP%'"
endlocal
