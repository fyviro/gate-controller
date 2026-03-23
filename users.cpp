#include "users.h"

#include <Preferences.h>
#include <algorithm>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "config.h"

/**
 * User directory: keyed by mobile (std::string) for O(1) lookup.
 * C++ also has std::map (ordered tree) and std::unordered_map (hash table);
 * unordered_map is usually better for string keys on ESP32.
 */
struct UserRecord {
  std::string mobile;
  std::string owner_key;
  std::string villa;
  std::string deviceId;
};

static std::unordered_map<std::string, UserRecord> g_users;

static const size_t MAX_USERS = 256;
static const size_t MAX_MOBILE_LEN = 20;
static const size_t MAX_SECRET_LEN = 128;
static const size_t MAX_VILLA_LEN = 32;
static const size_t MAX_DEVICE_LEN = 64;

/** NVS namespace (max 15 chars). */
static const char kNvsNamespace[] = "gateUsrs";

static std::string toStd(const String& s) { return std::string(s.c_str()); }

static size_t countResidentsForVilla(const std::string& villa) {
  size_t n = 0;
  for (const auto& kv : g_users) {
    if (kv.second.villa == villa) {
      n++;
    }
  }
  return n;
}

static void nvsMakeKey(char* buf, size_t buflen, char prefix, unsigned index) {
  snprintf(buf, buflen, "%c%03u", prefix, index);
}

static bool usersLoadFromNvsInternal() {
  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, true)) {
    return false;
  }

  uint16_t n = prefs.getUShort("ucnt", 0);
  if (n == 0 || n > MAX_USERS) {
    prefs.end();
    return false;
  }

  std::unordered_map<std::string, UserRecord> loaded;
  loaded.reserve(n);

  for (uint16_t i = 0; i < n; i++) {
    char kb[8];
    nvsMakeKey(kb, sizeof(kb), 'm', i);
    String ms = prefs.getString(kb, "");
    nvsMakeKey(kb, sizeof(kb), 'k', i);
    String ks = prefs.getString(kb, "");
    nvsMakeKey(kb, sizeof(kb), 'v', i);
    String vs = prefs.getString(kb, "");
    nvsMakeKey(kb, sizeof(kb), 'd', i);
    String ds = prefs.getString(kb, "");

    if (ms.length() == 0 || ks.length() == 0 || vs.length() == 0) {
      prefs.end();
      return false;
    }
    if (ms.length() > MAX_MOBILE_LEN || ks.length() > MAX_SECRET_LEN || vs.length() > MAX_VILLA_LEN ||
        ds.length() > MAX_DEVICE_LEN) {
      prefs.end();
      return false;
    }

    UserRecord rec;
    rec.mobile = toStd(ms);
    rec.owner_key = toStd(ks);
    rec.villa = toStd(vs);
    rec.deviceId = toStd(ds);

    if (!loaded.emplace(rec.mobile, std::move(rec)).second) {
      prefs.end();
      return false;
    }
  }

  prefs.end();
  g_users.swap(loaded);
  return g_users.size() == n;
}

bool usersSaveToNvs() {
  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, false)) {
    return false;
  }

  const uint16_t oldCount = prefs.getUShort("ucnt", 0);

  std::vector<std::string> sorted;
  sorted.reserve(g_users.size());
  for (const auto& kv : g_users) {
    sorted.push_back(kv.first);
  }
  std::sort(sorted.begin(), sorted.end());

  const uint16_t n = static_cast<uint16_t>(sorted.size());
  if (n > MAX_USERS) {
    prefs.end();
    return false;
  }

  for (uint16_t i = 0; i < n; i++) {
    const auto it = g_users.find(sorted[i]);
    if (it == g_users.end()) {
      prefs.end();
      return false;
    }
    const UserRecord& r = it->second;
    char kb[8];
    nvsMakeKey(kb, sizeof(kb), 'm', i);
    prefs.putString(kb, r.mobile.c_str());
    nvsMakeKey(kb, sizeof(kb), 'k', i);
    prefs.putString(kb, r.owner_key.c_str());
    nvsMakeKey(kb, sizeof(kb), 'v', i);
    prefs.putString(kb, r.villa.c_str());
    nvsMakeKey(kb, sizeof(kb), 'd', i);
    prefs.putString(kb, r.deviceId.c_str());
  }

  for (uint16_t i = n; i < oldCount; i++) {
    char kb[8];
    nvsMakeKey(kb, sizeof(kb), 'm', i);
    prefs.remove(kb);
    nvsMakeKey(kb, sizeof(kb), 'k', i);
    prefs.remove(kb);
    nvsMakeKey(kb, sizeof(kb), 'v', i);
    prefs.remove(kb);
    nvsMakeKey(kb, sizeof(kb), 'd', i);
    prefs.remove(kb);
  }

  prefs.putUShort("ucnt", n);
  prefs.end();
  return true;
}

