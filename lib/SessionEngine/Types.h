/*
 * =================================================================================
 * Project:   Lobster Lock - Self-Bondage Session Manager
 * File:      Types.h
 * =================================================================================
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

// --- Enums ---
enum DeviceState : uint8_t { READY, ARMED, LOCKED, ABORTED, COMPLETED, TESTING };
enum TriggerStrategy : uint8_t { STRAT_AUTO_COUNTDOWN, STRAT_BUTTON_TRIGGER };
enum DurationType : uint8_t { DUR_FIXED, DUR_RANDOM, DUR_RANGE_SHORT, DUR_RANGE_MEDIUM, DUR_RANGE_LONG };
enum DeterrentStrategy : uint8_t { DETERRENT_FIXED, DETERRENT_RANDOM };
enum SessionOutcome : uint8_t { OUTCOME_SUCCESS, OUTCOME_ABORTED, OUTCOME_UNKNOWN };

// --- Constants ---

// Reward codes
#define REWARD_HISTORY_SIZE 10
#define REWARD_CODE_LENGTH 32
#define REWARD_CHECKSUM_LENGTH 16

// Logging
#define SERIAL_QUEUE_SIZE 50
#define LOG_BUFFER_SIZE 150
#define MAX_LOG_LENGTH 150

// Hardware 
#define MAX_CHANNELS 4

// --- Configuration Structs ---
struct SessionConfig {
  DurationType durationType;
  uint32_t durationFixed;
  uint32_t durationMin;
  uint32_t durationMax;
  TriggerStrategy triggerStrategy;
  uint32_t channelDelays[MAX_CHANNELS];
  bool hideTimer;
  bool disableLED;
};

struct SessionPresets {
  // --- Generators ---
  uint32_t shortMin, shortMax;
  uint32_t mediumMin, mediumMax;
  uint32_t longMin, longMax;
    
  // --- Safety / Profile Limits (The "Ceiling" and "Floor") ---
  uint32_t maxSessionDuration;
  uint32_t minSessionDuration;
};

struct DeterrentConfig {
  bool enableStreaks;

  bool enableRewardCode;
  DeterrentStrategy rewardPenaltyStrategy;
  uint32_t rewardPenaltyMin, rewardPenaltyMax;
  uint32_t rewardPenalty;

  bool enablePaybackTime;
  DeterrentStrategy paybackTimeStrategy;
  uint32_t paybackTimeMin, paybackTimeMax; 
  uint32_t paybackTime;

  bool enableTimeModification;
  uint32_t timeModificationStep;
};

struct SystemDefaults {
  uint32_t longPressDuration;
  uint32_t extButtonSignalDuration;
  uint32_t testModeDuration;
  uint32_t keepAliveInterval;
  uint32_t keepAliveMaxStrikes;
  uint32_t bootLoopThreshold;
  uint32_t stableBootTime;
  uint32_t wifiMaxRetries;
  uint32_t armedTimeout;
};

// --- State Structs ---
struct SessionTimers {
  uint32_t lockDuration;
  uint32_t potentialDebtServed;
  uint32_t penaltyDuration;
  uint32_t lockRemaining;
  uint32_t penaltyRemaining;
  uint32_t testRemaining;
  uint32_t triggerTimeout;
  uint32_t channelDelays[MAX_CHANNELS];
};

struct SessionStats {
  uint32_t streaks;
  uint32_t completed;
  uint32_t aborted;
  uint32_t paybackAccumulated;
  uint32_t totalLockedTime;    
};

struct Reward {
  char code[REWARD_CODE_LENGTH + 1];
  char checksum[REWARD_CHECKSUM_LENGTH + 1];
};

extern const char *stateToString(DeviceState s);
extern const char *durTypeToString(DurationType d);
extern const char *outcomeToString(SessionOutcome o);