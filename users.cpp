#include "users.h"

#include <Preferences.h>
#include <algorithm>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "config.h"
#include "default_users.h"

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

static size_t countResidentsForVillaIn(const std::unordered_map<std::string, UserRecord>& m, const std::string& villa) {
  size_t n = 0;
  for (const auto& kv : m) {
    if (kv.second.villa == villa) {
      n++;
    }
  }
  return n;
}

static size_t countResidentsForVilla(const std::string& villa) { return countResidentsForVillaIn(g_users, villa); }

/** Fill g_users from default_users.h (replaces any existing entries). */
static void loadDefaultUsersFromHeader() {
  g_users.clear();
  for (size_t i = 0; i < DEFAULT_USER_COUNT; i++) {
    const DefaultUserSeed& s = DEFAULT_USERS[i];
    UserRecord rec;
    rec.mobile = s.mobile ? s.mobile : "";
    rec.owner_key = s.key ? s.key : "";
    rec.villa = s.villa ? s.villa : "";
    rec.deviceId = "";
    if (rec.mobile.empty() || rec.owner_key.empty() || rec.villa.empty()) {
      continue;
    }
    if (!g_users.emplace(rec.mobile, std::move(rec)).second) {
      Serial.printf("[users] duplicate mobile in default_users.h skipped: %s\n", s.mobile ? s.mobile : "?");
    }
  }
}

static void nvsMakeKey(char* buf, size_t buflen, char prefix, unsigned index) {
  snprintf(buf, buflen, "%c%03u", prefix, index);
}

/** putString returns 0 both on failure and for empty value; only non-empty can be checked. */
static bool prefsPutUserString(Preferences& prefs, const char* key, const char* val) {
  const char* v = val ? val : "";
  const size_t len = strlen(v);
  const size_t wr = prefs.putString(key, v);
  if (len > 0 && wr == 0) {
    return false;
  }
  return true;
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
    if (!prefsPutUserString(prefs, kb, r.mobile.c_str())) {
      prefs.end();
      return false;
    }
    nvsMakeKey(kb, sizeof(kb), 'k', i);
    if (!prefsPutUserString(prefs, kb, r.owner_key.c_str())) {
      prefs.end();
      return false;
    }
    nvsMakeKey(kb, sizeof(kb), 'v', i);
    if (!prefsPutUserString(prefs, kb, r.villa.c_str())) {
      prefs.end();
      return false;
    }
    nvsMakeKey(kb, sizeof(kb), 'd', i);
    if (!prefsPutUserString(prefs, kb, r.deviceId.c_str())) {
      prefs.end();
      return false;
    }
  }

  for (uint16_t i = n; i < oldCount; i++) {
    char kb[8];
    nvsMakeKey(kb, sizeof(kb), 'm', i);
    if (prefs.isKey(kb) && !prefs.remove(kb)) {
      prefs.end();
      return false;
    }
    nvsMakeKey(kb, sizeof(kb), 'k', i);
    if (prefs.isKey(kb) && !prefs.remove(kb)) {
      prefs.end();
      return false;
    }
    nvsMakeKey(kb, sizeof(kb), 'v', i);
    if (prefs.isKey(kb) && !prefs.remove(kb)) {
      prefs.end();
      return false;
    }
    nvsMakeKey(kb, sizeof(kb), 'd', i);
    if (prefs.isKey(kb) && !prefs.remove(kb)) {
      prefs.end();
      return false;
    }
  }

  if (prefs.putUShort("ucnt", n) != 2) {
    prefs.end();
    return false;
  }
  prefs.end();

  Preferences verify;
  if (!verify.begin(kNvsNamespace, true)) {
    return false;
  }
  const uint16_t stored = verify.getUShort("ucnt", 0xFFFF);
  verify.end();
  if (stored != n) {
    return false;
  }
  return true;
}

static bool adminTokenOk(const String& adminToken) {
  const char* required = ADDUSER_ADMIN_KEY;
  if (required[0] == '\0') {
    return true;
  }
  return adminToken == String(required);
}

bool usersRequireAdmin(const String& adminToken, String& errMsg) {
  if (!adminTokenOk(adminToken)) {
    errMsg = "Invalid or missing admin key (param a)";
    return false;
  }
  return true;
}

// void usersInit() {
//   if (!g_users.empty()) {
//     return;
//   }

//   uint16_t storedCount = 0;
//   bool nvsReadable = false;
//   {
//     Preferences peek;
//     if (peek.begin(kNvsNamespace, true)) {
//       nvsReadable = true;
//       storedCount = peek.getUShort("ucnt", 0);
//       peek.end();
//     }
//   }

//   g_users.clear();
//   if (usersLoadFromNvsInternal() && !g_users.empty()) {
//     Serial.printf("[users] Loaded %u user(s) from NVS\n", static_cast<unsigned>(g_users.size()));
//     return;
//   }

//   loadDefaultUsersFromHeader();
//   if (g_users.empty()) {
//     Serial.println("[users] default_users.h produced no users — check DEFAULT_USERS table");
//     return;
//   }

