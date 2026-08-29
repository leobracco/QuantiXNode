@echo off
setlocal
cd /d "%~dp0"
title FlowX 1.9.3 - Actualizar firmware (conserva configuracion)
echo ==========================================================
echo   FlowX Node v1.9.3 - SOLO ACTUALIZAR FIRMWARE
echo   Usar en placas que YA corren FlowX: actualiza el
echo   firmware SIN borrar la configuracion ni el WiFi.
echo   (Para placas con software viejo SK21 usar FLASHEAR.bat)
echo ==========================================================
echo.
pause

set PORT=%~1
set PORTARG=
if not "%PORT%"=="" set PORTARG=--port %PORT%

tools\esptool.exe --chip esp32 %PORTARG% --baud 460800 --before default_reset --after hard_reset write_flash -z --flash_mode dio --flash_freq 40m --flash_size detect 0xe000 bin\boot_app0.bin 0x10000 bin\firmware.bin
if errorlevel 1 goto :err

echo.
echo   LISTO. Firmware actualizado a 1.9.3, configuracion intacta.
pause
exit /b 0

:err
echo.
echo ****** FALLO ******
echo - Repeti manteniendo apretado BOOT al enchufar el USB.
echo - Con varios puertos COM:  ACTUALIZAR_FIRMWARE.bat COM5
pause
exit /b 1
