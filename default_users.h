#ifndef GATE_DEFAULT_USERS_H
#define GATE_DEFAULT_USERS_H

#include <stddef.h>

/**
 * Factory seed rows when NVS is empty or when NVS load fails (RAM-only fallback).
 * mobile = lookup key; key = HMAC secret; villa = gate label; deviceId starts empty (/open binds d=).
 *
 * Keep fields within limits in users.cpp (MAX_MOBILE_LEN, MAX_SECRET_LEN, MAX_VILLA_LEN).
 * Mobile numbers must be unique.
 */
struct DefaultUserSeed {
  const char* mobile;
  const char* key;
  const char* villa;
};

static const DefaultUserSeed DEFAULT_USERS[] = {
    {"9908195316", "a1b2c3d4e5f6", "74"},
};

static const size_t DEFAULT_USER_COUNT = sizeof(DEFAULT_USERS) / sizeof(DEFAULT_USERS[0]);

#endif
