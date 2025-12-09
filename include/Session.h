/*
 * =================================================================================
 * Project:   Lobster Lock - Self-Bondage Session Manager
 * File:      Session.h / Session.cpp
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Description:
 * Core business logic and state machine. Manages session lifecycles (READY,
 * ARMED, LOCKED), timer countdowns, penalty enforcement, reboot recovery,
 * and input event handling (Button presses).
 * =================================================================================
 */
#ifndef SESSION_H
#define SESSION_H

#include "Hardware.h"

// Main state machine enum.
enum DeviceState : uint8_t { VALIDATING, READY, ARMED, LOCKED, ABORTED, COMPLETED, TESTING };

// Trigger Strategy to move from ARMED -> LOCKED
enum TriggerStrategy : uint8_t { STRAT_AUTO_COUNTDOWN, STRAT_BUTTON_TRIGGER };

// Duration Type Enum (Intent)
enum DurationType : uint8_t { DUR_FIXED, DUR_RANDOM, DUR_RANGE };

// --- Session Constants & NVS ---
#define REWARD_HISTORY_SIZE 10
#define REWARD_CODE_LENGTH 32
#define REWARD_CHECKSUM_LENGTH 16

struct Reward {
  char code[REWARD_CODE_LENGTH + 1];
  char checksum[REWARD_CHECKSUM_LENGTH + 1];
};

struct SessionConfig {
  DurationType durationType;
  uint32_t fixedDuration;
  uint32_t minDuration;
  uint32_t maxDuration;
  TriggerStrategy triggerStrategy;
  uint32_t channelDelays[MAX_CHANNELS];
  bool hideTimer;
};

struct SessionTimers {
  uint32_t lockDuration;
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

struct DeterrentConfig {
  bool enableStreaks;
  bool enableRewardCode;
  uint32_t rewardPenalty;
  bool enablePaybackTime;
  uint32_t paybackTime;
};

struct SessionLimits {
  uint32_t minLockDuration;
  uint32_t maxLockDuration;
  uint32_t minRewardPenaltyDuration;
  uint32_t maxRewardPenaltyDuration;
  uint32_t minPaybackTime;
  uint32_t maxPaybackTime;
};

// =================================================================================
// SECTION: LIFECYCLE & RECOVERY
// =================================================================================
void handleRebootState();
void resetToReady(bool generateNewCode);

// =================================================================================
// SECTION: SESSION INITIATION
// =================================================================================
int startSession(unsigned long duration, unsigned long penalty, TriggerStrategy strategy, unsigned long *delays, bool hide);
int startTestSession();

// =================================================================================
// SECTION: ACTIVE STATE TRANSITIONS
// =================================================================================
void triggerLock(const char *source);
void enterLockedState(const char *source);
void stopTestSession();

// =================================================================================
// SECTION: SESSION TERMINATION
// =================================================================================
void abortSession(const char *source);
void completeSession();

// =================================================================================
// SECTION: PERIODIC LOGIC & WATCHDOGS
// =================================================================================
void handleOneSecondTick();

void setTimersForCurrentState();
bool checkKeepAliveWatchdog();

#endif