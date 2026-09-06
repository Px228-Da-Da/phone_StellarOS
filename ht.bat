@echo off
rem ============================================================
rem  ht - prilozheniya Hittis.
rem
rem    ht run   apps\demo.ht    okno na kompyutere, mysh kak palets
rem    ht build apps\demo.ht    sobrat .slt ryadom s ishodnikom
rem    ht shot  apps\demo.ht    snyat pervyy kadr v kartinku
rem    ht check                 progon vseh prilozheniy iz apps
rem
rem  Vsya rabota v tools/ht.sh: odno i to zhe rabotaet i tut, i v WSL.
rem ============================================================
chcp 65001 >nul
setlocal
set APP=%~2
if defined APP set APP=%APP:\=/%
wsl -e sh -lc "/mnt/d/OS_ANDROID/tools/ht.sh %1 '%APP%'"
endlocal
