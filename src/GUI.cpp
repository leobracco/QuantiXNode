// ============================================================================
// GUI.cpp - Handlers HTTP del portal FlowX — versión simplificada.
//   /   GET  ?toggle=N  → invierte override de la sección N
//   /   GET  ?cutall=1  → corta TODAS las secciones del portal
//   /   GET  ?freeall=1 → libera TODAS las secciones
//   /   GET             → muestra Page0 (cortes)
//   /   POST            → handleCredentials (form de red postea acá)
//   /p2 GET             → Page2 (config de red)
// ============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include "Globals.h"
#include "Structs.h"

extern WebServer server;

// Definidas en Pages.cpp
String GetPage0();
String GetPage2();

void handleCredentials();

// Cantidad de secciones expuestas en el portal — DEBE coincidir con
// PORTAL_SECTION_COUNT en Pages.cpp.
static const int PORTAL_SECTION_COUNT = 7;

// ============================================================================
// HandleRoot — "/"
// ============================================================================
void HandleRoot()
{
    Serial.printf("[HTTP] %s / desde %s\n",
                  server.method() == HTTP_POST ? "POST" : "GET",
                  server.client().remoteIP().toString().c_str());

    // POST de credenciales (form de Page2 postea acá)
    if (server.hasArg("prop1") || server.hasArg("bhost"))
    {
        handleCredentials();
        return;
    }

    // ── Acciones de override de secciones ──────────────────────────
    bool changed = false;

    if (server.hasArg("toggle"))
    {
        int i = server.arg("toggle").toInt();
        if (i >= 0 && i < PORTAL_SECTION_COUNT)
        {
            sectionForceOff ^= (uint16_t)(1u << i);
            changed = true;
            Serial.printf("[CORTES] toggle S%d → mask=0x%04X\n", i, sectionForceOff);
        }
    }
    else if (server.hasArg("cutall"))
    {
        sectionForceOff = (uint16_t)((1u << PORTAL_SECTION_COUNT) - 1); // bits 0..N-1
        changed = true;
        Serial.printf("[CORTES] CUT ALL → mask=0x%04X\n", sectionForceOff);
    }
    else if (server.hasArg("freeall"))
    {
        sectionForceOff = 0;
        changed = true;
        Serial.println("[CORTES] FREE ALL");
    }

    if (changed)
    {
        // Re-aplicar al toque: si AOG está mandando target, processValves()
        // usa el ÚLTIMO bits recibido (lo guarda en seccionesBits). Volvemos
        // a llamar processValves(seccionesBits) — el mask de override pisa.
        processValves(seccionesBits);
    }

    server.send(200, "text/html", GetPage0());
}

// ============================================================================
// HandlePage2 — "/p2" (config de red)
// ============================================================================
void HandlePage2()
{
    if (server.hasArg("rescan"))
    {
        WiFi.scanDelete();
        WiFi.scanNetworks(true);
        Serial.println("[HTTP] rescan WiFi pedido por user");
    }
    server.send(200, "text/html", GetPage2());
}

// ============================================================================
// handleCredentials — procesa el form de red (SSID/Pass + AP + Broker)
// ============================================================================
void handleCredentials()
{
    bool   OldMode         = MDLnetwork.WifiModeUseStation;
    String OldSSID         = String(MDLnetwork.SSID);
    String OldPassword     = String(MDLnetwork.Password);
    String OldAPPassword   = String(MDL.APpassword);
    String OldBrokerHost   = String(MDLnetwork.BrokerHost);
    uint16_t OldBrokerPort = MDLnetwork.BrokerPort;

    if (server.hasArg("prop1"))
    {
        String s = server.arg("prop1"); s.trim();
        s.toCharArray(MDLnetwork.SSID, sizeof(MDLnetwork.SSID));
    }
    if (server.hasArg("prop2"))
    {
        String s = server.arg("prop2"); s.trim();
        s.toCharArray(MDLnetwork.Password, sizeof(MDLnetwork.Password));
    }
    MDLnetwork.WifiModeUseStation = server.hasArg("connect");

    if (server.hasArg("prop3"))
    {
        String s = server.arg("prop3"); s.trim();
        const size_t kMaxApLen = sizeof(MDL.APpassword) - 1;
        if (s.length() > kMaxApLen) s.remove(kMaxApLen);
        if (s.length() == 0 || s.length() >= 8)
            s.toCharArray(MDL.APpassword, sizeof(MDL.APpassword));
    }

    if (server.hasArg("bhost"))
    {
        String h = server.arg("bhost"); h.trim();
        h.toCharArray(MDLnetwork.BrokerHost, sizeof(MDLnetwork.BrokerHost));
    }
    if (server.hasArg("bport"))
    {
        long p = server.arg("bport").toInt();
        if (p >= 1 && p <= 65535) MDLnetwork.BrokerPort = (uint16_t)p;
    }

    server.send(200, "text/html", GetPage0());

    bool stationChanged = (MDLnetwork.WifiModeUseStation != OldMode) ||
                          (String(MDLnetwork.SSID)     != OldSSID) ||
                          (String(MDLnetwork.Password) != OldPassword);
    bool brokerChanged  = (String(MDLnetwork.BrokerHost) != OldBrokerHost) ||
                          (MDLnetwork.BrokerPort != OldBrokerPort);
    bool apChanged      = (String(MDL.APpassword) != OldAPPassword);

    if (stationChanged || brokerChanged) SaveNetworks();
    if (apChanged)                       SaveData();

    if (stationChanged || brokerChanged || apChanged)
    {
        delay(500);
        ESP.restart();
    }
}
