#include "users.h"

#include <Preferences.h>
#include <algorithm>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include <ctype.h>

#include "config.h"
#include "default_users.h"
#include "rtc_device.h"

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

/** Runtime-only: villa string -> mobiles in that villa (derived from g_users; not persisted). */
static std::unordered_map<std::string, std::vector<std::string>> g_villaToMobiles;

static void rebuildVillaIndex() {
  g_villaToMobiles.clear();
  g_villaToMobiles.reserve(g_users.size() / 4 + 1);
  for (const auto& kv : g_users) {
    g_villaToMobiles[kv.second.villa].push_back(kv.first);
  }
}

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

/** One blob key — many per-user string keys exhaust NVS entry limits (~55 users × 4 ≈ 220 commits/keys). */
static const char kNvsUsersBlobKey[] = "users";
static const uint16_t kUserBlobMagic = 0x5547;
static const uint16_t kUserBlobVer = 1;

static void nvsMakeKey(char* buf, size_t buflen, char prefix, unsigned index) {
  snprintf(buf, buflen, "%c%03u", prefix, index);
}

static bool blobPull8(const uint8_t*& p, const uint8_t* end, uint8_t& out) {
  if (p >= end) {
    return false;
  }
  out = *p++;
  return true;
}

static bool blobPull16(const uint8_t*& p, const uint8_t* end, uint16_t& out) {
  if (p + 2 > end) {
    return false;
  }
  out = static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
  p += 2;
  return true;
}

static bool blobPullStr(const uint8_t*& p, const uint8_t* end, std::string& s, size_t maxLen) {
  uint8_t len = 0;
  if (!blobPull8(p, end, len)) {
    return false;
  }
  if (len > maxLen || p + len > end) {
    return false;
  }
  s.assign(reinterpret_cast<const char*>(p), len);
  p += len;
  return true;
}

static bool appendUserToBlob(std::vector<uint8_t>& b, const UserRecord& r) {
  if (r.mobile.size() > MAX_MOBILE_LEN || r.owner_key.size() > MAX_SECRET_LEN || r.villa.size() > MAX_VILLA_LEN ||
      r.deviceId.size() > MAX_DEVICE_LEN) {
    return false;
  }
  b.push_back(static_cast<uint8_t>(r.mobile.size()));
  b.insert(b.end(), r.mobile.begin(), r.mobile.end());
  b.push_back(static_cast<uint8_t>(r.owner_key.size()));
  b.insert(b.end(), r.owner_key.begin(), r.owner_key.end());
  b.push_back(static_cast<uint8_t>(r.villa.size()));
  b.insert(b.end(), r.villa.begin(), r.villa.end());
  b.push_back(static_cast<uint8_t>(r.deviceId.size()));
  b.insert(b.end(), r.deviceId.begin(), r.deviceId.end());
  return true;
}

static bool usersSerializeToBlob(std::vector<uint8_t>& out) {
  out.clear();
  out.reserve(g_users.size() * 48 + 16);
  out.push_back(static_cast<uint8_t>(kUserBlobMagic & 0xFF));
  out.push_back(static_cast<uint8_t>(kUserBlobMagic >> 8));
  out.push_back(static_cast<uint8_t>(kUserBlobVer & 0xFF));
  out.push_back(static_cast<uint8_t>(kUserBlobVer >> 8));
  if (g_users.size() > MAX_USERS) {
    return false;
  }
  const uint16_t n = static_cast<uint16_t>(g_users.size());
  out.push_back(static_cast<uint8_t>(n & 0xFF));
  out.push_back(static_cast<uint8_t>(n >> 8));
  std::vector<std::string> sorted;
  sorted.reserve(g_users.size());
  for (const auto& kv : g_users) {
    sorted.push_back(kv.first);
  }
  std::sort(sorted.begin(), sorted.end());
  for (const auto& mkey : sorted) {
    const UserRecord& r = g_users.at(mkey);
    if (!appendUserToBlob(out, r)) {
      return false;
    }
  }
  return true;
}

