# FlowXNode — API MQTT (referencia)

Documento de referencia del contrato MQTT que habla este firmware con AgIO/AOG.
Pensado para usar como contexto en sesiones futuras de Claude (o cualquier
integrador). Refleja el estado actual del código en `src/MQTT_Custom.cpp` y los
consumidores en AOG (`FlowXBridge.cs`, `FlowXLiveService.cs`, `FlowXController.cs`,
`NodoRegistryService.cs`).

> **Idioma:** todo el ecosistema Agro Parallel responde en castellano rioplatense.
> Código y nombres de campos JSON quedan en inglés snake_case.

---

## 1. Identidad del nodo

- **UID** = `efuse_MAC` formateado como `%04X%08X` (12 hex, mayúsculas).
  Ejemplo: `A1B2C3D4E5F6`.
- **Producto** = `flow` (lowercase) en el topic.
- **Tipo** = `FlowX` en el payload del announcement.
- **Hardware** = `SK21` (placa actual).
- **Versión firmware** = `FW_VERSION` (definido en `Globals.h`).

## 2. Broker

- **Embebido en AgIO (MQTTnet)** corriendo en la PC del tractor.
- **Default:** `192.168.5.10:1883`. Configurable per-nodo desde el portal web
  → persiste en `/network.json` (`MDLnetwork.BrokerHost` / `BrokerPort`).
- **QoS:** 0 en publish/subscribe (PubSubClient). Excepción: Last Will = QoS 1 retained.
- **Keepalive:** 15 s. Socket timeout: 2 s.
- **Buffer:** 1024 B (PubSubClient descarta silenciosamente paquetes mayores).

## 3. Patrón de topics

Forma canónica de 4 partes:

```
agp/{producto}/{uid}/{verbo}
```

`NodoRegistryService` en AOG suscribe a `agp/+/+/announcement` y
`agp/+/+/status_live` y **descarta cualquier topic con menos de 4 partes**.

| Topic | Dir | QoS | Retained |
|---|---|---|---|
| `agp/flow/{uid}/announcement`     | Node→PC | 0 | ✔ |
| `agp/flow/announcement`           | Node→PC | 0 | ✖ (legacy, mirror del anterior) |
| `agp/flow/{uid}/lwt`              | Broker→PC | 1 | ✔ (Last Will) |
| `agp/flow/{uid}/status_live`      | Node→PC | 0 | ✖ |
| `agp/flow/{uid}/target`           | PC→Node | 0 | ✖ |
| `agp/flow/{uid}/config`           | PC→Node | 0 | ✖ |
| `agp/flow/{uid}/cmd/{verb}`       | PC→Node | 0 | ✖ |
| `agp/flow/{uid}/ack`              | Node→PC | 0 | ✖ (envelope) |
| `agp/flow/{uid}/calibrar_result`  | Node→PC | 0 | ✔ |
| `agp/flow/{uid}/autotune_result`  | Node→PC | 0 | ✔ |
| `agp/flow/{uid}/caracterizar_result` | Node→PC | 0 | ✔ |
| `agp/flow/{uid}/ota/resultado`    | Node→PC | 0 | ✖ |

---

## 4. Mensajes Node → PC

### 4.1 `announcement` (retained)

Se publica:
- al reconectar al broker,
- cada **10 s** desde `mqttLoop()` (para sobrevivir reinicios de AgIO),
- mirror al topic legacy `agp/flow/announcement` (no retained).

```json
{
  "schema": "agp.flow.announcement/1",
  "seq": 42,
  "uid": "A1B2C3D4E5F6",
  "ip": "192.168.5.123",
  "ap_ip": "192.168.4.1",
  "type": "FlowX",
  "device": "flow",
  "fw": "1.9.0",
  "hw": "SK21",
  "rssi": -67,
  "uptime": 1234,
  "wifi": "AgIO_AP",
  "boot_reason": "poweron",
  "safe_mode": false,
  "crash_count": 0
}
```

