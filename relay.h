#ifndef GATE_RELAY_H
#define GATE_RELAY_H

/** Configure relay pin (call once from setup). */
void relayInit();

/** Pulse relay to open gate (blocking, same timing as original sketch). */
void triggerRelay();

#endif