static bool usersParseBlobIntoMap(const uint8_t* data, size_t len, std::unordered_map<std::string, UserRecord>& loaded) {
  const uint8_t* p = data;
  const uint8_t* end = data + len;
  uint16_t magic = 0;
  uint16_t ver = 0;
  uint16_t count = 0;
  if (!blobPull16(p, end, magic) || !blobPull16(p, end, ver)) {
    return false;
  }
  if (magic != kUserBlobMagic || ver != kUserBlobVer) {
    return false;
  }
  if (!blobPull16(p, end, count)) {
    return false;
  }
  if (count > MAX_USERS) {
    return false;
  }
  loaded.clear();
  loaded.reserve(count);
  for (uint16_t i = 0; i < count; i++) {
    UserRecord rec;
    if (!blobPullStr(p, end, rec.mobile, MAX_MOBILE_LEN) || !blobPullStr(p, end, rec.owner_key, MAX_SECRET_LEN) ||
        !blobPullStr(p, end, rec.villa, MAX_VILLA_LEN) || !blobPullStr(p, end, rec.deviceId, MAX_DEVICE_LEN)) {
      return false;
    }
    if (rec.mobile.empty() || rec.owner_key.empty() || rec.villa.empty()) {
      return false;
    }
    if (!loaded.emplace(rec.mobile, std::move(rec)).second) {
      return false;
    }
  }
  return p == end && loaded.size() == count;
}

static bool usersLoadLegacyNvs(Preferences& prefs) {
  uint16_t n = prefs.getUShort("ucnt", 0);
  if (n == 0 || n > MAX_USERS) {
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
      return false;
    }
    if (ms.length() > MAX_MOBILE_LEN || ks.length() > MAX_SECRET_LEN || vs.length() > MAX_VILLA_LEN ||
        ds.length() > MAX_DEVICE_LEN) {
      return false;
    }

    UserRecord rec;
    rec.mobile = toStd(ms);
    rec.owner_key = toStd(ks);
    rec.villa = toStd(vs);
    rec.deviceId = toStd(ds);

    if (!loaded.emplace(rec.mobile, std::move(rec)).second) {
      return false;
    }
  }

  if (loaded.size() != n) {
    return false;
  }
  g_users.swap(loaded);
  return true;
}

static bool usersLoadFromNvsInternal() {
  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, true)) {
    return false;
  }

  const size_t blobLen = prefs.getBytesLength(kNvsUsersBlobKey);
  if (blobLen > 0) {
    std::vector<uint8_t> buf(blobLen);
    if (prefs.getBytes(kNvsUsersBlobKey, buf.data(), blobLen) != blobLen) {
      prefs.end();
      return false;
    }
    prefs.end();
    std::unordered_map<std::string, UserRecord> loaded;
    if (!usersParseBlobIntoMap(buf.data(), buf.size(), loaded)) {
      return false;
    }
    g_users.swap(loaded);
    rebuildVillaIndex();
    return true;
  }

  const bool ok = usersLoadLegacyNvs(prefs);
  prefs.end();
  if (ok) {
    rebuildVillaIndex();
  }
  return ok;
}

bool usersSaveToNvs() {
  std::vector<uint8_t> blob;
  if (!usersSerializeToBlob(blob)) {
    return false;
  }

  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, false)) {
    return false;
  }

  if (!prefs.clear()) {
    prefs.end();
    return false;
  }

  if (prefs.putBytes(kNvsUsersBlobKey, blob.data(), blob.size()) != blob.size()) {
    prefs.end();
    return false;
  }
  prefs.end();

  Preferences verify;
  if (!verify.begin(kNvsNamespace, true)) {
    return false;
  }
  const size_t got = verify.getBytesLength(kNvsUsersBlobKey);
  verify.end();
  return got == blob.size();
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

