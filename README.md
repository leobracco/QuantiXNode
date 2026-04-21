# QuantiXNode

Firmware ESP32 (PlatformIO/Arduino) para nodo de control agrícola/industrial:
control de hasta 2 motores con encoder de flujo y PID, relés (GPIO / PCA9685 /
PCF8574), sensor de presión ADS1115, MQTT y portal web de configuración WiFi.

## Compilación y flasheo

```bash
pio run -e esp32doit-devkit-v1            # Compilar firmware
pio run -e esp32doit-devkit-v1 -t upload  # Flashear por USB
pio run -t buildfs -t uploadfs            # Subir contenido de data/ a LittleFS
```

## Provisioning inicial

Las credenciales reales no se versionan. El repo trae `data/network.json` con
campos vacíos (dispositivo arranca en modo AP `Quantix_AP`). Para desarrollo
local:

```bash
cp data/network.example.json data/network.json
$EDITOR data/network.json                       # Poner SSID / password / mqtt_host
git update-index --skip-worktree data/network.json  # Evita commits accidentales
pio run -t buildfs -t uploadfs
```

Si se prefiere provisionar sin tocar el FS: arrancar el dispositivo, conectarse
al AP `Quantix_AP` con la contraseña por defecto y usar el portal web
(`http://192.168.4.1`) para introducir SSID, password y host MQTT.

## Configuración

- `data/config.json` — parámetros del módulo (pines, PID, relés, timeouts).
  Los campos opcionales caen a defaults si faltan, así que configs antiguas
  siguen funcionando tras actualizar firmware.
- `data/network.json` — credenciales WiFi y MQTT (no versionado).

## Estructura del código

- `src/main.cpp` — `setup()` y `loop()` con scheduler cooperativo.
- `src/Begin.cpp` — bootstrap de hardware y carga de configuración.
- `src/Rate.cpp` — ISR de encoders y cálculo de Hz / UPM / RPM.
- `src/PID.cpp` / `src/Motor.cpp` — control PID y salida PWM.
- `src/Relays.cpp` — gestión de relés por driver.
- `src/Analog.cpp` — lectura del ADS1115.
- `src/MQTT_Custom.cpp` — publicación de estado y callback de comandos.
- `src/GUI.cpp` — portal web (solo setup WiFi, con auth básico).
- `include/Structs.h`, `include/Globals.h` — modelos y declaraciones comunes.

## Seguridad

- Las credenciales reales **no** deben commitearse. El `.gitignore` excluye
  `data/network.json` y `data/config.local.json`.
- El portal web exige HTTP Basic Auth con `HttpUser` / `HttpPass` del config.
- MQTT por ahora viaja en claro en la LAN; TLS queda fuera del alcance actual.

Si credenciales reales se hubieran subido en el pasado, rota las passwords
del entorno: la reescritura del historial git se hace manualmente con
`git filter-repo`.
