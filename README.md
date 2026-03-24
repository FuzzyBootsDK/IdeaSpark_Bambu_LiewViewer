# Bambu Lab Printer Monitor

An ESP32-C6 firmware that displays live Bambu Lab print status on a small SPI ST7789 display, styled after the Bambu Lab "Live Tracking" card UI. Two hardware variants are supported — see the Hardware section for wiring details.

---

## Hardware Variants

### Variant A — Lafvin / Waveshare ESP32-C6-LCD-1.47

| Component | Detail |
|---|---|
| MCU | Lafvin / Waveshare ESP32-C6 DevKit (integrated display) |
| Display | 1.47" ST7789 SPI, landscape 320×172 px |
| Source | `src/main.cpp` |

**Wiring**

| Signal | GPIO |
|---|---|
| SPI CLK | 7 |
| SPI MOSI | 6 |
| LCD CS | 14 |
| LCD DC | 15 |
| LCD RST | 21 |
| Backlight (PWM) | 22 |

---

### Variant B — Ideaspark ESP32-C6 + 1.9" Display

| Component | Detail |
|---|---|
| MCU | Ideaspark ESP32-C6 DevKit |
| Display | 1.9" ST7789 SPI, landscape 320×170 px |
| Source | `src_ideaspark/main.cpp` |

**Wiring**

| Signal | GPIO |
|---|---|
| SPI SCLK | 18 |
| SPI MOSI | 23 |
| LCD CS | 15 |
| LCD DC | 2 |
| LCD RST | 4 |
| Backlight (PWM) | 32 |

---

## How It Works — Detailed

The firmware is split into two clearly defined operational phases: a one-time configuration phase and an ongoing monitoring phase. Which phase runs is determined at boot by checking whether credentials exist in non-volatile flash.

---

### Phase 1 — First Boot: Captive Portal Configuration

When no credentials have been stored in NVS (or after a forced portal reset), the device enters provisioning mode.

#### What happens step by step

1. **Access point starts.** The ESP32 brings up a Wi-Fi AP named `ESP32-Setup` with no password and a fixed IP of `192.168.4.1`. No credentials are required to connect.

2. **DNS hijacking.** A `DNSServer` instance is started that responds to *every* DNS query (wildcard `"*"`) with the device's own IP. This forces any device that connects to believe the internet is the ESP32, which triggers the OS-level captive-portal detection flow.

3. **Captive-portal probe interception.** Every modern OS issues a background HTTP probe to a well-known URL when it joins a new network. The firmware explicitly handles all of them:
   - Android: `/generate_204`, `/gen_204`
   - iOS / macOS: `/hotspot-detect.html`, `/library/test/success.html`, `/success.txt`
   - Windows: `/connecttest.txt`, `/ncsi.txt`, `/redirect`
   - Firefox: `/canonical.html`
   
   All of these return the setup page HTML. Any other URL triggers a `302 redirect` to `http://192.168.4.1/`. The combined effect is that within seconds of connecting to `ESP32-Setup`, smartphones and computers automatically open a "Sign in to network" dialog showing the configuration form — no manual URL entry needed.

