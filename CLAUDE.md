# QuantiXNode — Memoria de proyecto

Este archivo es la guía para que Claude Code (y otras IAs) entiendan
rápidamente qué es QuantiXNode y cómo está organizado. Léelo al inicio de
cada sesión antes de tocar código.

## 1. Qué es

Firmware para un **nodo de control agrícola** basado en ESP32 (PlatformIO,
framework Arduino). Una placa puede gobernar hasta **2 motores dosificadores**
de manera independiente (p. ej. motor 0 = semilla, motor 1 = fertilizante) y
**16 relés** para cortes por secciones o surco a surco.

- Board: `esp32doit-devkit-v1`
- FS: LittleFS
- Libs externas: `PCF8574`, `Adafruit PWM Servo Driver (PCA9685)`,
  `PubSubClient`, `ArduinoJson 6`, `Ethernet`, `ESP2SOTA` (no usado).

## 2. Modelo mecánico (leer primero)

Cada motor mueve un engranaje; un sensor cuenta cada diente que pasa. Así es
como se traduce entre gramos y pulsos:

```
         motor
           │
           ▼
      ┌─────────┐     ┌───────────┐
      │engranaje│──►──│sensor pulso│
      └─────────┘     └───────────┘
      DientesPorVuelta
```

- **1 vuelta = `DientesPorVuelta` pulsos** (ej. 24).
- Durante **calibración** el operario le dice al sistema
  "quiero que gire N vueltas" → meta = `N × DientesPorVuelta` pulsos
  (ej. 10 vueltas × 24 = 240 pulsos). Al arrancar la calibración el firmware
  pone `TotalPulses = 0`, prende el motor con un PWM fijo y cuando
  `TotalPulses ≥ PulsosMetaCalibracion` lo apaga.
- Luego el operario pesa la dosis real (p. ej. 700 g en 10 surcos = 70 g por
  surco) y le dice al sistema "en estos 240 pulsos salieron 70 g".
  El backend calcula `GramosPorPulso = 70 / 240 ≈ 0.292 g/pulso` y se lo
  envía al firmware vía MQTT `config` (clave JSON `meter_cal`).
- En operación, el backend fija un **caudal objetivo** en pulsos por segundo
  (`pps`) vía MQTT `target`. El PID del motor ajusta el PWM hasta que la
  frecuencia medida (`Hz`) iguale al target. El backend convierte internamente
  entre gramos/segundo deseados y `pps` objetivo dividiendo por
  `GramosPorPulso`.

En el código, `UPM` y `TargetUPM` son **nombres heredados** (antes significaban
"Unidades Por Minuto"). Hoy almacenan **Hz (pulsos/segundo)**. No cambies el
nombre todavía porque el backend los espera así en JSON; está documentado en el
comentario de la struct.

## 3. Arquitectura del firmware

```
                      ┌──────────────┐
MQTT broker  ◄──────► │ MqttBroker    │ ◄── comandos entrantes
 (192.168.x:1883)     │ (MQTT_Custom) │
                      └──────┬───────┘
                             │
    ┌────────────────────────┼────────────────────────────┐
    │                        ▼                            │
    │            ┌────────────────────┐                   │
    │            │ Dispatcher callback│                   │
    │            │ (por tópico)       │                   │
    │            └──┬──┬──┬──┬──┬──┬──┘                   │
    │               │  │  │  │  │  │                      │
    │   config  target test cal relays cmd(reservado)     │
    │     │       │    │    │     │                       │
    │     ▼       ▼    ▼    ▼     ▼                       │
    │   Sensor[].*    PWM manual  RelayLo/Hi              │
    │                                                     │
    │            ┌────── loop() cada 50 ms ──────┐        │
    │            │                                │        │
    │   GetUPM → PIDmotor → CheckRelays → ReadAnalog → SendComm
    │   (pulsos)  (cálculo)   (hardware)   (presión)  (publica status)
    │                                                     │
    └─────────────────────────────────────────────────────┘

Interrupciones hardware:
 ISR_Sensor0/1 (RISING en FlowPin) → incrementa PulseCount[], escribe
 timestamps en Samples[] para que GetUPM calcule Hz en el loop.
```

