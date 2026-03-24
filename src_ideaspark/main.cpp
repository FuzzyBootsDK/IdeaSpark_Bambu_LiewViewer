/*
 * Ideaspark ESP32-C6 + 1.9" SPI ST7789  (170×320)
 * Bambu Lab Printer Monitor
 *
 * Phase 1 – No config saved:
 *   Starts AP "ESP32-Setup" → captive portal
 *   User fills in: WiFi SSID/password + Bambu IP / Access Code / Serial
 *   Everything is saved to NVS and the device restarts.
 *
 * Phase 2 – Config loaded:
 *   Connects to WiFi → syncs NTP (Copenhagen CET/CEST, GMT+1/+2)
 *   Connects to Bambu printer via MQTT-over-TLS on port 8883
 *   Display (landscape 320 × 170):
 *     Row 1 – "Live Tracking" centred + glass orb pulse indicator + signal bars
 *     Row 2 – glass pill progress bar + %
 *     Row 3 – ETA box: local completion time or remaining minutes
 *     Row 4 – TIME LEFT / LAYER info row
 *     Row 5 – Footer: printer IP + WiFi IP
 *   Browser page at the ESP's IP shows a simple status page.
 *
 * Wiring (Ideaspark ESP32-C6 → 1.9" LCD)
 * ────────────────────────────────────────
 *  SPI SCLK → GPIO 18   SPI MOSI → GPIO 23
 *  LCD CS   → GPIO 15   LCD DC   → GPIO  2
 *  LCD RST  → GPIO  4   LCD BLK  → GPIO 32
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <LovyanGFX.hpp>
#include <time.h>

// ─── Display ──────────────────────────────────────────────────────────────────

class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_ST7789 _panel;
    lgfx::Bus_SPI      _bus;
    lgfx::Light_PWM    _light;
public:
    LGFX() {
        {
            auto cfg        = _bus.config();
            cfg.spi_host    = SPI2_HOST;
            cfg.spi_mode    = 0;
            cfg.freq_write  = 40000000;
            cfg.freq_read   = 16000000;
            cfg.spi_3wire   = false;
            cfg.use_lock    = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk    = 18;
            cfg.pin_mosi    = 23;
            cfg.pin_miso    = -1;
            cfg.pin_dc      = 2;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg          = _panel.config();
            cfg.pin_cs        = 15;
            cfg.pin_rst       = 4;
            cfg.pin_busy      = -1;
            cfg.memory_width  = 240;
            cfg.memory_height = 320;
            cfg.panel_width   = 170;
            cfg.panel_height  = 320;
            cfg.offset_x      = 35;
            cfg.offset_y      = 0;
            cfg.invert        = true;
            cfg.rgb_order     = false;
            cfg.dlen_16bit    = false;
            cfg.bus_shared    = false;
            _panel.config(cfg);
        }
        {
            auto cfg        = _light.config();
            cfg.pin_bl      = 32;
            cfg.invert      = false;
            cfg.freq        = 44100;
            cfg.pwm_channel = 0;
            _light.config(cfg);
            _panel.setLight(&_light);
        }
        setPanel(&_panel);
    }
};

static LGFX        display;
static LGFX_Sprite canvas(&display);  // off-screen frame buffer – eliminates flicker

// ─── Network / MQTT ───────────────────────────────────────────────────────────

static const char*   AP_SSID  = "ESP32-Setup";
static const uint8_t DNS_PORT = 53;
static IPAddress     AP_IP(192, 168, 4, 1);

static WiFiClientSecure tlsClient;
static PubSubClient     mqtt(tlsClient);

// ─── State ────────────────────────────────────────────────────────────────────

static Preferences prefs;
static DNSServer   dns;
static WebServer   http(80);
static bool        portalMode = true;

// Printer state – updated live from MQTT
static String g_state       = "";   // "", IDLE, RUNNING, PAUSE, FINISH, FAILED
static int    g_percent     = -1;   // -1 = no data yet
static int    g_layer       = 0;
static int    g_totalLayers = 0;
static int    g_remaining   = -1;   // remaining minutes, -1 = unknown
static int    g_nozzleTemp  = -1;   // actual nozzle temp °C, -1 = unknown
static int    g_bedTemp     = -1;   // actual bed temp °C, -1 = unknown
static bool   g_dirty       = true; // true → redraw display in loop()
static bool   g_pulse       = false; // toggles each draw → pulsing live indicator
static bool   g_spriteOK    = false; // true when back-buffer sprite is allocated

// Config (loaded from NVS namespace "config")
static String cfg_ssid, cfg_pass, cfg_pip, cfg_code, cfg_serial;

// Timer for MQTT reconnect (initialised so the first attempt fires immediately)
static unsigned long g_lastMqttAttempt  = (unsigned long)(0UL - 20000UL);
static unsigned long g_lastDisplayDraw  = 0;

// Backlight idle-off: dims after 5 min of printer being idle
static bool          g_backlightOn  = true;
static unsigned long g_idleSinceMs  = 0;   // millis() when printer went idle (0 = active)

// MQTT failure tracking – revert to portal if printer is unreachable at boot
static int  g_mqttFailCount     = 0;
static bool g_mqttEverConnected = false;
static unsigned long g_lastMqttMessageMs = 0;
static unsigned long g_mqttMessageCount   = 0;

// ─── HTML ─────────────────────────────────────────────────────────────────────

static const char PORTAL_HTML[] PROGMEM = R"rawhtml(<!DOCTYPE html>
<html lang="en"><head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Device Setup</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,sans-serif;background:#0f172a;color:#e2e8f0;
     min-height:100vh;display:flex;align-items:center;justify-content:center;
     padding:1rem}
.card{background:#1e293b;border-radius:16px;padding:1.5rem 1.75rem;
      width:100%;max-width:430px;box-shadow:0 25px 50px rgba(0,0,0,.5)}
h2{color:#38bdf8;font-size:1rem;font-weight:700;margin:1.25rem 0 .5rem;
   padding-bottom:.35rem;border-bottom:1px solid #334155}
h2:first-child{margin-top:0}
label{font-size:.78rem;color:#94a3b8;display:block;margin:.6rem 0 .2rem}
input{width:100%;padding:.6rem .85rem;background:#0f172a;
      border:1px solid #334155;border-radius:6px;
      color:#e2e8f0;font-size:.95rem}
input:focus{outline:none;border-color:#38bdf8}
button{margin-top:1.25rem;width:100%;padding:.8rem;background:#38bdf8;
       color:#0f172a;border:none;border-radius:8px;
       font-size:1rem;font-weight:700;cursor:pointer}
button:hover{background:#7dd3fc}
.msg{margin-top:.75rem;text-align:center;font-size:.85rem;
     color:#f59e0b;min-height:1.2em}
</style></head>
<body><div class="card">
<h2>&#x1F4F6; WiFi</h2>
<form method="POST" action="/connect">
  <label>Network (SSID)</label>
  <input name="ssid" type="text" placeholder="Your WiFi name"
         value="%V_SSID%" required maxlength="32">
  <label>Password</label>
  <input name="pass" type="password"
         placeholder="Leave blank for open network" maxlength="64">

  <h2>&#x1F5A8; Bambu Lab Printer</h2>
  <label>Printer IP Address</label>
  <input name="pip" type="text" placeholder="192.168.1.x"
         value="%V_PIP%" required maxlength="39">
  <label>Access Code  <small style="color:#64748b">(shown on printer screen)</small></label>
  <input name="code" type="text" placeholder="8-character code"
         value="%V_CODE%" required maxlength="64">
  <label>Printer Serial Number</label>
  <input name="serial" type="text" placeholder="e.g. 01S00C123456789"
         value="%V_SERIAL%" required maxlength="64">

  <button type="submit">Save &amp; Connect &#x2192;</button>
</form>
<p class="msg">%MSG%</p>
</div></body></html>)rawhtml";

static const char STATUS_HTML[] PROGMEM = R"rawhtml(<!DOCTYPE html>
<html lang="en"><head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Bambu Monitor</title>
<style>
body{font-family:system-ui,sans-serif;background:#0f172a;color:#e2e8f0;
     min-height:100vh;display:flex;flex-direction:column;
     align-items:center;justify-content:center;gap:.5rem}
h1{font-size:2rem;color:#38bdf8}
p{color:#64748b}
</style></head>
<body>
<h1>Bambu Lab Monitor</h1>
<p>ESP32 online &#x2713;</p>
</body></html>)rawhtml";

// ─── Display helpers ──────────────────────────────────────────────────────────

static void drawAPScreen() {
    const int cx = display.width() / 2, cy = display.height() / 2;
    display.fillScreen(TFT_BLACK);
    display.setTextDatum(MC_DATUM);
    display.setFont(&lgfx::fonts::Font2);
    display.setTextColor(TFT_WHITE);
    display.drawString("Connect to WiFi:", cx, cy - 40);
    display.setFont(&lgfx::fonts::Font4);
    display.setTextColor(TFT_YELLOW);
    display.drawString(AP_SSID, cx, cy);
    display.setFont(&lgfx::fonts::Font0);
    display.setTextColor(TFT_CYAN);
    display.drawString("then open any browser", cx, cy + 40);
}

static void drawConnectingScreen(const String& ssid) {
    const int cx = display.width() / 2, cy = display.height() / 2;
    display.fillScreen(TFT_BLACK);
    display.setTextDatum(MC_DATUM);
    display.setFont(&lgfx::fonts::Font2);
    display.setTextColor(TFT_WHITE);
    display.drawString("Connecting to:", cx, cy - 16);
    display.setTextColor(TFT_YELLOW);
    display.drawString(ssid.c_str(), cx, cy + 16);
}

// Format minutes as "Xh Ym" (e.g. "2h 8m", "45m", "1h")
static String fmtDur(int minutes) {
    if (minutes <= 0) return (minutes == 0) ? "0m" : "---";
    int h = minutes / 60, m = minutes % 60;
    if (h > 0 && m > 0) return String(h) + "h " + String(m) + "m";
    return (h > 0) ? (String(h) + "h") : (String(m) + "m");
}

/*
 * Status screen – Bambu Lab "Live Tracking" style
 * Landscape 320 × 170
 *
 * All drawing goes into an off-screen LGFX_Sprite (if allocated at boot).
 * At the end the finished frame is pushed to the display in one SPI burst,
 * giving completely flicker-free updates.  If the sprite could not be
 * allocated (not enough heap) every draw call goes directly to the display
 * inside a startWrite/endWrite batch instead.
 *
 *  y   0-19   Header: "Live Tracking" centred + glass orb + signal bars
 *  y  22-43   Progress bar (blue glass pill) + % to the right
 *  y  50-104  ETA box: "ESTIMATED COMPLETION" label + time value
 *  y 111-157  Info row: TIME LEFT (left) | LAYER (right)
 *  y     163  Footer: printer IP + WiFi IP
 */
