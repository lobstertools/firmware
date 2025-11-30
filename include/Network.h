#ifndef NETWORK_H
#define NETWORK_H

#include <Arduino.h>
#include <freertos/timers.h>

void waitForNetwork();
void connectToWiFi();
void startBLEProvisioning();
void startMDNS();

extern TimerHandle_t wifiReconnectTimer;
extern int g_wifiRetries;
extern volatile bool g_credentialsReceived;

#endif