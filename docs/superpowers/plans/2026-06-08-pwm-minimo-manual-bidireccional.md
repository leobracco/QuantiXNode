# Búsqueda manual de PWM mínimo bidireccional — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Agregar un modo manual operado por MQTT desde AOG para buscar el PWM mínimo del actuador en ambos sentidos (abrir/cerrar) y persistir un piso por sentido que alimenta el PID.

**Architecture:** Modo manual como hermano de Calib/AutoTune/Char en `CalibAuto.cpp` (estado `MAN[ID]`, control exclusivo del PWM, failsafe de comms-loss). Tres verbos MQTT nuevos (`manual_pwm`, `manual_stop`, `save_pwm_min`). El PID elige el piso de PWM (`MinPWM` al abrir, `MinPWMNeg` al cerrar) según el signo del error. Persistencia en `/config.json` (`pid.pwmMin` + nuevo `pid.pwmMinNeg`).

**Tech Stack:** C++ / Arduino framework / PlatformIO (`esp32doit-devkit-v1`), PubSubClient (MQTT), ArduinoJson, LittleFS.

**Testing note:** Este repo NO tiene runner de tests (`test/` vacío; CLAUDE.md prohíbe inventar uno). La verificación de cada tarea es **compilación limpia** con `pio run`. La verificación funcional es en banco con `pio device monitor` (Task 8). No hay tests unitarios automatizados.

**Spec:** `docs/superpowers/specs/2026-06-08-pwm-minimo-manual-bidireccional-design.md`

---

### Task 1: Campos de struct para el segundo piso (`MinPWMNeg`, `pwmMinNeg`)

**Files:**
- Modify: `include/Structs.h:25` (pid struct) y `include/Structs.h:78` (SensorConfig)

- [ ] **Step 1: Agregar `pwmMinNeg` al sub-struct pid de `FlowConfig`**

En `include/Structs.h`, reemplazar:

```c
    struct
    {
        float kp, ki, kd;
        int pwmMin; // 0-4095 para resolución de 12 bits
    } pid;
```

por:

```c
    struct
    {
        float kp, ki, kd;
        int pwmMin;    // 0-4095: piso de PWM al ABRIR (output PID > 0)
        int pwmMinNeg; // 0-4095: piso de PWM al CERRAR (output PID < 0)
    } pid;
```

- [ ] **Step 2: Agregar `MinPWMNeg` a `SensorConfig`**

En `include/Structs.h`, reemplazar la línea:

```c
    int MinPWM, MaxPWM;
```

por:

```c
    int MinPWM, MaxPWM;
    int MinPWMNeg; // piso de PWM al cerrar (magnitud); MinPWM es el de abrir
```

- [ ] **Step 3: Compilar**

Run: `pio run`
Expected: build SUCCESS (los campos nuevos aún no se usan; solo deben compilar).

- [ ] **Step 4: Commit**

```bash
git add include/Structs.h
git commit -m "feat: agregar campos pwmMinNeg/MinPWMNeg para piso PWM al cerrar"
```

---

### Task 2: Persistencia del segundo piso (`Begin.cpp` + `config.json`)

**Files:**
- Modify: `src/Begin.cpp` (SaveData ~L223, LoadData ~L281 y ~L298, SetDefault ~L321 y ~L340)
- Modify: `data/config.json:24`

- [ ] **Step 1: Escribir `pwmMinNeg` en `SaveData`**

En `src/Begin.cpp`, reemplazar:

```c
    pid["kp"] = CFG.pid.kp;
    pid["ki"] = CFG.pid.ki;
    pid["pwmMin"] = CFG.pid.pwmMin;
```

por:

```c
    pid["kp"] = CFG.pid.kp;
    pid["ki"] = CFG.pid.ki;
    pid["pwmMin"] = CFG.pid.pwmMin;
    pid["pwmMinNeg"] = CFG.pid.pwmMinNeg;
```

- [ ] **Step 2: Leer `pwmMinNeg` en `LoadData`**

En `src/Begin.cpp`, reemplazar:

