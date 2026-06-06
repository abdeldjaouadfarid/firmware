#pragma once

// ── WiFi ──────────────────────────────────────────────────────────────────────
#define WIFI_SSID       "12345678"
#define WIFI_PASSWORD   "HIS2026$$"

// ── Backend ───────────────────────────────────────────────────────────────────
// Use your server machine's LAN IP (run `ipconfig` on the server to find it).
// The tracker and server must be on the same network.
#define SERVER_URL      "http://172.20.10.3:3000/api/tracker"

// ── Device ID ─────────────────────────────────────────────────────────────────
// Unique string per tracker — stored as `imei` in the database.
// Change this for each physical device you flash.
#define DEVICE_ID       "heltec_tracker_01"

// ── Behaviour ─────────────────────────────────────────────────────────────────
#define POST_INTERVAL_MS  10000UL   // how often to POST (ms) — 10 s default
