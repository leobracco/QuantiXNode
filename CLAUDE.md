# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Context within the AgroParallel ecosystem

This repo is the **FlowX ESP32 firmware** (`FlowXNode`) — a liquid dosing /
flowmeter controller node. It is one of the ESP32 nodes described in
`G:\AgroParallel\Productos\CLAUDE.md` (read that first for the big picture of
how AgIO/AOG/OrbitX/MQTT all fit together). FlowX nodes:

- Run on the tractor's LAN; they have **no internet of their own**.
- Talk MQTT only, against the **broker embedded in AgIO (MQTTnet)** running on
  the PC. Host/port come from `MDLnetwork.BrokerHost`/`BrokerPort` (web portal,
  persisted in `/network.json`); `initMQTT()` only falls back to the hardcoded
  `192.168.5.10:1883` when those are empty (`MQTT_Custom.cpp:414`).
- Are controlled by `FlowXBridge` inside AOG (publishes targets, receives
  telemetry). Receive.cpp is intentionally empty — the legacy UDP/CRC path was
  removed; everything is MQTT now.

## Build / flash / monitor (PlatformIO)

This is a PlatformIO project (`platformio.ini`) targeting `esp32doit-devkit-v1`,
Arduino framework, LittleFS filesystem, `min_spiffs.csv` partitions (two ~1.9 MB
app slots — **required for OTA**; the old `huge_app.csv` had a single 3 MB slot
and could not self-update. If the sketch nears 1.9 MB, migrate to
`default_8MB.csv` on an 8 MB module).

```bash
pio run                              # build
pio run -t upload                    # flash firmware over USB (921600 baud)
pio run -t uploadfs                  # flash LittleFS image from data/ (config.json, network.json)
pio device monitor -b 115200         # serial monitor
pio run -t clean
```

There is no test runner wired up (`test/` is empty); the `test` PIO env isn't
configured. Don't add a fake test command — just say so.

Library deps are declared in `platformio.ini` and auto-fetched; do NOT vendor
them into `lib/` (that dir is empty by design).

## Runtime architecture

`main.cpp` owns the singletons (`MDL`, `Sensor[]`, `MDLnetwork`, `CFG`,
`PWMServoDriver`, `PCF`, `WebServer server`). Every other `.cpp` reaches them
via `extern` declared in `include/Globals.h`. **If you add a new global, define
it in `main.cpp` and declare it `extern` in `Globals.h`** — that's the pattern
the codebase enforces (see the `GoodPins` comment at `main.cpp:52`).

Two parallel control channels exist (`MaxProductCount = 2` in `Structs.h`) —
typically channel 0 = semilla, channel 1 = fertilizante (see `data/config.json`).

### Main loop (`main.cpp:89`)

Fixed 50 ms tick (`LoopTime`):

1. `mqttLoop()` — reconnect / pump MQTT
2. `GetUPM()` — turn ISR pulse samples into Hz/UPM (`Rate.cpp`)
3. `GetSpeed()` — wheel speed from `WheelSpeed.cpp`
4. Per channel: if `CalibActive` → run open-loop at `ManualAdjust` PWM until
   `TotalPulses ≥ CalibTargetPulses`, else `PIDmotor(i)` (PI controller with
   anti-windup, `PID.cpp`).
5. `CheckRelays()` — section relays + 4 s comms-loss failsafe that drops all
   sections (`Relays.cpp:49`).
6. `ReadAnalog()` (pressure) and `SendComm()` (200 ms throttled telemetry).
7. Deferred reboot if `ResetTime` was set.

`loop()` also feeds the watchdog (`wdtFeed()`), runs `server.handleClient()`
for the web portal, and drains a pending OTA (`if (otaPendiente)
procesarOtaPendiente()`) — OTA is run here, **not** in the MQTT callback,
because `handleOTACommand()` blocks (downloads ~1 MB, flashes, reboots).

### Pulse counting