```c
    CFG.pid.kp = doc["FLOW"]["pid"]["kp"] | 2.5f;
    CFG.pid.ki = doc["FLOW"]["pid"]["ki"] | 1.5f;
    CFG.pid.pwmMin = doc["FLOW"]["pid"]["pwmMin"] | 800;
```

por:

```c
    CFG.pid.kp = doc["FLOW"]["pid"]["kp"] | 2.5f;
    CFG.pid.ki = doc["FLOW"]["pid"]["ki"] | 1.5f;
    CFG.pid.pwmMin = doc["FLOW"]["pid"]["pwmMin"] | 800;
    CFG.pid.pwmMinNeg = doc["FLOW"]["pid"]["pwmMinNeg"] | 800;
```

- [ ] **Step 3: Propagar `MinPWMNeg` a los canales en `LoadData`**

En `src/Begin.cpp`, dentro del `for` de sincronización de `Sensor[]`, reemplazar:

```c
        Sensor[i].MinPWM = CFG.pid.pwmMin;
        Sensor[i].MeterCal = CFG.MeterCal;
```

por:

```c
        Sensor[i].MinPWM = CFG.pid.pwmMin;
        Sensor[i].MinPWMNeg = CFG.pid.pwmMinNeg;
        Sensor[i].MeterCal = CFG.MeterCal;
```

- [ ] **Step 4: Default de `pwmMinNeg` en `SetDefault`**

En `src/Begin.cpp`, reemplazar:

```c
    CFG.pid.pwmMin = 800;
    CFG.pid.kp = 2.5f;
```

por:

```c
    CFG.pid.pwmMin = 800;
    CFG.pid.pwmMinNeg = 800;
    CFG.pid.kp = 2.5f;
```

- [ ] **Step 5: Default de `MinPWMNeg` por canal en `SetDefault`**

En `src/Begin.cpp`, dentro del `for` de defaults de canales, reemplazar:

```c
        Sensor[i].MinPWM = 0;
        Sensor[i].MaxPWM = 4095;
```

por:

```c
        Sensor[i].MinPWM = 0;
        Sensor[i].MinPWMNeg = 0;
        Sensor[i].MaxPWM = 4095;
```

- [ ] **Step 6: Agregar `pwmMinNeg` a la imagen de fábrica**

En `data/config.json`, reemplazar:

```json
      "kp": 2.5,
      "ki": 1.5,
      "kd": 0.0,
      "pwmMin": 800
```

por:

```json
      "kp": 2.5,
      "ki": 1.5,
      "kd": 0.0,
      "pwmMin": 800,
      "pwmMinNeg": 800
```

- [ ] **Step 7: Compilar**

Run: `pio run`
Expected: build SUCCESS.

- [ ] **Step 8: Commit**

```bash
git add src/Begin.cpp data/config.json
git commit -m "feat: persistir pwmMinNeg en config.json (load/save/default)"
```

---

### Task 3: PID usa un piso por sentido (`PID.cpp`)

**Files:**
- Modify: `src/PID.cpp:109-110`

- [ ] **Step 1: Elegir el piso según el signo del error**

En `src/PID.cpp`, reemplazar:

```c
            float mag = (float)Sensor[ID].MinPWM +
                        frac * (float)(Sensor[ID].MaxPWM - Sensor[ID].MinPWM);

            output = (error > 0 ? +mag : -mag);
```

por:

```c
            // Piso de PWM por sentido: abrir (error>0 → output>0) usa MinPWM,
            // cerrar (error<0 → output<0) usa MinPWMNeg. Cada sentido vence su
            // propia fricción mecánica, que suele ser distinta.
            float minFloor = (error > 0) ? (float)Sensor[ID].MinPWM
                                         : (float)Sensor[ID].MinPWMNeg;
            float mag = minFloor +
                        frac * (float)(Sensor[ID].MaxPWM - minFloor);

            output = (error > 0 ? +mag : -mag);
```

- [ ] **Step 2: Compilar**

Run: `pio run`
Expected: build SUCCESS.

- [ ] **Step 3: Commit**

```bash
git add src/PID.cpp
git commit -m "feat: PID usa piso de PWM por sentido (MinPWM abrir / MinPWMNeg cerrar)"
```

---

