@echo off
setlocal
cd /d "%~dp0"
title FlowX 1.9.3 - Flasheo completo
echo ==========================================================
echo   FlowX Node v1.9.3 - FLASHEO COMPLETO (via USB)
echo   Borra TODO lo que tenga la placa (firmware viejo SK21
echo   incluido) y deja FlowX con configuracion de fabrica.
echo ==========================================================
echo.
echo 1) Conecta la placa por USB.
echo 2) Si al final falla, repeti manteniendo apretado el boton
echo    BOOT de la placa mientras enchufas el USB.
echo.
pause

set PORT=%~1
set PORTARG=
if not "%PORT%"=="" set PORTARG=--port %PORT%

echo.
echo === Paso 1/2: borrando flash completa ===
tools\esptool.exe --chip esp32 %PORTARG% --baud 460800 erase_flash
if errorlevel 1 goto :err

echo.
echo === Paso 2/2: grabando FlowX 1.9.3 + configuracion de fabrica ===
tools\esptool.exe --chip esp32 %PORTARG% --baud 460800 --before default_reset --after hard_reset write_flash -z --flash_mode dio --flash_freq 40m --flash_size detect 0x1000 bin\bootloader.bin 0x8000 bin\partitions.bin 0xe000 bin\boot_app0.bin 0x10000 bin\firmware.bin 0x3D0000 bin\littlefs.bin
if errorlevel 1 goto :err

echo.
echo ==========================================================
echo   LISTO. La placa se reinicia sola con FlowX 1.9.3.
echo   Busca la red WiFi  FX-xxxxxxxxx  (clave: 12345678)
echo   y entra a  http://192.168.4.1/p2  para configurar red.
echo ==========================================================
pause
exit /b 0

:err
echo.
echo ****** FALLO EL FLASHEO ******
echo - Repeti manteniendo apretado BOOT al enchufar el USB.
echo - Si la notebook tiene varios puertos COM, indicalo asi:
echo       FLASHEAR.bat COM5
echo   (el numero de COM se ve en el Administrador de dispositivos)
echo - Si NO aparece ningun puerto COM al conectar la placa,
echo   falta el driver USB-serie (CH340 o CP210x) - ver LEEME.md
pause
exit /b 1
