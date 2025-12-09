/*
 * =================================================================================
 * Project:   Lobster Lock - Self-Bondage Session Manager
 * File:      Config.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Description:
 * Central configuration file. Defines hardware pin mappings, system constants,
 * default settings, safety limits (thermal/timing), and compiler flags.
 * =================================================================================
 */
#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include <string>

// --- Device Name String ---
#define DEVICE_NAME "LobsterLock-diymore-MOS"

// --- Enums & Structs ---

// These define how the physical device and firmware loops behave.
struct SystemDefaults {
  uint32_t longPressDuration;       // ms or seconds (depending on implementation)
  uint32_t extButtonSignalDuration; // Debounce/Signal check duration
  uint32_t testModeDuration;        // Duration of hardware test
  uint32_t failsafeMaxLock;         // Absolute safety override
  uint32_t keepAliveInterval;       // Watchdog ping interval
  uint32_t keepAliveMaxStrikes;     // Watchdog tolerance
  uint32_t bootLoopThreshold;       // Crash detection
  uint32_t stableBootTime;          // Crash detection
  uint32_t wifiMaxRetries;          // Network attempt limit
  uint32_t armedTimeoutSeconds;     // Auto-disarm timeout
};


#ifdef DEBUG_MODE
// ============================================================================
// DEBUG / DEVELOPMENT DEFAULTS
// ============================================================================
static const SystemDefaults DEFAULT_SYSTEM_DEFS = {
    5,     // longPressDuration
    10,    // extButtonSignalDuration
    240,   // testModeDuration
    300,   // failsafeMaxLock (Death Grip)
    10000, // keepAliveInterval
    4,     // keepAliveMaxStrikes
    5,     // bootLoopThreshold
    30000, // stableBootTime
    3,     // wifiMaxRetries
    60     // armedTimeoutSeconds
};

static const SessionLimits DEFAULT_SESSION_LIMITS = {
    10,   // minLockDuration
    240,  // maxLockDuration
    10,   // minRewardPenaltyDuration
    3600, // maxRewardPenaltyDuration
    10,   // minPaybackTime
    600   // maxPaybackTime
};

#else
// ============================================================================
// PRODUCTION / RELEASE DEFAULTS
// ============================================================================
static const SystemDefaults DEFAULT_SYSTEM_DEFS = {
    5,      // longPressDuration
    10,     // extButtonSignalDuration
    240,    // testModeDuration
    14400,  // failsafeMaxLock
    10000,  // keepAliveInterval
    4,      // keepAliveMaxStrikes
    5,      // bootLoopThreshold
    120000, // stableBootTime
    5,      // wifiMaxRetries
    1800    // armedTimeoutSeconds
};

static const SessionLimits DEFAULT_SESSION_LIMITS = {
    900,   // minLockDuration (15min)
    10800, // maxLockDuration (3hr)
    900,   // minRewardPenaltyDuration (15min)
    10800, // maxRewardPenaltyDuration (3hr)
    300,   // minPaybackTime (5min)
    2700   // maxPaybackTime (45min)
};
#endif

#endif