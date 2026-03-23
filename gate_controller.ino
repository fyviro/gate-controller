#include <WiFi.h>
#include <WebServer.h>
#include "mbedtls/md.h"
#include <Wire.h>
#include "RTClib.h"

#define RELAY_PIN 18

const char* ssid = "Gate-Access";
const char* password = "12345678";

WebServer server(80);
RTC_DS3231 rtc;

#define MAX_LOGS 100

struct LogEntry {
  String villa;
  String mobile;
  String time;
  String status;
};

LogEntry logs[MAX_LOGS];
int logIndex = 0;

// 🔐 USERS
struct User {
  const char* mobile;
  const char* owner_key;
  const char* villa;
  const char* deviceId;
};

User users[] = {
  {"9908195316", "a1b2c3d4e5f6", "74", ""}
};

const int USERS_COUNT = sizeof(users) / sizeof(users[0]);

bool gateBusy = false;

// 🔐 HMAC
String hmacSHA256(String key, String data) {
  byte hmacResult[32];

  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
  mbedtls_md_hmac_starts(&ctx, (const unsigned char*)key.c_str(), key.length());
  mbedtls_md_hmac_update(&ctx, (const unsigned char*)data.c_str(), data.length());
  mbedtls_md_hmac_finish(&ctx, hmacResult);
  mbedtls_md_free(&ctx);

  String result = "";
  for (int i = 0; i < 32; i++) {
    if (hmacResult[i] < 16) result += "0";
    result += String(hmacResult[i], HEX);
  }
  return result;
}

// 🔍 Get User
bool getUser(String mobile, String &key, String &villa, String &deviceId) {
  for (int i = 0; i < USERS_COUNT; i++) {
    if (mobile == users[i].mobile) {
      key = users[i].owner_key;
      villa = users[i].villa;
      deviceId = users[i].deviceId;
      return true;
    }
  }
  return false;
}

// 🚧 Relay
void triggerRelay() {
  if (gateBusy) return;

  gateBusy = true;

  digitalWrite(RELAY_PIN, LOW);
  delay(500);
  digitalWrite(RELAY_PIN, HIGH);

  delay(3000);
  gateBusy = false;
}

String buildResponse(String title, String color, String icon, String villa, String msg) {
  return R"rawliteral(
  <html><head><meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
  body{display:flex;justify-content:center;align-items:center;height:100vh;font-family:Arial;text-align:center;background:#f5f5f5;}
  .card{padding:30px;border-radius:12px;background:white;box-shadow:0 4px 10px rgba(0,0,0,0.1);}
  .icon{font-size:60px;color:)rawliteral" + color + R"rawliteral(;}
  </style></head><body>
  <div class="card">
  <div class="icon">)rawliteral" + icon + R"rawliteral(</div>
  <h2 style="color:)rawliteral" + color + R"rawliteral(;">)rawliteral" + title + R"rawliteral(</h2>
  <h3>Villa: )rawliteral" + villa + R"rawliteral(</h3>
  <p>)rawliteral" + msg + R"rawliteral(</p>
  </div></body></html>)rawliteral";
}

// 🧾 Logs
void addLog(String villa, String mobile, String status) {

  DateTime now = rtc.now();

  String ts = String(now.year()) + "-" +
              String(now.month()) + "-" +
              String(now.day()) + " " +
              String(now.hour()) + ":" +
              String(now.minute()) + ":" +
              String(now.second());

  logs[logIndex] = { villa, mobile, ts, status };
  logIndex = (logIndex + 1) % MAX_LOGS;
}

// 🚪 Handle Open
void handleOpen() {

  String mobile = server.arg("m");
  String ts = server.arg("t");
  String sig = server.arg("s");
  String deviceId = server.arg("d");

  String key, villa, udeviceId;


  if (mobile == "" || ts == "" || sig == "") {
    server.send(403, "text/html", buildResponse("Access Denied", "red", "❌", villa, "Missing details"));
    return;
  }

  if (!getUser(mobile, key, villa, udeviceId)) {
    server.send(403, "text/html", buildResponse("Access Denied", "red", "❌", villa, "Villa details incorrect"));
    addLog(mobile, mobile, "Denied");
    return;
  }


  // 🔐 HMAC
  String payload = mobile + ts;
  String expected = hmacSHA256(key, payload);

  if (expected != sig) {
    server.send(403, "text/html", buildResponse("Access Denied", "red", "❌", villa, "Invalid QR"));
    addLog(villa, mobile, "Denied");
    return;
  }

  // ⏱ Expiry (5 min)
  long tokenTime = ts.toInt();
  long currentTime = rtc.now().unixtime();

  if (labs(currentTime - tokenTime) > 300) {
    server.send(403, "text/html", buildResponse("Access Denied", "red", "❌", villa, "QR Expired"));
    addLog(villa, mobile, "Denied");
    return;
  }

  // 🔒 Device binding (validation only)
  if (deviceId != "") {
    if (udeviceId != "" && udeviceId != deviceId) {
      
      server.send(403, "text/html", buildResponse("Access Denied", "red", "❌", villa, "Invalid QR"));
      addLog(villa, mobile, "Denied");
      return;
    }
  }

  // 🚧 Open gate
  triggerRelay();

  server.send(200, "text/html", buildResponse("Access Granted", "green", "✔️", villa, ""));
  addLog(villa, mobile, "GRANTED");
}

// 📊 Logs Page
void handleLogs() {

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

// 🚀 Setup
void setup() {

  Serial.begin(115200);

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);

  WiFi.softAP(ssid, password);

  if (!rtc.begin()) {
    Serial.println("RTC not found");
    while (1);
  }

  Serial.println("RTC OK");
  Serial.println(WiFi.softAPIP());

  server.on("/open", handleOpen);
  server.on("/logs", handleLogs);
  server.begin();
}

// 🔁 Loop
void loop() {
  server.handleClient();
}