### Task 4: Módulo de modo manual (`CalibAuto.cpp` + `Globals.h`)

**Files:**
- Modify: `src/CalibAuto.cpp` (agregar al final del archivo, antes de cierre)
- Modify: `include/Globals.h` (prototipos + bump FW_VERSION)

- [ ] **Step 1: Declarar las funciones del modo manual en `Globals.h`**

En `include/Globals.h`, después del bloque de Caracterización (la línea `bool IsCharActive(byte ID);`), agregar:

```c
// --- Modo manual (búsqueda de pwm_min set/observar, bidireccional) ---
// El operario fija un PWM con signo (-4095..+4095) y observa el caudal en vivo
// por status_live. ManualSet toma control exclusivo del PWM (el PID no corre);
// RunManual lo re-aplica cada tick y corta por failsafe de comms-loss.
void ManualSet(byte ID, float value);
void ManualStop(byte ID);
void RunManual(byte ID);
bool IsManualActive(byte ID);
```

- [ ] **Step 2: Bump de `FW_VERSION` en `Globals.h`**

En `include/Globals.h`, reemplazar la línea `#define FW_VERSION "1.8.1" ...` por:

```c
#define FW_VERSION "1.9.0" // Modo manual MQTT (buscar pwm_min set/observar) + pwm_min bidireccional (piso PID por sentido abrir/cerrar); 1.8.1: Banda proporcional: velocidad motor mapea error->[pwm_min,pwm_max]; 1.8.0: valvula 3-posiciones sin integral + invertMotor; 1.7.0: PID bidireccional; 1.6.x: caracterizar/telemetria
```

- [ ] **Step 3: Implementar el estado y las funciones en `CalibAuto.cpp`**

En `src/CalibAuto.cpp`, agregar al FINAL del archivo (después de `RunChar`). Debe ir en este archivo porque accede a los estados estáticos `CAL`/`AT`/`CH` para cancelarlos:

```c
// --- Modo manual (búsqueda de pwm_min set/observar, bidireccional) -------
// El operario fija un PWM con signo y observa el caudal en vivo (status_live).
// Toma control exclusivo del PWM igual que Char/AutoTune: el PID no corre.
struct ManualState
{
    bool active;
    float value;        // PWM con signo aplicado (-4095..+4095)
    uint32_t lastCmdMs; // último manual_pwm recibido → failsafe comms-loss
    uint32_t startMs;   // inicio del modo → timeout duro
};
static ManualState MAN[MaxProductCount];

static const uint32_t MANUAL_COMMS_TIMEOUT_MS = 4000;   // 4s sin comando → stop
static const uint32_t MANUAL_HARD_TIMEOUT_MS  = 300000; // 5 min tope duro

bool IsManualActive(byte ID)
{
    if (ID >= MaxProductCount) return false;
    return MAN[ID].active;
}

void ManualSet(byte ID, float value)
{
    if (ID >= MaxProductCount) return;

    value = constrain(value, -4095.0f, 4095.0f);

    // Al entrar, cancelamos cualquier otro modo que pelee por el PWM del canal.
    if (!MAN[ID].active)
    {
        if (CAL[ID].active) CalibStop(ID, true);
        if (AT[ID].active)  AutoTuneStop(ID, true);
        if (CH[ID].active)  CharStop(ID, true);

        MAN[ID].active = true;
        MAN[ID].startMs = millis();
        Sensor[ID].FlowEnabled = true;
        Sensor[ID].PidState = 0; // off (no es el lazo PID)
        Serial.printf("[MANUAL] start ID=%u\n", ID);
    }

    MAN[ID].value = value;
    MAN[ID].lastCmdMs = millis();

    Sensor[ID].PWM = value; // reflejar en telemetría
    SetPWM(ID, value);      // >0 abre, <0 cierra
    Serial.printf("[MANUAL] ID=%u pwm=%.0f\n", ID, value);
}

void ManualStop(byte ID)
{
    if (ID >= MaxProductCount) return;
    if (!MAN[ID].active) return;

    MAN[ID].active = false;
    MAN[ID].value = 0;
    SetPWM(ID, 0);
    Sensor[ID].PWM = 0;
    Serial.printf("[MANUAL] stop ID=%u\n", ID);
}

void RunManual(byte ID)
{
    if (ID >= MaxProductCount) return;
    if (!MAN[ID].active) return;

    uint32_t now = millis();

    // Failsafe: si AOG deja de refrescar el comando, frenamos el motor.
    if (now - MAN[ID].lastCmdMs > MANUAL_COMMS_TIMEOUT_MS)
    {
        Serial.println("[MANUAL] comms-loss timeout — stop");
        ManualStop(ID);
        return;
    }
    // Tope duro por si quedó activo demasiado tiempo.
    if (now - MAN[ID].startMs > MANUAL_HARD_TIMEOUT_MS)
    {
        Serial.println("[MANUAL] hard timeout — stop");
        ManualStop(ID);
        return;
    }

    // Re-aplicar el PWM fijado.
    Sensor[ID].PWM = MAN[ID].value;
    SetPWM(ID, MAN[ID].value);
}
```

