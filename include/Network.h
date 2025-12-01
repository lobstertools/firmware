#ifndef NETWORK_H
#define NETWORK_H

#include <Arduino.h>
#include <freertos/timers.h>

// =================================================================================
// SECTION: WIFI LOGIC
// =================================================================================
void waitForNetwork();
void connectToWiFi();

// =================================================================================
// SECTION: BLE PROVISIONING
// =================================================================================
void startBLEProvisioning();

// =================================================================================
// SECTION: MDNS
// =================================================================================
void startMDNS();

// =================================================================================
// SECTION: GLOBALS & EXTERNS
// =================================================================================
extern TimerHandle_t wifiReconnectTimer;
extern int g_wifiRetries;
extern volatile bool g_credentialsReceived;

#endif