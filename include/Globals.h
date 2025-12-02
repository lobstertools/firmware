/*
 * =================================================================================
 * Project:   Lobster Lock - Self-Bondage Session Manager
 * File:      Globals.h / Globals.cpp
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
#include <Preferences.h>
#include <OneButton.h>
#include <jled.h>

#include "Config.h"

// =================================================================================
// SECTION: HARDWARE & SYSTEM OBJECTS
// =================================================================================

extern AsyncWebServer server;
extern SystemConfig g_systemConfig;

// --- Button Configuration ---
extern OneButton pcbButton;
extern OneButton extButton;

extern jled::JLed statusLed;

// --- Synchronization ---
// Mutex to guard shared state between Async Web Task and Main Loop Task
extern SemaphoreHandle_t stateMutex;
extern portMUX_TYPE timerMux; // Critical section for tick counter

// =================================================================================
// SECTION: STATE MANAGEMENT
// =================================================================================

extern SessionState currentState;
extern TriggerStrategy currentStrategy;
// Bitmask for enabled channels (loaded from Provisioning NVS)
// Bit 0 = Ch1, Bit 1 = Ch2, etc.
extern uint8_t g_enabledChannelsMask;

// =================================================================================
// SECTION: SESSION TIMERS & DELAYS
// =================================================================================

extern unsigned long lockSecondsRemaining;
extern unsigned long penaltySecondsRemaining;
extern unsigned long testSecondsRemaining;
extern unsigned long triggerTimeoutRemaining;

extern unsigned long penaltySecondsConfig;
extern unsigned long lockSecondsConfig;
extern bool hideTimer;

// Fixed array holding countdowns for each channel (Index 0-3).
extern unsigned long channelDelaysRemaining[MAX_CHANNELS];

// =================================================================================
// SECTION: FEATURE CONFIGURATION
// =================================================================================

extern bool enableStreaks;
extern bool enablePaybackTime;
extern bool enableRewardCode;
extern uint32_t paybackTimeSeconds;

// =================================================================================
// SECTION: STATISTICS & HISTORY
// =================================================================================

extern uint32_t sessionStreakCount;
extern uint32_t completedSessions;
extern uint32_t abortedSessions;
extern uint32_t paybackAccumulated;        // In seconds
extern uint32_t totalLockedSessionSeconds; // Total accumulated lock time

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
extern Preferences provisioningPrefs; // Namespace: "provisioning" (Hardware Config)
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