- [ ] **Step 4: Compilar**

Run: `pio run`
Expected: build SUCCESS. (Las funciones aún no se llaman desde el loop ni MQTT, pero deben compilar y linkear porque están declaradas en `Globals.h`.)

- [ ] **Step 5: Commit**

```bash
git add src/CalibAuto.cpp include/Globals.h
git commit -m "feat: modo manual de PWM (set/observar) con failsafe comms-loss"
```

---

### Task 5: Integración en el loop principal (`main.cpp`)

**Files:**
- Modify: `src/main.cpp:219` (cadena if/else-if del bloque por canal)

- [ ] **Step 1: Agregar `IsManualActive` como primera rama**

En `src/main.cpp`, reemplazar:

```c
            if (IsCharActive(i))
            {
                // Caracterización (rampa PWM 0..4095 buscando pwm_min/hz_max).
                // Toma control del PWM — el PID y la calibración no corren.
                RunChar(i);
            }
            else if (IsAutoTuneActive(i))
```

por:

```c
            if (IsManualActive(i))
            {
                // Modo manual (búsqueda de pwm_min set/observar). Toma control
                // exclusivo del PWM; PID/calibración/caracterización no corren.
                // Failsafe de comms-loss y timeout duro adentro de RunManual().
                RunManual(i);
            }
            else if (IsCharActive(i))
            {
                // Caracterización (rampa PWM 0..4095 buscando pwm_min/hz_max).
                // Toma control del PWM — el PID y la calibración no corren.
                RunChar(i);
            }
            else if (IsAutoTuneActive(i))
```

- [ ] **Step 2: Compilar**

Run: `pio run`
Expected: build SUCCESS.

- [ ] **Step 3: Commit**

```bash
git add src/main.cpp
git commit -m "feat: dar prioridad al modo manual en el loop (antes de Char/AutoTune/Calib/PID)"
```

---

### Task 6: Verbos MQTT (`MQTT_Custom.cpp`)

**Files:**
- Modify: `src/MQTT_Custom.cpp` (cadena de verbos, después del bloque `caracterizar_stop` ~L366)

- [ ] **Step 1: Agregar los verbos `manual_pwm`, `manual_stop`, `save_pwm_min`**

En `src/MQTT_Custom.cpp`, ubicar el bloque:

```c
        else if (verb == "caracterizar_stop")
        {
            CharStop(ID, true);
            AgpEnvelope_publishAck(mqttClient, "flow", uid, env.cmdId, "ok", "char_stopped");
        }
        else
        {
            Serial.printf("[CMD] verbo desconocido: %s\n", verb.c_str());
```

y reemplazarlo por (inserta los tres verbos nuevos entre `caracterizar_stop` y el `else` final):

