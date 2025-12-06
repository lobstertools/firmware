/*
 * =================================================================================
 * Project:   Lobster Lock - Self-Bondage Session Manager
 * File:      Globals.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Description:
 * Definitions of shared global state variables, synchronization primitives (Mutex),
 * hardware object instances (Button, LED), and persistent configuration flags.
 * =================================================================================
 */
#ifndef GLOBALS_H
#define GLOBALS_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <OneButton.h>
#include <Preferences.h>
#include <jled.h>

#include "Config.h"

// =================================================================================
// SECTION: HARDWARE & SYSTEM OBJECTS
// =================================================================================
extern SystemDefaults g_systemDefaults;
extern SessionLimits g_sessionLimits;

// --- Button Configuration ---
extern OneButton pcbButton;
extern OneButton extButton;

extern jled::JLed statusLed;

// --- Synchronization ---
extern SemaphoreHandle_t stateMutex;
extern portMUX_TYPE timerMux;

// --- Channels ---
extern uint8_t g_enabledChannelsMask; // Bit 0 = Ch1, Bit 1 = Ch2, etc.

// --- Pedal Connection ---
extern unsigned long extButtonSignalStartTime;

// =================================================================================
// SECTION: STATE MANAGEMENT
// =================================================================================
extern AsyncWebServer server;
extern DeviceState g_currentState;
extern ActiveSessionConfig g_activeSessionConfig;

// =================================================================================
// SECTION: SESSION TIMERS & DELAYS
// =================================================================================
extern SessionTimers g_sessionTimers;

// =================================================================================
// SECTION: FEATURE CONFIGURATION
// =================================================================================
extern DeterrentConfig g_deterrentConfig;

// =================================================================================
// SECTION: STATISTICS & HISTORY
// =================================================================================
extern SessionStats g_sessionStats;

// Global array to hold reward history.
extern Reward rewardHistory[REWARD_HISTORY_SIZE];

// =================================================================================
// SECTION: WATCHDOGS & INPUT TRACKING
// =================================================================================
extern volatile unsigned long g_buttonPressStartTime;
extern unsigned long g_lastKeepAliveTime; // For watchdog. 0 = disarmed.
extern int g_currentKeepAliveStrikes;     // Counter for missed calls

// =================================================================================
// SECTION: STORAGE (PREFERENCES)
// =================================================================================
extern Preferences wifiPreferences;   // Namespace: "wifi-creds"
extern Preferences provisioningPrefs; // Namespace: "provisioning" (Feature and Hardware Config)
extern Preferences sessionState;      // Namespace: "session" (Dynamic State)
extern Preferences bootPrefs;         // Namespace: "boot" (Crash tracking)

// =================================================================================
// SECTION: NETWORK STATE
// =================================================================================
extern char g_wifiSSID[33];
extern char g_wifiPass[65];
extern bool g_wifiCredentialsExist;
extern volatile bool g_triggerProvisioning;

#endif