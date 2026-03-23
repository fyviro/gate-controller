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
 * Build HTML card. `icon` is either:
 * - Short label / emoji (shown as styled text), or
 * - Image URL or data URL (e.g. data:image/png;base64,....) for <img src="...">.
 * Do not pass raw binary image bytes as String — only a proper URL or data: URI.
 */
static String buildResponse(const String& title, const String& color, const String& icon, const String& villa,
                            const String& msg) {
  String iconBlock;
  if (iconLooksLikeImageSrc(icon)) {
    iconBlock = "<img class=\"icon-img\" alt=\"\" src=\"";
    iconBlock += htmlEscape(icon);
    iconBlock += "\">";
  } else {
    iconBlock = "<span class=\"icon-emoji\" style=\"color:";
    iconBlock += htmlEscape(color);
    iconBlock += ";\">";
    iconBlock += htmlEscape(icon);
    iconBlock += "</span>";
  }

  String html;
  html.reserve(400 + title.length() + villa.length() + msg.length() + icon.length());
  html += "<!DOCTYPE html><html><head><meta charset=\"UTF-8\">";
  html += "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
  html += "<style>";
  html += "body{display:flex;justify-content:center;align-items:center;min-height:100vh;margin:0;";
  html += "font-family:Arial,sans-serif;text-align:center;background:#f5f5f5;}";
  html += ".card{padding:30px;border-radius:12px;background:#fff;box-shadow:0 4px 10px rgba(0,0,0,0.1);";
  html += "max-width:92%;}";
  html += ".icon-emoji{font-size:60px;line-height:1.2;display:block;margin-bottom:8px;}";
  html += ".icon-img{display:block;margin:8px auto;max-width:96px;max-height:96px;object-fit:contain;}";
  html += "</style></head><body><div class=\"card\">";
  html += iconBlock;
  html += "<h2 style=\"color:";
  html += htmlEscape(color);
  html += ";\">";
  html += htmlEscape(title);
  html += "</h2><h3>Villa: ";
  html += htmlEscape(villa);
  html += "</h3><p>";
  html += htmlEscape(msg);
  html += "</p></div></body></html>";
  return html;
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
    server.send(403, "text/html", buildResponse("Access Denied", "red", "❌", "-", "Missing m, t, or s"));
    return;
  }

  if (!getUser(mobile, key, villa, udeviceId)) {
    server.send(403, "text/html", buildResponse("Access Denied", "red", "❌", "-", "Unknown mobile"));
    addLog("-", mobile, "Denied");
    return;
  }

  if (!isPlausibleUnixTs(ts)) {
    server.send(403, "text/html", buildResponse("Access Denied", "red", "❌", villa, "Invalid timestamp"));
    addLog(villa, mobile, "Denied");
    return;
  }

  String payload = mobile + ts;
  String expected = hmacSHA256(key, payload);

  // Firmware uses uppercase hex; JS often emits lowercase — accept both.
  if (!expected.equalsIgnoreCase(sig)) {
    server.send(403, "text/html", buildResponse("Access Denied", "red", "❌", villa, "Invalid signature"));
    addLog(villa, mobile, "Denied");
    return;
  }

  long tokenTime = ts.toInt();
  long currentTime = rtc.now().unixtime();

  if (labs(currentTime - tokenTime) > QR_VALID_WINDOW_SEC) {
    server.send(403, "text/html", buildResponse("Access Denied", "red", "❌", villa, "QR Expired"));
    addLog(villa, mobile, "Denied");
    return;
  }

  String bindErr;
  if (!usersEnsureDeviceBinding(mobile, deviceId, bindErr)) {
    server.send(403, "text/html", buildResponse("Access Denied", "red", "❌", villa, bindErr));
    addLog(villa, mobile, "Denied");
    return;
  }

  triggerRelay();

  server.send(200, "text/html", buildResponse("Access Granted", "green", "✔️", villa, ""));
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

static void handleAddUser() {
  String villa = argPrefer("villa", "v");
  String mobile = argPrefer("mobile", "m");
  String secret = argPrefer("secret", "k");
  String deviceId = server.arg("d");
  String admin = server.arg("a");

  String err;
  if (!usersAdd(mobile, secret, villa, deviceId, admin, err)) {
    server.send(400, "text/html", buildResponse("Add user failed", "red", "❌", villa, err));
    return;
  }

  server.send(200, "text/html", buildResponse("User added", "green", "✔️", villa, "Mobile: " + mobile));
}

void registerWebRoutes() {
  server.on("/open", handleOpen);
  server.on("/logs", handleLogs);
  server.on("/adduser", handleAddUser);
  server.on("/adduer", handleAddUser);  // common typo alias
}

void webServerBegin() {
  server.begin();
}