static void drawPrintStatus() {
    const int W = 320, H = 170;
    (void)H;

    Serial.printf("[DRAW] state=%s pct=%d layer=%d/%d rem=%dmin\n",
        g_state.c_str(), g_percent, g_layer, g_totalLayers, g_remaining);

    // Route all draw calls to the sprite back-buffer when available
    LovyanGFX& gfx = g_spriteOK
        ? static_cast<LovyanGFX&>(canvas)
        : static_cast<LovyanGFX&>(display);

    const uint32_t C_BG     = gfx.color565( 10,  14,  26);
    const uint32_t C_HDR    = gfx.color565( 17,  24,  52);
    const uint32_t C_BORDER = gfx.color565( 40,  58,  98);
    const uint32_t C_DIM    = gfx.color565( 65,  80, 115);
    const uint32_t C_LABEL  = gfx.color565(120, 140, 175);
    const uint32_t C_GREEN  = gfx.color565( 55, 215,  80);
    const uint32_t C_AMBER  = gfx.color565(235, 155,  20);
    const uint32_t C_BLUE   = gfx.color565( 59, 130, 246);
    const uint32_t C_TRACK  = gfx.color565( 22,  30,  62);
    const uint32_t C_TEXT   = gfx.color565(195, 205, 220);  // muted off-white for body text

    // For direct-draw fallback: open one SPI batch for the whole frame
    if (!g_spriteOK) display.startWrite();

    gfx.fillScreen(C_BG);

    // ── Header bar (y=0..19) ──────────────────────────────────────────────────
    gfx.fillRect(0, 0, W, 20, C_HDR);

    // Centre "Live Tracking" + glass orb as a single group
    g_pulse = !g_pulse;
    const int orbR = 5, orbGap = 7;
    gfx.setFont(&lgfx::fonts::Font2);
    int txtW  = gfx.textWidth("Live Tracking");
    int grpX  = (W - (txtW + orbGap + orbR * 2)) / 2;
    const int orbCY = 10;
    int orbCX = grpX + txtW + orbGap + orbR;

    gfx.setTextDatum(ML_DATUM);
    gfx.setTextColor(C_TEXT);
    gfx.drawString("Live Tracking", grpX, orbCY);

    // Glass orb: dark rim → base colour → bright highlight spot
    // Alternates green (live) ↔ light grey (pulse off) each redraw
    uint32_t orbEdge = g_pulse
        ? gfx.color565( 15, 100,  35)   // deep green rim
        : gfx.color565( 85,  95, 105);   // dark grey rim
    uint32_t orbBase = g_pulse
        ? gfx.color565( 40, 190,  70)   // mid green
        : gfx.color565(150, 160, 170);   // light grey
    uint32_t orbHi   = g_pulse
        ? gfx.color565(170, 255, 195)   // bright green glint
        : gfx.color565(225, 232, 242);   // near-white glint

    gfx.fillCircle(orbCX,     orbCY,     orbR,     orbEdge);
    gfx.fillCircle(orbCX,     orbCY,     orbR - 1, orbBase);
    gfx.fillCircle(orbCX - 2, orbCY - 2, 2,        orbHi);

    // Signal bars on far right
    for (int i = 0; i < 3; i++) {
        int sh = 5 + i * 4;
        gfx.fillRect(W - 30 + i * 6, 19 - sh, 5, sh,
            mqtt.connected() ? C_GREEN : C_DIM);
    }

    // ── Progress bar (y=22, h=22) ─────────────────────────────────────────────
    const int barX = 8, barY = 22, barH = 22, barW = W - 68;
    const int barR = barH / 2;  // pill radius = 11

    // Track (empty pill)
    gfx.fillRoundRect(barX, barY, barW, barH, barR, C_TRACK);
    gfx.drawRoundRect(barX, barY, barW, barH, barR, C_BORDER);

    const bool isFinish = (g_state == "FINISH");
    const bool isFailed = (g_state == "FAILED");
    int pct = isFinish ? 100 : constrain(g_percent >= 0 ? g_percent : 0, 0, 100);

    if (pct > 0) {
        int fillW = max(barH, (barW * pct / 100));
        if (fillW > barW) fillW = barW;

        // Base fill: solid blue pill
        gfx.fillRoundRect(barX, barY, fillW, barH, barR, C_BLUE);

        // Glass sheen: row-by-row highlight on top half (11 rows – fast)
        for (int row = 0; row < barH / 2; row++) {
            float alpha = 0.55f * (1.0f - (float)row / (float)(barH / 2));
            uint8_t r  = (uint8_t)(59  + alpha * (255 - 59));
            uint8_t gv = (uint8_t)(130 + alpha * (255 - 130));
            uint8_t bv = (uint8_t)(246 + alpha * (255 - 246));
            int rowInset = 0;
            if (row < barR) {
                int dy = barR - row;
                rowInset = barR - (int)sqrtf((float)(barR * barR - dy * dy));
            }
            int rowX = barX + rowInset;
            int rowW = fillW - rowInset;
            if (row < barR && fillW == barW) {
                int dy = barR - row;
                int dx = (int)sqrtf((float)(barR * barR - dy * dy));
                rowW = (barX + barW - barR + dx) - rowX;
            }
            if (rowW > 0)
                gfx.fillRect(rowX, barY + row, rowW, 1,
                    gfx.color565(r, gv, bv));
        }
    }

    gfx.setFont(&lgfx::fonts::Font4);
    gfx.setTextDatum(MC_DATUM);
    gfx.setTextColor(C_TEXT);
    gfx.drawString(
        (g_percent >= 0 ? String(pct) + "%" : "--").c_str(),
        barX + barW + (W - barX - barW) / 2, barY + barH / 2);

    // ── ETA box (y=50, h=55) ──────────────────────────────────────────────────
    const int ETA_Y = 50, ETA_H = 55;
    gfx.drawRect(4, ETA_Y, W - 8, ETA_H, C_BORDER);

    gfx.setFont(&lgfx::fonts::Font0);
    gfx.setTextDatum(TC_DATUM);
    gfx.setTextColor(C_LABEL);
    gfx.drawString("ESTIMATED COMPLETION", W / 2, ETA_Y + 5);

    gfx.setFont(&lgfx::fonts::Font4);
    gfx.setTextDatum(MC_DATUM);
    const int etaValY = ETA_Y + 34;
    if (isFinish) {
        gfx.setTextColor(C_GREEN);
        gfx.drawString("Print Complete!", W / 2, etaValY);
    } else if (isFailed) {
        gfx.setTextColor(TFT_RED);
        gfx.drawString("Print FAILED", W / 2, etaValY);
    } else if (g_state == "PREPARE") {
        gfx.setTextColor(C_AMBER);
        gfx.drawString("Heating up...", W / 2, etaValY);
    } else if (g_state.isEmpty() || g_state == "IDLE") {
        gfx.setTextColor(C_DIM);
        gfx.drawString("Printer Idle", W / 2, etaValY);
    } else if (g_remaining > 0 && g_remaining < 9999) {
        time_t now = time(nullptr);
        if (now > 1000000000UL) {   // NTP synced
            time_t    etaT  = now + (time_t)g_remaining * 60;
            struct tm etaTm = *localtime(&etaT);
            struct tm nowTm = *localtime(&now);
            char buf[24];
            if (etaTm.tm_mday == nowTm.tm_mday && etaTm.tm_mon == nowTm.tm_mon)
                strftime(buf, sizeof(buf), "Today at %H:%M", &etaTm);
            else
                strftime(buf, sizeof(buf), "Tomorrow %H:%M", &etaTm);
            gfx.setTextColor(C_TEXT);
            gfx.drawString(buf, W / 2, etaValY);
        } else {
            gfx.setTextColor(C_TEXT);
            gfx.drawString(("~" + String(g_remaining) + " min").c_str(), W / 2, etaValY);
        }
    } else {
        gfx.setTextColor(C_DIM);
        gfx.drawString("---", W / 2, etaValY);
    }

    // ── Info row (y=111, h=47) ────────────────────────────────────────────────
    const int LYR_Y = 111, LYR_H = 47;
    gfx.drawRect(4, LYR_Y, W - 8, LYR_H, C_BORDER);
    gfx.drawFastVLine(W / 2, LYR_Y + 6, LYR_H - 12, C_BORDER);

    // Left column: Time remaining
    gfx.setFont(&lgfx::fonts::Font0);
    gfx.setTextDatum(TC_DATUM);
    gfx.setTextColor(C_LABEL);
    gfx.drawString("TIME LEFT", W / 4, LYR_Y + 4);
    gfx.setFont(&lgfx::fonts::Font4);
    gfx.setTextDatum(MC_DATUM);
    gfx.setTextColor(C_TEXT);
    gfx.drawString(
        (g_remaining > 0 && g_remaining < 9999 ? fmtDur(g_remaining) : "---").c_str(),
        W / 4, LYR_Y + LYR_H / 2 + 6);

    // Right column: Layer
    gfx.setFont(&lgfx::fonts::Font0);
    gfx.setTextDatum(TC_DATUM);
    gfx.setTextColor(C_LABEL);
    gfx.drawString("LAYER", W * 3 / 4, LYR_Y + 4);
    gfx.setFont(&lgfx::fonts::Font4);
    gfx.setTextDatum(MC_DATUM);
    gfx.setTextColor(C_TEXT);
    gfx.drawString(
        (!isFinish && g_totalLayers > 0
            ? (String(g_layer) + " / " + String(g_totalLayers))
            : String("--- / ---")).c_str(),
        W * 3 / 4, LYR_Y + LYR_H / 2 + 6);

    // ── Footer ────────────────────────────────────────────────────────────────
    gfx.setFont(&lgfx::fonts::Font0);
    gfx.setTextDatum(MC_DATUM);
    gfx.setTextColor(C_DIM);
    gfx.drawString(
        (cfg_pip + "   WiFi: " + WiFi.localIP().toString()).c_str(), W / 2, 163);

    // Commit: push sprite to display in one burst, or close the SPI batch
    if (g_spriteOK) {
        canvas.pushSprite(0, 0);
    } else {
        display.endWrite();
    }
    g_dirty = false;
}

