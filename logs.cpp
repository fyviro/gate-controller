#include "logs.h"
#include "rtc_device.h"

LogEntry logs[MAX_LOGS];
int logIndex = 0;

void addLog(String villa, String mobile, String status) {
  DateTime now = rtc.now();

  String ts = String(now.year()) + "-" + String(now.month()) + "-" + String(now.day()) + " " +
              String(now.hour()) + ":" + String(now.minute()) + ":" + String(now.second());

  logs[logIndex] = {villa, mobile, ts, status};
  logIndex = (logIndex + 1) % MAX_LOGS;
}
