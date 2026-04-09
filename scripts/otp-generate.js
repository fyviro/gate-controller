#!/usr/bin/env node
/**
 * Generate OTP exactly like firmware/users.cpp and app/index.html.
 *
 * Usage:
 *   node scripts/otp-generate.js <secret> [unixTs]
 * Example:
 *   node scripts/otp-generate.js a1b2c3d4e5f6
 */
const secret = process.argv[2];
if (!secret) {
  console.error("Usage: node scripts/otp-generate.js <secret> [unixTs]");
  process.exit(1);
}
const unixTs = Number(process.argv[3] || Math.floor(Date.now() / 1000));

function djb2U32(str) {
  let h = 5381 >>> 0;
  for (let i = 0; i < str.length; i++) {
    h = (((h << 5) + h) + str.charCodeAt(i)) >>> 0;
  }
  return h >>> 0;
}

const tw = Math.floor(unixTs / 300);
const payload = `${secret}|${tw}`;
const h = djb2U32(payload) >>> 0;
const t = Math.imul(tw, 0x9e3779b9) >>> 0;
const otp = ((h ^ t) >>> 0) % 1000000;
console.log(String(otp).padStart(6, "0"));