// ─── MQTT ─────────────────────────────────────────────────────────────────────

static void onMqttMessage(char* /*topic*/, byte* payload, unsigned int len) {
    g_lastMqttMessageMs = millis();
    g_mqttMessageCount++;

    Serial.printf("[MQTT] Message #%lu (%u bytes)\n", g_mqttMessageCount, len);

    // Filter keeps only the handful of fields we need – saves heap + parse time
    JsonDocument filter;
    filter["print"]["gcode_state"]       = true;
    filter["print"]["mc_percent"]        = true;
    filter["print"]["layer_num"]         = true;
    filter["print"]["total_layer_num"]   = true;
    filter["print"]["mc_remaining_time"] = true;
    filter["print"]["nozzle_temper"]     = true;
    filter["print"]["bed_temper"]        = true;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, (const char*)payload, len,
                                                DeserializationOption::Filter(filter));
    if (err) {
        Serial.printf("[MQTT] JSON parse failed: %s (msg %u bytes, buf %u)\n",
            err.c_str(), len, (unsigned)mqtt.getBufferSize());
        return;
    }

    JsonVariant pr = doc["print"];
    if (!pr.is<JsonObject>()) {
        pr = doc;
    }
    if (!pr.is<JsonObject>()) return;

    const char* st = pr["gcode_state"] | "";
    if (*st) g_state = String(st);

    // Use -99 as "not present in this message" sentinel
    bool progressUpdated = false;
    int v;
    v = pr["mc_percent"]        | -99; if (v >= 0)  { g_percent     = v; progressUpdated = true; }
    v = pr["layer_num"]         | -99; if (v >= 0)  { g_layer       = v; progressUpdated = true; }
    v = pr["total_layer_num"]   | -99; if (v >  0)  { g_totalLayers = v; progressUpdated = true; }
    v = pr["mc_remaining_time"] | -99; if (v >= 0)  { g_remaining   = v; progressUpdated = true; }
    float ft;
    ft = pr["nozzle_temper"]    | -99.0f; if (ft >= 0.0f) g_nozzleTemp = (int)ft;
    ft = pr["bed_temper"]       | -99.0f; if (ft >= 0.0f) g_bedTemp    = (int)ft;

    // Bambu sends delta updates; some print packets omit gcode_state.
    // If we get actual progress fields, treat the job as printing so the
    // display leaves the idle view and redraws the progress screen.
    if (!*st && progressUpdated &&
        (g_state.isEmpty() || g_state == "IDLE" || g_state == "FINISH" || g_state == "FAILED")) {
        g_state = "RUNNING";
    }

    g_dirty = true;

    // Backlight: restore immediately when printing resumes; start idle timer otherwise
    bool isActive = (g_state == "RUNNING" || g_state == "PAUSE" || g_state == "PREPARE");
    if (isActive) {
        if (!g_backlightOn) {
            display.setBrightness(220);
            g_backlightOn = true;
            g_lastDisplayDraw = 0;  // force immediate redraw when screen comes back on
        }
        g_idleSinceMs = 0;
    } else if (g_idleSinceMs == 0) {
        g_idleSinceMs = millis();
    }
}

