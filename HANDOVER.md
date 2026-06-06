# Heltec Wireless Tracker — Firmware Handover

**Project:** LiveTrack — IoT livestock tracking demo
**Component:** GPS tracker firmware (`E:\startupv2\firmware`)
**Hardware:** Heltec Wireless Tracker **V1.2** (pin-compatible with V1.1)
**Status:** Working end-to-end (WiFi → backend → DB → dashboard/mobile). Needs open-sky for a GPS lock.
**Last updated:** 2026-06-06

---

## 1. What this firmware does

Every 10 seconds the tracker:
1. Reads its GPS position from the on-board UC6580 GNSS receiver.
2. Reads the LiPo battery voltage (→ percentage).
3. Sends an HTTP `POST` over WiFi to the backend as JSON.

It deliberately **does not use LoRa/LoRaWAN** — only WiFi. (Bluetooth is unused.)

**Payload** → `POST http://<server-ip>:3000/api/tracker`
```json
{ "node_id": "heltec_tracker_01", "latitude": 36.755949, "longitude": 3.223332, "battery": 0 }
```

---

## 2. Files

| File | Purpose |
|------|---------|
| `platformio.ini` | Board, libraries, build flags, COM port |
| `src/config.h`   | **Edit this** — WiFi, server URL, device ID, interval |
| `src/main.cpp`   | Firmware logic (GPS, battery, WiFi, HTTP) |

### `config.h` — the only file you normally edit
```c
#define WIFI_SSID       "12345678"
#define WIFI_PASSWORD   "********"                              // in config.h
#define SERVER_URL      "http://172.20.10.3:3000/api/tracker"   // server LAN IP
#define DEVICE_ID       "heltec_tracker_01"                     // == tracker.imei in DB
#define POST_INTERVAL_MS 10000UL
```

---

## 3. Toolchain & build

- **PlatformIO Core** (installed via `pip install platformio`, Python 3.11).
- Board in `platformio.ini` is **`heltec_wifi_lora_32_V3`**. The real `heltec_wireless_tracker` ID is *not* in PlatformIO's espressif32 board list; the V3 is the **same MCU** (ESP32-S3, 8 MB) and works because every pin is defined explicitly in `main.cpp`.
- Libraries: `TinyGPSPlus` (NMEA parsing), `ArduinoJson` (JSON), plus built-in `WiFi` + `HTTPClient`.

### Build, flash, monitor
```powershell
# from E:\startupv2\firmware
pio run -t upload          # build + flash (COM4 is pinned in platformio.ini)
pio device monitor         # serial @ 115200
```
> PowerShell note: `&&` is not valid; run commands on separate lines or use `;`.

---

## 4. Pin map (verified against the Meshtastic `heltec_wireless_tracker` variant)

| Function | GPIO | Notes |
|---|---|---|
| GNSS UART RX (ESP32 ← GPS TX) | 33 | |
| GNSS UART TX (ESP32 → GPS RX) | 34 | |
| GNSS baud rate | — | **115200** (UC6580 default; *not* 9600) |
| Vext / GNSS power enable | 3 | active **HIGH** |
| Battery ADC read | 1 | ADC1_CH0 |
| Battery ADC enable (`ADC_CTRL`) | 2 | active HIGH, via internal pull-up |
| Battery divider multiplier | — | 4.9 × 1.045 ≈ **5.12**, read with `analogReadMilliVolts` + 2.5 dB atten |

---

## 5. Backend integration (critical rule)

The backend matches the firmware's `node_id` against the **`imei`** column of the `trackers` table (`backend/src/modules/tracker/tracker.controller.js` → `ingestPosition`).

**For a tracker to appear in a farmer's app it MUST:**
1. have `imei` **==** the firmware's `DEVICE_ID`, **and**
2. have a non-null `owner_id` (assigned to a farmer).

A ping for an unknown `imei` auto-creates an **owner-less** tracker (invisible to farmers). This was the #1 source of "shows inactive / no location."

On each valid ping the backend updates position/status/battery, emits a Socket.IO `location:update`, and runs the geofence + battery-alert checks.

DB: PostgreSQL `livestock_demo` @ localhost:5432, user `postgres`. Tables use snake_case (`imei`, `owner_id`, `last_seen_at`, `battery_level`).

---

## 6. Current device/data state

- Tracker row: `imei = heltec_tracker_01`, owned by **farmer1**.
- Geofence "UserGeofence": center (36.7563, 3.2261), radius (set in dashboard).
- Battery reads **0%** because no LiPo is attached (USB-only) — expected, not a fault.

---

## 7. Troubleshooting (every issue we hit, and the fix)

| Symptom | Cause | Fix |
|---|---|---|
| `UnknownBoard 'heltec_wireless_tracker'` | Board ID not in PlatformIO | Use `heltec_wifi_lora_32_V3` |
| Serial monitor blank (flash OK) | ESP32-S3 native USB needs CDC routing | `build_flags = -DARDUINO_USB_CDC_ON_BOOT=1` |
| `Could not open COMx` / port busy | Monitor still holds the port | Stop monitor (`Ctrl+C`) before `upload` |
| `No fix`, `sats=0` forever, but `chars` rising | Wrong GPS baud (9600) | Set `GPS_BAUD 115200` |
| Battery always 0% | Wrong ADC enable pin (37) + wrong scale | `BAT_CTRL_PIN 2`, scale ≈ 5.12 |
| App shows tracker inactive / no location | `node_id ≠ imei`, or no owner | Make `imei == DEVICE_ID`, assign owner |
| Geofence not on map | radius too small + app loads zones once at login | Use ≥100 m radius; **reload the app** |
| No geofence alert | Alerts fire only on a **boundary crossing**; animal never crossed | Start inside zone, then move out |
| Tracker stopped sending indoors | A 15 s freshness gate (since reverted) | Reverted; now posts last-known too |

---

## 8. Next steps / ideas

- Attach a LiPo to validate the battery reading (expect 3.0–4.2 V).
- Add deep-sleep between posts for battery life (currently always-on).
- Optional: server-side **push notifications** (Expo Push) for geofence alerts when the phone is locked/app closed — current vibrate/ring is **foreground only**.
- Optional: external u.FL GNSS antenna for faster/indoor-ish lock.

---

## 9. Quick demo without GPS (fallback for presentations)

If you can't get outside, feed positions straight to the backend to drive the map/alerts:
```powershell
$inside  = '{"node_id":"heltec_tracker_01","latitude":36.7563,"longitude":3.2261,"battery":80}'
$outside = '{"node_id":"heltec_tracker_01","latitude":36.755949,"longitude":3.223332,"battery":80}'
Invoke-RestMethod http://localhost:3000/api/tracker -Method Post -ContentType application/json -Body $inside
Start-Sleep 3
Invoke-RestMethod http://localhost:3000/api/tracker -Method Post -ContentType application/json -Body $outside
```