void usersInit() {
  if (!g_users.empty()) {
    return;
  }

  uint16_t legacyCount = 0;
  bool nvsReadable = false;
  bool hasBlob = false;
  {
    Preferences peek;
    if (peek.begin(kNvsNamespace, true)) {
      nvsReadable = true;
      legacyCount = peek.getUShort("ucnt", 0);
      hasBlob = peek.getBytesLength(kNvsUsersBlobKey) > 0;
      peek.end();
    }
  }

  g_users.clear();
  if (usersLoadFromNvsInternal() && !g_users.empty()) {
    Serial.printf("[users] Loaded %u user(s) from NVS\n", static_cast<unsigned>(g_users.size()));
    return;
  }

  g_users.clear();
  loadDefaultUsersFromHeader();
  rebuildVillaIndex();
  if (g_users.empty()) {
    Serial.println("[users] default_users.h produced no users");
    return;
  }

  if (nvsReadable && (legacyCount > 0 || hasBlob)) {
    Serial.printf(
        "[users] NVS had user data but load failed — %u default user(s) in RAM only; "
        "POST /adduser or bulk import to rewrite NVS\n",
        static_cast<unsigned>(g_users.size()));
    return;
  }

  if (!usersSaveToNvs()) {
    Serial.println("[users] NVS save failed after default seed (RAM-only until fixed)");
  } else {
    Serial.printf("[users] Seeded %u users into NVS\n", static_cast<unsigned>(g_users.size()));
  }
}

/** Same 32-bit djb2 as app/index.html (generateOTP). */
static uint32_t djb2_u32(const std::string& s) {
  uint32_t h = 5381;
  for (size_t i = 0; i < s.length(); i++) {
    h = (((h << 5) + h) + static_cast<uint32_t>(static_cast<unsigned char>(s[i]))) & 0xFFFFFFFFu;
  }
  return h;
}

bool usersValidateOtpForVilla(const String& villaIn, const String& otp6, String& matchedMobile) {
  /* OTP bucket = unix/300 (5 min). ±1 bucket handles RTC skew. */
  if (otp6.length() != 6) {
    return false;
  }
  for (unsigned i = 0; i < 6; i++) {
    if (!isdigit(static_cast<unsigned char>(otp6[i]))) {
      return false;
    }
  }
  const uint32_t want = static_cast<uint32_t>(otp6.toInt());
  const std::string villaStd = toStd(villaIn);
  const uint32_t unixNow = rtc.now().unixtime();
  const uint32_t tw = unixNow / 300;
  const auto vit = g_villaToMobiles.find(villaStd);
  if (vit == g_villaToMobiles.end()) {
    return false;
  }
  for (const std::string& mob : vit->second) {
    const auto uit = g_users.find(mob);
    if (uit == g_users.end()) {
      continue;
    }
    const uint32_t sn = djb2_u32(uit->second.owner_key) % 1000000u;
    for (int delta = -1; delta <= 1; delta++) {
      int64_t t64 = static_cast<int64_t>(tw) + delta;
      if (t64 < 0 || t64 > 0xFFFFFFFFLL) {
        continue;
      }
      const uint32_t twUse = static_cast<uint32_t>(t64);
      const uint32_t otp = static_cast<uint32_t>(
          (static_cast<uint64_t>(twUse) + static_cast<uint64_t>(sn)) % 1000000ULL);
      if (otp == want) {
        matchedMobile = String(mob.c_str());
        return true;
      }
    }
  }
  return false;
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
    rebuildVillaIndex();
    errMsg = "NVS save failed";
    return false;
  }
  rebuildVillaIndex();
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
  rebuildVillaIndex();
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
    rebuildVillaIndex();
    errMsg = "NVS save failed; no changes applied";
    summary = detail;
    return false;
  }

  rebuildVillaIndex();
  summary = "Added " + String(added) + ", updated " + String(updated) + ", errors " + String(errors) + ".\n" + detail;
  errMsg = "";
  return true;
}

size_t usersCount() { return g_users.size(); }
