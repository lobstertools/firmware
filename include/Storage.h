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

// =================================================================================
// SECTION: NVS STATE PERSISTENCE
// =================================================================================
bool loadState();
void saveState(bool force = false);

#endif