- `seq`: contador monotónico. Si vuelve a 0 → el nodo rebooteó.
- `boot_reason`: `poweron | task_wdt | brownout | panic | sw_reset | …`.
- `safe_mode`: `true` cuando hubo ≥3 crashes seguidos. En ese estado el nodo
  **solo acepta** `ping` y `clear_safe_mode`.

### 4.2 `lwt` (Last Will Testament)

Publicado por el broker cuando detecta caída TCP no limpia.

```json
{ "online": false, "reason": "unexpected_disconnect" }
```

Al reconectar, el nodo lo sobrescribe inmediatamente con:

```json
{ "online": true }
```

### 4.3 `status_live` (throttled 200 ms)

**Solo se publica para canal 0** (`producto_id = 0`). AOG indexa por `uid` en
`FlowXLiveService`; publicar 2 canales haría que uno pise al otro.

```json
{
  "schema": "agp.flow.status_live/1",
  "seq": 1543,
  "uid": "A1B2C3D4E5F6",
  "id": 0,
  "producto_id": 0,
  "id_placa": 79,
  "caudal_lmin": 12.34,
  "target_lmin": 12.00,
  "error_lmin": -0.34,
  "pwm": 1820,
  "pid_estado": "ok",

  "lmin": 12.34,
  "pps_target": 12.00,
  "sec_on": true,
  "pulsos": 18432
}
```

Campos canónicos (lee AOG): `caudal_lmin`, `target_lmin`, `error_lmin`, `pwm`,
`pid_estado`. Las claves `lmin`/`pps_target`/`sec_on`/`pulsos` quedan por
compatibilidad legacy.

`pid_estado` ∈ `off | sin_pulsos | saturado | ok`.

> **Nota:** las unidades del firmware son **Hz/UPM** (pulsos por segundo). El
> nombre `*_lmin` se conserva por compatibilidad con el bridge — la conversión
> a L/min real ocurre del lado del bridge usando `MeterCal`.

### 4.4 `calibrar_result` (retained)

```json
{
  "uid": "A1B2C3D4E5F6",
  "producto_id": 0,
  "ok": true,
  "pulsos": 1450,
  "duration_ms": 8234,
  "error": ""
}
```

AOG calcula `meter_cal = pulsos / vol_l` a partir de este payload.

### 4.5 `autotune_result` (retained)

```json
{
  "uid": "A1B2C3D4E5F6",
  "producto_id": 0,
  "ok": true,
  "kp": 18.5,
  "ki": 42.1,
  "kd": 0.83,
  "ku": 30.8,
  "tu_ms": 880,
  "error": ""
}
```

Fórmulas Ziegler-Nichols relay feedback:
`Ku = 4·h/(π·a)` · `Kp = 0.6·Ku` · `Ki = 1.2·Ku/Tu` · `Kd = 0.075·Ku·Tu`.

### 4.6 `caracterizar_result` (retained)

Resultado del barrido automático PWM 0→4095 (verbo `caracterizar_start`). La UI
de PilotX grafica las curvas `pwm[]` / `hz[]` y usa `pwm_min` como sugerencia
del piso de arranque del actuador.

```json
{
  "uid": "A1B2C3D4E5F6",
  "producto_id": 0,
  "ok": true,
  "pwm_min": 2480,
  "pwm_min_estable": 2700,
  "hz_max": 38.5,
  "lmin_max": 46.2,
  "error": "",
  "pwm": [0, 256, 512, 768, 1024, 1280, 1536, 1792, 2048, 2304, 2560, 2816],
  "hz":  [0, 0,   0,   0,    0,    0,    0,    0,    0,    2.1,  8.4,  15.7]
}
```

- `pwm_min`: primer PWM donde el motor reacciona (Hz > 0).
- `pwm_min_estable`: PWM donde el caudal queda estable.
- `hz_max` / `lmin_max`: techo del caudalímetro/bomba (L/min vía `MeterCal`).
- `pwm[]` / `hz[]`: curva completa del barrido (mismo largo, punto a punto).