```c
        else if (verb == "caracterizar_stop")
        {
            CharStop(ID, true);
            AgpEnvelope_publishAck(mqttClient, "flow", uid, env.cmdId, "ok", "char_stopped");
        }
        else if (verb == "manual_pwm")
        {
            // Búsqueda manual de pwm_min: aplica un PWM con signo (-4095..+4095).
            // value>0 abre, value<0 cierra. Refresca el failsafe de comms-loss.
            float value = doc["value"] | 0.0f;
            ManualSet(ID, value);
            AgpEnvelope_publishAck(mqttClient, "flow", uid, env.cmdId, "ok", "manual_pwm_set");
        }
        else if (verb == "manual_stop")
        {
            ManualStop(ID);
            AgpEnvelope_publishAck(mqttClient, "flow", uid, env.cmdId, "ok", "manual_stopped");
        }
        else if (verb == "save_pwm_min")
        {
            // Persiste el piso de PWM del sentido indicado. value = magnitud 0..4095.
            const char *dir = doc["dir"] | "";
            int value = doc["value"] | 0;
            value = constrain(value, 0, 4095);
            if (strcmp(dir, "pos") == 0)
            {
                CFG.pid.pwmMin = value;
                Sensor[ID].MinPWM = value;
                SaveConfig();
                AgpEnvelope_publishAck(mqttClient, "flow", uid, env.cmdId, "ok", "pwm_min_saved");
            }
            else if (strcmp(dir, "neg") == 0)
            {
                CFG.pid.pwmMinNeg = value;
                Sensor[ID].MinPWMNeg = value;
                SaveConfig();
                AgpEnvelope_publishAck(mqttClient, "flow", uid, env.cmdId, "ok", "pwm_min_saved");
            }
            else
            {
                AgpEnvelope_publishAck(mqttClient, "flow", uid, env.cmdId, "rejected", "dir_invalido");
            }
        }
        else
        {
            Serial.printf("[CMD] verbo desconocido: %s\n", verb.c_str());
```

- [ ] **Step 2: Compilar**

Run: `pio run`
Expected: build SUCCESS.

- [ ] **Step 3: Commit**

```bash
git add src/MQTT_Custom.cpp
git commit -m "feat: verbos MQTT manual_pwm/manual_stop/save_pwm_min"
```

---

### Task 7: Documentación (`CLAUDE.md` + `docs/API.md`)

**Files:**
- Modify: `CLAUDE.md` (tabla del contrato MQTT + nota de los dos pisos del PID)
- Modify: `docs/API.md` (verbos nuevos + campo `pwmMinNeg`)

- [ ] **Step 1: Actualizar la tabla MQTT en `CLAUDE.md`**

En `CLAUDE.md`, en la fila de `cmd/<verb>` de la tabla del contrato MQTT, reemplazar:

```
| `agp/flow/{uid}/cmd/<verb>`        | PC → Node | Verbs: `calibrar_start {vol_l,pwm,producto_id}`, `calibrar_stop {producto_id}`, `autotune_start {setpoint_hz,pwm_high,pwm_low,producto_id}`, `autotune_stop {producto_id}`, `ota {url,version,sha256?}` |
```

por:

```
| `agp/flow/{uid}/cmd/<verb>`        | PC → Node | Verbs: `calibrar_start {vol_l,pwm,producto_id}`, `calibrar_stop {producto_id}`, `autotune_start {setpoint_hz,pwm_high,pwm_low,producto_id}`, `autotune_stop {producto_id}`, `caracterizar_start/stop {producto_id}`, `manual_pwm {value,producto_id}` (PWM con signo −4095..+4095; set/observar pwm_min), `manual_stop {producto_id}`, `save_pwm_min {dir:"pos"\|"neg", value, producto_id}`, `ota {url,version,sha256?}` |
```

- [ ] **Step 2: Documentar los dos pisos del PID en `CLAUDE.md`**

En `CLAUDE.md`, en la sección de `PID.cpp` / convenciones, agregar esta viñeta al bloque de gotchas (después de la viñeta de `ResetPIDState`):

```
- El PID usa un piso de PWM por sentido: `Sensor[ID].MinPWM` al abrir
  (output>0) y `Sensor[ID].MinPWMNeg` al cerrar (output<0). Se persisten como
  `FLOW.pid.pwmMin` / `FLOW.pid.pwmMinNeg` en `/config.json` y se setean por
  MQTT con `save_pwm_min {dir,value}`. La búsqueda manual del valor se hace con
  `manual_pwm`/`manual_stop` (modo set/observar, hermano de calib/autotune/char
  en `CalibAuto.cpp`, con failsafe de comms-loss de 4 s).
```

