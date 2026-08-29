// ============================================================================
// OTA.h - Firmware update via HTTP (patrón unificado AgroParallel)
//
// Flow:
//   1. PC publica MQTT a agp/flow/{UID}/cmd/ota con payload:
//      { "url":"http://<LAN>:8088/firmware/FlowX/1.1.0/firmware.bin",
//        "version":"1.1.0" }
//   2. MQTT_Custom encola la OTA (otaPendiente=true) y la procesa fuera del
//      callback en el loop principal (handleOTACommand es bloqueante).
//   3. handleOTACommand baja el .bin por HTTP, lo flashea con Update.h y
//      reinicia el ESP. Publica progreso/resultado vía callback registrado.
// ============================================================================

#ifndef OTA_H
#define OTA_H

#include <Arduino.h>
#include <functional>

// Registrar callback que publica resultados al broker MQTT.
// MQTT_Custom.cpp lo registra con una lambda que invoca mqttClient.publish(...).
// El callback recibe status ("iniciando" | "ok" | "error") y detalle (puede ser "").
void setOTAStatusCallback(std::function<void(const char *status, const char *detalle, const String &version)> cb);

// Descargar e instalar firmware desde la URL dada. BLOQUEANTE.
// Reinicia el ESP al terminar (con éxito o error).
//
// expectedSha256: hash SHA-256 del .bin en hex lowercase (64 chars). Si no
// está vacío, se calcula incrementalmente mientras se escribe a flash y se
// compara antes de Update.end(); si no coincide, aborta sin bootear el
// binario corrupto. Si viene vacío (PC legacy o sin manifest), se omite.
void handleOTACommand(const String &url, const String &version, const String &expectedSha256 = String(""));

#endif
