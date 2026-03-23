#!/usr/bin/env node
/**
 * Verifies gate-access.html HMAC matches what the ESP32 computes:
 *   payload = mobile + timestampString   (UTF-8 bytes, no separator)
 *   s       = HMAC-SHA256(secret, payload) as lowercase hex (server accepts any case)
 *
 * Usage:
 *   node scripts/verify-open-hmac.js [mobile] [secret] [unixTs]
 *
 * Prints a sample /open query string (same shape as the QR URL).
 */
const crypto = require("crypto");

const m = process.argv[2] || "9908195316";
const k = process.argv[3] || "a1b2c3d4e5f6";
const t = process.argv[4] || String(Math.floor(Date.now() / 1000));

const payload = m + t;
const sig = crypto.createHmac("sha256", k).update(payload, "utf8").digest("hex");

const params = new URLSearchParams({ m, t, d: "sample-device-id", s: sig });
console.log("Payload bytes (UTF-8):", JSON.stringify(payload));
console.log("sig (64 hex chars):", sig);
console.log("Sample URL path: /open?" + params.toString());
