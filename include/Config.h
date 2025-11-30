/*
 * Config.h
 * Shared configuration, constants, enums, and structs.
 */
#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include <string>

// --- Hardware & Global Configuration ---
#define SERIAL_BAUD_RATE 115200

#define DEFAULT_WDT_TIMEOUT 20 // Relaxed for READY state
#define CRITICAL_WDT_TIMEOUT 5 // Tight for LOCKED state
#define MAX_SAFE_TEMP_C 85.0   // Safety Threshold (85°C)

// --- Logging Visuals ---
#define LOG_SEP_MAJOR "=========================================================================="
#define LOG_SEP_MINOR "--------------------------------------------------------------------------"
#define LOG_PREFIX_STATE ">>> STATE CHANGE: "

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

// Default values (used if NVS is empty)
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
    5,      // bootLoopThreshold (Default)
    120000, // stableBootTimeMs (Default 2 Minutes)
    5,      // wifiMaxRetries (Default)
    1800    // armedTimeoutSeconds (30 Minutes)
};

#endif