### 3.1 Capas

| Capa                 | Dónde vive                     | Contiene                     |
|----------------------|--------------------------------|------------------------------|
| Lógica pura          | `include/core/*.h`             | PID, mediana, enums          |
| HAL (interfaces)     | `include/hal/*.h`              | IPwmDriver, IFilesystem      |
| HAL (adapters)       | `include/hal/*.h`              | LedcPwm, LittleFsFilesystem  |
| Infra                | `include/Log.h`, `Constants.h` | logging, números mágicos     |
| Dominio (globals)    | `src/*.cpp`, `include/Structs.h` | Sensor[], MDL, MDLnetwork  |
| Entrada/Salida       | MQTT_Custom.cpp, GUI.cpp       | red + portal web             |

### 3.2 Loop principal (`src/main.cpp`)

El loop corre cada `LOOP_INTERVAL_MS` (50 ms) y en cada ciclo:
1. `esp_task_wdt_reset()` — alimenta el watchdog (timeout `WatchdogSec`, def 5s).
2. `server.handleClient()` + `mqttLoop()` — sirve el portal web y procesa MQTT.
3. Por cada sensor (0..SensorCount-1):
   - Si `CalibrandoAhora`: mantiene PWM fijo hasta alcanzar `PulsosMetaCalibracion`.
   - Si no: ejecuta `PIDmotor(i)`.
4. `CheckRelays()` — aplica `RelayLo`/`RelayHi` al hardware elegido en `RelayControl`.
5. `ReadAnalog()` — lee ADS1115 si `ADSfound`.
6. `SendComm()` — publica status por MQTT cada `SEND_INTERVAL_MS` (200 ms).
7. `ResetTime` — si se activó desde GUI/MQTT, reinicia tras `RESTART_DELAY_MS`.

### 3.3 Modelo de datos

`include/Structs.h`:

```cpp
struct ModuleConfig MDL;         // config del módulo (una vez)
struct SensorConfig Sensor[2];   // config + estado por motor
struct ModuleNetwork MDLnetwork; // credenciales WiFi + MQTT
```

Campos clave de `SensorConfig` (nombre en código → significado):