- [ ] **Step 3: Documentar en `docs/API.md`**

En `docs/API.md`, agregar una sección con los verbos nuevos. Ubicar dónde están documentados los comandos `cmd/` existentes y agregar:

```markdown
### Modo manual de PWM (búsqueda de pwm_min)

`agp/flow/{uid}/cmd/manual_pwm` — aplica un PWM con signo y entra a modo manual.

```json
{ "producto_id": 0, "value": 1200 }
```

- `value`: −4095..+4095. `>0` abre, `<0` cierra.
- Reentra el failsafe: hay que reenviar el comando al menos cada 4 s o el
  firmware frena el motor (comms-loss). Tope duro de 5 min.
- Ack: `manual_pwm_set`.

`agp/flow/{uid}/cmd/manual_stop` — sale de modo manual (PWM 0). Ack: `manual_stopped`.

```json
{ "producto_id": 0 }
```

`agp/flow/{uid}/cmd/save_pwm_min` — persiste el piso de PWM de un sentido.

```json
{ "producto_id": 0, "dir": "pos", "value": 850 }
```

- `dir`: `"pos"` (abrir → `pwmMin`) o `"neg"` (cerrar → `pwmMinNeg`).
- `value`: magnitud 0..4095.
- Persiste en `/config.json` (`FLOW.pid.pwmMin` / `FLOW.pid.pwmMinNeg`) y lo
  aplica al canal. Ack: `pwm_min_saved` (o `rejected`/`dir_invalido`).
```

- [ ] **Step 4: Commit**

```bash
git add CLAUDE.md docs/API.md
git commit -m "docs: documentar verbos manual_pwm/manual_stop/save_pwm_min y pwm_min bidireccional"
```

---

### Task 8: Verificación de banco (hardware, manual)

**Files:** ninguno (verificación funcional con `pio device monitor`).

No hay tests automatizados. Esta tarea es una checklist de banco con el ESP32 conectado y agua/aire circulando.

- [ ] **Step 1: Flashear firmware + filesystem**

```bash
pio run -t upload
pio run -t uploadfs
pio device monitor -b 115200
```

- [ ] **Step 2: Verificar modo manual positivo (abrir)**

Publicar (desde AOG o un cliente MQTT) a `agp/flow/<uid>/cmd/manual_pwm` con `{"producto_id":0,"value":1200}`.
Expected en serial: `[MANUAL] start ID=0` y `[MANUAL] ID=0 pwm=1200`. El motor abre; `status_live` muestra `pwm:1200` y caudal creciente.

- [ ] **Step 3: Verificar modo manual negativo (cerrar)**

Publicar `{"producto_id":0,"value":-1200}`.
Expected: `[MANUAL] ID=0 pwm=-1200`; el motor cierra (sentido opuesto).

- [ ] **Step 4: Verificar failsafe de comms-loss**

Dejar de publicar `manual_pwm` y esperar >4 s.
Expected en serial: `[MANUAL] comms-loss timeout — stop`; el motor frena (PWM 0).

- [ ] **Step 5: Verificar persistencia de los dos pisos**

Publicar `save_pwm_min {"producto_id":0,"dir":"pos","value":850}` y luego `{"dir":"neg","value":700}`. Reiniciar el ESP.
Expected: tras el reboot, `LoadData` deja `Sensor[0].MinPWM=850` y `Sensor[0].MinPWMNeg=700` (verificar agregando un print temporal o inspeccionando el comportamiento del PID). `/config.json` en LittleFS contiene `pwmMin:850` y `pwmMinNeg:700`.

- [ ] **Step 6: Verificar el PID por sentido**

Con un `target` que requiera abrir y luego cerrar, confirmar que el PWM mínimo aplicado al abrir respeta `MinPWM` y al cerrar respeta `MinPWMNeg` (observar el `pwm` en `status_live` cuando el lazo recién sale de la hold-band en cada sentido).

- [ ] **Step 7: Commit final si hubo ajustes**

Si la verificación de banco no requirió cambios, no hay commit. Si hubo fixes, commitearlos con un mensaje descriptivo.