//   if (nvsReadable && storedCount > 0) {
//     Serial.printf(
//         "[users] NVS contained users but load failed — %u default user(s) in RAM only; "
//         "POST /adduser to rewrite NVS, or erase flash if this repeats\n",
//         static_cast<unsigned>(g_users.size()));
//     return;
//   }

//   if (!usersSaveToNvs()) {
//     Serial.println("[users] NVS save failed after default seed (RAM-only until fixed)");
//   }
// }

void usersInit() {
  if (!g_users.empty()) {
    return;
  }

  uint16_t storedCount = 0;
  bool nvsReadable = false;

  // 🔍 Peek NVS metadata
  {
    Preferences peek;
    if (peek.begin(kNvsNamespace, true)) {
      nvsReadable = true;
      storedCount = peek.getUShort("ucnt", 0);
      peek.end();
    }
  }

  // 🚀 Try loading from NVS (DO NOT clear before this)
  bool loaded = usersLoadFromNvsInternal();

  // ✅ Success: data loaded
  if (loaded && !g_users.empty()) {
    Serial.printf("[users] Loaded %u user(s) from NVS\n",
                  static_cast<unsigned>(g_users.size()));
    return;
  }

  // ❗ Load failed
  Serial.println("[users] ERROR: Failed to load users from NVS");

  // 🔴 CRITICAL FIX: Do NOT overwrite if NVS had data
  if (nvsReadable && storedCount > 0) {
    Serial.println("[users] NVS has data but load failed — NOT overwriting");
    return;
  }

  // ✅ First boot (no data in NVS)
  Serial.println("[users] First boot — loading default users");

  loadDefaultUsersFromHeader();

  if (g_users.empty()) {
    Serial.println("[users] default_users.h empty — nothing to seed");
    return;
  }

  // 💾 Save defaults to NVS
  if (!usersSaveToNvs()) {
    Serial.println("[users] NVS save failed after default seed");
  } else {
    Serial.printf("[users] Seeded %u users into NVS\n",
                  static_cast<unsigned>(g_users.size()));
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
  if (!adminTokenOk(adminToken)) {
    errMsg = "Invalid or missing admin key (param a)";
    return false;
  }

  if (mobile.length() == 0 || secretKey.length() == 0 || villa.length() == 0) {
    errMsg = "Missing mobile, secret, or villa";
    return false;
  }

  if (mobile.length() > MAX_MOBILE_LEN || secretKey.length() > MAX_SECRET_LEN || villa.length() > MAX_VILLA_LEN ||
      deviceId.length() > MAX_DEVICE_LEN) {
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
  rec.deviceId = deviceId.length() > 0 ? toStd(deviceId) : "";

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

static void jsonAppendEscaped(String& out, const String& s) {
  out += '"';
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c == '\t') {
      out += "\\t";
    } else {
      out += c;
    }
  }
  out += '"';
}

void usersBuildJsonList(String& out) {
  out = "";
  out.reserve(g_users.size() * 48 + 4);
  out += '[';
  std::vector<std::string> sorted;
  sorted.reserve(g_users.size());
  for (const auto& kv : g_users) {
    sorted.push_back(kv.first);
  }
  std::sort(sorted.begin(), sorted.end());
  bool first = true;
  for (const auto& mkey : sorted) {
    const UserRecord& r = g_users.at(mkey);
    if (!first) {
      out += ',';
    }
    first = false;
    out += "{\"mobile\":";
    jsonAppendEscaped(out, String(r.mobile.c_str()));
    out += ",\"villa\":";
    jsonAppendEscaped(out, String(r.villa.c_str()));
    out += ",\"deviceBound\":";
    out += r.deviceId.empty() ? "false" : "true";
    out += '}';
  }
  out += ']';
}

bool usersUpdate(const String& mobile, const String& secretKey, const String& villa, bool resetDeviceBinding,
                 const String& adminToken, String& errMsg) {
  if (!adminTokenOk(adminToken)) {
    errMsg = "Invalid or missing admin key (param a)";
    return false;
  }

  if (mobile.length() == 0) {
    errMsg = "Missing mobile";
    return false;
  }

  if (mobile.length() > MAX_MOBILE_LEN) {
    errMsg = "Field too long";
    return false;
  }

  if (secretKey.length() > MAX_SECRET_LEN || villa.length() > MAX_VILLA_LEN) {
    errMsg = "Field too long";
    return false;
  }

  std::string m = toStd(mobile);
  auto it = g_users.find(m);
  if (it == g_users.end()) {
    errMsg = "Mobile not registered";
    return false;
  }

  if (villa.length() > 0) {
    const std::string villaStd = toStd(villa);
    if (villaStd != it->second.villa) {
      if (countResidentsForVilla(villaStd) >= MAX_RESIDENTS_PER_VILLA) {
        errMsg = "Villa already has max residents (" + String(MAX_RESIDENTS_PER_VILLA) + ")";
        return false;
      }
      it->second.villa = villaStd;
    }
  }

  if (secretKey.length() > 0) {
    it->second.owner_key = toStd(secretKey);
  }

  if (resetDeviceBinding) {
    it->second.deviceId.clear();
  }

  if (!usersSaveToNvs()) {
    errMsg = "NVS save failed";
    return false;
  }
  return true;
}

/** Trim spaces and strip one pair of surrounding "..." (CSV-style empty is "" → empty string). */
static void normalizeCsvField(String& s) {
  s.trim();
  if (s.length() >= 2 && s.charAt(0) == '"' && s.charAt(s.length() - 1) == '"') {
    s = s.substring(1, s.length() - 1);
    s.trim();
  }
}

static bool splitUserCsvLine(const String& line, String& mobile, String& secret, String& villa, String& deviceId,
                             bool& hasDeviceCol) {
  String parts[4];
  int n = 0;
  int start = 0;
  for (int i = 0; i <= line.length(); i++) {
    if (i == line.length() || line.charAt(i) == ',') {
      if (n >= 4) {
        return false;
      }
      parts[n++] = line.substring(start, i);
      start = i + 1;
    }
  }
  if (n != 3 && n != 4) {
    return false;
  }
  for (int i = 0; i < n; i++) {
    normalizeCsvField(parts[i]);
  }
  mobile = parts[0];
  secret = parts[1];
  villa = parts[2];
  hasDeviceCol = (n == 4);
  deviceId = hasDeviceCol ? parts[3] : "";
  return mobile.length() > 0 && secret.length() > 0 && villa.length() > 0;
}

bool usersImportCsv(const String& csv, const String& adminToken, String& summary, String& errMsg) {
  summary = "";
  if (!usersRequireAdmin(adminToken, errMsg)) {
    return false;
  }
  if (csv.length() > MAX_CSV_IMPORT_BYTES) {
    errMsg = "CSV too large (max " + String(MAX_CSV_IMPORT_BYTES) + " bytes)";
    return false;
  }

  std::unordered_map<std::string, UserRecord> next = g_users;

  int added = 0;
  int updated = 0;
  int errors = 0;
  unsigned lineNo = 0;
  size_t pos = 0;
  String detail;
  detail.reserve(256);

  while (pos < csv.length()) {
    size_t nl = csv.indexOf('\n', pos);
    if (nl == (size_t)-1) {
      nl = csv.length();
    }
    String line = csv.substring(pos, nl);
    pos = nl + 1;
    line.trim();
    line.replace("\r", "");
    lineNo++;
    if (line.length() == 0 || line.charAt(0) == '#') {
      continue;
    }

    String mobile, secret, villa, deviceId;
    bool hasDevCol = false;
    if (!splitUserCsvLine(line, mobile, secret, villa, deviceId, hasDevCol)) {
      detail += "Line " + String(lineNo) + ": need mobile,secret,villa[,deviceId]\n";
      errors++;
      continue;
    }

    if (mobile.length() > MAX_MOBILE_LEN || secret.length() > MAX_SECRET_LEN || villa.length() > MAX_VILLA_LEN ||
        deviceId.length() > MAX_DEVICE_LEN) {
      detail += "Line " + String(lineNo) + ": field too long\n";
      errors++;
      continue;
    }

    const std::string ms = toStd(mobile);
    const std::string villaStd = toStd(villa);
    auto it = next.find(ms);

    if (it == next.end()) {
      if (next.size() >= MAX_USERS) {
        detail += "Line " + String(lineNo) + ": user store full\n";
        errors++;
        continue;
      }
      if (countResidentsForVillaIn(next, villaStd) >= MAX_RESIDENTS_PER_VILLA) {
        detail += "Line " + String(lineNo) + ": villa full\n";
        errors++;
        continue;
      }
      UserRecord rec;
      rec.mobile = ms;
      rec.owner_key = toStd(secret);
      rec.villa = villaStd;
      rec.deviceId = hasDevCol ? toStd(deviceId) : "";
      std::string key = ms;
      next.emplace(std::move(key), std::move(rec));
      added++;
    } else {
      it->second.owner_key = toStd(secret);
      if (villaStd != it->second.villa) {
        size_t cnt = 0;
        for (const auto& kv : next) {
          if (kv.first == ms) {
            continue;
          }
          if (kv.second.villa == villaStd) {
            cnt++;
          }
        }
        if (cnt >= MAX_RESIDENTS_PER_VILLA) {
          detail += "Line " + String(lineNo) + ": villa full\n";
          errors++;
          continue;
        }
        it->second.villa = villaStd;
      }
      if (hasDevCol) {
        it->second.deviceId = toStd(deviceId);
      }
      updated++;
    }
  }

  g_users.swap(next);
  if (!usersSaveToNvs()) {
    g_users.swap(next);
    errMsg = "NVS save failed; no changes applied";
    summary = detail;
    return false;
  }

  summary = "Added " + String(added) + ", updated " + String(updated) + ", errors " + String(errors) + ".\n" + detail;
  errMsg = "";
  return true;
}

size_t usersCount() { return g_users.size(); }