> **Sentido único:** la caracterización barre solo en sentido **abrir** (PWM ≥ 0).
> El piso del sentido **cerrar** (`pwm_min` negativo) se busca con el modo manual
> (ver `manual_pwm` / `save_pwm_min`).

### 4.7 `ota/resultado`

```json
{
  "uid": "A1B2C3D4E5F6",
  "status": "ok",
  "version": "1.3.1",
  "detalle": ""
}
```

`status` ∈ `ok | error | progress`. En errores `detalle` lleva el motivo
(`parametros_faltantes`, `sha_mismatch`, `download_failed`, …).

### 4.8 `ack` (Tanda 2 — envelope cmd_id)

Topic: `agp/flow/{uid}/ack`. Publicado para cualquier `cmd/*` que traiga
`_meta.cmd_id` en el payload (sin `cmd_id` no se publica ack).

```json
{
  "cmd_id": "uuid-1234",
  "status": "ok",
  "detail": "calib_started",
  "ts_ms": 1834567
}
```

- `ts_ms`: `millis()` del nodo (no Unix) — sirve para medir latencia, no fecha.
- `status` ∈ `ok | rejected`.

Detalles típicos por verbo:

| `detail` | Cuándo |
|---|---|
| `pong` | `ping` |
| `safe_mode_cleared` | `clear_safe_mode` |
| `ota_queued` | `ota` encolada |
| `calib_started` / `calib_stopped` | calibración |
| `autotune_started` / `autotune_stopped` | auto-tune |
| `char_started` / `char_stopped` | caracterización |
| `manual_pwm_set` / `manual_stopped` | modo manual |
| `pwm_min_saved` | `save_pwm_min` ok |
| `duplicate` | cmd_id repetido (no se re-ejecuta) |
| `expired` | TTL vencido (rechazado) |
| `safe_mode_active` | nodo en safe-mode (rechazado) |
| `parametros_faltantes` | falta `url`/`version` en OTA (rechazado) |
| `dir_invalido` | `save_pwm_min` sin `dir` válido (rechazado) |
| `unknown_verb` | verbo desconocido (rechazado) |

---

## 5. Mensajes PC → Node

### 5.1 `target` — setpoint runtime

Publicado cada vez que cambia velocidad, dosis o secciones.

```json
{
  "t": 12.5,
  "sec": [1, 0, 1, 0, 0, 0, 0, 0],
  "pwm_min": 200,
  "pwm_max": 4095,
  "pid": { "kp": 18.5, "ki": 42.1, "kd": 0.83 }
}
```

- `t`: target en L/min (el firmware lo convierte a Hz vía `MeterCal/60`).
- `sec`: array de 0/1, hasta 16 secciones. `sec[i]=1` → bit `i` del bitmask.
- `pwm_min`, `pwm_max` y `pid` son opcionales — solo se aplican si vienen.
- `pwm_min`: piso del PID en sentido **abrir**. (El piso de **cerrar** se
  persiste aparte con `save_pwm_min dir:"neg"` — ver 5.3.)
- `pwm_max`: techo del PID. Si llega `0`, `≤ pwm_min` o `> 4095`, el firmware lo
  fuerza a `4095`.
- El nodo enciende Master + secciones automáticamente vía `processValves()`.
- **Failsafe:** si pasan **4 s** sin recibir `target`, el firmware cierra todas
  las secciones (`Relays.cpp::CheckRelays`).

### 5.2 `config` — hardware/electroválvulas (persiste)

Todos los campos opcionales. Cualquier cambio dispara `processValves()` para
aplicarse al toque y se guarda en `/config.json` de forma diferida.

```json
{
  "meterCal": 50.0,
  "is3Wire": true,
  "invertRelay": false,
  "invertMotor": false,
  "sectionIs3Wire": [-1, 1, 0, -1, -1, -1, -1, -1, -1, -1]
}
```

