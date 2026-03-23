# Gate controller — validated flow

This document matches the current firmware (`gate_controller.ino`, `web_server.cpp`, `users.cpp`, `config.h`).

## Boot

1. `setup()` → `relayInit()` → `WiFi.softAP(AP_SSID, AP_PASSWORD)` → RTC init (blocks if missing).
2. `usersInit()` → load users from NVS namespace `gateUsrs`; if missing/invalid, seed default user and save to NVS.
3. `registerWebRoutes()` → `webServerBegin()`.
4. `loop()` → `server.handleClient()`.

## Data model

- In-memory + NVS: `std::unordered_map` **key = mobile** (string).
- Each record: `owner_key` (HMAC secret), `villa`, `deviceId` (empty until first successful `/open` with `d`).

## A. Register resident (`GET /adduser` or `/adduer`)

| Param | Alt | Required | Notes |
|-------|-----|----------|--------|
| `villa` | `v` | yes | Villa id string |
| `mobile` | `m` | yes | Map key; must be unique |
| `secret` | `k` | yes | Stored as `owner_key` for HMAC |
| `a` | — | if `ADDUSER_ADMIN_KEY` set | Must match macro |
| `d` | — | ignored | Always stored empty; binding only via `/open` |

**Checks:** admin token (if configured), field lengths, global `MAX_USERS`, **≤ `MAX_RESIDENTS_PER_VILLA` mobiles per same villa**, duplicate mobile rejected.  
**Persist:** `usersSaveToNvs()` on success.

## B. Open gate (`GET /open`)

**Query:** `m`, `v` or `villa`, `t`, `s`, `d`

| Step | Check |
|------|--------|
| 1 | All of `m`, `v`/`villa`, `t`, `s` non-empty |
| 2 | `getUser(m)` → unknown mobile → 403 |
| 3 | Request villa **==** stored villa for that mobile → else 403 “Villa mismatch” |
| 4 | `t` is 9–12 decimal digits only → else 403 “Invalid timestamp” |
| 5 | `payload = m + t` (string concat, **no villa in HMAC**) |
| 6 | `expected = hex(HMAC-SHA256(owner_key, payload))` — compare to `s` **case-insensitive** |
| 7 | `abs(rtc.unixnow - t) <= QR_VALID_WINDOW_SEC` → else 403 “QR Expired” |
| 8 | `usersEnsureDeviceBinding(m, d)` — see below |
| 9 | `triggerRelay()` → 200 “Access Granted” + log |

### Device binding (`usersEnsureDeviceBinding`)

- Stored `deviceId` **empty:** `d` must be non-empty (length ≤ limit); value saved + NVS write; then allow.
- Stored `deviceId` **set:** `d` must **exactly match** stored value; else 403.

## C. Owner / QR HTML tool (must match firmware)

1. Same `mobile`, `villa`, and `owner_key` as registered via `/adduser`.
2. `t` = current unix time in **seconds** (string of digits only).
3. `s` = HMAC-SHA256(`owner_key`, `mobile + t`), **hex** (upper or lower case accepted by device).
4. URL includes `d` = stable device id (first successful open **binds** it).

Example path shape:

```text
/open?m=...&v=...&t=...&s=...&d=...
```

## D. Known limitations (by design)

- No nonce/replay cache: a valid `(m,t,s,d)` works repeatedly until `t` expires.
- `/logs` has no auth.
- Secrets and NVS are not encrypted on flash.

## E. Order sanity

Villa is verified **before** HMAC so a wrong villa fails fast. HMAC is checked **before** expiry so signature is validated first. Device bind runs **after** crypto + time so failed QRs do not bind devices.
