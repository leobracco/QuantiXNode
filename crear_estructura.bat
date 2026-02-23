@echo off
setlocal EnableDelayedExpansion

echo ========================================================
echo      GENERADOR DE ESQUELETO SK21 (PLATFORMIO)
echo ========================================================

REM --- 1. Crear Directorios ---
if not exist include mkdir include
if not exist src mkdir src

REM --- 2. Crear Headers (.h) ---

echo [+] Creando include/Structs.h...
(
echo #ifndef STRUCTS_H
echo #define STRUCTS_H
echo.
echo #include ^<Arduino.h^>
echo.
echo // [IMPORTANTE] PEGA AQUI LAS DEFINICIONES DE:
echo // struct ModuleConfig { ... };
echo // struct SensorConfig { ... };
echo // struct ModuleNetwork { ... };
echo.
echo #endif
) > include\Structs.h

echo [+] Creando include/Globals.h...
(
echo #ifndef GLOBALS_H
echo #define GLOBALS_H
echo.
echo #include ^<Arduino.h^>
echo #include "Structs.h"
echo.
echo // [IMPORTANTE] PEGA AQUI LAS VARIABLES EXTERN Y PROTOTIPOS:
echo // extern ModuleConfig MDL;
echo // extern SensorConfig Sensor[];
echo // void SetPWM(...);
echo.
echo #endif
) > include\Globals.h

echo [+] Creando include/PCA95x5_RC.h...
(
echo #pragma once
echo // PEGA AQUI EL CONTENIDO DE LA LIBRERIA PCA95x5
) > include\PCA95x5_RC.h

REM --- 3. Crear Sources (.cpp) ---

echo [+] Creando src/Analog.cpp...
(
echo #include ^<Arduino.h^>
echo #include ^<Wire.h^>
echo "Globals.h"
echo.
echo // --- PEGA AQUI EL CONTENIDO DE Analog.ino ---
) > src\Analog.cpp

echo [+] Creando src/Begin.cpp...
(
echo #include ^<Arduino.h^>
echo #include ^<EEPROM.h^>
echo "Globals.h"
echo.
echo // --- PEGA AQUI EL CONTENIDO DE Begin.ino ---
) > src\Begin.cpp

echo [+] Creando src/GUI.cpp...
(
echo #include ^<Arduino.h^>
echo #include ^<WebServer.h^>
echo "Globals.h"
echo.
echo // --- PEGA AQUI EL CONTENIDO DE GUI.ino ---
) > src\GUI.cpp

echo [+] Creando src/Motor.cpp...
(
echo #include ^<Arduino.h^>
echo "Globals.h"
echo.
echo // --- PEGA AQUI EL CONTENIDO DE Motor.ino ---
) > src\Motor.cpp

echo [+] Creando src/PID.cpp...
(
echo #include ^<Arduino.h^>
echo "Globals.h"
echo.
echo // --- PEGA AQUI EL CONTENIDO DE PID.ino (VERSION MEJORADA) ---
) > src\PID.cpp

echo [+] Creando src/Rate.cpp...
(
echo #include ^<Arduino.h^>
echo "Globals.h"
echo.
echo // --- PEGA AQUI EL CONTENIDO DE Rate.ino (CON DEBOUNCE) ---
) > src\Rate.cpp

echo [+] Creando src/Receive.cpp...
(
echo #include ^<Arduino.h^>
echo #include ^<WiFiUdp.h^>
echo #include ^<EthernetUdp.h^>
echo "Globals.h"
echo.
echo // --- PEGA AQUI EL CONTENIDO DE Receive.ino ---
) > src\Receive.cpp

echo [+] Creando src/Relays.cpp...
(
echo #include ^<Arduino.h^>
echo #include ^<Adafruit_PWMServoDriver.h^>
echo #include ^<PCF8574.h^>
echo "Globals.h"
echo.
echo // --- PEGA AQUI EL CONTENIDO DE Relays.ino ---
) > src\Relays.cpp

echo [+] Creando src/Send.cpp...
(
echo #include ^<Arduino.h^>
echo #include ^<WiFi.h^>
echo #include ^<Ethernet.h^>
echo "Globals.h"
echo.
echo // --- PEGA AQUI EL CONTENIDO DE Send.ino ---
) > src\Send.cpp

echo [+] Creando src/WheelSpeed.cpp...
(
echo #include ^<Arduino.h^>
echo "Globals.h"
echo.
echo // --- PEGA AQUI EL CONTENIDO DE WheelSpeed.ino ---
) > src\WheelSpeed.cpp

echo [+] Creando src/PgNetwork.cpp...
(
echo #include ^<Arduino.h^>
echo "Globals.h"
echo.
echo // --- PEGA AQUI EL CONTENIDO DE PgNetwork.ino ---
) > src\PgNetwork.cpp

echo [+] Creando src/PgStart.cpp...
(
echo #include ^<Arduino.h^>
echo "Globals.h"
echo.
echo // --- PEGA AQUI EL CONTENIDO DE PgStart.ino ---
) > src\PgStart.cpp

echo [+] Creando src/PgSwitches.cpp...
(
echo #include ^<Arduino.h^>
echo "Globals.h"
echo.
echo // --- PEGA AQUI EL CONTENIDO DE PgSwitches.ino ---
) > src\PgSwitches.cpp

echo [+] Creando src/MQTT_Custom.cpp...
(
echo #include ^<Arduino.h^>
echo #include ^<WiFi.h^>
echo #include ^<PubSubClient.h^>
echo #include ^<ArduinoJson.h^>
echo "Structs.h"
echo "Globals.h"
echo.
echo // --- PEGA AQUI EL CODIGO MQTT QUE CREAMOS ---
) > src\MQTT_Custom.cpp

echo.
echo ========================================================
echo      ARCHIVOS CREADOS. LISTO PARA PEGAR CODIGO.
echo ========================================================
pause