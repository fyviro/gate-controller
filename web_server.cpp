#include "web_server.h"
#include <Arduino.h>
#include <ctype.h>
#include "auth.h"
#include "config.h"
#include "logs.h"
#include "relay.h"
#include "rtc_device.h"
#include "users.h"

WebServer server(80);

/**
 * Green checkmark for successful gate open (inline SVG).
 * Emoji ticks often ignore CSS color in browsers; this stays visibly green (#22c55e).
 */
static const char GATE_ICON_OK[] =
    "data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' "
    "fill='none' stroke='%2322c55e' stroke-width='2.8' stroke-linecap='round' "
    "stroke-linejoin='round'><path d='M20 6L9 17l-5-5'/></svg>";

/** Escape text / attributes so UTF-8 emoji and user data don't break HTML. */
static String htmlEscape(const String& s) {
  String o;
  o.reserve(s.length() + 16);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '&':
        o += "&amp;";
        break;
      case '<':
        o += "&lt;";
        break;
      case '>':
        o += "&gt;";
        break;
      case '"':
        o += "&quot;";
        break;
      default:
        o += c;
        break;
    }
  }
  return o;
}

/** Use <img src="..."> for URLs or data:image/...; base64 must be passed as full data URL string. */
static bool iconLooksLikeImageSrc(const String& icon) {
  if (icon.length() == 0) {
    return false;
  }
  if (icon.startsWith("http://") || icon.startsWith("https://")) {
    return true;
  }
  if (icon.startsWith("data:image")) {
    return true;
  }
  if (icon[0] == '/') {
    return true;
  }
  return false;
}

/**
 * Same layout as your R"rawliteral(...)" template; dynamic parts spliced in.
 * title/villa/message/color escaped for HTML; image URLs use <img>.
 */
static String buildResponse(const String& title, const String& color, const String& icon, const String& villa,
                            const String& message, bool isSuccess) {
  String bg = isSuccess ? "#f0f8f0" : "#fff5f5";

  String iconInner;
  if (iconLooksLikeImageSrc(icon)) {
    iconInner = "<img class=\"icon-img\" alt=\"\" src=\"";
    iconInner += htmlEscape(icon);
    iconInner += "\" style=\"max-width:80px;max-height:80px;object-fit:contain;vertical-align:middle;\">";
  } else {
    iconInner = htmlEscape(icon);
  }

  const String titleHtml = htmlEscape(title);
  const String villaHtml = htmlEscape(villa);
  const String msgHtml = htmlEscape(message);
  const String colorHtml = htmlEscape(color);

  return String(R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
body {
  display:flex; justify-content:center; align-items:center;
  height:100vh; font-family:Arial; text-align:center;
  background:)rawliteral") +
         bg +
         R"rawliteral(;
}
.card {
  padding:30px; border-radius:12px;
  background:white; box-shadow:0 4px 10px rgba(0,0,0,0.1);
}
.icon {
  font-size:80px; color:)rawliteral" +
         colorHtml +
         R"rawliteral(;
}
</style>
</head>
<body>

<div class="card">
  <div class="icon">)rawliteral" +
         iconInner +
         R"rawliteral(</div>
  <h2 style="color:)rawliteral" +
         colorHtml +
         R"rawliteral(;">)rawliteral" +
         titleHtml +
         R"rawliteral(</h2>
  <h3>Villa: )rawliteral" +
         villaHtml +
         R"rawliteral(</h3>
  <p>)rawliteral" +
         msgHtml +
         R"rawliteral(</p>
</div>
<script>
setTimeout(function () { window.close(); }, 3000);
</script>

</body>
</html>
)rawliteral";
}

/** Prefer long name, fall back to short query key (e.g. villa / v). */
static String argPrefer(const char* primary, const char* aliasName) {
  if (server.hasArg(primary) && server.arg(primary).length() > 0) {
    return server.arg(primary);
  }
  return server.arg(aliasName);
}

/** Decimal unix-seconds only (matches typical QR generators). */
static bool isPlausibleUnixTs(const String& ts) {
  if (ts.length() < 9 || ts.length() > 12) {
    return false;
  }
  for (unsigned i = 0; i < ts.length(); i++) {
    if (!isdigit(static_cast<unsigned char>(ts[i]))) {
      return false;
    }
  }
  return true;
}

