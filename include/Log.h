#pragma once

#include <Arduino.h>
#include <stdarg.h>

namespace quantix {
namespace log {

enum class Level : uint8_t {
    ERROR = 0,
    WARN  = 1,
    INFO  = 2,
    DEBUG = 3,
};

void init(Level level);
void setLevel(Level level);
Level getLevel();

// logf imprime `[ms][LVL][tag] fmt...\n` en Serial. Sin String, sin heap.
void logf(Level level, const char* tag, const char* fmt, ...);

}  // namespace log
}  // namespace quantix

// Short-circuit por nivel para no construir argumentos si el log está filtrado.
#define LOG_E(tag, ...) \
    do { if ((int)::quantix::log::getLevel() >= (int)::quantix::log::Level::ERROR) \
        ::quantix::log::logf(::quantix::log::Level::ERROR, tag, __VA_ARGS__); } while (0)

#define LOG_W(tag, ...) \
    do { if ((int)::quantix::log::getLevel() >= (int)::quantix::log::Level::WARN) \
        ::quantix::log::logf(::quantix::log::Level::WARN, tag, __VA_ARGS__); } while (0)

#define LOG_I(tag, ...) \
    do { if ((int)::quantix::log::getLevel() >= (int)::quantix::log::Level::INFO) \
        ::quantix::log::logf(::quantix::log::Level::INFO, tag, __VA_ARGS__); } while (0)

#define LOG_D(tag, ...) \
    do { if ((int)::quantix::log::getLevel() >= (int)::quantix::log::Level::DEBUG) \
        ::quantix::log::logf(::quantix::log::Level::DEBUG, tag, __VA_ARGS__); } while (0)
