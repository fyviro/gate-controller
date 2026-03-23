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

static String buildResponse(String title, String color, String icon, String villa, String msg) {
  return R"rawliteral(
  <html><head><meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
  body{display:flex;justify-content:center;align-items:center;height:100vh;font-family:Arial;text-align:center;background:#f5f5f5;}
  .card{padding:30px;border-radius:12px;background:white;box-shadow:0 4px 10px rgba(0,0,0,0.1);}
  .icon{font-size:60px;color:)rawliteral" +
         color + R"rawliteral(;}
  </style></head><body>
  <div class="card">
  <div class="icon">)rawliteral" +
         icon + R"rawliteral(</div>
  <h2 style="color:)rawliteral" +
         color + R"rawliteral(;">)rawliteral" +
         title + R"rawliteral(</h2>
  <h3>Villa: )rawliteral" +
         villa + R"rawliteral(</h3>
  <p>)rawliteral" +
         msg + R"rawliteral(</p>
  </div></body></html>)rawliteral";
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
