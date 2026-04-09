#!/usr/bin/env node
/**
 * Standalone mock of ESP32 /open endpoint for local testing.
 *
 * Mirrors core logic from web_server.cpp + users.cpp:
 * - GET /open?otp=true&v=&c=&t=...
 * - GET /open?m=&t=&s=&d=&otp=false
 * - Relative time window check (baseTS/baseMillis), no RTC
 * - OTP validation by villa (djb2 + tw*0x9E3779B9, ±1 window)
 * - QR HMAC validation and one-time signature replay cache
 * - In-memory device binding (loaded from CSV optional deviceId column)
 *
 * Usage:
 *   node scripts/mock-gate-server.js
 *   PORT=8081 USERS_CSV=./residents.csv node scripts/mock-gate-server.js
 */
const http = require("node:http");
const fs = require("node:fs");
const path = require("node:path");
const crypto = require("node:crypto");

const PORT = Number(process.env.PORT || 8080);
const USERS_CSV = path.resolve(process.env.USERS_CSV || "residents.csv");
const QR_VALID_WINDOW_SEC = 1800;
const REPLAY_CACHE_SIZE = 20;

let baseTS = 0;
let baseMillisSec = 0;
let sigIndex = 0;
const usedSigs = new Array(REPLAY_CACHE_SIZE).fill("");

/** mobile -> { mobile, owner_key, villa, deviceId } */
const users = new Map();
/** villa -> [mobile, ...] */
const villaToMobiles = new Map();

function nowMillisSec() {
  return Math.floor(Date.now() / 1000);
}

function htmlEscape(s) {
  return String(s)
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;")
    .replaceAll("'", "&#39;");
}

function buildResponse(title, color, icon, villa, msg) {
  return `<!DOCTYPE html>
<html><head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Gate</title>
<style>
body{font-family:Arial;background:#0b1220;color:#fff;display:flex;align-items:center;justify-content:center;min-height:100vh;margin:0}
.card{background:#111827;border:1px solid #374151;border-radius:16px;padding:22px;max-width:360px;width:90%;text-align:center}
h2{margin:4px 0 10px;color:${htmlEscape(color)}}
h3{margin:0 0 8px;color:#cbd5e1}
p{margin:8px 0 0;color:#9ca3af}
</style></head><body>
<div class="card">
  <div style="font-size:28px">${htmlEscape(icon)}</div>
  <h2>${htmlEscape(title)}</h2>
  <h3>Villa: ${htmlEscape(villa)}</h3>
  <p>${htmlEscape(msg)}</p>
</div>
</body></html>`;
}

function parseCsvField(raw) {
  let s = String(raw ?? "").trim();
  if (s.length >= 2 && s.startsWith('"') && s.endsWith('"')) {
    s = s.slice(1, -1).trim();
  }
  return s;
}

function rebuildVillaIndex() {
  villaToMobiles.clear();
  for (const [mobile, rec] of users.entries()) {
    if (!villaToMobiles.has(rec.villa)) {
      villaToMobiles.set(rec.villa, []);
    }
    villaToMobiles.get(rec.villa).push(mobile);
  }
}

function loadUsersFromCsv(csvPath) {
  const content = fs.readFileSync(csvPath, "utf8");
  users.clear();
  for (const lineRaw of content.split(/\r?\n/)) {
    const line = lineRaw.trim();
    if (!line || line.startsWith("#")) continue;
    const parts = line.split(",");
    if (parts.length < 3) continue;

    const mobile = parseCsvField(parts[0]);
    const owner_key = parseCsvField(parts[1]);
    const villa = parseCsvField(parts[2]);
    const deviceId = parts.length >= 4 ? parseCsvField(parts[3]) : "";
    if (!mobile || !owner_key || !villa) continue;

    users.set(mobile, { mobile, owner_key, villa, deviceId });
  }
  rebuildVillaIndex();
}

function isPlausibleUnixTs(ts) {
  return /^\d{9,12}$/.test(ts);
}

function relativeTokenWithinWindow(tokenTime) {
  const now = nowMillisSec();
  if (baseTS === 0) {
    baseTS = tokenTime;
    baseMillisSec = now;
  }
  const currentTime = baseTS + (now - baseMillisSec);
  return Math.abs(currentTime - tokenTime) <= QR_VALID_WINDOW_SEC;
}

function hmacSha256Hex(secret, payload) {
  return crypto.createHmac("sha256", secret).update(payload, "utf8").digest("hex");
}

function djb2U32(str) {
  let h = 5381 >>> 0;
  for (let i = 0; i < str.length; i++) {
    h = (((h << 5) + h) + str.charCodeAt(i)) >>> 0;
  }
  return h >>> 0;
}

