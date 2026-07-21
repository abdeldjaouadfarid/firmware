#pragma once

// ── WiFi ──────────────────────────────────────────────────────────────────────
#define WIFI_SSID       "12345678"
#define WIFI_PASSWORD   "HIS2026$$"

// ── Backend ───────────────────────────────────────────────────────────────────
// Use your server machine's LAN IP (run `ipconfig` on the server to find it).
// The tracker and server must be on the same network.
#define SERVER_URL      "https://backend-wvbf.onrender.com/api/tracker"

// ── Device ID ─────────────────────────────────────────────────────────────────
// Unique string per tracker — stored as `imei` in the database.
// IMPORTANT: change this for EACH physical device you flash, or two units will
// collide on the same database row and overwrite each other's position.
#define DEVICE_ID       "heltec_tracker_01"

// ── Device key ────────────────────────────────────────────────────────────────
// Shared secret sent as the X-Tracker-Key header. MUST match TRACKER_API_KEY in
// the backend .env, otherwise the server rejects every POST with 401.
#define DEVICE_KEY      "834e596afceac526aaa7910c66d5986b435c1e4c6582c3f8"

// ── Behaviour ─────────────────────────────────────────────────────────────────
#define POST_INTERVAL_MS  10000UL   // how often to POST (ms) — 10 s default
