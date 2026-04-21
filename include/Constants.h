#pragma once

#include <stdint.h>
#include <stddef.h>

namespace quantix {
namespace constants {

// --- PWM ---
constexpr uint16_t PWM_MAX = 4095;
constexpr uint32_t PWM_FREQ_HZ = 1000;
constexpr uint8_t PWM_RESOLUTION_BITS = 12;

// --- I2C ---
constexpr uint8_t ADS1115_ADDR = 0x48;
constexpr uint8_t PCA9685_ADDR = 0x40;
constexpr uint8_t PCF8574_ADDR_BASE = 0x20;

// --- Timing ---
constexpr uint32_t LOOP_INTERVAL_MS = 50;
constexpr uint32_t SEND_INTERVAL_MS = 200;
constexpr uint32_t AP_FALLBACK_TIMEOUT_MS = 10000;
constexpr uint32_t PULSE_TIMEOUT_MS = 2000;
constexpr uint32_t MQTT_RECONNECT_BASE_MS = 1000;
constexpr uint32_t MQTT_RECONNECT_CAP_MS = 30000;
constexpr uint32_t RESTART_DELAY_MS = 500;
constexpr uint32_t PENDING_RESTART_MS = 5000;

// --- Filtros de pulso ---
constexpr uint32_t MIN_PULSE_MICROS = 250;

// --- Tamaños de JSON ---
constexpr size_t JSON_CONFIG_DOC_SIZE = 2048;
constexpr size_t JSON_NETWORK_DOC_SIZE = 512;
constexpr size_t JSON_STATUS_DOC_SIZE = 384;
constexpr size_t JSON_STATUS_BUF_SIZE = 512;
constexpr size_t JSON_CMD_DOC_SIZE = 384;
constexpr size_t JSON_ANNOUNCE_DOC_SIZE = 200;

// --- Valor de pin "no conectado" (replica de NC en Structs.h por si se usa sin él) ---
constexpr uint8_t PIN_NC = 0xFF;

}  // namespace constants
}  // namespace quantix
