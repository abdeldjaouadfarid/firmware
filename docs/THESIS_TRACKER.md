# Design and Implementation of a WiFi-Based GPS Livestock Tracker

*Documentation of the hardware tracking node for the LiveTrack IoT livestock-monitoring system.*

> This chapter documents the development of the physical GPS tracking device:
> the hardware platform, the embedded firmware, its integration with the backend,
> the engineering problems encountered, and how each was resolved. It is written
> to be adapted directly into a thesis report.

---

## 1. Introduction and objective

The LiveTrack system tracks the position of livestock and raises alerts when an
animal leaves a defined safe area (a *geofence*). The system has three software
tiers — a Node.js/Express + PostgreSQL backend, a Next.js web dashboard, and a
React Native (Expo) mobile application — and one **hardware tier**: the tracking
node attached to the animal.

The objective of this part of the work was to build a working hardware tracker
that:

1. determines its own geographic position using GPS/GNSS;
2. measures its battery state of charge;
3. transmits this telemetry, together with a unique device identifier, to the
   backend over a wireless link;

The device chosen was the **Heltec Wireless Tracker V1.2** development board.
Although the board includes a LoRa radio, **LoRaWAN was intentionally not used**;
the device's built-in **WiFi** interface was selected as the transport because it
provides a simple, internet-routable HTTP path to the existing backend without
requiring a LoRa gateway or network server.

---

## 2. Hardware platform

The Heltec Wireless Tracker V1.2 integrates the following relevant components:

| Component | Part | Role in this project |
|---|---|---|
| Microcontroller | Espressif **ESP32-S3** (240 MHz, dual-core, 8 MB flash) | Runs the firmware; provides WiFi + Bluetooth |
| GNSS receiver | **UC6580** multi-constellation (GPS, GLONASS, Galileo, BeiDou) | Provides position fixes via NMEA over UART |
| LoRa radio | Semtech **SX1262** | *Unused* |
| Antennas | On-board ceramic GNSS antenna (+ u.FL connector) | Receives satellite signals |
| Power | LiPo battery connector + USB-C | Powers the node; battery voltage is measured |

The ESP32-S3 communicates with the UC6580 over a hardware UART and controls the
power rail that supplies the GNSS section. The USB-C port connects directly to
the ESP32-S3's **native USB-Serial/JTAG** controller (USB VID `0x303A`, PID
`0x1001`) — i.e. there is no separate USB-to-UART bridge chip. This detail later
proved important for debugging (Section 6.2).

---

## 3. System architecture

```
   ┌──────────────────────┐     WiFi / HTTP POST (JSON)     ┌─────────────────────┐
   │  Heltec Wireless      │  ─────────────────────────────▶ │  Backend (Node.js)  │
   │  Tracker V1.2         │   POST /api/tracker             │  Express + Prisma   │
   │                       │   { node_id, latitude,          │  PostgreSQL         │
   │  ESP32-S3 + UC6580    │     longitude, battery }        │  Socket.IO          │
   └──────────────────────┘                                  └──────────┬──────────┘
                                                                         │ real-time
                                                          location:update│ geofence:alert
                                                                         ▼
                                                      ┌───────────────────────────────┐
                                                      │ Web dashboard  +  Mobile app   │
                                                      │ (live map, alerts, vibration)  │
                                                      └───────────────────────────────┘
```

The tracker is a pure data **producer**. It performs no local logic beyond
acquiring and transmitting telemetry; geofence evaluation and alerting are
performed centrally by the backend, which then pushes events to the clients in
real time over Socket.IO.

---

## 4. Firmware design

The firmware was developed in **C++** using the **Arduino framework** under
**PlatformIO**. It is organised into a configuration header (`config.h`) and the
main program (`main.cpp`), and depends on two third-party libraries:
**TinyGPSPlus** (NMEA sentence parsing) and **ArduinoJson** (payload
serialisation), in addition to the built-in `WiFi` and `HTTPClient` libraries.

### 4.1 Program flow

**Setup (once at power-on):**
1. Initialise the USB serial console for logging.
2. Drive the **Vext** power-enable pin high to power the GNSS receiver.
3. Open the hardware UART to the UC6580 at **115200 baud**.
4. Connect to WiFi in station mode.

