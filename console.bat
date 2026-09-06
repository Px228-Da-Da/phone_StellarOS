@echo off
rem Терминал StellarOS: показывает вывод ядра, идущий по USB.
rem Порт ищется по идентификаторам устройства, а не по номеру COM.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\console.ps1"