`ISR_Sensor0` / `ISR_Sensor1` (in `Rate.cpp`) are pinned to `IRAM_ATTR`.
Pulses < 250 µs apart are rejected as debounce. A ring buffer of pulse-period
samples (size `Sensor[i].PulseSampleSize`, max 20) is medianed in `GetUPM()` to
compute Hz. **Do not allocate or use `String`/Serial inside the ISRs.**

### Actuator output (`Motor.cpp`)

Each channel maps to two LEDC channels (`ID*2`, `ID*2 + 1`) for an H-bridge.
PCA9685 is also initialized (12-bit @ 1 kHz) and is wired to the PCF8574 /
relay expansion options selected by `MDL.RelayControl` (1 = GPIO, 5 = PCA9685,
6 = PCF8574). The PCA's `OE` is held LOW from `PCB_SDA=27 / PCB_SCL=26 /
PCA_OE=23` — see `Begin.cpp:22`.

Section valves come in two flavors driven by `CFG.Is3Wire`:
- 3-wire: single GPIO ON/OFF
- 2-wire motorized: pinA/pinB polarity inversion (open/close)

`CFG.InvertRelay` inverts the master + 3-wire logic.

### Persistence

`LittleFS.begin(true)` (formats on failure) in `setup()`. Two JSON files in
LittleFS root:

- `/config.json` — `MDL` + `FLOW` (`CFG`) including PID gains and `SectionPins[10][2]`
- `/network.json` — STA SSID/password + `WifiModeUseStation`

`Begin.cpp` is the source of truth for defaults (`SetDefault()`,
`Load/SaveData`, `Load/SaveNetworks`). `MQTT_Custom.cpp` calls `SaveConfig()`
(alias of `SaveData`) when a `config` topic arrives.

### Wi-Fi fallback

`DoSetup()` tries STA with saved creds for 8 s, then drops to AP
`FlowX_Node_AP / 12345678`. Web portal = handlers in `GUI.cpp` + HTML builders
in `Pages.cpp`:
- `/`  → `HandleRoot` serves `GetPage0()` (section-override panel). Query args
  `?toggle=N` / `?cutall=1` / `?freeall=1` flip bits in the `sectionForceOff`
  mask (bit set = section forced OFF) and re-apply via `processValves()`.
- `/p2` → `HandlePage2` serves `GetPage2()` (network + broker config). Its form
  POSTs back to `/`; `handleCredentials()` saves SSID/pass, AP password, and
  `BrokerHost`/`BrokerPort`, then reboots if anything changed.

`PORTAL_SECTION_COUNT` in `GUI.cpp` must match the same constant in `Pages.cpp`.
(The legacy `PgStart/PgSwitches/PgNetwork` modules are gone — don't reference
them.)

## MQTT contract (must match `FlowXBridge` / `FlowXLiveService` / `FlowXController` in AOG)

UID is the ESP32 efuse MAC formatted as `%04X%08X` (12 hex chars). Topics
match exactly what AOG subscribes/publishes; **do not change them without
also updating the AOG side**.

| Topic                              | Direction | Notes |
|---|---|---|
| `agp/flow/{uid}/announcement`      | Node → PC | retained: `{uid,type:"flow",ip,version,hw:"SK21"}` |
| `agp/flow/{uid}/status_live`       | Node → PC | telemetry for channel 0 only (see below). Keys: `caudal_lmin`, `target_lmin`, `error_lmin`, `pwm`, `pid_estado`. Plus legacy `lmin`/`pps_target`/`sec_on`/`pulsos` for back-compat. Throttled 200 ms by `Send.cpp`. |
| `agp/flow/{uid}/target`            | PC → Node | `{ t:<L/min>, sec:[0/1,...×≤16], pwm_min:<int>, pid:{kp,ki,kd} }` |
| `agp/flow/{uid}/config`            | PC → Node | `{ meterCal, is3Wire, invertRelay }` — triggers `SaveConfig()` |
| `agp/flow/{uid}/cmd/<verb>`        | PC → Node | Verbs: `calibrar_start {vol_l,pwm,producto_id}`, `calibrar_stop {producto_id}`, `autotune_start {setpoint_hz,pwm_high,pwm_low,producto_id}`, `autotune_stop {producto_id}`, `ota {url,version,sha256?}` |
| `agp/flow/{uid}/ota/progress`      | Node → PC | OTA status: `"iniciando"` / `"ok"` / `"error:<detalle>"` (see `OTA.h`) |
| `agp/flow/{uid}/calibrar_result`   | Node → PC | retained: `{uid, producto_id, ok, pulsos, duration_ms, error}` — AOG calcula `meter_cal = pulsos/vol_l` |
| `agp/flow/{uid}/autotune_result`   | Node → PC | retained: `{uid, producto_id, ok, kp, ki, kd, ku, tu_ms, error}` |

**Why the 4-part topic shape matters:** AOG's `NodoRegistryService` subscribes
to `agp/+/+/announcement` and `agp/+/+/status_live` and drops anything with
fewer than 4 segments. Keep that shape if you add new topics.

The `sec` array is converted to a `uint16_t` bitmask (cable 0 = bit 0) and fed
to `processValves()`. **Importantly**, when a `target` arrives the firmware
also derives `Sensor[0].TargetUPM = lmin * MeterCal / 60` (Hz) and flips
`Sensor[0].FlowEnabled` to the PID can act. See `applyTargetLmin()` in
`MQTT_Custom.cpp`.

**Channel-0 only for `status_live`:** `FlowXLiveService` keys readings by
`uid`, so publishing per-channel would have channel 1 overwrite channel 0 in
the cache. Until AOG supports multi-product, `sendMQTTStatus()` early-returns
for `ID != 0`. Don't "fix" that without coordinating an AOG-side change.

## Calibration + Auto-Tune (CalibAuto.cpp)

- **Calibrar** — `CalibStart(ID, vol_l, pwm)` reset `TotalPulses=0`, fija
  `ManualAdjust=pwm` y `CalibActive=true`. El loop principal en `main.cpp`
  aplica `SetPWM(i, ManualAdjust)` mientras corre. Para parar, `calibrar_stop`
  (manual) o el timeout duro de 5 min en `RunCalibAuto`. Publica
  `calibrar_result` con `{ok, pulsos, duration_ms}` y AOG calcula `meter_cal`.
- **Auto-tune** — Ziegler-Nichols por relay feedback. Alterna PWM
  `pwm_high`/`pwm_low` cruzando un setpoint en Hz, mide período Tu y amplitud
  de salida `a`. Estimación: `Ku = 4·h / (π·a)` (describing function),
  `Kp=0.6·Ku, Ki=1.2·Ku/Tu, Kd=0.075·Ku·Tu`. Timeout 30 s, 4–8 muestras.
  Aplica los gains al `Sensor[ID]` y publica `autotune_result`.
- Ambos toman control exclusivo del PWM del canal — el `PIDmotor()` no corre
  mientras estén activos.

## Reliability layer ("Tanda 2")

Three header-only safety subsystems wrap the runtime. They are complementary —
don't conflate them:

- **Watchdog (`main.cpp`)** — Task WDT, 8 s timeout, `trigger_panic`. `loop()`
  must call `wdtFeed()` every tick. Long synchronous waits (WiFi.begin, OTA
  stream) either feed it explicitly or bracket themselves with
  `wdtSuspend()`/`wdtResume()` (= `esp_task_wdt_delete/add(NULL)`). Always
  suspend the WDT around blocking flash/OTA work.
- **Safe-mode (`AgpSafeMode.h`)** — persists a `crash_count` in NVS. Call
  `AgpSafeMode_begin()` once in `setup()`; it increments the counter when
  `esp_reset_reason()` is a crash (TASK_WDT/INT_WDT/PANIC/BROWNOUT/WDT) and
  resets it on a clean poweron/sw_reset. At `crash_count >= 3`
  (`AGP_SAFE_MODE_THRESHOLD`), `AgpSafeMode_isActive()` is true and the firmware
  **must** keep PWM=0, relays OFF, and refuse dangerous MQTT cmds
  (OTA/calibrar/autotune) until a `clear_safe_mode` cmd calls
  `AgpSafeMode_clear()`. Distinct from `boot_reason` (instantaneous, published
  in `/announcement` via `bootReasonStr()`).
- **Command envelope (`AgpEnvelope.h`)** — optional `_meta:{cmd_id, ttl_ms,
  ts_ms}` on inbound cmds. `MQTT_Custom.cpp` parses it
  (`AgpEnvelope_parseFromJson`), drops duplicates by `cmd_id`
  (`AgpEnvelope_isDuplicate`) and expired cmds (TTL), and publishes an ack so
  the AOG bridge can close its promise. **Legacy cmds without `_meta` still
  execute** (just without ack) — keep that back-compat path.

## OTA (`OTA.cpp` / `OTA.h`)

Fully implemented (no longer "in progress"). A `cmd/ota {url, version, sha256?}`
message sets `otaPendiente`; the loop calls `procesarOtaPendiente()` →
`handleOTACommand()`, which downloads the `.bin` over HTTP, optionally verifies
SHA-256 incrementally (aborts on mismatch before `Update.end()`), flashes with
`Update.h`, and reboots. Progress/result is published to `.../ota/progress` via
the callback registered with `setOTAStatusCallback()`. Requires the dual-slot
`min_spiffs.csv` partition table (see Build section).

## Conventions / gotchas

- Pin sentinel for "unused" is `const uint8_t NC = 255;` (`Structs.h:6`). Check
  for `!= NC` before touching a pin.
- All hardware pin assignments come from `CFG` (loaded from `/config.json`).
  Defaults in `SetDefault()` reflect the SK21 PCB: FlowPin 17, RegA 32, RegB
  33, Master 14, plus three section pin pairs.
- Comms-loss safety: 4 s without a `target` message → `processValves(0)` kills
  all sections (`Relays.cpp:52`). Don't bypass this.
- `Receive.cpp` is deliberately empty — do not resurrect the UDP path.
- The MQTT broker host/port live in `MDLnetwork.BrokerHost`/`BrokerPort` and
  are persisted in `/network.json`. Default is `192.168.5.10:1883` (AgIO on
  the tractor PC). If you need to change them at runtime, do it from the web
  portal (`/p2` page — `GUI.cpp` `HandlePage2` + `Pages.cpp` `GetPage2`) —
  don't hardcode again in `initMQTT()`.
- The `MDL.APpassword` default in `data/config.json` is literally
  `"quantix_admin"` (leftover from QuantiX) — keep in mind when reviewing.
- `ResetPIDState(ID)` exists for a reason: call it whenever the setpoint or
  enabled-state of a channel changes, to avoid integral kicks.

## Common edits map

| You want to… | Look in |
|---|---|
| Change PID behavior or anti-windup | `src/PID.cpp` |
| Change pulse debounce / sampling / Hz calc | `src/Rate.cpp` |
| Change master/section relay logic, 2-wire vs 3-wire | `src/Relays.cpp` |
| Change PWM drive / LEDC channel mapping / calibration sweep | `src/Motor.cpp` |
| Add a new MQTT topic or payload field | `src/MQTT_Custom.cpp` (+ keep `FlowXBridge` in AOG in sync) |
| Add a global / change defaults / change persistence | `src/Begin.cpp` + `src/main.cpp` + `include/Globals.h` |
| Change telemetry cadence | `src/Send.cpp` (`SendTime`) |
| Add / edit a web-portal page | `src/Pages.cpp` (`GetPage0`/`GetPage2`) + `src/GUI.cpp` handlers |
| Change OTA download / SHA / flash | `src/OTA.cpp` + `include/OTA.h` |
| Change watchdog / safe-mode / crash counter | `src/main.cpp` (WDT) + `include/AgpSafeMode.h` |
| Change command dedup / TTL / ack | `include/AgpEnvelope.h` (+ `MQTT_Custom.cpp` parse) |
