#pragma once

#include "IFilesystem.h"
#include <LittleFS.h>
#include "Log.h"

namespace quantix {
namespace hal {

class LittleFsFilesystem final : public IFilesystem {
public:
    bool exists(const char* path) override {
        return LittleFS.exists(path);
    }

    bool readJson(const char* path, JsonDocument& out) override {
        File f = LittleFS.open(path, "r");
        if (!f) {
            LOG_E("fs", "No se pudo abrir %s para lectura", path);
            return false;
        }
        DeserializationError err = deserializeJson(out, f);
        f.close();
        if (err) {
            LOG_E("fs", "JSON inválido en %s: %s", path, err.c_str());
            return false;
        }
        return true;
    }

    bool writeJson(const char* path, const JsonDocument& in) override {
        File f = LittleFS.open(path, "w");
        if (!f) {
            LOG_E("fs", "No se pudo abrir %s para escritura", path);
            return false;
        }
        size_t n = serializeJson(in, f);
        f.close();
        if (n == 0) {
            LOG_E("fs", "Escritura vacía en %s", path);
            return false;
        }
        return true;
    }
};

}  // namespace hal
}  // namespace quantix