| Campo | Tipo | Significado |
|---|---|---|
| `meterCal` | float | Pulsos por litro del caudalímetro (canal 0). |
| `is3Wire`  | bool  | Default global: `true` = 3 cables (on/off), `false` = 2 cables (inversión de polaridad pinA/pinB). |
| `invertRelay` | bool | Invierte la lógica del Master y de las salidas 3-wire (módulos relé activos-en-LOW). |
| `invertMotor` | bool | Invierte el sentido del actuador (intercambia abrir/cerrar del H-bridge del motor/válvula). |
| `sectionIs3Wire` | int8[10] | Override per-sección. Valores válidos: `-1` (usa global), `0` (forzar 2-cables), `1` (forzar 3-cables). Cualquier otro valor se sanitiza a `-1`. |

> **Modelo 3 cables:** `pinA` da `HIGH` para abrir, `LOW` para cerrar. `pinB` no se usa.
> **Modelo 2 cables (motorizada/latching):** `pinA`/`pinB` en H-bridge. Abrir = `A↑ B↓`, cerrar = `A↓ B↑`. `invertRelay` **no aplica** acá: es polaridad, no lógica.

### 5.3 `cmd/{verb}` — comandos

Verbo en el topic, JSON en el body. Soporta envelope opcional (`_meta.cmd_id`,
`_meta.ttl_ms`, `_meta.ts_ms`) que dispara dedup + TTL + ack automático.

#### `ping`
```json
{}
```
→ ack `ok / pong`. Único verbo (junto a `clear_safe_mode`) permitido en safe-mode.

#### `clear_safe_mode`
```json
{}
```
→ resetea `safe_mode` y `crash_count`.

#### `ota`
```json
{
  "url": "http://192.168.5.10:8080/firmware/FlowX/1.3.1/firmware.bin",
  "version": "1.3.1",
  "sha256": "abc123…"
}
```
- `url` y `version` obligatorios; falta cualquiera → ack `rejected / parametros_faltantes`.
- `sha256` opcional pero recomendado (64 hex). Sin él, no se verifica integridad.
- El nodo cierra Master + secciones, frena PWM, aborta calib/autotune y
  ejecuta `HTTPUpdate`. Reboot automático al terminar.

#### `calibrar_start`
```json
{
  "vol_l": 1.0,
  "pwm": 2048,
  "producto_id": 0
}
```
- Resetea `TotalPulses=0`, fija `ManualAdjust=pwm`, `CalibActive=true`.
- Loop principal aplica `SetPWM(producto_id, pwm)` continuamente.
- Timeout duro: **5 min**.
- Termina al recibir `calibrar_stop` o al alcanzar `CalibTargetPulses` (si está seteado).
- Publica `calibrar_result` (retained).

#### `calibrar_stop`
```json
{ "producto_id": 0 }
```

#### `autotune_start`
```json
{
  "setpoint_hz": 5.0,
  "pwm_high": 4095,
  "pwm_low": 200,
  "producto_id": 0
}
```
- Relay feedback Ziegler-Nichols: alterna `pwm_high` / `pwm_low` cruzando
  `setpoint_hz`. Mide período `Tu` y amplitud de salida `a`.
- Timeout: **30 s**. Muestras: 4–8.
- Aplica los gains al `Sensor[producto_id]` automáticamente.
- Publica `autotune_result` (retained).

#### `autotune_stop`
```json
{ "producto_id": 0 }
```

#### `caracterizar_start`
```json
{ "producto_id": 0 }
```
- Barrido automático PWM 0→4095 (sentido abrir) con dwell por paso, midiendo Hz.
- Requiere Master + al menos una sección abierta y bomba con líquido circulando
  (lo valida `CharStart()`).
- Toma control exclusivo del PWM — el PID no corre mientras dura.
- Publica `caracterizar_result` (retained) con la curva + `pwm_min`/`hz_max`.

#### `caracterizar_stop`
```json
{ "producto_id": 0 }
```

#### `manual_pwm` — búsqueda manual de `pwm_min` (bidireccional)

El operario fija un PWM **con signo** y observa el caudal en vivo por
`status_live` (`pwm` + `caudal_lmin`). Sirve para encontrar a mano el piso de
arranque del actuador en cada sentido.

```json
{ "value": 2500, "producto_id": 0 }
```

