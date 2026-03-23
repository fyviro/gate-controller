#ifndef GATE_USERS_H
#define GATE_USERS_H

#include <Arduino.h>

/** Seed built-in users; call once from setup() before serving HTTP. */
void usersInit();

bool getUser(String mobile, String& key, String& villa, String& deviceId);

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
bool usersAdd(const String& mobile, const String& secretKey, const String& villa, const String& deviceId,
              const String& adminToken, String& errMsg);

/** Current number of registered users (for diagnostics). */
size_t usersCount();

/**
 * Persist the current user map to NVS (called automatically after a successful add).
 * Returns false if Preferences could not write (e.g. NVS full).
 */
bool usersSaveToNvs();

#endif