4. **Setup form.** The portal page asks for five values:
   - **Wi-Fi SSID** and **password** — the home/office network to connect to.
   - **Bambu printer IP** — the LAN IP address of the 3D printer (found in the printer's network settings screen).
   - **Bambu access code** — the 8-character authentication code displayed on the printer's touchscreen (also called the "LAN access code").
   - **Bambu serial number** — the full device serial (e.g. `01S00C123456789`), used to construct the MQTT topic for this specific printer.

5. **Credential validation.** Submitted values are checked for length limits before use. Empty required fields or oversized inputs return an HTTP 400 with an error message.

6. **Wi-Fi connection attempt.** The ESP switches to `WIFI_AP_STA` mode and tries to connect to the supplied SSID, retrying for up to 10 seconds. If it succeeds:
   - All five values are written to NVS namespace `"config"` via Arduino `Preferences`.
   - The user sees a "Connected! Restarting..." message.
   - After a 2-second delay the device reboots into Phase 2.
   
   If the connection fails, the portal remains active and the form shows a retry message. Previously entered values are pre-filled so the user only needs to correct what went wrong.

7. **NVS "force portal" flag.** If MQTT fails to connect 30 times in a row during Phase 2 *and* the device has never once successfully reached the printer, it writes `force_portal = true` to NVS before restarting. On the next boot this flag is read, cleared, and the device goes straight into Phase 1 instead of Phase 2 — allowing the user to correct the printer IP or access code without needing to wipe flash.

---

### Phase 2 — Normal Operation: Live Printer Monitor

Once credentials exist in NVS the device operates as a permanent status display.

#### Boot sequence

1. **Display init.** The ST7789 panel is initialised via LovyanGFX, rotated to landscape, backlight set to full brightness (220/255).
2. **Sprite allocation.** A 320×172 (or 320×170 for the Ideaspark variant) 16-bit colour sprite is allocated in heap RAM. This ~110 KB off-screen buffer enables flicker-free rendering — every frame is composited entirely in RAM and then transferred to the display in a single DMA SPI burst. If allocation fails (not enough contiguous heap), the code falls back to direct-draw with `startWrite()`/`endWrite()` batching.
3. **NVS load.** Credentials are read from the `"config"` namespace.
4. **"Connecting…" splash screen** shows the target SSID immediately.
5. **Wi-Fi connect (STA mode).** The device connects and logs its IP to the serial port.
6. **CPU clock reduction.** After connecting, `setCpuFrequencyMhz(80)` halves the ESP32-C6's clock from 160 MHz to 80 MHz. This significantly reduces power consumption with no perceptible effect on display performance or MQTT throughput at 80 MHz.
7. **TX power reduction.** `WiFi.setTxPower(WIFI_POWER_8_5dBm)` lowers the radio transmit power to 8.5 dBm. On a home LAN the printer and router are typically within a few metres, so this reduction has no effect on connectivity while reducing power draw.
8. **NTP sync.** `configTzTime()` is called with the POSIX timezone string `CET-1CEST,M3.5.0,M10.5.0/3` which selects Central European Time and handles the summer/winter DST transitions automatically. The time servers `pool.ntp.org` and `time.google.com` are used. The sync happens in the background; the display checks `time(nullptr) > 1000000000` to detect when the clock is valid before showing a wall-clock ETA.
9. **MQTT setup.** A `WiFiClientSecure` TLS socket is configured with `setInsecure()` because Bambu Lab printers use a self-signed certificate. `PubSubClient` is configured to use this socket, targeting the printer IP on port 8883. The receive buffer is enlarged to 16 384 bytes because Bambu AMS payloads (tray/filament status) can exceed the default 8 KB limit.
10. **HTTP server.** A minimal web server starts on port 80 serving a simple status page at `/` — useful for confirming the device is online.
11. **First draw.** `drawPrintStatus()` renders an "idle" frame immediately so the screen shows something meaningful before any MQTT data arrives.

#### MQTT connection and data flow

`mqttMaintain()` is called every `loop()` iteration. When connected, it calls `mqtt.loop()` which processes incoming data and sends MQTT keepalive PINGs. When disconnected, it waits 10 seconds between reconnection attempts to avoid hammering the printer.

On each successful connection:
- The device subscribes to `device/<serial>/report`.
- It publishes a `pushall` command to `device/<serial>/request`. This is the Bambu Lab local MQTT API's request that triggers the printer to send a complete status snapshot immediately. Without this command, the printer only sends delta updates; fields like `total_layer_num` which never change mid-print would never arrive.

Bambu Lab MQTT payloads are JSON with all print data nested under a `"print"` key. ArduinoJson's `DeserializationOption::Filter` is used to discard every field except the seven being tracked, dramatically reducing both parsing time and heap usage on large AMS payloads.

The seven tracked fields:

| MQTT field | Variable | Description |
|---|---|---|
| `gcode_state` | `g_state` | Print state: `IDLE`, `RUNNING`, `PAUSE`, `FINISH`, `FAILED`, `PREPARE` |
| `mc_percent` | `g_percent` | Progress 0–100 |
| `layer_num` | `g_layer` | Current layer number |
| `total_layer_num` | `g_totalLayers` | Total number of layers |
| `mc_remaining_time` | `g_remaining` | Minutes remaining |
| `nozzle_temper` | `g_nozzleTemp` | Nozzle temperature °C (parsed, not yet displayed) |
| `bed_temper` | `g_bedTemp` | Bed temperature °C (parsed, not yet displayed) |

Because Bambu sends incremental delta messages (each message only contains the fields that changed), a message may have progress data but no `gcode_state`. When progress fields arrive for a printer that was last known to be IDLE or FINISH, the code promotes the state to RUNNING so the display reflects that a print is active.

#### Display loop

`loop()` runs at ~20 Hz (limited by a 50 ms `delay()`). On each iteration:

1. `mqttMaintain()` — reconnect if needed, pump incoming messages.
2. `http.handleClient()` — serve any pending web requests.
3. **Auto-dim check.** If the backlight is on and the printer has been inactive for more than 5 minutes (`g_idleSinceMs` is set when state transitions out of RUNNING/PAUSE/PREPARE), the backlight is turned off (`setBrightness(0)`) to prevent burn-in. It turns back on at full brightness automatically the moment a new active print message arrives.
4. **Redraw check.** The display is redrawn if either `g_dirty` is true (set by any MQTT message) or 2 seconds have elapsed since the last draw. The 2-second periodic redraw keeps the glass-orb pulse indicator animating even during quiet periods.

---

## Display Layout

```
┌──────────────────────────────────────────────────────────┐
│        Live Tracking  ●                          ▌▌▌     │  ← Header
├──────────────────────────────────────────────────────────┤
│  [████████████████████░░░░░░░]              72%          │  ← Progress bar
│                                                          │
│  ┌────────────────────────────────────────────────────┐  │
│  │  ESTIMATED COMPLETION                              │  │  ← ETA card
│  │              Today at 14:35                        │  │
│  └────────────────────────────────────────────────────┘  │
│  ┌────────────────────────┬───────────────────────────┐  │
│  │  TIME LEFT             │  LAYER                    │  │  ← Info row
│  │     1h 22m             │    125 / 500              │  │
│  └────────────────────────┴───────────────────────────┘  │
│          192.168.1.50   WiFi: 192.168.1.101               │  ← Footer
└──────────────────────────────────────────────────────────┘
```

### Header bar (y = 0–19)

A dark navy (`#11183A`) bar spans the full width. The text "Live Tracking" and a small glass orb are measured together and centred as a single group. On every redraw `g_pulse` is toggled, switching the orb between:
- **Green** (active): a deep green outer rim → mid-green fill → bright green glint spot offset up-left, simulating a lit glass bead.
- **Grey** (inactive): dark grey rim → light grey fill → near-white glint.

The alternation creates a subtle heartbeat effect, visually confirming the display is live even when print values haven't changed.

Three signal bars in the top-right corner grow progressively taller (5 px, 9 px, 13 px). They are drawn in bright green when the MQTT socket is connected and in dim blue-grey when disconnected.

### Progress bar (y = 22, h = 22)

A full-pill `fillRoundRect` (border radius = half the bar height, making perfect semicircular ends) is drawn in dark indigo as the empty track. The active fill is a solid blue `fillRoundRect` of proportional width.

A glass-sheen pass iterates over the top 11 pixel rows of the fill. For each row:
- The base blue colour is blended linearly toward white, with peak brightness at the topmost row (alpha ≈ 0.55) fading to nothing at row 11.
- The left and right arc boundaries of the pill cap are computed per-row using `sqrt(r² - dy²)` so the sheen precisely follows the rounded contour rather than clipping to a rectangle.

The percentage value (e.g. `72%`) is drawn in muted cool blue-grey (`rgb(195, 205, 220)`) in Font4 to the right of the bar.

### ETA card (y = 50, h = 55)

A thin border rectangle encloses a `ESTIMATED COMPLETION` label (small Font0, in dim periwinkle) and a large centred value in Font4. The value and colour changes with printer state:

| State | Text | Colour |
|---|---|---|
| `FINISH` | `Print Complete!` | Bright green |
| `FAILED` | `Print FAILED` | Red |
| `PREPARE` | `Heating up...` | Amber |
| `IDLE` / empty | `Printer Idle` | Dim blue-grey |
| Printing + NTP synced | `Today at HH:MM` or `Tomorrow HH:MM` | Muted white |
| Printing + no NTP | `~NN min` | Muted white |
| No ETA data | `---` | Dim blue-grey |

The ETA time is computed as `now + g_remaining * 60` seconds and formatted in local time (Copenhagen CET/CEST). "Today" vs "Tomorrow" is determined by comparing the calendar day of the ETA against the current day — not by a 24-hour offset — so a print finishing at 00:30 correctly shows "Tomorrow" even if it is only 10 minutes away.

### Info row (y = 111, h = 47)

A bordered rectangle is split into two equal columns by a vertical line. The left column shows "TIME LEFT" (remaining print time, formatted as `Xh Ym` / `Xh` / `Xm`). The right column shows "LAYER" (`current / total`). Column labels are in small Font0 at dim periwinkle; values are in large Font4 at muted white.

### Footer (y = 163/165)

A single centred line in very dim blue-grey shows the printer's IP address and the ESP32's own Wi-Fi IP, useful for confirming connectivity at a glance.

---

## Anti-Flicker: Double-Buffered Sprite Rendering

Every pixel of every frame is drawn into an off-screen `LGFX_Sprite` object in RAM. No SPI pixel transfer happens during composition. When the frame is complete, `canvas.pushSprite(0, 0)` initiates a single DMA transfer that overwrites the entire panel in one uninterrupted burst.

From the display's perspective, its contents change atomically from old frame to new frame with no partial-update artefacts, no visible clear-to-black flash, and no horizontal-tear lines.

If the heap cannot accommodate the ~110 KB contiguous allocation (reported on serial as `[SPRITE] FAILED`), the firmware falls back to batched direct drawing: `display.startWrite()` opens an SPI transaction and all draw calls within the frame share that single transaction, reducing per-call overhead significantly until `display.endWrite()` closes it.

---

## Connection Failure Behaviour

**Wi-Fi fail at boot** — Falls back to captive portal mode automatically.

**MQTT fail (printer unreachable)** — Retried every 10 seconds indefinitely. If the device has *never* successfully connected to the printer (i.e. the IP or access code may be wrong) and 30 consecutive failures occur, it writes `force_portal = true` to NVS and reboots into the captive portal so the user can correct the printer credentials.

**MQTT disconnect after prior success** — Retried every 10 seconds with no reboot. Temporary network glitches recover silently.

---

## Serial Diagnostics

The firmware emits structured log lines that make debugging straightforward:

```
[SPRITE] OK – back-buffer active (free heap: 187432)
[WiFi] IP: 192.168.1.101
[MQTT] Connecting...
[MQTT] Subscribed: device/01S00C123456789/report
[MQTT] Requested full push_status
[MQTT] Message #1 (2847 bytes)
[DRAW] state=RUNNING pct=42 layer=87/206 rem=74min
[MQTT] Message #2 (184 bytes)
[DRAW] state=RUNNING pct=43 layer=89/206 rem=72min
```

A `[DRAW]` line appears every time the display is refreshed (every 2 seconds at minimum), so its absence indicates the loop is stuck. A `[MQTT] Message #1` within 15 seconds of boot confirms the printer is reachable and responding.

---

## Dependencies

| Library | Version | Purpose |
|---|---|---|
| [LovyanGFX](https://github.com/lovyan03/LovyanGFX) | ^1.2.0 | ST7789 SPI driver, sprite rendering, DMA transfer |
| [PubSubClient](https://github.com/knolleary/pubsubclient) | ^2.8 | MQTT client over TLS |
| [ArduinoJson](https://arduinojson.org/) | ^7.0 | JSON parsing with field filtering |

---

## Build & Flash

This project uses [PlatformIO](https://platformio.org/).

```powershell
# Build (Lafvin/Waveshare variant)
pio run

# Flash
pio run --target upload

# Serial monitor (115200 baud)
pio device monitor
```

To build the Ideaspark variant, change `src_dir` in `platformio.ini` to `src_ideaspark` before running.

Target environment: `esp32-c6-devkitc-1` (Arduino framework, 921600 baud upload).