function usersValidateOtpForVilla(villa, otp6, unixSeconds) {
  if (!/^\d{6}$/.test(otp6)) return { ok: false, mobile: "" };
  const want = Number(otp6);
  const mobiles = villaToMobiles.get(villa) || [];
  const tw = Math.floor(unixSeconds / 300);

  for (const mobile of mobiles) {
    const rec = users.get(mobile);
    if (!rec) continue;
    for (let delta = -1; delta <= 1; delta++) {
      const twUse = tw + delta;
      if (twUse < 0) continue;
      const payload = `${rec.owner_key}|${twUse}`;
      const h = djb2U32(payload) >>> 0;
      const t = Math.imul(twUse, 0x9e3779b9) >>> 0;
      const otp = (h ^ t) >>> 0;
      if ((otp % 1000000) === want) {
        return { ok: true, mobile };
      }
    }
  }
  return { ok: false, mobile: "" };
}

function send(res, status, contentType, body) {
  res.writeHead(status, { "Content-Type": contentType, "Connection": "close" });
  res.end(body);
}

function handleOpenOtp(res, params) {
  const villa = (params.get("villa") || params.get("v") || "").trim();
  const code = (params.get("c") || params.get("code") || "").trim();
  const ts = (params.get("t") || "").trim();

  if (!villa || code.length !== 6 || !ts) {
    return send(res, 400, "text/plain", "Missing v/villa, c/code (6-digit), or t (unix timestamp)");
  }
  if (!/^\d{6}$/.test(code)) {
    return send(res, 400, "text/plain", "Invalid OTP format");
  }
  if (!isPlausibleUnixTs(ts)) {
    return send(res, 403, "text/plain", "Invalid timestamp");
  }
  if (!villaToMobiles.has(villa)) {
    return send(res, 404, "text/plain", "Unknown villa");
  }
  const tokenTime = Number(ts);
  if (!relativeTokenWithinWindow(tokenTime)) {
    return send(res, 403, "text/plain", "OTP expired");
  }

  const out = usersValidateOtpForVilla(villa, code, tokenTime);
  if (!out.ok) {
    return send(res, 401, "text/plain", "Invalid OTP");
  }
  return send(res, 200, "text/html", buildResponse("Access Granted", "green", "✅", villa, ""));
}

function handleOpenQr(res, params) {
  const mobile = (params.get("m") || "").trim();
  const ts = (params.get("t") || "").trim();
  const sig = (params.get("s") || "").trim();
  const deviceId = (params.get("d") || "").trim();

  if (!mobile || !ts || !sig) {
    return send(res, 403, "text/html", buildResponse("Access Denied", "red", "❌", "-", "Missing m, t, or s"));
  }
  if (!isPlausibleUnixTs(ts)) {
    return send(res, 403, "text/html", buildResponse("Access Denied", "red", "❌", "-", "Invalid timestamp"));
  }
  const user = users.get(mobile);
  if (!user) {
    return send(res, 403, "text/html", buildResponse("Access Denied", "red", "❌", "-", "Unknown mobile"));
  }
  if (usedSigs.includes(sig)) {
    return send(res, 403, "text/html", buildResponse("Access Denied", "red", "❌", user.villa, "Already Used"));
  }

  const tokenTime = Number(ts);
  if (!relativeTokenWithinWindow(tokenTime)) {
    return send(res, 403, "text/html", buildResponse("Access Denied", "red", "❌", user.villa, "QR Expired"));
  }

  const expected = hmacSha256Hex(user.owner_key, mobile + ts);
  if (expected.toLowerCase() !== sig.toLowerCase()) {
    return send(res, 403, "text/html", buildResponse("Access Denied", "red", "❌", user.villa, "Invalid signature"));
  }

  // Same device-binding behavior as firmware.
  if (!user.deviceId) {
    if (!deviceId) {
      return send(
        res,
        403,
        "text/html",
        buildResponse("Access Denied", "red", "❌", user.villa, "Device ID required on first access")
      );
    }
    user.deviceId = deviceId;
  } else if (user.deviceId !== deviceId) {
    return send(res, 403, "text/html", buildResponse("Access Denied", "red", "❌", user.villa, "Device mismatch"));
  }

  usedSigs[sigIndex] = sig;
  sigIndex = (sigIndex + 1) % REPLAY_CACHE_SIZE;
  return send(res, 200, "text/html", buildResponse("Access Granted", "green", "✅", user.villa, ""));
}

function route(req, res) {
  const u = new URL(req.url, `http://${req.headers.host || "localhost"}`);
  if (u.pathname === "/health") {
    return send(res, 200, "application/json", JSON.stringify({ ok: true, users: users.size }));
  }
  if (u.pathname !== "/open") {
    return send(res, 404, "text/plain", "Not Found");
  }
  const otpFlag = (u.searchParams.get("otp") || "").trim().toLowerCase();
  if (otpFlag === "true" || otpFlag === "1") {
    return handleOpenOtp(res, u.searchParams);
  }
  return handleOpenQr(res, u.searchParams);
}

function main() {
  loadUsersFromCsv(USERS_CSV);
  const server = http.createServer(route);
  server.listen(PORT, "0.0.0.0", () => {
    console.log(`[mock-gate] listening on http://127.0.0.1:${PORT}`);
    console.log(`[mock-gate] users loaded: ${users.size} from ${USERS_CSV}`);
    console.log("[mock-gate] routes: GET /health, GET /open");
  });
}

main();
