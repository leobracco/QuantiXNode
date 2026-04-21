#pragma once

#include <ArduinoJson.h>

namespace quantix {
namespace hal {

// Interfaz mínima para cargar y guardar documentos JSON desde un filesystem.
// Permite inyectar un mock en tests nativos sin LittleFS.
class IFilesystem {
public:
    virtual ~IFilesystem() = default;

    virtual bool exists(const char* path) = 0;
    virtual bool readJson(const char* path, JsonDocument& out) = 0;
    virtual bool writeJson(const char* path, const JsonDocument& in) = 0;
};

}  // namespace hal
}  // namespace quantix