- `value`: PWM con signo, rango **-4095..+4095**. `> 0` = **abrir**, `< 0` =
  **cerrar**, `0` = motor parado (mantiene posición).
- Toma control **exclusivo** del PWM: cancela calibración/autotune/caracterización
  y el PID no corre mientras el modo manual está activo.
- **Failsafe:** si pasan **4 s** sin un nuevo `manual_pwm`, el firmware corta el
  PWM. Timeout duro de seguridad: **5 min**. Para refrescar, re-publicar el
  valor periódicamente (la UI debería mandarlo cada ~1-2 s).
- → ack `ok / manual_pwm_set`.

#### `manual_stop`
```json
{ "producto_id": 0 }
```
Sale del modo manual, corta el PWM y devuelve el control al PID. → ack `ok / manual_stopped`.

#### `save_pwm_min` — persiste el piso de PWM por sentido

```json
{ "dir": "pos", "value": 2480, "producto_id": 0 }
```

- `dir`: `"pos"` (sentido abrir → `MinPWM`) o `"neg"` (sentido cerrar → `MinPWMNeg`).
- `value`: **magnitud** del piso, `0..4095` (se constrainea). Siempre positivo,
  aún para `dir:"neg"` — el signo lo aplica el PID según el sentido del error.
- Persiste en `/config.json` (`FLOW.pid.pwmMin` / `FLOW.pid.pwmMinNeg`) vía
  `SaveConfig()` y lo aplica de inmediato al canal.
- `dir` distinto de `pos`/`neg` → ack `rejected / dir_invalido`.
- → ack `ok / pwm_min_saved`.

> **Por qué dos pisos:** el actuador es una válvula motorizada (integrador) —
> el PWM es la **velocidad** del motor con signo, no la posición. Abrir y cerrar
> suelen tener fricción mecánica distinta, así que cada sentido necesita su
> propio piso para vencerla. El PID (`PID.cpp`) elige `MinPWM` cuando `error > 0`
> (abrir) y `MinPWMNeg` cuando `error < 0` (cerrar).

### 5.4 Envelope `_meta` (opcional, Tanda 2)

Sobre cualquier `cmd/*`:

```json
{
  "vol_l": 1.0,
  "pwm": 2048,
  "_meta": {
    "cmd_id": "uuid-1234",
    "ts_ms": 1733088000000,
    "ttl_ms": 5000,
    "source": "PilotX",
    "ver": 1,
    "seq": 17
  }
}
```

| Campo `_meta` | Tipo | Uso |
|---|---|---|
| `cmd_id` | string ≤24 | **Obligatorio** para que haya ack. Dedup ring de **64** entradas: si se repite → ack `ok / duplicate`, no se re-ejecuta. |
| `ts_ms` | int64 | Unix ms del emisor. Ancla el reloj local del nodo (sin RTC) para evaluar TTL. |
| `ttl_ms` | int | Si `now_estimado - ts_ms > ttl_ms` → ack `rejected / expired`, no se ejecuta. `0` = sin TTL. |
| `source` | string ≤15 | Identificador del emisor (ej. `PilotX`). Informativo. |
| `ver` | int | Versión del envelope (default 1). |
| `seq` | long | Secuencia del emisor. Informativo. |

- Sin `_meta` (o sin `cmd_id`): el verbo se ejecuta normal pero **no** se publica ack.
- El TTL usa un ancla epoch-local: el primer `ts_ms` recibido fija el offset
  contra `millis()`. Hasta que llega ese primer cmd, **nada expira** (se prefiere
  ejecutar antes que rechazar por falta de reloj).

---

## 6. Convenciones y reglas no-evidentes

- **`producto_id`** (también acepta `producto`): índice de canal, default 0. Si
  `≥ MaxProductCount` (=2), se fuerza a 0.
- **`MaxProductCount = 2`** (`Structs.h`). Típicamente canal 0 = semilla,
  canal 1 = fertilizante.
