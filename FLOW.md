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

## A. Users API (`/adduser` and typo alias `/adduer`)

Same params work in **query string** or **`application/x-www-form-urlencoded`** body for POST/PUT.

### `GET` — list users (JSON)

- **Response:** `[{"mobile":"...","villa":"...","deviceBound":true|false}, ...]` — **no secrets**.
- **`deviceBound`** means “stored `deviceId` is non-empty” (same idea as first `/open` binding). It is **not** a separate field — if CSV had `""` as the 4th column without quote stripping, the literal two-quote string was wrongly stored; use `,,` / empty 4th field, or re-import after firmware strips quotes.
- **Query:** `a=` if `ADDUSER_ADMIN_KEY` is set.

### `POST` — create user

| Param | Alt | Required | Notes |
|-------|-----|----------|--------|
| `villa` | `v` | yes | |
| `mobile` | `m` | yes | Must not exist yet |
| `secret` | `k` | yes | |
| `a` | — | if admin key set | |
| `d` | — | optional | If set, pre-stores device id (else bind on first `/open`) |

**Checks:** admin, lengths, `MAX_USERS`, ≤ `MAX_RESIDENTS_PER_VILLA` per villa, duplicate mobile rejected.  
**Persist:** NVS on success. **Response:** HTML success page.

### `POST` — bulk CSV (`/adduser/bulk` or `/adduer/bulk`)

- **Body:** `application/x-www-form-urlencoded` with `a=` (admin) and **`csv=`** — full CSV text (max **`MAX_CSV_IMPORT_BYTES`**, default 8192).
- **Lines:** `mobile,secret,villa` or `mobile,secret,villa,deviceId` — **no commas inside fields**. `#` starts a comment line; blank lines skipped. Optional CSV-style quotes per field; **`""` in column 4 means empty device** (not the literal two-character id).
- **Behaviour:** new mobile → **add**; existing mobile → **update** secret/villa; 4th column if present sets **deviceId** (empty 4th field clears it). **One NVS write** after all rows.
- **Memory:** holds CSV in RAM plus a **temporary copy** of the user map while applying — fine for dozens of users; very large maps + huge CSV may stress heap on small ESP32 modules.
- **Response:** `text/plain` summary (`Added n, updated m, errors k` + per-line messages).

### `PUT` — update user

| Param | Alt | Required | Notes |
|-------|-----|----------|--------|
| `mobile` | `m` | yes | Must already exist |
| `secret` | `k` | no | Omit to keep current secret |
| `villa` | `v` | no | Omit to keep current villa |
| `reset_device` | — | no | `1` / `true` / `yes` clears `deviceId` (re-bind on next `/open`) |
| `a` | — | if admin key set | |

**Persist:** NVS on success. **Response:** HTML success page.

## B. Open gate (`GET /open`)

**Query:** `m`, `t`, `s`, `d` — **no villa in URL**; villa for logs/UI comes from the user record keyed by `m`.

| Step | Check |
|------|--------|
| 1 | `m`, `t`, `s` non-empty |
| 2 | `getUser(m)` → unknown mobile → 403 |
| 3 | `t` is 9–12 decimal digits only → else 403 “Invalid timestamp” |
| 4 | `payload = m + t` (string concat) |
| 5 | `expected = hex(HMAC-SHA256(owner_key, payload))` — compare to `s` **case-insensitive** |
| 6 | `abs(rtc.unixnow - t) <= QR_VALID_WINDOW_SEC` → else 403 “QR Expired” |
| 7 | `usersEnsureDeviceBinding(m, d)` — see below |
| 8 | `triggerRelay()` → 200 “Access Granted” + log |

### Device binding (`usersEnsureDeviceBinding`)

- Stored `deviceId` **empty:** `d` must be non-empty (length ≤ limit); value saved + NVS write; then allow.
- Stored `deviceId` **set:** `d` must **exactly match** stored value; else 403.

## C. Owner / QR HTML tool (must match firmware)

Reference implementation in-repo: **`gate-access.html`** (resident phone; QR encodes `http://192.168.4.1/open?...` — ESP32 Soft AP default).


1. Same `mobile` and `owner_key` as registered via `/adduser` (villa is only on the server).
2. `t` = current unix time in **seconds** (string of digits only).
3. `s` = HMAC-SHA256(`owner_key`, `mobile + t`), **hex** (upper or lower case accepted by device).
4. URL includes `d` = stable device id (first successful open **binds** it).

Example path shape:

```text
/open?m=...&t=...&s=...&d=...
```

## Cross-check: `gate-access.html` vs firmware

| Item | Client (`gate-access.html`) | Server (`web_server.cpp` + `auth.cpp`) |
|------|-----------------------------|----------------------------------------|
| Path | `GET /open?...` | `server.on("/open", handleOpen)` |
| Params | `m`, `t`, `s`, `d` (URL-encoded in query string) | `server.arg("m|t|s|d")`, trimmed |
| HMAC data | `mobile + String(unixSeconds)` UTF-8 | `mobile + ts` same string concat |
| HMAC algo | Web Crypto `HMAC` + `SHA-256` | mbedTLS `MBEDTLS_MD_SHA256` |
| Sig format | 64-char lowercase hex | Uppercase hex; compare **case-insensitive** |
| Clock window | UI timer `QR_VALID_SEC = 300` | `QR_VALID_WINDOW_SEC` in `config.h` — **keep both 300** |
| Device | Stable `localStorage` id, always sent as `d` | `usersEnsureDeviceBinding` |

Optional: run `node scripts/verify-open-hmac.js <mobile> <secret> <t>` to print a signed query string using the same algorithm as the HTML page.

## D. Known limitations (by design)

- No nonce/replay cache: a valid `(m,t,s,d)` works repeatedly until `t` expires.
- `/logs` has no auth.
- Secrets and NVS are not encrypted on flash.

## E. Order sanity

Lookup by `m` supplies villa for logging/UI. HMAC is checked **before** expiry. Device bind runs **after** crypto + time so failed QRs do not bind devices.
