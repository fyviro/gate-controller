#ifndef GATE_AUTH_H
#define GATE_AUTH_H

#include <Arduino.h>

String hmacSHA256(String key, String data);

#endif