- **Piso de PWM bidireccional:** el PID usa `MinPWM` para abrir (`error > 0`) y
  `MinPWMNeg` para cerrar (`error < 0`). Se persisten en `FLOW.pid.pwmMin` /
  `FLOW.pid.pwmMinNeg`. `pwm_min` del `target` solo toca el de abrir; para el de
  cerrar usar `cmd/save_pwm_min dir:"neg"`.
- **Modo manual exclusivo:** `manual_pwm` toma control del PWM y desactiva PID,
  calibración, autotune y caracterización del canal hasta `manual_stop` (o
  failsafe 4 s / timeout 5 min). Es la herramienta para hallar `pwm_min` a mano.
- **Failsafe comms-loss:** 4 s sin `target` → `processValves(0)`. No bypasear.
- **Pin sentinel:** `NC = 255` en `Structs.h`. Cualquier helper de pin debe
  chequear `!= NC` antes de escribir.
- **`Receive.cpp` está vacío a propósito.** No reactivar el path UDP/CRC viejo.
- **Channel-0-only para `status_live`** hasta que AOG soporte multi-producto.
  Cambiarlo requiere coordinación AOG-side (sub-topic per `producto_id`).
- **Cualquier `SaveConfig()` en el callback MQTT bloquea el cliente.** Usar
  el flag `mqttPendingSave = true` y dejar que `processPendingSave()` corra
  desde `loop()`.
- **Defaults SK21:** `FlowPin=17`, `RegA=32`, `RegB=33`, `Master=14`,
  3 pares de sección configurados.
- **`MDL.APpassword` default:** `"quantix_admin"` (leftover de QuantiX —
  cambiar a `12345678` desde el portal o en `Begin.cpp::SetDefault`).
- **WiFi AP siempre online** (`WIFI_AP_STA` desde `Begin.cpp`). Watchdog cada
  5 s en `mqttLoop()` re-levanta el softAP si el core lo tiró.
- **SSID del AP:** `FX-{últimos 9 hex del UID}` (ej. `FX-C3D4E5F6`).

---

## 7. Ejemplos de sesión

### 7.1 Suscribirse a todo lo de un nodo (mosquitto_sub)

```bash
mosquitto_sub -h 192.168.5.10 -t 'agp/flow/A1B2C3D4E5F6/#' -v
```

### 7.2 Mandar un target manual

```bash
mosquitto_pub -h 192.168.5.10 \
  -t 'agp/flow/A1B2C3D4E5F6/target' \
  -m '{"t":15.0,"sec":[1,1,0,0,0,0,0,0]}'
```

### 7.3 Configurar electroválvulas mixtas

(secciones 0 y 1 con válvulas 3-cables, sección 2 con 2-cables motorizada,
el resto sigue el default global)

```bash
mosquitto_pub -h 192.168.5.10 \
  -t 'agp/flow/A1B2C3D4E5F6/config' \
  -m '{"sectionIs3Wire":[1,1,0,-1,-1,-1,-1,-1,-1,-1]}'
```

### 7.4 Disparar calibración con envelope

```bash
mosquitto_pub -h 192.168.5.10 \
  -t 'agp/flow/A1B2C3D4E5F6/cmd/calibrar_start' \
  -m '{"vol_l":2.0,"pwm":2500,"producto_id":0,
       "_meta":{"cmd_id":"cal-001","ts_ms":1733088000000,"ttl_ms":10000}}'
```

### 7.5 Disparar auto-tune

```bash
mosquitto_pub -h 192.168.5.10 \
  -t 'agp/flow/A1B2C3D4E5F6/cmd/autotune_start' \
  -m '{"setpoint_hz":5.0,"pwm_high":4095,"pwm_low":200,"producto_id":0}'
```

### 7.6 OTA manual

```bash
mosquitto_pub -h 192.168.5.10 \
  -t 'agp/flow/A1B2C3D4E5F6/cmd/ota' \
  -m '{"url":"http://192.168.5.10:8080/firmware/FlowX/1.9.0/firmware.bin",
       "version":"1.9.0",
       "sha256":"<64hex>"}'
```

### 7.7 Buscar `pwm_min` a mano y guardarlo

