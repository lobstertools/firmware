/*
 * =================================================================================
 * Project:   Lobster Lock - Self-Bondage Session Manager
 * File:      Network.h / Network.cpp
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Description:
 * Network management module. Handles Wi-Fi connection logic, mDNS advertising,
 * and the BLE Provisioning fallback mechanism for setting credentials and 
 * initial configuration.
 * =================================================================================
 */
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