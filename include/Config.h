/*
 * =================================================================================
 * Project:   Lobster Lock - Self-Bondage Session Manager
 * File:      include/Config.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Description:
 * Central configuration file. Defines hardware pin mappings, system constants,
 * default settings, safety limits (thermal/timing), and compiler flags.
 * =================================================================================
 */
#pragma once
#include "Types.h"

// --- Device Name String ---
#define DEVICE_NAME "LobsterLock-diymore-MOS"

// =================================================================================
// SECTION: HARDWARE & SYSTEM OBJECTS
// =================================================================================

#define SERIAL_BAUD_RATE 115200
#define DEFAULT_WDT_TIMEOUT 20 // Relaxed for READY state
#define CRITICAL_WDT_TIMEOUT 5 // Tight for LOCKED state
#define MAX_SAFE_TEMP_C 85.0   // Safety Threshold (85°C)

// System Identification
#define MAGIC_VALUE 0x3CBDD200

// --- Pin Definitions ---
#define PCB_BUTTON_PIN 0 // Standard ESP32 Boot Button

#ifdef DEBUG_MODE
// Development (Diymore Debug)
#define STATUS_LED_PIN 23
// EXT_BUTTON_PIN is purposefully undefined in Debug mode
#else
// Production (Diymore Release)
#define STATUS_LED_PIN 21
#define EXT_BUTTON_PIN 15 // External NC Switch
#endif

// --- Channel-Specific Configuration ---

static const int HARDWARE_PINS[MAX_CHANNELS] = {16, 17, 26, 27};

// --- Enums & Structs ---

#ifdef DEBUG_MODE
// ============================================================================
// DEBUG / DEVELOPMENT DEFAULTS
// ============================================================================
static const SystemDefaults DEFAULT_SYSTEM_DEFS = {
    5,     // longPressDuration
    10,    // extButtonSignalDuration
    240,   // testModeDuration
    10000, // keepAliveInterval
    4,     // keepAliveMaxStrikes
    5,     // bootLoopThreshold
    30000, // stableBootTime
    3,     // wifiMaxRetries
    60     // armedTimeoutSeconds
};

#else
// ============================================================================
// PRODUCTION / RELEASE DEFAULTS
// ============================================================================
static const SystemDefaults DEFAULT_SYSTEM_DEFS = {
    5,      // longPressDuration
    10,     // extButtonSignalDuration
    240,    // testModeDuration
    10000,  // keepAliveInterval
    4,      // keepAliveMaxStrikes
    5,      // bootLoopThreshold
    120000, // stableBootTime
    5,      // wifiMaxRetries
    1800    // armedTimeoutSeconds
};
#endif