Abrir despacio hasta ver caudal, observar el `pwm` en `status_live`, y guardar
ese valor como piso del sentido abrir:

```bash
# 1. probar PWM en sentido abrir (repetir cada ~1-2 s para refrescar failsafe)
mosquitto_pub -h 192.168.5.10 \
  -t 'agp/flow/A1B2C3D4E5F6/cmd/manual_pwm' -m '{"value":2480,"producto_id":0}'

# 2. probar sentido cerrar (valor negativo)
mosquitto_pub -h 192.168.5.10 \
  -t 'agp/flow/A1B2C3D4E5F6/cmd/manual_pwm' -m '{"value":-2200,"producto_id":0}'

# 3. salir del modo manual
mosquitto_pub -h 192.168.5.10 \
  -t 'agp/flow/A1B2C3D4E5F6/cmd/manual_stop' -m '{"producto_id":0}'

# 4. persistir cada piso (magnitud positiva, el signo lo pone el PID)
mosquitto_pub -h 192.168.5.10 \
  -t 'agp/flow/A1B2C3D4E5F6/cmd/save_pwm_min' -m '{"dir":"pos","value":2480}'
mosquitto_pub -h 192.168.5.10 \
  -t 'agp/flow/A1B2C3D4E5F6/cmd/save_pwm_min' -m '{"dir":"neg","value":2200}'
```

### 7.8 Caracterización automática (curva PWM→Hz)

```bash
mosquitto_pub -h 192.168.5.10 \
  -t 'agp/flow/A1B2C3D4E5F6/cmd/caracterizar_start' -m '{"producto_id":0}'
# … leer agp/flow/A1B2C3D4E5F6/caracterizar_result (retained) …
```

---

## 8. Mapa de archivos relevantes

| Archivo | Para qué |
|---|---|
| `src/MQTT_Custom.cpp` | Topics, callback, publish helpers, reconnect, AP watchdog |
| `src/CalibAuto.cpp`   | State machine de calibración + auto-tune Ziegler-Nichols |
| `src/Relays.cpp`      | `processValves()` + override 2/3 cables per sección |
| `src/Rate.cpp`        | ISRs de pulsos, Hz/UPM, debounce 250 µs |
| `src/PID.cpp`         | Lazo PID con anti-windup, `pid_estado` |
| `src/Send.cpp`        | Throttle de `status_live` (200 ms) |
| `src/Begin.cpp`       | `SetDefault`, `Save/LoadData`, `Save/LoadNetworks` |
| `include/Structs.h`   | `FlowConfig`, `SensorConfig`, `ModuleConfig`, `ModuleNetwork` |
| `include/Globals.h`   | Externs + prototipos (`publicarAnnouncement`, `processPendingSave`, …) |
| `data/config.json`    | Imagen LittleFS inicial |
| `data/network.json`   | Imagen LittleFS inicial |

## 9. Cambios sensibles (coordinación AOG-side)

Cualquier modificación de los items de abajo **requiere también tocar** el
bridge en AOG (`SourceCode/GPS/AgroParallel/FlowX/`):

- Forma de los topics (especialmente el patrón de 4 partes).
- Nombres de campos canónicos en `status_live` (`caudal_lmin`, `target_lmin`,
  `error_lmin`, `pwm`, `pid_estado`).
- Esquema de `calibrar_result` / `autotune_result` / `caracterizar_result`.
- Verbos aceptados en `cmd/*` (`calibrar_*`, `autotune_*`, `caracterizar_*`,
  `manual_pwm`, `manual_stop`, `save_pwm_min`, `ota`, `ping`, `clear_safe_mode`).
- Semántica del piso bidireccional (`pwm_min` abrir vs `save_pwm_min dir:"neg"` cerrar).
- Asunción de canal-0-único para `status_live`.

## 10. Versionado

- `schema` en announcement: `agp.flow.announcement/1`.
- `schema` en status_live: `agp.flow.status_live/1`.

Subir el sufijo `/N` solo en breaking changes. Los consumidores deben aceptar
ambos durante la transición.