**Loop (continuously):**
1. Continuously feed incoming UART bytes to the TinyGPS++ parser.
2. Every 10 seconds:
   - if a valid position is available, read the battery voltage, build a JSON
     document, and `POST` it to the backend;
   - otherwise, print a diagnostic line (satellite count, bytes received, parse
     statistics, WiFi state) and wait.

### 4.2 Position acquisition

The UC6580 continuously emits standard NMEA-0183 sentences (`$GNRMC`, `$GNGGA`,
`$GxGSV`, …). TinyGPS++ parses these and exposes latitude, longitude, satellite
count, and HDOP. A position is transmitted once the parser reports a valid
location; the firmware keeps reporting the last known location during brief
signal dropouts so the animal does not disappear from the map.

### 4.3 Battery measurement

Battery state of charge is derived from the LiPo voltage. The voltage is read on
an ADC-capable pin through an on-board resistive divider; a dedicated control pin
must be asserted to energise the divider before sampling. The calibrated pin
voltage (obtained with the ESP32's `analogReadMilliVolts()` function at 2.5 dB
attenuation) is multiplied by the divider ratio (≈ 5.12) to recover the battery
voltage, which is then linearly mapped to a 0–100 % range between 3.0 V and
4.2 V.

### 4.4 Data transmission

Telemetry is sent as a single JSON object to the unauthenticated endpoint
`POST /api/tracker`:

```json
{ "node_id": "heltec_tracker_01", "latitude": 36.755949,
  "longitude": 3.223332, "battery": 0 }
```

The `node_id` is a unique, human-readable identifier configured per device. On
the server this value is matched against the device's `imei` field; if no match
exists the backend auto-registers the device on first contact.

### 4.5 Verified pin configuration

Because the exact board variant was not directly supported by the toolchain
(Section 6.1), all hardware pins were defined explicitly in firmware and
cross-checked against the open-source Meshtastic project's tested board
definition for the Heltec Wireless Tracker:

| Function | GPIO | Detail |
|---|---|---|
| GNSS UART RX (MCU ← GPS) | 33 | |
| GNSS UART TX (MCU → GPS) | 34 | |
| GNSS UART baud | — | 115200 |
| GNSS power enable (Vext) | 3 | active HIGH |
| Battery ADC input | 1 | ADC1 channel 0 |
| Battery ADC enable | 2 | active HIGH |
| Divider multiplier | — | ≈ 5.12 |

---

## 5. Backend integration

The backend exposes `POST /api/tracker`, which validates the payload and calls a
shared `ingestPosition` routine. This routine:

1. looks up the tracker by `imei` (auto-registering it if absent);
2. updates the stored latitude, longitude, status (`active`), battery level and
   last-seen timestamp;
3. emits a Socket.IO `location:update` event to the owning user;
4. evaluates all of the owner's geofences and, on a boundary crossing, creates
   an alert and emits a `geofence:alert` event.

A key integration constraint was identified and documented: **a tracker is only
visible to a farmer when its `imei` equals the firmware's `node_id` *and* the
tracker record has an assigned owner.** Pings carrying an unrecognised
identifier silently create owner-less records that never appear in the user's
application.

---

## 6. Engineering challenges and solutions

This section records the non-trivial problems encountered during bring-up and
how each was diagnosed and resolved. They are included because they constitute a
substantial part of the practical engineering contribution.

### 6.1 Unsupported board identifier
The PlatformIO espressif32 platform did not include a `heltec_wireless_tracker`
board definition, so compilation failed. **Resolution:** the project targets the
`heltec_wifi_lora_32_V3` definition, which uses the identical ESP32-S3 / 8 MB
flash MCU. Because every peripheral pin is declared explicitly in the firmware,
the substitution has no functional effect.

### 6.2 No serial output despite successful flashing
After flashing, the serial monitor showed nothing, even though uploads
succeeded. The cause is specific to the ESP32-S3's native USB: by default the
Arduino core routes `Serial` to hardware UART0 (physical pins), not to the USB
port being monitored. **Resolution:** the build flag
`-DARDUINO_USB_CDC_ON_BOOT=1` was added, which maps `Serial` onto the native
USB-Serial/JTAG interface, restoring console output.

### 6.3 Serial port contention during flashing
Uploads intermittently failed with "could not open port … Access is denied". The
serial monitor holds the COM port open, preventing the uploader from using it.
**Resolution:** the monitor must be stopped before each upload — an operational
procedure rather than a code change.

