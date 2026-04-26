// ============================================================================
// Pages.cpp - Páginas web del portal QuantiX (estilo Agro Parallel)
// Genera HTML inline con tema oscuro + verde acento #7ac943
// ============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include "Globals.h"
#include "Structs.h"

extern char uid[13];

// CSS compartido (tema Agro Parallel oscuro)
static const char CSS[] PROGMEM = R"(
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#000;color:#f0f2f5;font-family:'Segoe UI',sans-serif;padding:16px;max-width:600px;margin:auto}
h1{color:#7ac943;font-size:20px;margin-bottom:4px}
h2{color:#7ac943;font-size:15px;margin:16px 0 8px;border-bottom:1px solid #222;padding-bottom:4px}
.sub{color:#8c919b;font-size:12px;margin-bottom:16px}
.card{background:#121214;border:1px solid #202024;border-radius:8px;padding:14px;margin-bottom:12px}
.card .accent{width:3px;background:#7ac943;border-radius:2px;position:absolute;left:0;top:8px;bottom:8px}
label{color:#8c919b;font-size:13px;display:block;margin-bottom:4px}
input[type=text],input[type=password],input[type=number],select{
  background:#0c0c0e;color:#7ac943;border:1px solid #202024;border-radius:4px;
  padding:8px 10px;width:100%;font-size:14px;font-family:Consolas,monospace;margin-bottom:10px}
input[type=range]{width:100%;accent-color:#7ac943}
.btn{background:#7ac943;color:#000;border:none;border-radius:6px;padding:10px 20px;
  font-weight:bold;font-size:14px;cursor:pointer;margin:4px}
.btn:hover{background:#8dd450}
.btn-sec{background:#202024;color:#f0f2f5}
.btn-sec:hover{background:#2a2a2e}
.btn-danger{background:#e74c4c;color:#fff}
.row{display:flex;gap:10px;align-items:center;flex-wrap:wrap}
.kpi{text-align:center;flex:1;min-width:80px}
.kpi .val{font-size:24px;font-weight:bold;color:#f0f2f5}
.kpi .lbl{font-size:11px;color:#4b4e55}
.led{width:10px;height:10px;border-radius:50%;display:inline-block;margin-right:6px}
.led-on{background:#7ac943}.led-off{background:#e74c4c}.led-warn{background:#f5a623}
.nav{display:flex;gap:8px;margin-bottom:16px}
.nav a{color:#8c919b;text-decoration:none;padding:6px 14px;border-radius:4px;font-weight:bold;font-size:13px}
.nav a.active{background:#7ac943;color:#000}
.nav a:hover{background:#1a1a1e}
footer{text-align:center;color:#4b4e55;font-size:11px;margin-top:20px;padding-top:10px;border-top:1px solid #202024}
</style>
)";

// Header compartido
static String pageHeader(const char* title, int activePage) {
    String h = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>QuantiX</title>";
    h += FPSTR(CSS);
    h += "</head><body>";
    h += "<h1>🌿 Agro Parallel — QuantiX</h1>";
    h += "<div class='sub'>Nodo: " + String(uid) + " · " + String(title) + "</div>";
    // Nav
    h += "<div class='nav'>";
    h += "<a href='/' class='"; h += (activePage == 0 ? "active" : ""); h += "'>Estado</a>";
    h += "<a href='/p1' class='"; h += (activePage == 1 ? "active" : ""); h += "'>Motores</a>";
    h += "<a href='/p2' class='"; h += (activePage == 2 ? "active" : ""); h += "'>WiFi</a>";
    h += "</div>";
    return h;
}

static String pageFooter() {
    return "<footer>QuantiX · Agro Parallel · FW 1.1.0</footer></body></html>";
}

// ================================================================
// Página 0: Estado y KPIs
// ================================================================
String GetPage0() {
    String p = pageHeader("Estado", 0);

    // KPIs
    p += "<div class='card'><div class='row'>";
    for (int i = 0; i < MDL.SensorCount; i++) {
        p += "<div class='kpi'><div class='val'>" + String((int)Sensor[i].RPM) + "</div><div class='lbl'>RPM M" + String(i) + "</div></div>";
        p += "<div class='kpi'><div class='val'>" + String((int)Sensor[i].PWM) + "</div><div class='lbl'>PWM M" + String(i) + "</div></div>";
    }
    p += "<div class='kpi'><div class='val'>" + String(millis() / 1000) + "</div><div class='lbl'>Uptime s</div></div>";
    p += "</div></div>";

    // Estado motores
    for (int i = 0; i < MDL.SensorCount; i++) {
        p += "<div class='card'>";
        p += "<h2>Motor " + String(i) + " — " + String(Sensor[i].FlowEnabled ? "ACTIVO" : "PARADO") + "</h2>";
        p += "<div class='row'>";
        p += "<div class='kpi'><div class='val'>" + String(Sensor[i].Hz, 1) + "</div><div class='lbl'>Hz Real</div></div>";
        p += "<div class='kpi'><div class='val'>" + String(Sensor[i].TargetUPM, 1) + "</div><div class='lbl'>Hz Target</div></div>";
        p += "<div class='kpi'><div class='val'>" + String(Sensor[i].TotalPulses) + "</div><div class='lbl'>Pulsos</div></div>";
        p += "<div class='kpi'><div class='val'>" + String(Sensor[i].MeterCal, 1) + "</div><div class='lbl'>MeterCal</div></div>";
        p += "</div>";

        // LED de estado
        p += "<p style='margin-top:8px'>";
        p += "<span class='led " + String(Sensor[i].FlowEnabled ? "led-on" : "led-off") + "'></span>";
        p += Sensor[i].FlowEnabled ? "Sección ON" : "Sección OFF";
        if (Sensor[i].CalibActive) p += " · <span class='led led-warn'></span>CALIBRANDO";
        p += "</p></div>";
    }

    // WiFi + MQTT status
    p += "<div class='card'>";
    p += "<span class='led " + String(WiFi.status() == WL_CONNECTED ? "led-on" : "led-off") + "'></span>";
    p += "WiFi: " + String(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "Desconectado");
    p += " · SSID: " + String(MDLnetwork.SSID);
    p += "</div>";

    p += pageFooter();
    return p;
}

// ================================================================
// Página 1: Configuración de Motores (PID, PWM, Calibración)
// ================================================================
String GetPage1() {
    String p = pageHeader("Motores", 1);

    p += "<form method='POST' action='/p1'>";

    for (int i = 0; i < MDL.SensorCount; i++) {
        p += "<div class='card'><h2>Motor " + String(i) + "</h2>";

        p += "<div class='row'>";
        p += "<div><label>Kp</label><input type='number' step='0.1' name='kp" + String(i) + "' value='" + String(Sensor[i].Kp, 2) + "'></div>";
        p += "<div><label>Ki</label><input type='number' step='0.1' name='ki" + String(i) + "' value='" + String(Sensor[i].Ki, 2) + "'></div>";
        p += "</div>";

        p += "<div class='row'>";
        p += "<div><label>PWM Min</label><input type='number' name='pmin" + String(i) + "' value='" + String(Sensor[i].MinPWM) + "'></div>";
        p += "<div><label>PWM Max</label><input type='number' name='pmax" + String(i) + "' value='" + String(Sensor[i].MaxPWM) + "'></div>";
        p += "<div><label>MeterCal</label><input type='number' step='0.1' name='mcal" + String(i) + "' value='" + String(Sensor[i].MeterCal, 1) + "'></div>";
        p += "</div>";

        p += "<div class='row'>";
        p += "<div><label>Deadband %</label><input type='number' name='db" + String(i) + "' value='" + String(Sensor[i].Deadband) + "'></div>";
        p += "<div><label>SlewRate</label><input type='number' name='sr" + String(i) + "' value='" + String(Sensor[i].SlewRate) + "'></div>";
        p += "</div>";

        p += "</div>";
    }

    p += "<button type='submit' class='btn'>✓ GUARDAR</button>";
    p += "</form>";

    p += pageFooter();
    return p;
}

// ================================================================
// Página 2: WiFi Manager (escaneo + conexión)
// ================================================================
String GetPage2() {
    String p = pageHeader("WiFi", 2);

    p += "<div class='card'>";
    p += "<h2>Conexión WiFi</h2>";

    // Estado actual
    p += "<p><span class='led " + String(WiFi.status() == WL_CONNECTED ? "led-on" : "led-off") + "'></span>";
    if (WiFi.status() == WL_CONNECTED) {
        p += "Conectado a: <b>" + String(MDLnetwork.SSID) + "</b> · IP: " + WiFi.localIP().toString();
    } else {
        p += "No conectado";
    }
    p += "</p></div>";

    // Form de conexión
    p += "<form method='POST' action='/'>";

    // Escaneo de redes
    p += "<div class='card'><h2>Redes disponibles</h2>";
    int n = WiFi.scanNetworks();
    if (n > 0) {
        p += "<select onchange=\"document.getElementById('ssid').value=this.value\" style='margin-bottom:8px'>";
        p += "<option value=''>Seleccioná una red...</option>";
        for (int i = 0; i < n; i++) {
            p += "<option value='" + WiFi.SSID(i) + "'>" + WiFi.SSID(i);
            p += " (" + String(WiFi.RSSI(i)) + " dBm)</option>";
        }
        p += "</select>";
    } else {
        p += "<p style='color:#8c919b'>No se encontraron redes. Recargá la página.</p>";
    }
    p += "</div>";

    p += "<div class='card'>";
    p += "<label>SSID (nombre de red)</label>";
    p += "<input type='text' id='ssid' name='prop1' value='" + String(MDLnetwork.SSID) + "'>";
    p += "<label>Contraseña</label>";
    p += "<input type='password' name='prop2' value='" + String(MDLnetwork.Password) + "'>";
    p += "<label>Password del AP (hotspot)</label>";
    p += "<input type='text' name='prop3' value='" + String(MDL.APpassword) + "'>";
    p += "<label style='margin-top:8px'><input type='checkbox' name='connect'";
    if (MDLnetwork.WifiModeUseStation) p += " checked";
    p += "> Conectar a router (Station Mode)</label>";
    p += "</div>";

    p += "<button type='submit' class='btn'>📶 CONECTAR Y REINICIAR</button>";
    p += "</form>";

    // AP info
    p += "<div class='card'>";
    p += "<h2>Access Point</h2>";
    p += "<p>AP SSID: <b>QX-" + String(uid + 3) + "</b></p>";
    p += "<p>AP IP: " + WiFi.softAPIP().toString() + "</p>";
    p += "<p>Password: " + String(MDL.APpassword) + "</p>";
    p += "</div>";

    p += pageFooter();
    return p;
}
