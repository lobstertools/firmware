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

// --- Hardware & Global Configuration ---
#define SERIAL_BAUD_RATE 115200

#define DEFAULT_WDT_TIMEOUT 20 // Relaxed for READY state
#define CRITICAL_WDT_TIMEOUT 5 // Tight for LOCKED state
#define MAX_SAFE_TEMP_C 85.0   // Safety Threshold (85°C)

// --- System Identification & Validation ---
#define MAGIC_VALUE 0x3CADD1FF

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
static const int HARDWARE_PINS[4] = {16, 17, 26, 27};
#define MAX_CHANNELS 4

// --- Session Constants & NVS ---
#define REWARD_HISTORY_SIZE 10
#define REWARD_CODE_LENGTH 32
#define REWARD_CHECKSUM_LENGTH 16

// --- Enums & Structs ---

// Main state machine enum.
enum SessionState : uint8_t { READY, ARMED, LOCKED, ABORTED, COMPLETED, TESTING };

// Trigger Strategy (How we move from ARMED -> LOCKED)
enum TriggerStrategy : uint8_t { STRAT_AUTO_COUNTDOWN, STRAT_BUTTON_TRIGGER };

// Struct for storing reward code history.
struct Reward {
  char code[REWARD_CODE_LENGTH + 1];
  char checksum[REWARD_CHECKSUM_LENGTH + 1];
};

// --- SYTEM PREFERENCES ---
struct SystemConfig {
  uint32_t longPressSeconds;
  uint32_t minLockSeconds;
  uint32_t maxLockSeconds;
  uint32_t minPenaltySeconds;
  uint32_t maxPenaltySeconds;
  uint32_t minPaybackTimeSeconds;
  uint32_t maxPaybackTimeSeconds;
  uint32_t testModeDurationSeconds;
  uint32_t failsafeMaxLockSeconds;
  uint32_t keepAliveIntervalMs;
  uint32_t keepAliveMaxStrikes;
  uint32_t bootLoopThreshold;
  uint32_t stableBootTimeMs;
  uint32_t wifiMaxRetries;
  uint32_t armedTimeoutSeconds;
};

#ifdef DEBUG_MODE
// ============================================================================
// DEBUG / DEVELOPMENT DEFAULTS
// Optimized for rapid iteration and testing short lifecycles.
// ============================================================================
static const SystemConfig DEFAULT_SETTINGS = {
    1,     // longPressSeconds (1s for quick triggering)
    10,    // minLockSeconds (10s minimum for quick lock cycles)
    3600,  // maxLockSeconds (1 hour - allows testing longer ranges if needed)
    10,    // minPenaltySeconds (10s penalty)
    3600,  // maxPenaltySeconds
    10,    // minPaybackTimeSeconds (10s minimum debt)
    600,   // maxPaybackTimeSeconds (10 min cap)
    240,   // testModeDurationSeconds (4m hardware test)
    600,   // failsafeMaxLockSeconds (10 min failsafe for safety during debug)
    10000, // keepAliveIntervalMs
    4,     // keepAliveMaxStrikes
    5,     // bootLoopThreshold
    30000, // stableBootTimeMs (30s to consider boot stable)
    3,     // wifiMaxRetries (Fail faster)
    300    // armedTimeoutSeconds (5 min idle timeout)
};

#else
// ============================================================================
// PRODUCTION / RELEASE DEFAULTS
// ============================================================================
static const SystemConfig DEFAULT_SETTINGS = {
    3,      // longPressSeconds
    900,    // minLockSeconds (15 min)
    10800,  // maxLockSeconds (180 min)
    900,    // minPenaltySeconds (15 min)
    10800,  // maxPenaltySeconds (180 min)
    300,    // minPaybackTimeSeconds (5 min)
    2700,   // maxPaybackTimeSeconds (45 min)
    240,    // testModeDurationSeconds
    14400,  // failsafeMaxLockSeconds (4 hours)
    10000,  // keepAliveIntervalMs
    4,      // keepAliveMaxStrikes
    5,      // bootLoopThreshold
    120000, // stableBootTimeMs (2 Minutes)
    5,      // wifiMaxRetries
    1800    // armedTimeoutSeconds (30 Minutes)
};

#endif
#endif