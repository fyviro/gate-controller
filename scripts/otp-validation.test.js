#!/usr/bin/env node
/**
 * Local test harness for OTP validation behavior.
 *
 * Why this exists:
 * - Lets you verify OTP math + timestamp-window logic before going to the gate.
 * - Mirrors firmware behavior after the "t query timestamp" change:
 *   - OTP uses query time t (unix seconds), not RTC.
 *   - Time acceptance uses relative clock (baseTS/baseMillis + millis drift), same as QR.
 *
 * Run:
 *   node --test scripts/otp-validation.test.js
 */
const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");

const QR_VALID_WINDOW_SEC = 1800; // keep in sync with config.h

function djb2U32(str) {
  let h = 5381 >>> 0;
  for (let i = 0; i < str.length; i++) {
    h = (((h << 5) + h) + str.charCodeAt(i)) >>> 0;
  }
  return h >>> 0;
}

function otp6FromSecret(secret, unixSeconds) {
  const tw = Math.floor(unixSeconds / 300);
  const payload = `${secret}|${tw}`;
  const h = djb2U32(payload) >>> 0;
  const t = Math.imul(tw, 0x9e3779b9) >>> 0;
  const mixed = (h ^ t) >>> 0;
  return String(mixed % 1000000).padStart(6, "0");
}

function usersValidateOtpForVillaSim(villa, otp6, unixSeconds, villaToSecrets) {
  if (otp6.length !== 6 || !/^\d{6}$/.test(otp6)) return false;
  const secrets = villaToSecrets.get(villa) || [];
  const want = Number(otp6);
  const tw = Math.floor(unixSeconds / 300);
  for (const secret of secrets) {
    for (let delta = -1; delta <= 1; delta++) {
      const twUse = tw + delta;
      if (twUse < 0) continue;
      const payload = `${secret}|${twUse}`;
      const h = djb2U32(payload) >>> 0;
      const t = Math.imul(twUse, 0x9e3779b9) >>> 0;
      const got = (h ^ t) >>> 0;
      if ((got % 1000000) === want) return true;
    }
  }
  return false;
}

function isPlausibleUnixTs(ts) {
  return /^\d{9,12}$/.test(ts);
}

function parseCsvField(raw) {
  const s = String(raw ?? "").trim();
  if (s.length >= 2 && s.startsWith("\"") && s.endsWith("\"")) {
    return s.slice(1, -1).trim();
  }
  return s;
}

function loadResidentsVillaSecrets(csvPath) {
  const full = path.resolve(csvPath);
  const txt = fs.readFileSync(full, "utf8");
  const villaToSecrets = new Map();
  let rowCount = 0;

  for (const lineRaw of txt.split(/\r?\n/)) {
    const line = lineRaw.trim();
    if (!line || line.startsWith("#")) continue;
    const parts = line.split(",");
    if (parts.length < 3) continue;

    const secret = parseCsvField(parts[1]);
    const villa = parseCsvField(parts[2]);
    if (!secret || !villa) continue;

    if (!villaToSecrets.has(villa)) villaToSecrets.set(villa, []);
    villaToSecrets.get(villa).push(secret);
    rowCount++;
  }
  return { villaToSecrets, rowCount };
}

class RelativeClock {
  constructor(windowSec = QR_VALID_WINDOW_SEC) {
    this.windowSec = windowSec;
    this.baseTS = 0;
    this.baseMillis = 0;
  }

  withinWindow(tokenTime, nowMillisSec) {
    if (this.baseTS === 0) {
      this.baseTS = tokenTime;
      this.baseMillis = nowMillisSec;
    }
    const currentTime = this.baseTS + (nowMillisSec - this.baseMillis);
    return Math.abs(currentTime - tokenTime) <= this.windowSec;
  }
}

function handleOpenOtpSim({ v, c, t }, nowMillisSec, clock, villaToSecrets) {
  const villa = (v || "").trim();
  const code = (c || "").trim();
  const ts = (t || "").trim();

  if (!villa || code.length !== 6 || !ts) {
    return { status: 400, body: "Missing v/villa, c/code (6-digit), or t (unix timestamp)" };
  }
  if (!/^\d{6}$/.test(code)) {
    return { status: 400, body: "Invalid OTP format" };
  }
  if (!isPlausibleUnixTs(ts)) {
    return { status: 403, body: "Invalid timestamp" };
  }

  const tokenTime = Number(ts);
  if (!clock.withinWindow(tokenTime, nowMillisSec)) {
    return { status: 403, body: "OTP expired" };
  }
  if (!usersValidateOtpForVillaSim(villa, code, tokenTime, villaToSecrets)) {
    return { status: 401, body: "Invalid OTP" };
  }
  return { status: 200, body: "Access Granted" };
}

