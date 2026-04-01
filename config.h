#ifndef GATE_CONFIG_H
#define GATE_CONFIG_H

/** GPIO for gate relay (active LOW pulse in triggerRelay). */
#define RELAY_PIN 18

/** Rolling access log size. */
#define MAX_LOGS 100

/** Signed token validity window (seconds), compared against RTC unix time. */
#define QR_VALID_WINDOW_SEC 1800

/**
 * Max distinct mobile numbers per villa (same villa id string). Map key is still mobile.
 *
 * QR /open (owner HTML must match):
 *   Query: m, t, s, d  (villa is NOT sent — server uses villa from registration for that mobile)
 *   HMAC payload = mobile + timestamp
 *   s = hex HMAC-SHA256(secret, payload)
 *   d: required on first open (binds device); must match on every later open.
 */
#define MAX_RESIDENTS_PER_VILLA 4

/** Soft-AP credentials — defined in config.cpp (not macros: avoids name clash with variables). */
extern const char* AP_SSID;
extern const char* AP_PASSWORD;

/**
 * /adduser protection: if non-empty, callers must pass query param `a` with the same value.
 * Example below is 10 letters — change it to your own secret before deployment.
 */
#define ADDUSER_ADMIN_KEY "KxQmNpRjWt"

/**
 * Max size (bytes) of CSV text in POST /adduser/bulk field `csv`.
 * Keeps RAM bounded on ESP32 (~8 KB + one copy of user map during import).
 */
#define MAX_CSV_IMPORT_BYTES 8192

#endif
