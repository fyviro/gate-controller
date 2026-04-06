#ifndef GATE_USERS_H
#define GATE_USERS_H

#include <Arduino.h>

/**
 * Load users from NVS (flash), or install factory seed on first/empty flash.
 * After a successful POST /adduser (or bulk/update), data is written to NVS and survives
 * power-off and reboot. A normal firmware upload keeps NVS; only a full flash erase clears users.
 */
void usersInit();

/** True if admin token is acceptable for /adduser APIs (see ADDUSER_ADMIN_KEY in config.h). */
bool usersRequireAdmin(const String& adminToken, String& errMsg);

bool getUser(String mobile, String& key, String& villa, String& deviceId);

/**
 * OTP mode (/open?otp=true&...): match 6-digit code for any resident of villa (RTC unix, 5 min buckets).
 * OTP = (timeWindow + djb2(secret)%1e6) % 1e6 — one code per window; PWA disables regenerate until next window.
 */
bool usersValidateOtpForVilla(const String& villa, const String& otp6, String& matchedMobile);

/**
 * Enforce device binding after HMAC + time checks.
 * If stored deviceId is empty: requires non-empty requestDeviceId, saves it to NVS.
 * If stored deviceId is set: requestDeviceId must match exactly.
 */
bool usersEnsureDeviceBinding(const String& mobile, const String& requestDeviceId, String& errMsg);

/**
 * Add a user (dynamic store backed by std::unordered_map in users.cpp).
 * New users always start with empty deviceId; binding happens on first /open with d=.
 * @param deviceId ignored (kept for API compatibility); use empty from /adduser.
 * @param adminToken value of query param `a`; must match ADDUSER_ADMIN_KEY when that macro is non-empty.
 * @param errMsg human-readable failure reason when returning false.
 */
/** If deviceId non-empty, store it (CSV/bulk); web POST usually leaves empty. */
bool usersAdd(const String& mobile, const String& secretKey, const String& villa, const String& deviceId,
              const String& adminToken, String& errMsg);

/**
 * Update an existing user (mobile must exist).
 * Empty secretKey → keep current secret. Empty villa → keep current villa.
 * resetDeviceBinding → clear stored device id (visitor must re-bind on next /open).
 */
bool usersUpdate(const String& mobile, const String& secretKey, const String& villa, bool resetDeviceBinding,
                 const String& adminToken, String& errMsg);

/** JSON array: [{"mobile":"...","villa":"...","deviceBound":true}, ...] (no secrets). */
void usersBuildJsonList(String& out);

/**
 * Bulk import from CSV (POST /adduser/bulk, form field `csv`).
 * Rows: mobile,secret,villa[,deviceId] — no commas inside fields. Lines starting with # ignored.
 * 3 columns: device unchanged on update; empty device on new user.
 * 4 columns: set deviceId (empty 4th field clears binding). New users upserted; existing updated.
 * One NVS write at end; uses ~2× user map RAM briefly. Max length MAX_CSV_IMPORT_BYTES.
 */
bool usersImportCsv(const String& csv, const String& adminToken, String& summary, String& errMsg);

/** Current number of registered users (for diagnostics). */
size_t usersCount();

/**
 * Persist the current user map to NVS (called automatically after a successful add).
 * Returns false if Preferences could not write (e.g. NVS full).
 */
bool usersSaveToNvs();

#endif
