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
enum DeviceState : uint8_t { VALIDATING, READY, ARMED, LOCKED, ABORTED, COMPLETED, TESTING };

// Trigger Strategy (How we move from ARMED -> LOCKED)
enum TriggerStrategy : uint8_t { STRAT_AUTO_COUNTDOWN, STRAT_BUTTON_TRIGGER };

// Duration Type Enum (Intent)
enum DurationType : uint8_t { DUR_FIXED, DUR_RANDOM, DUR_RANGE };

// Struct for storing reward code history.
struct Reward {
  char code[REWARD_CODE_LENGTH + 1];
  char checksum[REWARD_CHECKSUM_LENGTH + 1];
};

// Struct to hold the Active Session Configuration (Intent & Logic)
struct ActiveSessionConfig {
  DurationType durationType;
  uint32_t durationMin;
  uint32_t durationMax;

  TriggerStrategy triggerStrategy;
  uint32_t channelDelays[MAX_CHANNELS];
  bool hideTimer;
};

// Struct to hold the Remaining Session Timers
struct SessionTimers {
  uint32_t lockDuration;
  uint32_t penaltyDuration;
  uint32_t lockRemaining;
  uint32_t penaltyRemaining;
  uint32_t testRemaining;
  uint32_t triggerTimeout;

  uint32_t channelDelays[MAX_CHANNELS];
};

// Struct to hold the accumulated session statistics
struct SessionStats {
  uint32_t streaks;
  uint32_t completed;
  uint32_t aborted;
  uint32_t paybackAccumulated;
  uint32_t totalLockedTime;
};

// Struct to hold the Deterrent Configuration during provisioning
struct DeterrentConfig {
  bool enableStreaks;
  bool enableRewardCode;
  uint32_t rewardPenalty;
  bool enablePaybackTime;
  uint32_t paybackTime;
};

// --- SYSTEM PREFERENCES  ---

// 1. Hardware & System Behavior Defaults
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

// 2. Session Constraints (Limits)
// These define the allowable ranges for a user to configure a session.
struct SessionLimits {
  uint32_t minLockDuration;
  uint32_t maxLockDuration;
  uint32_t minRewardPenaltyDuration;
  uint32_t maxRewardPenaltyDuration;
  uint32_t minPaybackTime;
  uint32_t maxPaybackTime;
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
    3600, // maxLockDuration
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