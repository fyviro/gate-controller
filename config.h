#ifndef GATE_CONFIG_H
#define GATE_CONFIG_H

/** GPIO for gate relay (active LOW pulse in triggerRelay). */
#define RELAY_PIN 18

/** Rolling access log size. */
#define MAX_LOGS 100

/** Signed token validity window (seconds), compared against RTC unix time. */
#define QR_VALID_WINDOW_SEC 300

/**
 * Max distinct mobile numbers per villa (same villa id string). Map key is still mobile.
 *
 * QR /open (owner HTML must match):
 *   Query: m, v or villa, t, s, d
 *   HMAC payload = mobile + timestamp  (villa is NOT part of the signature)
 *   s = hex HMAC-SHA256(secret, payload)
 *   Villa in URL must still match the villa stored for that mobile (separate check).
 *   d: required on first open (binds device); must match on every later open.
 */
#define MAX_RESIDENTS_PER_VILLA 4

/** Soft-AP credentials — change before deployment. */
#define AP_SSID "Gate-Access"
#define AP_PASSWORD "12345678"

/**
 * /adduser protection: if this string is non-empty, callers must pass query param
 * `a` with the same value. Leave empty only for lab / trusted networks.
 */
#define ADDUSER_ADMIN_KEY ""

#endif
