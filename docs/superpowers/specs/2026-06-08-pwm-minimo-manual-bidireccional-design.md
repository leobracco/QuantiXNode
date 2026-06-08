# Diseño — Búsqueda manual de PWM mínimo bidireccional

Fecha: 2026-06-08
Producto: FlowX ESP32 firmware (`FlowXNode`)

## Objetivo

Hoy el firmware encuentra el `pwm_min` con una **rampa automática** 0→4095
(`caracterizar_start` → `CharStart`/`RunChar`/`CharStop` en `CalibAuto.cpp`).
Se quiere agregar una **búsqueda manual** del PWM mínimo, operada desde AOG por
MQTT, en la que el operario fija un PWM puntual, observa el caudal en vivo y
repite hasta hallar el umbral de arranque. La búsqueda debe cubrir ambos
sentidos del actuador bidireccional (PWM>0 abre, PWM<0 cierra) y persistir un
mínimo por sentido.

Decisiones tomadas (brainstorming):
- Modo manual = **PWM fijo set/observar** (el firmware solo aplica el PWM pedido).
- Persistir **dos** valores: `pwm_min` positivo (abrir) y negativo (cerrar).
- Operado **por MQTT desde AOG**; AOG manda el valor final a guardar.
- **Mantener** la rampa automática actual (`caracterizar_*`) en paralelo.
- El `pwm_min` negativo **alimenta el piso del PID** al cerrar.
- `manual_pwm` recibe un `value` **con signo** (−4095..+4095).

## No-objetivos (YAGNI)

- No se toca el portal web del nodo (la operación es MQTT-only).
- No se elimina ni modifica la rampa automática `caracterizar_*`.
- El `pwm_min` del mensaje `target` de AOG sigue mapeando solo al positivo
  (`CFG.pid.pwmMin`), por back-compat; el negativo se setea solo vía
  `save_pwm_min`.

## Contrato MQTT (nuevos verbos `agp/flow/{uid}/cmd/<verb>`)

| Verbo | Payload | Efecto | Ack |
|---|---|---|---|
| `manual_pwm` | `{producto_id, value}`, `value` ∈ −4095..+4095 | Entra a modo manual si no estaba activo; aplica el PWM con signo (>0 abre, <0 cierra); refresca el timeout de seguridad. | `manual_pwm_set` |
| `manual_stop` | `{producto_id}` | Sale de modo manual, PWM 0. | `manual_stopped` |
| `save_pwm_min` | `{producto_id, dir:"pos"\|"neg", value}`, `value` magnitud 0..4095 | Persiste el mínimo del sentido indicado y lo aplica al `Sensor`. | `pwm_min_saved` |

Notas:
- `value` de `save_pwm_min` es **magnitud** (positiva); el sentido lo da `dir`.
- El operario observa el caudal por el `status_live` ya existente (canal 0:
  `caudal_lmin`, `pwm`, `pid_estado`, etc.). En modo manual el PWM aplicado se
  refleja en `Sensor[0].PWM` para que aparezca en la telemetría.
- Los acks usan el envelope existente (`AgpEnvelope_publishAck`), igual que los
  demás verbos.
- Validaciones: `producto_id` (ID) dentro de rango (`< MaxProductCount`);
  `value` se `constrain`ea a −4095..+4095 (manual) y 0..4095 (save); `dir`
  desconocido → ack `rejected`.

## Módulo de modo manual (`CalibAuto.cpp`, patrón hermano de Calib/AutoTune/Char)

Estado nuevo por canal:

```c
struct ManualState {
    bool     active;
    float    value;       // PWM con signo aplicado
    uint32_t lastCmdMs;   // último manual_pwm recibido (comms-loss)
    uint32_t startMs;     // inicio del modo (timeout duro)
};
ManualState MAN[MaxProductCount];
```

Funciones (declaradas `extern` en `Globals.h`):
- `bool IsManualActive(byte ID)`
- `void ManualSet(byte ID, float value)` — activa si hace falta; cancela
  Calib/AutoTune/Char (`CalibStop`/`AutoTuneStop`/`CharStop`, todos pelean por
  el mismo PWM del canal); toma control (`FlowEnabled=true`, `PidState=0`);
  guarda `value`; setea `lastCmdMs`/`startMs`.
