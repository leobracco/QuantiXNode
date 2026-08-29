# Kit de campo — FlowX Node v1.9.3

Todo lo necesario para flashear una placa **SK21** con el firmware **FlowX Node**
desde cualquier notebook Windows, **sin instalar nada** (esptool viene incluido).

## Contenido

| Archivo | Qué es |
|---|---|
| `FLASHEAR.bat` | **El que vas a usar.** Borra todo (firmware SK21 viejo incluido) y graba FlowX 1.9.3 con config de fábrica |
| `ACTUALIZAR_FIRMWARE.bat` | Solo para placas que **ya** corren FlowX: actualiza firmware sin tocar la configuración |
| `bin/` | Binarios: bootloader, particiones, firmware 1.9.3, imagen LittleFS (config.json + network.json de fábrica) |
| `tools/esptool.exe` | esptool v4.9.0 standalone (no requiere Python) |
| `FlowX_v1.9.3.bin` | El firmware solo, para subir a OrbitX / OTA por MQTT |

## Cómo bajar este kit en la notebook

```
git clone https://github.com/leobracco/QuantiXNode.git FlowXNode
cd FlowXNode\KitCampo_FlowX_1.9.3
```

(Si no hay git en la notebook: desde el navegador, GitHub → repo `leobracco/QuantiXNode`
→ botón verde **Code → Download ZIP**, descomprimir, entrar a `KitCampo_FlowX_1.9.3`.)

**Bajalo ANTES de salir al campo — en el campo no va a haber internet.**

## Flasheo en el campo (paso a paso)

1. Conectar la placa a la notebook por **USB**.
2. Verificar que aparezca un puerto COM (Administrador de dispositivos → Puertos).
   Si no aparece, falta el driver USB-serie (ver Problemas, abajo).
3. Doble click en **`FLASHEAR.bat`**. Detecta el puerto solo; si hay varios COM,
   correrlo desde una consola como `FLASHEAR.bat COM5`.
4. Esperar los dos pasos (borrado + grabado, ~1 minuto). Al terminar la placa
   se reinicia sola ya con FlowX.

### Configuración después de flashear

La placa arranca sin red configurada y levanta su propio WiFi:

1. Desde el celular o la notebook, conectarse a la red **`FX-xxxxxxxxx`**
   (clave **`12345678`**).
2. Abrir el navegador en **`http://192.168.4.1`**.
3. Ir a la página de **Red** (`http://192.168.4.1/p2`) y cargar:
   - **SSID y clave** del WiFi del tractor (la red donde está la PC con AOG).
   - **Broker MQTT**: IP de la PC del tractor (por defecto ya viene `192.168.5.10`,
     puerto `1883` — cambiar solo si la PC tiene otra IP).
4. Guardar → la placa se reinicia, se conecta al WiFi del tractor y aparece
   sola en AOG (publica su announcement por MQTT).

### Config de fábrica que queda grabada (pines SK21)

- Sensor de caudal: GPIO **17** · Motor regulador: GPIO **32/33** (H-bridge)
- Master: GPIO **14** · Secciones 3-wire: **27/12**, **13/15**, **2/4**
- PID: kp 2.5, ki 1.5, kd 0, pwm_min 800 · meterCal 50 pulsos/L
- Todo esto se ajusta después desde AOG por MQTT (target/config/calibrar/autotune).

## Problemas frecuentes

- **"FALLO" o no detecta el chip** → mantener apretado el botón **BOOT** de la
  placa mientras se enchufa el USB (o mientras el script intenta conectar) y
  reintentar.
- **No aparece ningún COM** → instalar driver del conversor USB-serie:
  - CH340: https://www.wch-ic.com/downloads/CH341SER_EXE.html
  - CP210x: https://www.silabs.com/developer-tools/usb-to-uart-bridge-vcp-drivers
  (bajarlos antes de salir al campo si la notebook nunca flasheó un ESP32)
- **Se corta a mitad del grabado** → cable USB de datos de mala calidad; probar
  otro cable/puerto. También se puede bajar la velocidad editando el `.bat`
  (cambiar `460800` por `115200`).
- **Quedó a medias / no arranca** → volver a correr `FLASHEAR.bat` completo,
  siempre deja la placa en estado limpio.

## Verificación rápida (opcional, con monitor serie)

Si hay algún problema, el log serie a **115200 baudios** (PuTTY / Arduino IDE
Serial Monitor) muestra el arranque: versión, estado del WiFi, IP del AP y
conexión MQTT.
