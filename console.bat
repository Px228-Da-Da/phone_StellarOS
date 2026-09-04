@echo off
rem Терминал VELO-OS: показывает вывод ядра, идущий по USB.
rem Порт ищется по идентификаторам устройства, а не по номеру COM.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\console.ps1"