- `void ManualStop(byte ID)` — `active=false`, `SetPWM(ID,0)`, `Sensor[ID].PWM=0`.
- `void RunManual(byte ID)` — cada tick desde `main.loop()`: chequea timeouts;
  si ok, `SetPWM(ID, value)` y `Sensor[ID].PWM = value` (telemetría).

## Seguridad

- **Comms-loss**: si `millis() - MAN[ID].lastCmdMs > MANUAL_COMMS_TIMEOUT_MS`
  (4000 ms, alineado al failsafe de secciones de `Relays.cpp`) → `ManualStop`.
- **Timeout duro**: si `millis() - MAN[ID].startMs > MANUAL_HARD_TIMEOUT_MS`
  (300000 ms, como la calibración) → `ManualStop`.
- Ambos chequeos viven en `RunManual`.

## PID bidireccional con dos pisos (`PID.cpp`)

En la banda proporcional, el piso de magnitud se elige por sentido del error:

```c
float minFloor = (error > 0) ? Sensor[ID].MinPWM : Sensor[ID].MinPWMNeg;
float mag = minFloor + frac * (Sensor[ID].MaxPWM - minFloor);
output = (error > 0 ? +mag : -mag);
```

- `Sensor[ID].MinPWM`  = piso al **abrir** (positivo; el de hoy).
- `Sensor[ID].MinPWMNeg` = piso al **cerrar**.
- `MaxPWM` se mantiene compartido entre sentidos.

## Structs + persistencia

`include/Structs.h`:
- `SensorConfig`: agregar `int MinPWMNeg;` junto a `MinPWM, MaxPWM`.
- `FlowConfig.pid`: agregar `int pwmMinNeg;` (se mantiene `pwmMin` como el
  positivo, para no romper `config.json` ni el `pwm_min` del `target`).

`src/Begin.cpp`:
- `SaveData`: escribir `pid["pwmMinNeg"] = CFG.pid.pwmMinNeg;`.
- `LoadData`: `CFG.pid.pwmMinNeg = doc["FLOW"]["pid"]["pwmMinNeg"] | 800;` y
  `Sensor[i].MinPWMNeg = CFG.pid.pwmMinNeg;` (espejo de cómo se setea
  `MinPWM` desde `pwmMin`).
- `SetDefault`: `CFG.pid.pwmMinNeg = 800;` y `Sensor[i].MinPWMNeg = 0;`
  (mismo patrón que `pwmMin`/`MinPWM`).

`src/MQTT_Custom.cpp` — handler `save_pwm_min`:
- `dir=="pos"` → `CFG.pid.pwmMin = value; Sensor[ID].MinPWM = value;`
- `dir=="neg"` → `CFG.pid.pwmMinNeg = value; Sensor[ID].MinPWMNeg = value;`
- luego `SaveConfig();`

`data/config.json`: agregar `"pwmMinNeg": 800` bajo `FLOW.pid` (opcional; el
default del loader lo cubre).

## Integración en el loop (`src/main.cpp`)

El bloque por canal es una cadena `if/else-if`. Hoy es:
`IsCharActive → IsAutoTuneActive → CalibActive → PIDmotor`.
Se agrega `IsManualActive` como **primera** rama (toma control exclusivo del
PWM, igual que Char/AutoTune):

```c
if (IsManualActive(i))            RunManual(i);
else if (IsCharActive(i))         RunChar(i);
else if (IsAutoTuneActive(i))     RunCalibAuto(i);
else if (Sensor[i].CalibActive)   { /* open-loop calib actual */ }
else                              PIDmotor(i);
```

## Verificación

- No hay runner de tests (`test/` vacío) — no se agrega comando de test falso.
- Verificación: `pio run` (compila limpio) + prueba de banco con
  `pio device monitor` confirmando: aplica PWM con signo, timeouts cortan el
  motor, `save_pwm_min` persiste y sobrevive reboot, y el PID respeta el piso
  por sentido.

## Documentación a actualizar

- `CLAUDE.md`: agregar los verbos `manual_pwm`/`manual_stop`/`save_pwm_min` a
  la tabla del contrato MQTT y mencionar los dos pisos del PID.
- `docs/API.md`: documentar los nuevos verbos y el campo `pwmMinNeg`.

## Archivos tocados

`include/Structs.h`, `include/Globals.h`, `src/CalibAuto.cpp`, `src/PID.cpp`,
`src/Begin.cpp`, `src/MQTT_Custom.cpp`, `src/main.cpp`, `data/config.json`,
`CLAUDE.md`, `docs/API.md`.