static void handleOpen() {
  String mobile = server.arg("m");
  String ts = server.arg("t");
  String sig = server.arg("s");
  String deviceId = server.arg("d");
  mobile.trim();
  ts.trim();
  sig.trim();
  deviceId.trim();

  String key, villa, udeviceId;

  // Villa comes only from registration (lookup by mobile), not from the QR URL.
  if (mobile == "" || ts == "" || sig == "") {
    server.send(403, "text/html",
                  buildResponse("Access Denied", "red", "❌", "-", "Missing m, t, or s", false));
    return;
  }

  if (!getUser(mobile, key, villa, udeviceId)) {
    server.send(403, "text/html", buildResponse("Access Denied", "red", "❌", "-", "Unknown mobile", false));
    addLog("-", mobile, "Denied");
    return;
  }

  if (!isPlausibleUnixTs(ts)) {
    server.send(403, "text/html", buildResponse("Access Denied", "red", "❌", villa, "Invalid timestamp", false));
    addLog(villa, mobile, "Denied");
    return;
  }

  String payload = mobile + ts;
  String expected = hmacSHA256(key, payload);

  // Firmware uses uppercase hex; JS often emits lowercase — accept both.
  if (!expected.equalsIgnoreCase(sig)) {
    server.send(403, "text/html", buildResponse("Access Denied", "red", "❌", villa, "Invalid signature", false));
    addLog(villa, mobile, "Denied");
    return;
  }
 
  long tokenTime = ts.toInt();
  // If RTC is not initialized (year < 2022), sync it
  DateTime now = rtc.now();
  if (now.year() < 2022) {
    Serial.println("RTC not set. Syncing from QR...");

    rtc.adjust(DateTime(tokenTime - 19800)); // convert IST → UTC
  }
  long currentTime = rtc.now().unixtime() + 19800;
  if (labs(currentTime - tokenTime) > QR_VALID_WINDOW_SEC) {
    server.send(403, "text/html",
      buildResponse("Access Denied", "red", "❌", villa, "QR Expired", false)
    );
    addLog(villa, mobile, "Denied");
    return;
  }

  String bindErr;
  if (!usersEnsureDeviceBinding(mobile, deviceId, bindErr)) {
    server.send(403, "text/html", buildResponse("Access Denied", "red", "❌", villa, bindErr, false));
    addLog(villa, mobile, "Denied");
    return;
  }

  triggerRelay();

  server.send(200, "text/html", buildResponse("Access Granted", "green", GATE_ICON_OK, villa, "", true));
  addLog(villa, mobile, "GRANTED");
}

static void handleLogs() {
  String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{font-family:Arial;} table{width:100%;border-collapse:collapse;} td,th{border:1px solid #ccc;padding:8px;text-align:left;} th{background:#eee;}</style>";
  html += "</head><body>";

  html += "<h2>Gate Logs</h2>";
  html += "<table>";
  html += "<tr><th>Time</th><th>Villa</th><th>Mobile</th><th>Status</th></tr>";

  for (int i = 0; i < MAX_LOGS; i++) {
    if (logs[i].mobile != "") {
      html += "<tr>";
      html += "<td>" + logs[i].time + "</td>";
      html += "<td>" + logs[i].villa + "</td>";
      html += "<td>" + logs[i].mobile + "</td>";
      html += "<td>" + logs[i].status + "</td>";
      html += "</tr>";
    }
  }

  html += "</table></body></html>";

  server.send(200, "text/html", html);
}

/** GET /adduser — list users as JSON (no secrets). Query: a= admin if configured. */
static void handleAddUserGet() {
  String admin = server.arg("a");
  String err;
  if (!usersRequireAdmin(admin, err)) {
    server.send(403, "application/json", "{\"error\":\"forbidden\"}");
    return;
  }
  String json;
  usersBuildJsonList(json);
  server.send(200, "application/json", json);
}

/** POST /adduser — create user. Form/query: m, v/villa, k/secret, a= */
static void handleAddUserPost() {
  String villa = argPrefer("villa", "v");
  String mobile = argPrefer("mobile", "m");
  String secret = argPrefer("secret", "k");
  String deviceId = server.arg("d");
  String admin = server.arg("a");

  String err;
  if (!usersAdd(mobile, secret, villa, deviceId, admin, err)) {
    server.send(400, "text/html", buildResponse("Add user failed", "red", "❌", villa, err, false));
    return;
  }

  server.send(200, "text/html", buildResponse("User added", "green", GATE_ICON_OK, villa, "Mobile: " + mobile, true));
}

/**
 * PUT /adduser — update existing user.
 * m/mobile required. k/secret and v/villa optional (omit to keep current).
 * reset_device=1 (or true/yes) clears device binding for next /open.
 */
static void handleAddUserPut() {
  String villa = argPrefer("villa", "v");
  String mobile = argPrefer("mobile", "m");
  String secret = argPrefer("secret", "k");
  String admin = server.arg("a");
  String reset = server.arg("reset_device");
  const bool resetDev =
      (reset == "1" || reset == "true" || reset.equalsIgnoreCase("yes"));

  String err;
  if (!usersUpdate(mobile, secret, villa, resetDev, admin, err)) {
    server.send(400, "text/html",
                  buildResponse("Update failed", "red", "❌", villa.length() ? villa : "-", err, false));
    return;
  }

  server.send(200, "text/html", buildResponse("User updated", "green", GATE_ICON_OK, mobile, "", true));
}

/** POST /adduser/bulk — form: a=, csv= (mobile,secret,villa[,deviceId] per line). Max MAX_CSV_IMPORT_BYTES. */
static void handleAddUserBulk() {
  if (server.method() != HTTP_POST) {
    server.send(405, "text/plain", "Use POST");
    return;
  }
  String admin = server.arg("a");
  String csv = server.arg("csv");
  String summary;
  String err;
  if (!usersImportCsv(csv, admin, summary, err)) {
    server.send(400, "text/plain", err + "\n" + summary);
    return;
  }
  server.send(200, "text/plain", summary);
}

static void handleAddUserRequest() {
  switch (server.method()) {
    case HTTP_GET:
      handleAddUserGet();
      break;
    case HTTP_POST:
      handleAddUserPost();
      break;
    case HTTP_PUT:
      handleAddUserPut();
      break;
    default:
      server.send(405, "text/plain", "Method Not Allowed");
      break;
  }
}

void registerWebRoutes() {
  server.on("/open", handleOpen);
  server.on("/logs", handleLogs);
  server.on("/adduser", handleAddUserRequest);
  server.on("/adduer", handleAddUserRequest);
  server.on("/adduser/bulk", handleAddUserBulk);
  server.on("/adduer/bulk", handleAddUserBulk);
}

void webServerBegin() {
  server.begin();
}