// Called every loop() iteration. Maintains MQTT connection and pumps messages.
static void mqttMaintain() {
    if (mqtt.connected()) {
        mqtt.loop();
        g_mqttEverConnected = true;
        return;
    }

    unsigned long now = millis();
    if (now - g_lastMqttAttempt < 10000) return;
    g_lastMqttAttempt = now;

    Serial.println("[MQTT] Connecting...");
    String clientId = "esp32-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    if (mqtt.connect(clientId.c_str(), "bblp", cfg_code.c_str())) {
        String topic = "device/" + cfg_serial + "/report";
        mqtt.subscribe(topic.c_str());
        Serial.println("[MQTT] Subscribed: " + topic);

        // Request a full status push so we immediately get gcode_state,
        // total_layer_num etc. (delta updates never carry these fields).
        String reqTopic = "device/" + cfg_serial + "/request";
        String reqPayload = "{\"pushing\":{\"sequence_id\":\"1\",\"command\":\"pushall\"}}";  
        mqtt.publish(reqTopic.c_str(), reqPayload.c_str());
        Serial.println("[MQTT] Requested full push_status");

        g_dirty = true;
        g_mqttFailCount    = 0;
        g_mqttEverConnected = true;
    } else {
        g_mqttFailCount++;
        Serial.printf("[MQTT] Failed, rc=%d (attempt %d/30)\n", mqtt.state(), g_mqttFailCount);
        if (!g_mqttEverConnected && g_mqttFailCount >= 30) {
            Serial.println("[MQTT] Printer unreachable – reverting to captive portal");
            Preferences p;
            p.begin("config", false);
            p.putBool("force_portal", true);
            p.end();
            ESP.restart();
        }
    }
}