test("OTP known vector stays stable", () => {
  // Fixed vector to catch accidental algorithm drift.
  assert.equal(otp6FromSecret("a1b2c3d4e5f6", 1710000000), "565895");
});

test("OTP accepts same and neighboring 5-min bucket", () => {
  const villaToSecrets = new Map([["89", ["a1b2c3d4e5f6"]]]);
  const ts = 1710000000;
  const otpNow = otp6FromSecret("a1b2c3d4e5f6", ts);
  const otpPrevWindow = otp6FromSecret("a1b2c3d4e5f6", ts - 300);
  const otpNextWindow = otp6FromSecret("a1b2c3d4e5f6", ts + 300);
  assert.equal(usersValidateOtpForVillaSim("89", otpNow, ts, villaToSecrets), true);
  assert.equal(usersValidateOtpForVillaSim("89", otpPrevWindow, ts, villaToSecrets), true);
  assert.equal(usersValidateOtpForVillaSim("89", otpNextWindow, ts, villaToSecrets), true);
});

test("OTP rejects two windows away", () => {
  const villaToSecrets = new Map([["89", ["a1b2c3d4e5f6"]]]);
  const ts = 1710000000;
  const otpFar = otp6FromSecret("a1b2c3d4e5f6", ts + 600);
  assert.equal(usersValidateOtpForVillaSim("89", otpFar, ts, villaToSecrets), false);
});

test("OTP endpoint rejects missing/invalid fields", () => {
  const villaToSecrets = new Map([["89", ["a1b2c3d4e5f6"]]]);
  const clock = new RelativeClock();
  assert.equal(handleOpenOtpSim({ v: "89", c: "123456", t: "" }, 10, clock, villaToSecrets).status, 400);
  assert.equal(handleOpenOtpSim({ v: "89", c: "12x456", t: "1710000000" }, 10, clock, villaToSecrets).status, 400);
  assert.equal(handleOpenOtpSim({ v: "89", c: "123456", t: "abc" }, 10, clock, villaToSecrets).status, 403);
});

test("OTP endpoint uses t-based relative window (same as QR)", () => {
  const villaToSecrets = new Map([["89", ["a1b2c3d4e5f6"]]]);
  const clock = new RelativeClock(1800);

  const t0 = 1710000000;
  const otp0 = otp6FromSecret("a1b2c3d4e5f6", t0);

  // First request seeds baseTS/baseMillis and succeeds.
  const ok = handleOpenOtpSim({ v: "89", c: otp0, t: String(t0) }, 100, clock, villaToSecrets);
  assert.equal(ok.status, 200);

  // Within window should still pass.
  const tWithin = t0 + 1200;
  const otpWithin = otp6FromSecret("a1b2c3d4e5f6", tWithin);
  const stillOk = handleOpenOtpSim(
    { v: "89", c: otpWithin, t: String(tWithin) },
    1300,
    clock,
    villaToSecrets
  );
  assert.equal(stillOk.status, 200);

  // Stale token should fail once drift exceeds window.
  const tExpired = t0;
  const otpExpired = otp0;
  const expired = handleOpenOtpSim(
    { v: "89", c: otpExpired, t: String(tExpired) },
    2001,
    clock,
    villaToSecrets
  );
  assert.equal(expired.status, 403);
  assert.equal(expired.body, "OTP expired");
});

test("residents.csv data validates with OTP algorithm", () => {
  const { villaToSecrets, rowCount } = loadResidentsVillaSecrets("residents.csv");
  assert.ok(rowCount > 0, "residents.csv should contain at least one user row");

  const ts = 1710000000;
  for (const [villa, secrets] of villaToSecrets.entries()) {
    for (const secret of secrets) {
      const otp = otp6FromSecret(secret, ts);
      assert.equal(
        usersValidateOtpForVillaSim(villa, otp, ts, villaToSecrets),
        true,
        `Expected OTP to validate for villa=${villa}`
      );
    }
  }
});

