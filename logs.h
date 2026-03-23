#ifndef GATE_LOGS_H
#define GATE_LOGS_H

#include <Arduino.h>
#include "config.h"
#include "types.h"

extern LogEntry logs[MAX_LOGS];
extern int logIndex;

void addLog(String villa, String mobile, String status);

#endif