// ─── HTTP handlers ────────────────────────────────────────────────────────────

static void sendPortalPage(const String& msg, int code = 200) {
    String page(FPSTR(PORTAL_HTML));
    page.replace("%MSG%",     msg);
    // Pre-fill saved values so the user only needs to fix what changed
    page.replace("%V_SSID%",   cfg_ssid);
    page.replace("%V_PIP%",    cfg_pip);
    page.replace("%V_CODE%",   cfg_code);
    page.replace("%V_SERIAL%", cfg_serial);
    http.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    http.sendHeader("Pragma", "no-cache");
    http.send(code, "text/html", page);
}

static void onPortalRoot()       { sendPortalPage(""); }
static void onCaptivePortalHit() { sendPortalPage(""); }

static void onCaptiveRedirect() {
    http.sendHeader("Location", "http://" + AP_IP.toString() + "/", true);
    http.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    http.send(302, "text/plain", "");
}

static void onConnect() {
    String ssid   = http.arg("ssid");   ssid.trim();
    String pass   = http.arg("pass");
    String pip    = http.arg("pip");    pip.trim();
    String code   = http.arg("code");   code.trim();
    String serial = http.arg("serial"); serial.trim();

    if (ssid.isEmpty()   || ssid.length()   > 32 ||
        pass.length()    > 64                     ||
        pip.isEmpty()    || pip.length()    > 39  ||
        code.isEmpty()   || code.length()   > 64  ||
        serial.isEmpty() || serial.length() > 64) {
        sendPortalPage("Invalid input – check all fields.", 400);
        return;
    }

    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    int tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries++ < 20) delay(500);

    if (WiFi.status() == WL_CONNECTED) {
        prefs.begin("config", false);
        prefs.putString("ssid",   ssid);
        prefs.putString("pass",   pass);
        prefs.putString("pip",    pip);
        prefs.putString("code",   code);
        prefs.putString("serial", serial);
        prefs.end();

        sendPortalPage("Connected to " + ssid + "! Restarting...");
        delay(2000);
        ESP.restart();
    } else {
        WiFi.mode(WIFI_AP);
        sendPortalPage("Could not connect to WiFi. Check SSID / password.");
    }
}

