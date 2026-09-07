@echo off
rem ============================================================
rem  ht - prilozheniya Hittis.
rem
rem    ht ide   [ishodniki\demo.ht]  sreda razrabotki v brauzere
rem    ht send  ishodniki\demo.ht    otpravit v telefon po provodu
rem    ht run   ishodniki\demo.ht    okno na kompyutere, mysh kak palets
rem
rem  Dve papki: v ishodniki/ tekst na Hittis, v apps/ gotovye .slt.
rem  Sistema chitaet tolko apps/ i nichego ne kompiliruet.
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
        echo ukazhi prilozhenie: ht send ishodniki\demo.ht
        goto :eof
    )
    wsl -e sh -lc "/mnt/d/OS_ANDROID/tools/ht.sh build '%APP%'"
    if errorlevel 1 goto :eof
    rem Gotovoe lezhit v apps/, imya beryom ot ishodnika bez papki.
    for %%F in ("%~2") do set "NAME=%%~nF"
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\send.ps1" "%~dp0apps\!NAME!.slt"
    goto :eof
)

wsl -e sh -lc "/mnt/d/OS_ANDROID/tools/ht.sh %1 '%APP%'"
endlocal
