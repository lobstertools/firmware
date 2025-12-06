/*
 * =================================================================================
 * Project:   Lobster Lock - Self-Bondage Session Manager
 * File:      Storage.h / Storage.cpp
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Description:
 * NVS (Non-Volatile Storage) abstraction. Handles saving and loading session
 * state, WiFi credentials, and statistics to ESP32 Preferences to survive
 * power cycles and reboots.
 * =================================================================================
 */
#ifndef STORAGE_H
#define STORAGE_H

#include <Arduino.h>

/**
 * Loads the entire session state and config from NVS.
 * @return true if valid data was found.
 */
bool loadState();

/**
 * Saves the entire session state and config to NVS.
 * @param force If false, might skip saving (reserved for future optimization).
 */
void saveState(bool force);

// --- NEW: WiFi Credentials Management ---
void loadWiFiCredentials();
void saveWiFiCredentials(const char *ssid, const char *pass);

// --- NEW: Provisioning (Hardware) Config Management ---
void loadProvisioningConfig();
void saveProvisioningConfig();

#endif