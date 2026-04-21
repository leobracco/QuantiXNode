#include <Arduino.h>
#include <WebServer.h>
#include "Globals.h"
#include "Structs.h"
#include "Constants.h"
#include "Log.h"

extern WebServer server;

namespace {
// Auth HTTP Basic reutilizada por todos los handlers del portal.
bool requireAuth()
{
    if (!server.authenticate(MDL.HttpUser, MDL.HttpPass))
    {
        server.requestAuthentication();
        return false;
    }
    return true;
}
}  // namespace

// --- PROTOTIPOS DE FUNCIONES EXTERNAS ---
// Estas funciones están definidas en PgStart.cpp, PgSwitches.cpp y PgNetwork.cpp
// Las declaramos aquí para poder usarlas.
String GetPage0(); // Página de Inicio
String GetPage1(); // Página de Switches/Sensores
String GetPage2(); // Página de Red

// --- PROTOTIPOS LOCALES ---
void handleCredentials();

// --- MANEJADORES DE RUTAS (HANDLERS) ---

void HandleRoot()
{
    if (!requireAuth()) return;

    if (server.hasArg("prop1"))
    {
        handleCredentials();
    }
    else
    {
        server.send(200, "text/html", GetPage0());
    }
}

void HandlePage1()
{
    if (!requireAuth()) return;
    server.send(200, "text/html", GetPage1());
}

void HandlePage2()
{
    if (!requireAuth()) return;
    server.send(200, "text/html", GetPage2());
}

// --- LÓGICA DE GUARDADO DE CONFIGURACIÓN ---

void handleCredentials()
{
    bool OldMode = MDLnetwork.WifiModeUseStation;
    String OldSSID = String(MDLnetwork.SSID);
    String OldPassword = String(MDLnetwork.Password);
    String OldAPPassword = String(MDL.APpassword);

    // 1. Recoger Argumentos del Formulario
    String newSSID = server.arg("prop1");
    newSSID.trim();
    
    String newPassword = server.arg("prop2");
    newPassword.trim();

    // Password del Hotspot (AP)
    String newAPPassword = OldAPPassword;
    if (server.hasArg("prop3"))
    {
        newAPPassword = server.arg("prop3");
        newAPPassword.trim();
        
        // Limitar longitud máxima para evitar desbordes
        const size_t kMaxApLen = 10;
        if (newAPPassword.length() > kMaxApLen)
        {
            newAPPassword.remove(kMaxApLen);
        }
    }

    // 2. Actualizar Estructuras en Memoria
    newSSID.toCharArray(MDLnetwork.SSID, sizeof(MDLnetwork.SSID));
    newPassword.toCharArray(MDLnetwork.Password, sizeof(MDLnetwork.Password));
    
    // Checkbox "Connect to Router"
    MDLnetwork.WifiModeUseStation = server.hasArg("connect");

    if (server.hasArg("prop3"))
    {
        newAPPassword.toCharArray(MDL.APpassword, sizeof(MDL.APpassword));
    }

    // 3. Enviar respuesta al navegador (Renderizar página actualizada)
    server.send(200, "text/html", GetPage0());

    // 4. Detectar Cambios y Guardar en EEPROM
    bool stationChanged = (MDLnetwork.WifiModeUseStation != OldMode) ||
                          (String(MDLnetwork.SSID) != OldSSID) ||
                          (String(MDLnetwork.Password) != OldPassword);

    bool apChanged = (String(MDL.APpassword) != OldAPPassword);

    if (stationChanged)
    {
        SaveNetworks(); // Función en Begin.cpp
    }

    if (apChanged)
    {
        SaveData();     // Función en Begin.cpp
    }

    // 5. Reinicio diferido: el loop principal llama a ESP.restart() cuando
    //    pasa `PENDING_RESTART_MS` desde `ResetTime`. Así el navegador recibe
    //    la respuesta y no bloqueamos el task actual.
    if (stationChanged || apChanged)
    {
        ResetTime = millis();
        LOG_I("web", "Credenciales actualizadas, reinicio diferido");
    }
}