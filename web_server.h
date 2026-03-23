#ifndef GATE_WEB_SERVER_H
#define GATE_WEB_SERVER_H

#include <WebServer.h>

extern WebServer server;

void registerWebRoutes();
void webServerBegin();

#endif