| Código                   | Significado                                      |
|--------------------------|--------------------------------------------------|
| `FlowPin`                | GPIO donde llega el tren de pulsos del sensor    |
| `IN1`, `IN2`             | GPIOs del puente H (motor DC)                    |
| `DientesPorVuelta`       | dientes del engranaje (antes `PulsesPerRev`)     |
| `GramosPorPulso`         | constante de calibración (antes `MeterCal`)      |
| `CalibrandoAhora`        | `true` durante calibración (antes `CalibActive`) |
| `PulsosMetaCalibracion`  | meta de pulsos al calibrar                       |
| `TotalPulses`            | acumulador desde el último reset                 |
| `Hz`                     | frecuencia instantánea de pulsos (mediana)       |
| `UPM`                    | duplica `Hz` (nombre legacy)                     |
| `TargetUPM`              | objetivo en pulsos/seg desde backend (`pps`)     |
| `PWM`                    | salida calculada por el PID (0..PWM_MAX)         |
| `Kp`/`Ki`/`Kd`           | ganancias PID                                    |
| `MinPWM`/`MaxPWM`        | límites físicos del duty                         |
| `Deadband` (%)           | zona muerta para no "temblar" al llegar          |
| `SlewRate`               | rampa máxima de cambio de PWM por ciclo          |
| `PulseSampleSize`        | tamaño del buffer de mediana                     |
| `CommTime`               | timestamp último msg MQTT (dead-man's switch)    |

### 3.4 Globales de estado (en `main.cpp`)

| Global                   | Propósito                                          |
|--------------------------|----------------------------------------------------|
| `RelayLo`/`RelayHi`      | máscara 8+8 bits de relés deseados                 |
| `Button[16]`             | estado alternativo vía `WifiMasterOn`              |
| `WifiMasterOn`           | si `true`, se usan `Button[]` en vez de `RelayLo/Hi` (legacy; MQTT relays lo deja en `false`) |
| `WifiSwitchesTimer`      | ventana de `WifiTimeoutMs` para `WifiMasterOn`     |
| `ResetTime`              | si >0, reinicia el ESP tras `RESTART_DELAY_MS`     |
| `PressureReading`        | última lectura ADS1115 (sensor presión)            |
| `PCF_found`/`ADSfound`   | flags de presencia I²C                             |

## 4. Protocolo MQTT

Identidad única: `uid` = MAC en hex (12 chars). Todos los tópicos usan el
patrón `agp/quantix/{uid}/<canal>`.

### 4.1 Publicados por el firmware

| Tópico              | Cada  | Payload                                                         |
|---------------------|-------|-----------------------------------------------------------------|
| `status_live`       | 200ms | `{uid, id, id_placa, rpm, pulsos, pwm, pps_real, pps_target, meter_cal, load_pct, calibrando?, relays_mask}` |
| `agp/quantix/announcement` | al conectar | `{uid, ip, type:"MOTOR"}`                              |

`relays_mask` es un entero 16-bit con la máscara actual (`RelayHi<<8 | RelayLo`).

### 4.2 Suscritos por el firmware

Todos los tópicos que reciben input aceptan JSON. El callback valida rango,
tipo y existencia de claves antes de mutar estado.

#### `config` — parámetros persistentes (PID, calibración, límites)

```json
{
  "configs": [
    {
      "idx": 0,
      "config_pid":   {"kp": 2.5, "ki": 1.5, "kd": 0.0},
      "calibracion":  {"pwm_min": 150, "pwm_max": 4095},
      "meter_cal":    0.292
    }
  ]
}
```

Tras procesar, el firmware hace `SaveData()` a `config.json` en LittleFS
y llama a `ResetPIDState(id)` para limpiar el integrador.

#### `target` — caudal objetivo en tiempo real

```json
{"id": 0, "pps": 38.5, "seccion_on": true}
```

- `pps` es la frecuencia objetivo en pulsos/segundo.
- `seccion_on=false` corta (TargetUPM=0).
- Refresca `CommTime` del sensor (dead-man's switch).

#### `test` — slider manual PWM (web/debug)

```json
{"id": 1, "cmd": "start", "pwm": 1500}
```

- `cmd="start"` pone el motor en manual a ese PWM, `cmd="stop"` vuelve a auto.

#### `cal` — calibración controlada

```json
{"id": 0, "cmd": "start", "pulsos": 240, "pwm": 1000}
```

- `start` resetea `TotalPulses`, arranca con ese PWM y corta al llegar a
  `pulsos`. En el loop, `main.cpp` detecta la meta y devuelve a `AutoOn=true`.

#### `relays` — **corte por secciones o surco a surco** (nuevo)

Dos formatos, ambos válidos:

```json
// Actualización masiva: 16 bits, 1 bit por sección (bit 0..7 = bajos, 8..15 = altos)
{"mask": 65535}
{"mask": "0x00FF"}         // sólo las 8 bajas
{"low": 15, "high": 0}     // 4 bajas ON, resto OFF

// Surco a surco (toggle individual)
{"id": 3, "on": true}      // enciende la sección 3
{"id": 11, "on": false}    // apaga la sección 11
```

Efecto:
1. Actualiza `RelayLo`/`RelayHi`.
2. Pone `WifiMasterOn=false` (MQTT-driven).
3. Refresca `CommTime` de todos los sensores → mantiene vivo el dead-man's switch.

Si no llega un refresh dentro de `CommTimeoutMs` (def 4 s), `CheckRelays()`
apaga todas las salidas automáticamente. El backend debe republicar la máscara
al menos cada ~3 s para mantener secciones activas.

#### `cmd` — reservado

Declarado y suscrito pero sin handler aún. Lugar natural para futuros comandos
(`reboot`, `factory_reset`, `ota_start`...).

## 5. Configuración en LittleFS

### `/config.json`
Config del módulo y sensores. Se crea con defaults si falta. Formato
retrocompatible: campos no presentes caen a defaults via `doc["x"] | default`.

Campos añadidos en refactor: `LogLevel` (0..3), `HttpUser`, `HttpPass`,
`WatchdogSec`, `CommTimeoutMs`, `WifiTimeoutMs`.

### `/network.json`
Credenciales WiFi + broker MQTT. Excluido del repo por `.gitignore`; usar
`data/network.example.json` como plantilla.

Claves: `SSID`, `Password`, `Mode`, `mqtt_host`, `mqtt_port`, `mqtt_user`,
`mqtt_pass`, `mqtt_keepalive`.

## 6. Hardware y pines

- Encoder motor 0 (semilla): `FlowPin=17` por defecto. ISR sube `RISING`.
- Encoder motor 1 (fertilizante): `FlowPin=16`.
- Motor 0 puente H: `IN1=32`, `IN2=33`.
- Motor 1 puente H: `IN1=25`, `IN2=26`.
- PCA9685 en I²C `0x40` para relés modo 5 (16 canales).
- PCF8574 en I²C `0x20` para relés modo 6 (8 canales).
- ADS1115 en I²C `0x48` para sensor de presión (AIN0).

Resolución PWM: 12 bits → duty ∈ `[0, 4095]` (constante `PWM_MAX`).

## 7. Sistema de logging

`include/Log.h`. Niveles `ERROR/WARN/INFO/DEBUG`. Usar las macros:

```cpp
LOG_E("mqtt", "broker cayó, state=%d", rc);
LOG_I("boot", "QuantiX listo");
LOG_D("pid",  "M%u tgt=%.1f act=%.1f", id, t, a);
```

El nivel se fija desde `config.json` (`LogLevel`) al cargar. Las macros hacen
short-circuit: si el nivel está filtrado no se construyen argumentos.

## 8. Tests

```bash
pio test -e native
```

Compila `test/test_native/` contra `include/core/` usando Unity. Cubre PID
(target=0, anti-windup, deadband, slew rate, min-kick) y mediana (vacío,
único, todos iguales, outlier, monotónico, par). Sin hardware.

## 9. Convenciones que me gustaría mantener

- Comentarios en **español**, identificadores en **inglés** (con excepciones
  del dominio: `GramosPorPulso`, `DientesPorVuelta`, `CalibrandoAhora`,
  `PulsosMetaCalibracion`).
- Nunca meter lógica pesada en ISRs: sólo incrementar contadores y guardar
  timestamps en arrays `volatile`.
- Leer variables compartidas con ISR bajo `noInterrupts()/interrupts()`.
- No usar `String` de Arduino en el hot path (fragmenta heap en ESP32).
- No introducir `delay()` en el loop principal; preferir scheduler cooperativo
  por `millis()` o máquinas de estado.
- Cada nueva magic number → `Constants.h`.
- Cada nuevo log disperso → macro `LOG_*`.

## 10. Cosas pendientes / deuda técnica

- `GUI.cpp` define `HandleRoot/HandlePage1/HandlePage2` pero nunca llama a
  `server.on(...)` ni a `server.begin()`. El portal web no está realmente
  sirviendo; `GetPage0/1/2` ni siquiera tienen implementación. La auth básica
  está lista para el día que se cablee.
- `WheelSpeed.cpp` es un stub.
- `Receive.cpp` está vacío intencionalmente (control pasó a MQTT).
- `esp_task_wdt_init` en Arduino-ESP32 v3.x cambia de firma (toma struct).
  Si se actualiza el framework hay que ajustar `main.cpp:65`.
- El backend sigue hablando en claves JSON históricas (`meter_cal`, `pps`).
  Cuando se sincronice el backend se podrán renombrar también los identifiers
  del protocolo.