static void onStatus() {
    http.send(200, "text/html", FPSTR(STATUS_HTML));
}

// ─── Setup ────────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);

    display.init();
    display.setRotation(1);   // landscape 320 × 170
    display.setBrightness(220);
    display.fillScreen(TFT_BLACK);
    g_spriteOK = (canvas.createSprite(320, 170) != nullptr);
    Serial.printf("[SPRITE] %s (free heap: %u)\n",
        g_spriteOK ? "OK – back-buffer active" : "FAILED – direct draw fallback",
        (unsigned)ESP.getFreeHeap());

    // Load saved config
    prefs.begin("config", false);  // read-write so we can clear force_portal flag
    cfg_ssid   = prefs.getString("ssid",   "");
    cfg_pass   = prefs.getString("pass",   "");
    cfg_pip    = prefs.getString("pip",    "");
    cfg_code   = prefs.getString("code",   "");
    cfg_serial = prefs.getString("serial", "");
    bool forcePortal = prefs.getBool("force_portal", false);
    if (forcePortal) prefs.putBool("force_portal", false);  // clear immediately
    prefs.end();

    const bool hasConfig = (cfg_ssid.length()   > 0 &&
                            cfg_pip.length()    > 0 &&
                            cfg_code.length()   > 0 &&
                            cfg_serial.length() > 0);

    if (!forcePortal && hasConfig) {
        // ── STA + MQTT monitor mode ───────────────────────────────────────────
        drawConnectingScreen(cfg_ssid);

        WiFi.mode(WIFI_STA);
        WiFi.begin(cfg_ssid.c_str(), cfg_pass.c_str());
        int tries = 0;
        while (WiFi.status() != WL_CONNECTED && tries++ < 20) delay(500);

        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("[WiFi] IP: %s\n", WiFi.localIP().toString().c_str());

            setCpuFrequencyMhz(80);                    // halves CPU dynamic power
            WiFi.setTxPower(WIFI_POWER_8_5dBm);       // lower TX power (same LAN)

            // Sync NTP – Copenhagen uses CET (UTC+1) / CEST (UTC+2)
            configTzTime("CET-1CEST,M3.5.0,M10.5.0/3",
                         "pool.ntp.org", "time.google.com");

            // Bambu Lab MQTT: TLS port 8883, username "bblp", pw = access code
            tlsClient.setInsecure();   // Bambu uses a self-signed certificate
            mqtt.setServer(cfg_pip.c_str(), 8883);
            mqtt.setCallback(onMqttMessage);
            mqtt.setBufferSize(32768); // Bambu AMS payloads with 2× AMS units can exceed 16 KB

            portalMode = false;
            http.on("/",           HTTP_GET, onStatus);
            http.on("/index.html", HTTP_GET, onStatus);
            http.begin();

            g_dirty = true;
            drawPrintStatus();
            return;
        }

        Serial.println("[WiFi] Failed – falling back to portal");
    }

    // ── AP / Captive-portal mode ──────────────────────────────────────────────
    portalMode = true;

    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
    WiFi.softAP(AP_SSID, nullptr);  // open network
    delay(200);                     // let the radio fully initialise

    dns.start(DNS_PORT, "*", AP_IP);

    http.on("/",                          HTTP_GET,  onPortalRoot);
    http.on("/connect",                   HTTP_POST, onConnect);
    // OS captive-portal probe paths
    http.on("/generate_204",              HTTP_GET,  onCaptivePortalHit);  // Android
    http.on("/gen_204",                   HTTP_GET,  onCaptivePortalHit);
    http.on("/hotspot-detect.html",       HTTP_GET,  onCaptivePortalHit);  // iOS/macOS
    http.on("/library/test/success.html", HTTP_GET,  onCaptivePortalHit);
    http.on("/success.txt",               HTTP_GET,  onCaptivePortalHit);
    http.on("/connecttest.txt",           HTTP_GET,  onCaptivePortalHit);  // Windows
    http.on("/ncsi.txt",                  HTTP_GET,  onCaptivePortalHit);
    http.on("/redirect",                  HTTP_GET,  onCaptivePortalHit);
    http.on("/canonical.html",            HTTP_GET,  onCaptivePortalHit);  // Firefox
    http.onNotFound(onCaptiveRedirect);
    http.begin();

    drawAPScreen();
}

// ─── Loop ─────────────────────────────────────────────────────────────────────

void loop() {
    if (portalMode) {
        dns.processNextRequest();
        http.handleClient();
        return;
    }

    mqttMaintain();
    http.handleClient();

    unsigned long now = millis();

    // Auto-dim backlight after 5 minutes of printer being idle
    if (g_backlightOn && g_idleSinceMs != 0 &&
        (now - g_idleSinceMs >= 5UL * 60UL * 1000UL)) {
        display.setBrightness(0);
        g_backlightOn = false;
    }

    // Redraw on any MQTT state change (g_dirty), or every 2 s for the clock / pulse dot
    if (g_backlightOn && (g_dirty || (now - g_lastDisplayDraw >= 2000))) {
        drawPrintStatus();
        g_lastDisplayDraw = millis();
    }

    delay(50);
}
