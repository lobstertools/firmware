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
#include <esp_task_wdt.h>

#include "Globals.h"
#include "Logger.h"
#include "Session.h"
#include "Storage.h"
#include "Utils.h"

/**
 * Loads the entire session state and config from NVS (Preferences)
 * Uses key-value pairs for robustness and flash longevity.
 * @return true if a valid session was loaded, false otherwise.
 */
bool loadState() {
  sessionState.begin("session", true); // Read-only
  unsigned long magic = sessionState.getULong("magic", 0);

  // Check magic value first. If it's not present or correct,
  // we assume the rest of the data is invalid.
  if (magic != MAGIC_VALUE) {
    // No valid data.
    sessionState.end();
    return false; // Report failure
  }

  logMessage("Valid session data found in NVS.");

  // Load all values from NVS, providing a default for each
  currentState = (SessionState)sessionState.getUChar("state", (uint8_t)READY);
  currentStrategy = (TriggerStrategy)sessionState.getUChar("strategy", (uint8_t)STRAT_AUTO_COUNTDOWN);

  lockSecondsRemaining = sessionState.getULong("lockRemain", 0);
  penaltySecondsRemaining = sessionState.getULong("penaltyRemain", 0);
  penaltySecondsConfig = sessionState.getULong("penaltyConfig", 0);
  lockSecondsConfig = sessionState.getULong("lockConfig", 0);
  testSecondsRemaining = sessionState.getULong("testRemain", 0);

  hideTimer = sessionState.getBool("hideTimer", false);

  // Load device configuration (Session Specific)
  enableStreaks = sessionState.getBool("enableStreaks", true);
  enablePaybackTime = sessionState.getBool("enablePayback", true);
  enableRewardCode = sessionState.getBool("enableCode", true);
  paybackTimeSeconds = sessionState.getUInt("paybackSeconds", 900);

  // Load persistent session counters
  sessionStreakCount = sessionState.getUInt("streak", 0);
  completedSessions = sessionState.getUInt("completed", 0);
  abortedSessions = sessionState.getUInt("aborted", 0);
  paybackAccumulated = sessionState.getUInt("paybackAccum", 0);
  totalLockedSessionSeconds = sessionState.getUInt("totalLocked", 0);

  // Load arrays (as binary blobs)
  sessionState.getBytes("delays", channelDelaysRemaining, sizeof(channelDelaysRemaining));
  sessionState.getBytes("rewards", rewardHistory, sizeof(rewardHistory));

  sessionState.end(); // Done reading

  return true; // Report success
}

/**
 * Saves the entire session state and config to NVS (Preferences)
 * Uses key-value pairs for robustness and flash longevity.
 */
void saveState(bool force) {
  if (!force)
    return;

  char logBuf[100];
  snprintf(logBuf, sizeof(logBuf), "Saving state to NVS: %s", stateToString(currentState));
  logMessage(logBuf);

  esp_task_wdt_reset(); // Feed before potentially slow commit

  sessionState.begin("session", false); // Open namespace in read/write

  // Save all dynamic state variables
  sessionState.putUChar("state", (uint8_t)currentState);
  sessionState.putUChar("strategy", (uint8_t)currentStrategy);

  sessionState.putULong("lockRemain", lockSecondsRemaining);
  sessionState.putULong("penaltyRemain", penaltySecondsRemaining);
  sessionState.putULong("penaltyConfig", penaltySecondsConfig);
  sessionState.putULong("lockConfig", lockSecondsConfig);
  sessionState.putULong("testRemain", testSecondsRemaining);

  // Save device configuration
  sessionState.putBool("hideTimer", hideTimer);
  sessionState.putBool("enableStreaks", enableStreaks);
  sessionState.putBool("enablePayback", enablePaybackTime);
  sessionState.putBool("enableCode", enableRewardCode);
  sessionState.putUInt("paybackSeconds", paybackTimeSeconds);

  // Save persistent counters
  sessionState.putUInt("streak", sessionStreakCount);
  sessionState.putUInt("completed", completedSessions);
  sessionState.putUInt("aborted", abortedSessions);
  sessionState.putUInt("paybackAccum", paybackAccumulated);
  sessionState.putUInt("totalLocked", totalLockedSessionSeconds);

  // Save arrays as binary "blobs"
  sessionState.putBytes("delays", channelDelaysRemaining, sizeof(channelDelaysRemaining));
  sessionState.putBytes("rewards", rewardHistory, sizeof(rewardHistory));

  // Save magic value
  sessionState.putULong("magic", MAGIC_VALUE);

  sessionState.end(); // This commits the changes

  esp_task_wdt_reset(); // And feed after
}