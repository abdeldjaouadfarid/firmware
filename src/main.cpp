#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TinyGPS++.h>
#include "config.h"

// ── Heltec Wireless Tracker v1.1 / v1.2 pin assignments ──────────────────────
// Verify against https://docs.heltec.org/en/node/esp32/wireless_tracker/hardware_update_log.html
// if something does not work.
#define GPS_RX_PIN   33   // ESP32 RX ← GPS TX (UC6580)
#define GPS_TX_PIN   34   // ESP32 TX → GPS RX (UC6580)
#define GPS_BAUD     115200  // UC6580 on the Wireless Tracker defaults to 115200, not 9600
#define VEXT_PIN      3   // HIGH = power on for GPS + peripherals (V1.1 confirmed)

// Battery ADC — values from Meshtastic's tested heltec_wireless_tracker variant.
#define BAT_ADC_PIN    1   // VBAT_Read, ADC1_CH0
#define BAT_CTRL_PIN   2   // ADC_CTRL — active HIGH, powers the divider (V1.1/V1.2)
// Divider ratio that scales Vbat back up from the ADC pin voltage.
#define BAT_VOLT_SCALE (4.9f * 1.045f)   // ≈ 5.12
#define BAT_V_MIN      3.0f   // voltage at 0 %
#define BAT_V_MAX      4.2f   // voltage at 100 %

// ─────────────────────────────────────────────────────────────────────────────

HardwareSerial gpsSerial(1);
TinyGPSPlus    gps;

unsigned long lastPost = 0;

// ── Battery ───────────────────────────────────────────────────────────────────
// Returns battery voltage in volts. 0 likely means no LiPo attached (USB-only).
float readBatteryVoltage() {
    // ADC_CTRL is active HIGH; the variant uses an internal pull-up rather than
    // actively driving it, so enable the divider the same way.
    pinMode(BAT_CTRL_PIN, INPUT_PULLUP);
    // Low attenuation: the divider output is small, this improves accuracy.
    analogSetPinAttenuation(BAT_ADC_PIN, ADC_2_5db);
    delay(20);
    uint32_t mv = analogReadMilliVolts(BAT_ADC_PIN);   // calibrated pin voltage (mV)
    pinMode(BAT_CTRL_PIN, INPUT);                      // stop powering the divider

    return (mv * BAT_VOLT_SCALE) / 1000.0f;
}

int voltageToPercent(float voltage) {
    int pct = (int)(((voltage - BAT_V_MIN) / (BAT_V_MAX - BAT_V_MIN)) * 100.0f);
    return constrain(pct, 0, 100);
}

// ── WiFi ──────────────────────────────────────────────────────────────────────
void connectWiFi() {
    Serial.printf("Connecting to %s", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 40) {
        delay(500);
        Serial.print('.');
        attempts++;
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\nWiFi connected  IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.println("\nWiFi connect failed — will retry on next POST");
    }
}

// ── HTTP POST ─────────────────────────────────────────────────────────────────
void postLocation(double lat, double lng, int battery) {
    if (WiFi.status() != WL_CONNECTED) {
        connectWiFi();
        if (WiFi.status() != WL_CONNECTED) return;
    }

    HTTPClient http;
    http.begin(SERVER_URL);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(8000);

    JsonDocument doc;
    doc["node_id"]   = DEVICE_ID;
    doc["latitude"]  = serialized(String(lat, 6));
    doc["longitude"] = serialized(String(lng, 6));
    doc["battery"]   = battery;

    String body;
    serializeJson(doc, body);

    int code = http.POST(body);
    if (code > 0) {
        Serial.printf("[POST %d] %s\n", code, http.getString().c_str());
    } else {
        Serial.printf("[POST error] %s\n", HTTPClient::errorToString(code).c_str());
    }
    http.end();
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println("\n=== Heltec Wireless Tracker firmware ===");
    Serial.printf("Device ID : %s\n", DEVICE_ID);
    Serial.printf("Server    : %s\n", SERVER_URL);

    // Power on GPS module via Vext rail
    pinMode(VEXT_PIN, OUTPUT);
    digitalWrite(VEXT_PIN, HIGH);
    delay(200);

    gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
    Serial.println("GPS serial started — waiting for fix...");

    connectWiFi();
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    // Keep the GPS parser fed — call as often as possible.
    // For the first 15 s also echo the raw bytes so we can eyeball the NMEA
    // stream (readable "$GNGGA,..." = right baud; garbage = wrong baud).
    while (gpsSerial.available()) {
        char c = gpsSerial.read();
        if (millis() < 15000) Serial.write(c);
        gps.encode(c);
    }

    unsigned long now = millis();
    if (now - lastPost < POST_INTERVAL_MS) return;
    lastPost = now;

    // Post as soon as we have ANY valid position. TinyGPS keeps the last known
    // fix after signal loss (its age just grows), so the tracker keeps reporting
    // its last location through brief GPS dropouts instead of going dark.
    if (!gps.location.isValid()) {
        // No fix yet this session. Diagnostics:
        //  chars=0          → GPS not sending data (power/pin/baud) — check wiring
        //  chars>0, csum>0  → data arriving but corrupt → wrong baud rate
        //  chars>0, csum=0  → all good, just needs sky view / time to lock
        Serial.printf("[GPS] No fix yet  sats=%d  chars=%lu  good=%lu  csumErr=%lu  | WiFi=%s\n",
                      gps.satellites.value(), gps.charsProcessed(),
                      gps.passedChecksum(), gps.failedChecksum(),
                      WiFi.status() == WL_CONNECTED ? "ok" : "down");
        return;
    }

    double lat   = gps.location.lat();
    double lng   = gps.location.lng();
    unsigned long fixAge = gps.location.age();   // ms since last real fix (small = fresh)
    float  volts = readBatteryVoltage();
    int    bat   = voltageToPercent(volts);

    Serial.printf("[GPS] lat=%.6f  lng=%.6f  sats=%d  hdop=%.1f  fixAge=%lums  | bat=%d%% (%.2fV)\n",
                  lat, lng, gps.satellites.value(), gps.hdop.hdop(), fixAge, bat, volts);

    postLocation(lat, lng, bat);
}