static bool adminTokenOk(const String& adminToken) {
  const char* required = ADDUSER_ADMIN_KEY;
  if (required[0] == '\0') {
    return true;
  }
  return adminToken == String(required);
}

void usersInit() {
  if (!g_users.empty()) {
    return;
  }

  g_users.clear();
  if (usersLoadFromNvsInternal() && !g_users.empty()) {
    return;
  }

  g_users.clear();
  UserRecord seed;
  seed.mobile = "9908195316";
  seed.owner_key = "a1b2c3d4e5f6";
  seed.villa = "74";
  seed.deviceId = "";
  g_users.emplace(seed.mobile, std::move(seed));

  if (!usersSaveToNvs()) {
    Serial.println("[users] NVS save failed after seed (RAM-only until fixed)");
  }
}

bool getUser(String mobile, String& key, String& villa, String& deviceId) {
  auto it = g_users.find(toStd(mobile));
  if (it == g_users.end()) {
    return false;
  }
  key = String(it->second.owner_key.c_str());
  villa = String(it->second.villa.c_str());
  deviceId = String(it->second.deviceId.c_str());
  return true;
}

bool usersEnsureDeviceBinding(const String& mobile, const String& requestDeviceId, String& errMsg) {
  auto it = g_users.find(toStd(mobile));
  if (it == g_users.end()) {
    errMsg = "Unknown mobile";
    return false;
  }

  std::string& stored = it->second.deviceId;

  if (stored.empty()) {
    if (requestDeviceId.length() == 0) {
      errMsg = "Device ID required on first access";
      return false;
    }
    if (requestDeviceId.length() > MAX_DEVICE_LEN) {
      errMsg = "Device ID too long";
      return false;
    }
    stored = toStd(requestDeviceId);
    if (!usersSaveToNvs()) {
      stored.clear();
      errMsg = "NVS save failed (device bind)";
      return false;
    }
    return true;
  }

  if (requestDeviceId != String(stored.c_str())) {
    errMsg = "Device ID mismatch";
    return false;
  }
  return true;
}

bool usersAdd(const String& mobile, const String& secretKey, const String& villa, const String& deviceId,
              const String& adminToken, String& errMsg) {
  (void)deviceId;
  if (!adminTokenOk(adminToken)) {
    errMsg = "Invalid or missing admin key (param a)";
    return false;
  }

  if (mobile.length() == 0 || secretKey.length() == 0 || villa.length() == 0) {
    errMsg = "Missing mobile, secret, or villa";
    return false;
  }

  if (mobile.length() > MAX_MOBILE_LEN || secretKey.length() > MAX_SECRET_LEN || villa.length() > MAX_VILLA_LEN) {
    errMsg = "Field too long";
    return false;
  }

  if (g_users.size() >= MAX_USERS) {
    errMsg = "User store full";
    return false;
  }

  const std::string villaStd = toStd(villa);
  if (countResidentsForVilla(villaStd) >= MAX_RESIDENTS_PER_VILLA) {
    errMsg = "Villa already has max residents (" + String(MAX_RESIDENTS_PER_VILLA) + ")";
    return false;
  }

  std::string m = toStd(mobile);
  if (g_users.find(m) != g_users.end()) {
    errMsg = "Mobile already registered";
    return false;
  }

  UserRecord rec;
  rec.mobile = m;
  rec.owner_key = toStd(secretKey);
  rec.villa = villaStd;
  rec.deviceId = "";

  const auto inserted = g_users.emplace(std::move(m), std::move(rec));
  if (!inserted.second) {
    errMsg = "Mobile already registered";
    return false;
  }

  if (!usersSaveToNvs()) {
    g_users.erase(inserted.first);
    errMsg = "NVS save failed";
    return false;
  }
  return true;
}

size_t usersCount() { return g_users.size(); }
