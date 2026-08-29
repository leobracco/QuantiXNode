// ============================================================================
// Pages.cpp - Portal FlowX (estilo Agro Parallel) — versión simplificada.
// Sólo 2 páginas:
//   /   → CORTES manuales de las 7 secciones (override que pisa al target MQTT)
//   /p2 → RED (WiFi STA + AP + Broker MQTT)
// La configuración de PID, MeterCal y modo electroválvula vive sólo por MQTT
// (FlowXBridge en AOG la maneja). Acá el operario sólo corta secciones y
// configura la red.
// ============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "Globals.h"
#include "Structs.h"

extern char uid[13];
extern PubSubClient mqttClient;

// Cantidad de secciones expuestas en el portal — pedido del operario.
static const int PORTAL_SECTION_COUNT = 7;

// CSS compartido (tema Agro Parallel oscuro).
static const char CSS[] PROGMEM = R"(
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#000;color:#f0f2f5;font-family:'Segoe UI',sans-serif;padding:16px;max-width:640px;margin:auto}
h1{color:#7ac943;font-size:20px;margin-bottom:4px}
h2{color:#7ac943;font-size:15px;margin:16px 0 8px;border-bottom:1px solid #222;padding-bottom:4px}
.sub{color:#8c919b;font-size:12px;margin-bottom:16px}
.card{background:#121214;border:1px solid #202024;border-radius:8px;padding:14px;margin-bottom:12px;position:relative}
label{color:#8c919b;font-size:13px;display:block;margin-bottom:4px}
input[type=text],input[type=password],input[type=number],select{
  background:#0c0c0e;color:#7ac943;border:1px solid #202024;border-radius:4px;
  padding:8px 10px;width:100%;font-size:14px;font-family:Consolas,monospace;margin-bottom:10px}
.btn{background:#7ac943;color:#000;border:none;border-radius:6px;padding:10px 20px;
  font-weight:bold;font-size:14px;cursor:pointer;margin:4px;text-decoration:none;display:inline-block}
.btn:hover{background:#8dd450}
.btn-sec{background:#202024;color:#f0f2f5}
.btn-sec:hover{background:#2a2a2e}
.btn-danger{background:#e74c4c;color:#fff}
.btn-danger:hover{background:#ff5e5e}
.row{display:flex;gap:10px;align-items:center;flex-wrap:wrap}
.led{width:10px;height:10px;border-radius:50%;display:inline-block;margin-right:6px;vertical-align:middle}
.led-on{background:#7ac943}.led-off{background:#e74c4c}.led-warn{background:#f5a623}
.nav{display:flex;gap:8px;margin-bottom:16px;flex-wrap:wrap}
.nav a{color:#8c919b;text-decoration:none;padding:6px 14px;border-radius:4px;font-weight:bold;font-size:13px}
.nav a.active{background:#7ac943;color:#000}
.nav a:hover{background:#1a1a1e}

/* Grid de cortes de secciones */
.sec-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(120px,1fr));gap:10px;margin-top:10px}
.sec-card{background:#0c0c0e;border:2px solid #202024;border-radius:8px;padding:14px;text-align:center;text-decoration:none;color:#f0f2f5;transition:all .12s}
.sec-card .sec-num{font-size:11px;color:#8c919b;margin-bottom:6px}
.sec-card .sec-state{font-size:18px;font-weight:bold}
.sec-card.s-free{border-color:#7ac943}
.sec-card.s-free .sec-state{color:#7ac943}
.sec-card.s-cut{border-color:#e74c4c;background:#1a0808}
.sec-card.s-cut .sec-state{color:#e74c4c}
.sec-card:hover{transform:scale(1.03);border-color:#f5a623}

footer{text-align:center;color:#4b4e55;font-size:11px;margin-top:20px;padding-top:10px;border-top:1px solid #202024}
</style>
)";

// Header compartido con nav de 2 entradas.
static String pageHeader(const char *title, int activePage)
{
    String h = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
    h += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
    h += "<title>FlowX</title>";
    h += FPSTR(CSS);
    h += "</head><body>";
    h += "<h1>🌿 Agro Parallel — FlowX</h1>";
    h += "<div class='sub'>Nodo: " + String(uid) + " · " + String(title) + "</div>";
    h += "<div class='nav'>";
    h += "<a href='/' class='";    h += (activePage == 0 ? "active" : ""); h += "'>Cortes</a>";
    h += "<a href='/p2' class='";  h += (activePage == 2 ? "active" : ""); h += "'>Red</a>";
    h += "</div>";
    return h;
}

static String pageFooter()
{
    String f = "<footer>FlowX · Agro Parallel · FW ";
    f += FW_VERSION;
    f += "</footer></body></html>";
    return f;
}

// ============================================================================
// Página 0 — Cortes manuales de las 7 secciones
//
// Cada tarjeta es un link a "/?toggle=N" — un click invierte el estado de
// override de esa sección. El estado mostrado es el MASK de override
// (sectionForceOff), NO el estado físico del relay: el relay puede estar
// apagado porque AOG no mandó "sec[i]=1" en el último target. Lo que se ve
// acá es: "¿la estoy forzando-apagada desde el portal?" .
// ============================================================================
String GetPage0()
{
    String p = pageHeader("Cortes", 0);

    // ── Acciones rápidas ───────────────────────────────────────────
    p += "<div class='card'><h2>Acciones rápidas</h2>";
    p += "<a href='/?cutall=1' class='btn btn-danger'>⛔ CORTAR TODO</a>";
    p += "<a href='/?freeall=1' class='btn'>✓ LIBERAR TODO</a>";
    p += "</div>";

    // ── Grid de secciones (7 secciones expuestas en el portal) ─────
    p += "<div class='card'><h2>Secciones (click para cortar/habilitar)</h2>";
    // Cada tarjeta muestra DOS cosas distintas, no las confundas:
    //   · Título grande  = quién MANDA esta sección (AUTO = la decide AOG /
    //                       CORTADA = forzada apagada a mano desde este portal).
    //   · Línea de abajo = cómo está la válvula AHORA MISMO (abierta/cerrada).
    // Una sección en AUTO puede estar "cerrada" si AOG no la abrió todavía:
    // no es contradicción, son dos niveles (permiso vs estado real).
    p += "<p class='sub'>Verde = AUTO (la maneja AOG) &middot; Rojo = CORTADA a mano. "
         "Abajo de cada tarjeta: la v&aacute;lvula en este momento.</p>";
    p += "<div class='sec-grid'>";
    for (int i = 0; i < PORTAL_SECTION_COUNT; i++)
    {
        bool forced = (sectionForceOff >> i) & 0x1;
        bool physOn = (seccionesBits >> i) & 0x1;
        p += "<a href='/?toggle=" + String(i) + "' class='sec-card "
           + (forced ? "s-cut" : "s-free") + "'>";
        p += "<div class='sec-num'>Secci&oacute;n " + String(i) + "</div>";
        p += "<div class='sec-state'>" + String(forced ? "CORTADA" : "AUTO (AOG)") + "</div>";
        p += "<div class='sub' style='margin-top:6px'>";
        p += "<span class='led " + String(physOn ? "led-on" : "led-off") + "'></span>";
        p += physOn ? "v&aacute;lvula abierta" : "v&aacute;lvula cerrada";
        p += "</div>";
        p += "</a>";
    }
    p += "</div></div>";

    // ── Estado WiFi + MQTT (compacto) ──────────────────────────────
    p += "<div class='card'>";
    p += "<span class='led " + String(WiFi.status() == WL_CONNECTED ? "led-on" : "led-off") + "'></span>";
    p += "WiFi: " + String(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "desconectado");
    p += "<br><span class='led " + String(mqttClient.connected() ? "led-on" : "led-off") + "'></span>";
    p += "MQTT: " + String(mqttClient.connected() ? "conectado" : "desconectado");
    p += "</div>";

    p += pageFooter();
    return p;
}

// ============================================================================
// Página 2 — Red (WiFi STA + AP + Broker MQTT)
// Idéntica funcionalmente a la versión anterior — solo cambia el nav.
// ============================================================================
String GetPage2()
{
    // Decidir si necesitamos auto-refresh ANTES del header. Si el scan está
    // corriendo, metemos meta refresh=6 para que el browser vuelva solo.
    int n = WiFi.scanComplete();
    bool scanRunning = (n == WIFI_SCAN_FAILED || n == WIFI_SCAN_RUNNING);
    if (n == WIFI_SCAN_FAILED) WiFi.scanNetworks(true);

    String p = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
    p += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
    if (scanRunning) p += "<meta http-equiv='refresh' content='6'>";
    p += "<title>FlowX</title>";
    p += FPSTR(CSS);
    p += "</head><body>";
    p += "<h1>🌿 Agro Parallel — FlowX</h1>";
    p += "<div class='sub'>Nodo: " + String(uid) + " · Red</div>";
    p += "<div class='nav'>";
    p += "<a href='/'>Cortes</a>";
    p += "<a href='/p2' class='active'>Red</a>";
    p += "</div>";

    // ── Estado WiFi actual ────────────────────────────────────────
    p += "<div class='card'><h2>Conexión actual</h2>";
    p += "<p><span class='led " + String(WiFi.status() == WL_CONNECTED ? "led-on" : "led-off") + "'></span>";
    if (WiFi.status() == WL_CONNECTED)
        p += "Conectado a: <b>" + String(MDLnetwork.SSID) + "</b> · IP: " + WiFi.localIP().toString();
    else
        p += "STA no conectada";
    p += "</p></div>";

    // ── Form — POST a / (handleCredentials) ────────────────────────
    p += "<form method='POST' action='/'>";

    p += "<div class='card'><h2>Redes disponibles</h2>";
    if (scanRunning)
    {
        p += "<p class='sub'>⏳ Escaneando redes (5-10 s en modo AP+STA)... esta página se recarga sola.</p>";
        p += "<p class='sub' style='margin-top:6px'>Si conocés el SSID, podés escribirlo a mano abajo y guardar — no es necesario esperar al scan.</p>";
    }
    else if (n > 0)
    {
        p += "<select onchange=\"document.getElementById('ssid').value=this.value\">";
        p += "<option value=''>— Seleccioná una red (" + String(n) + " encontradas) —</option>";
        for (int i = 0; i < n; i++)
        {
            p += "<option value='" + WiFi.SSID(i) + "'>" + WiFi.SSID(i);
            p += " (" + String(WiFi.RSSI(i)) + " dBm)</option>";
        }
        p += "</select>";
        p += "<p class='sub'><a href='/p2?rescan=1' style='color:#7ac943'>↻ Re-escanear</a></p>";
    }
    else
    {
        p += "<p class='sub'>No se encontraron redes.</p>";
        p += "<p class='sub'><a href='/p2?rescan=1' style='color:#7ac943'>↻ Reintentar scan</a></p>";
    }
    p += "</div>";

    // ── Credenciales STA ─────────────────────────────────────────
    p += "<div class='card'><h2>WiFi Station (LAN del tractor)</h2>";
    p += "<label>SSID</label>";
    p += "<input type='text' id='ssid' name='prop1' value='" + String(MDLnetwork.SSID) + "'>";
    p += "<label>Password</label>";
    p += "<input type='password' name='prop2' value='" + String(MDLnetwork.Password) + "'>";
    p += "<label style='margin-top:6px'><input type='checkbox' name='connect'";
    if (MDLnetwork.WifiModeUseStation) p += " checked";
    p += "> Conectar a router (Station Mode)</label>";
    p += "</div>";

    // ── Password del AP ──────────────────────────────────────────
    p += "<div class='card'><h2>Access Point</h2>";
    p += "<p class='sub'>SSID del AP: <b>FX-" + String(uid + 3) + "</b> · IP: "
       + WiFi.softAPIP().toString() + "</p>";
    p += "<label>Password AP (min 8 chars)</label>";
    p += "<input type='text' name='prop3' value='" + String(MDL.APpassword) + "'>";
    p += "</div>";

    // ── Broker MQTT ──────────────────────────────────────────────
    p += "<div class='card'><h2>Broker MQTT</h2>";
    p += "<p class='sub'>IP/hostname de la PC con AgIO. Default 192.168.5.10:1883.</p>";
    p += "<label>Host</label>";
    p += "<input type='text' name='bhost' value='"
       + String(MDLnetwork.BrokerHost[0] ? MDLnetwork.BrokerHost : "192.168.5.10") + "'>";
    p += "<label>Puerto</label>";
    p += "<input type='number' min='1' max='65535' name='bport' value='"
       + String(MDLnetwork.BrokerPort ? MDLnetwork.BrokerPort : 1883) + "'>";
    p += "</div>";

    p += "<button type='submit' class='btn'>📶 GUARDAR Y REINICIAR</button>";
    p += "</form>";

    p += pageFooter();
    return p;
}