### 6.4 GNSS receiver never produced a fix
The most significant firmware bug: the receiver reported zero usable satellites
indefinitely. Diagnostic counters added to the firmware showed that bytes *were*
arriving and parsing *without checksum errors* — proving the wiring and power
were correct — yet no valid sentences were produced. The root cause was an
incorrect **UART baud rate**: the firmware assumed 9600 baud, whereas the UC6580
defaults to **115200**. At the wrong rate the bytes were unintelligible.
**Resolution:** setting the baud rate to 115200 immediately yielded valid NMEA
data and a position fix.

### 6.5 Battery always reported 0 %
The battery percentage was always zero. Two errors were found by comparing
against a reference implementation: the wrong GPIO was used to enable the ADC
divider, and an incorrect voltage-divider ratio was applied. **Resolution:** the
correct enable pin (GPIO 2) and divider multiplier (≈ 5.12) were adopted, and the
calibrated `analogReadMilliVolts()` API was used. (With no battery attached, i.e.
USB-only operation, a reading of 0 V is the expected and correct result.)

### 6.6 Device not appearing in the application
A registered tracker remained "inactive" with no position. Inspection of the
database revealed two records: one created automatically by the firmware (correct
position data but no owner) and one created manually in the dashboard (an owner,
but an identifier that the firmware never transmitted). This was the `node_id`
vs. `imei` constraint of Section 5. **Resolution:** the records were
reconciled so that a single owned tracker carries the same identifier the
firmware sends.

### 6.7 Geofence not displayed and no alerts
A newly created geofence did not appear on the map, and no alert was produced.
Three independent causes were identified: (a) the configured radius (5 m) was
smaller than a screen pixel at normal zoom and smaller than GPS error; (b) the
mobile client loads geofences only once, at login, so a zone created afterwards
required an application reload; and (c) alerts are generated **only on a boundary
crossing**, whereas the stationary animal had always been outside the zone and so
never crossed it. **Resolution:** a realistic radius (≥ 100 m) was used, the
client was reloaded, and the alert path was validated by injecting an
inside-then-outside position sequence, which correctly produced enter and exit
alerts.

### 6.8 Mobile alerting (vibration / sound)
A requirement emerged for the phone to physically alert the user when an animal
leaves the zone. This was implemented by handling the `geofence:alert` event in
the mobile client and triggering device **vibration** (always available) and a
local **notification with sound** (where supported). A documented limitation is
that, under Expo Go on Android, local notifications are unavailable (a
development build is required), and that this in-app mechanism functions only
while the application is in the foreground; true background/locked-screen alerts
would require server-side push notifications.

---

## 7. Results

The complete telemetry chain was verified working: the tracker acquired a GPS
fix outdoors, transmitted its coordinates over WiFi, and the backend stored a
real position (latitude 36.7558° N, longitude 3.2238° E — Algiers) and rendered
it on the dashboard and mobile map. The geofence subsystem was confirmed to
generate and deliver enter/exit alerts, which in turn triggered the mobile
device's vibration feedback.

---

## 8. Limitations and future work

- **GPS requires open sky.** Indoors the receiver cannot lock; first acquisition
  (cold start) takes 1–5 minutes outdoors. An external active antenna would
  improve this.
- **Power.** The node currently runs continuously. Introducing deep-sleep
  between transmissions would substantially extend battery life.
- **Transport coverage.** WiFi limits operation to areas with a known network.
  For true field deployment the originally-excluded LoRaWAN path (or a cellular
  modem) would extend range beyond WiFi coverage.
- **Background alerting.** Push notifications would allow alerts to reach the
  farmer when the app is closed or the phone is locked.
- **Security.** The ingest endpoint is currently unauthenticated for simplicity;
  a per-device token or signature should be added for production.

---

## Appendix A — Configuration parameters (`config.h`)

| Parameter | Meaning |
|---|---|
| `WIFI_SSID` / `WIFI_PASSWORD` | Network the tracker joins |
| `SERVER_URL` | Backend ingest endpoint (`http://<server-ip>:3000/api/tracker`) |
| `DEVICE_ID` | Unique identifier; must equal the tracker's `imei` in the database |
| `POST_INTERVAL_MS` | Transmission period (10 000 ms) |

## Appendix B — Toolchain

PlatformIO Core; Espressif 32 platform v6.13.0; Arduino framework; libraries
TinyGPSPlus and ArduinoJson. Build/flash with `pio run -t upload`; observe logs
with `pio device monitor` at 115200 baud.
