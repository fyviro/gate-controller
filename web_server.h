#ifndef GATE_WEB_SERVER_H
#define GATE_WEB_SERVER_H

#include <WebServer.h>

extern WebServer server;

void registerWebRoutes();
void webServerBegin();

extern String usedSigs[20];   // store last 20 used QR
extern int sigIndex;

extern long baseTS;
extern long baseMillis;


#endif
