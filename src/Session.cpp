/*
 * =================================================================================
 * Project:   Lobster Lock - Self-Bondage Session Manager
 * File:      Session.cpp
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Description:
 * Core business logic and state machine. Manages session lifecycles (READY,
 * ARMED, LOCKED), timer countdowns, penalty enforcement, reboot recovery,
 * and input event handling (Button presses).
 * =================================================================================
 */
#include "Session.h"
#include "Globals.h"
#include "Hardware.h"
#include "Logger.h"
#include "Storage.h"
#include "Utils.h"

// =================================================================================
// SECTION: LIFECYCLE and RECOVERY
// =================================================================================

void handleRebootState() {

  switch (g_currentState) {
  case LOCKED:
  case ARMED:
  case TESTING:
    // These are active states. A reboot during them is an abort.
    logKeyValue("Session", "Reboot detected during session. Aborting session...");
    abortSession("Reboot");
    break;

  case COMPLETED:
    // Session was finished. Reset, but ensure we validate hardware before allowing new lock.
    logKeyValue("Session", "Loaded COMPLETED state. Resetting to VALIDATING.");
    resetToReady(true);
    g_currentState = VALIDATING;
    break;

  case READY:
    // If we load READY, we must assume hardware needs to be re-verified.
    logKeyValue("Session", "Loaded READY state. Forcing hardware validation.");
    g_currentState = VALIDATING; // Force Validating
    break;

  case ABORTED:
  default:
    // ABORTED is the only state safe to resume immediately (you are in the penalty box).
    logKeyValue("Session", "Resuming in-progress state.");
    break;
  }
}

/**
 * Resets the device state to READY, generates a new reward code.
 * Does NOT reset counters or payback.
 */
void resetToReady(bool generateNewCode) {
  char logBuf[150];
  snprintf(logBuf, sizeof(logBuf), "%sREADY", LOG_PREFIX_STATE);
  logKeyValue("Session", logBuf);

  g_currentState = READY;

  // Timers are configured based on the new READY state (Disarms everything)
  setTimersForCurrentState();

  // Clear all timers using the new structure
  g_sessionTimers.lockDuration = 0;
  g_sessionTimers.penaltyDuration = 0;
  g_sessionTimers.lockRemaining = 0;
  g_sessionTimers.penaltyRemaining = 0;
  g_sessionTimers.testRemaining = 0;
  g_sessionTimers.triggerTimeout = 0;

  // Reset Active Config flags
  g_activeSessionConfig.hideTimer = false;

  // Clear Active Channel Delays in Active Config AND Live Timers
  for (int i = 0; i < MAX_CHANNELS; i++) {
    g_activeSessionConfig.channelDelays[i] = 0;
    g_sessionTimers.channelDelays[i] = 0;
  }

  // Only generate new code if requested.
  if (generateNewCode) {
    // Shift reward history
    for (int i = REWARD_HISTORY_SIZE - 1; i > 0; i--) {
      rewardHistory[i] = rewardHistory[i - 1];
    }

    // Generate new code
    generateUniqueSessionCode(rewardHistory[0].code, rewardHistory[0].checksum);

    char codeSnippet[9];
    strncpy(codeSnippet, rewardHistory[0].code, 8);
    codeSnippet[8] = '\0';

    snprintf(logBuf, sizeof(logBuf), "New Reward Code Generated");
    logKeyValue("Session", logBuf);
    snprintf(logBuf, sizeof(logBuf), " %-20s : %s...", "Code Snippet", codeSnippet);
    logKeyValue("Session", logBuf);
  } else {
    logKeyValue("Session", "Preserving existing reward code.");
  }

  saveState(true); // Force save
}

// =================================================================================
// SECTION: SESSION INITIATION
// =================================================================================

/**
 * Validates configuration and starts a new session in ARMED state.
 * @param duration: The base lock time requested (Calculated by WebAPI)
 * @param penalty: The penalty time (Provided via WebAPI from Global Config)
 * @param strategy: Auto or Manual
 * @param delays: Array of channel delays
 * @param hide: Visibility flag
 */
int startSession(unsigned long duration, unsigned long penalty, TriggerStrategy strategy, unsigned long *delays, bool hide) {
  char errorLogBuf[128];

  // State check
  if (g_currentState != READY) {
    snprintf(errorLogBuf, sizeof(errorLogBuf), "Start Failed: Device not READY (Current State: %d)", g_currentState);
    logKeyValue("Session", errorLogBuf);
    return 409;
  }

  // 1. Validate ranges against SESSION LIMITS
  if (duration < g_sessionLimits.minLockDuration || duration > g_sessionLimits.maxLockDuration) {
    snprintf(errorLogBuf, sizeof(errorLogBuf), "Start Failed: Duration %lu s out of range (%lu-%lu s)", duration,
             g_sessionLimits.minLockDuration, g_sessionLimits.maxLockDuration);
    logKeyValue("Session", errorLogBuf);
    return 400;
  }

  // Only enforce penalty range if Reward Code is enabled
  if (g_deterrentConfig.enableRewardCode) {
    if (penalty < g_sessionLimits.minRewardPenaltyDuration || penalty > g_sessionLimits.maxRewardPenaltyDuration) {
      snprintf(errorLogBuf, sizeof(errorLogBuf), "Start Failed: Penalty %lu s out of range (%lu-%lu s)", penalty,
               g_sessionLimits.minRewardPenaltyDuration, g_sessionLimits.maxRewardPenaltyDuration);
      logKeyValue("Session", errorLogBuf);
      return 400;
    }
  }

  // 2. Populate Active Configuration (Intent & Logic)
  // Note: Metadata (durationType, min, max) is handled by WebAPI. We just handle logic here.
  for (int i = 0; i < MAX_CHANNELS; i++)
    g_activeSessionConfig.channelDelays[i] = delays[i];

  g_activeSessionConfig.hideTimer = hide;
  g_activeSessionConfig.triggerStrategy = strategy;

  // 3. Apply Payback Debt
  unsigned long paybackInSeconds = g_sessionStats.paybackAccumulated;
  if (paybackInSeconds > 0) {
    logKeyValue("Session", "Applying pending payback time to this session.");
  }

  // 4. Populate Session Timers (The authoritative counters)
  g_sessionTimers.lockDuration = duration + paybackInSeconds;
  g_sessionTimers.penaltyDuration = penalty;

  // Reset counters
  g_sessionTimers.lockRemaining = 0; // Will be set on Lock transition
  g_sessionTimers.penaltyRemaining = 0;

  // Initialize the live countdown timers from the config
  for (int i = 0; i < MAX_CHANNELS; i++) {
    g_sessionTimers.channelDelays[i] = g_activeSessionConfig.channelDelays[i];
  }

  // LOGGING VISUALS: Arming Block
  char logBuf[256];
  snprintf(logBuf, sizeof(logBuf), "%sARMED", LOG_PREFIX_STATE);
  logKeyValue("Session", logBuf);

  snprintf(logBuf, sizeof(logBuf), "Strategy: %s",
           (g_activeSessionConfig.triggerStrategy == STRAT_BUTTON_TRIGGER ? "Manual Button" : "Auto Countdown"));
  logKeyValue("Session", logBuf);

  char timeStr[64];
  formatSeconds(g_sessionTimers.lockDuration, timeStr, sizeof(timeStr));
  snprintf(logBuf, sizeof(logBuf), "Total Lock Time: %s", timeStr);
  logKeyValue("Session", logBuf);

  snprintf(logBuf, sizeof(logBuf), "Timer: %s", (g_activeSessionConfig.hideTimer ? "Hidden" : "Visible"));
  logKeyValue("Session", logBuf);

  // --- NEW: Log Configured Delays ---
  char delayLogBuf[128] = "Configured Delays: ";
  for (int i = 0; i < MAX_CHANNELS; i++) {
    char tmp[20];
    snprintf(tmp, sizeof(tmp), "[%d]%lu ", i + 1, g_activeSessionConfig.channelDelays[i]);
    strncat(delayLogBuf, tmp, sizeof(delayLogBuf) - strlen(delayLogBuf) - 1);
  }
  logKeyValue("Session", delayLogBuf);
  // ----------------------------------

  // Setup based on strategy
  if (g_activeSessionConfig.triggerStrategy == STRAT_BUTTON_TRIGGER) {
    // Wait for Button: Set Timeout using SYSTEM DEFAULT
    g_sessionTimers.triggerTimeout = g_systemDefaults.armedTimeoutSeconds;
    char tBuf1[50];
    formatSeconds(g_systemDefaults.armedTimeoutSeconds, tBuf1, sizeof(tBuf1));
    snprintf(logBuf, sizeof(logBuf), "Waiting for Manual Trigger. Trigger Timeout set to %s", tBuf1);
    logKeyValue("Session", logBuf);
  } else {
    // Auto: Timers start ticking immediately in handleOneSecondTick
    logKeyValue("Session", "Auto Sequence Started.");
  }

  // Enter ARMED state
  g_currentState = ARMED;

  setTimersForCurrentState();

  saveState(true);

  return 200;
}

/**
 * Starts the hardware test session.
 */
int startTestSession() {
  if (g_currentState != READY) {
    return 409;
  }

  char logBuf[150];
  snprintf(logBuf, sizeof(logBuf), "%sTESTING", LOG_PREFIX_STATE);
  logKeyValue("Session", logBuf);

  g_sessionTimers.testRemaining = g_systemDefaults.testModeDuration;

  g_currentState = TESTING;

  setTimersForCurrentState(); // Handles Watchdog arming

  saveState(true);

  return 200;
}

// =================================================================================
// SECTION: ACTIVE STATE TRANSITIONS (Triggers & Locks)
// =================================================================================

/**
 * Logic to trigger the lock from a manual source (Button).
 */
void triggerLock(const char *source) {
  if (g_currentState == ARMED) {
    if (g_activeSessionConfig.triggerStrategy == STRAT_BUTTON_TRIGGER) {
      enterLockedState(source);
    }
  } else if (g_currentState == TESTING) {
    logKeyValue("Session", "Trigger ignored: Currently in Hardware Test.");
  }
}

/**
 * Transition from ARMED to LOCKED.
 * Used by Button Press or Auto Countdown completion.
 */
void enterLockedState(const char *source) {
  char logBuf[100];
  snprintf(logBuf, sizeof(logBuf), "%sLOCKED", LOG_PREFIX_STATE);
  logKeyValue("Session", logBuf);
  snprintf(logBuf, sizeof(logBuf), "Source: %s", source);
  logKeyValue("Session", logBuf);

  g_currentState = LOCKED;
  g_sessionTimers.lockRemaining = g_sessionTimers.lockDuration;

  // This will Arms Failsafe & KeepAlive because state is LOCKED
  setTimersForCurrentState();

  saveState(true);
}

/**
 * Stops the test session and returns to READY.
 */
void stopTestSession() {
  logKeyValue("Session", "Stopping test session.");
  g_currentState = READY;

  g_sessionTimers.testRemaining = 0;

  setTimersForCurrentState(); // Disarms watchdogs

  saveState(true);
}

// =================================================================================
// SECTION: SESSION TERMINATION (Complete & Abort)
// =================================================================================

/**
 * Called when a session completes (either Lock timer OR Penalty timer ends).
 */
void completeSession() {
  DeviceState previousState = g_currentState;

  // LOGGING VISUALS: Complete Block
  char logBuf[100];
  snprintf(logBuf, sizeof(logBuf), "%sCOMPLETED", LOG_PREFIX_STATE);
  logKeyValue("Session", logBuf);

  g_currentState = COMPLETED;

  // Update timers based on COMPLETED state (Disarms Failsafe/Watchdogs)
  setTimersForCurrentState();

  if (previousState == LOCKED) {
    // --- SUCCESS PATH ---
    // On successful completion of a LOCK, we clear debt and reward streaks.
    if (g_sessionStats.paybackAccumulated > 0) {
      logKeyValue("Session", "Valid session complete. Clearing accumulated payback debt.");
      g_sessionStats.paybackAccumulated = 0;
    }

    g_sessionStats.completed++;

    // Check Deterrent Config
    if (g_deterrentConfig.enableStreaks) {
      g_sessionStats.streaks++;
    }

    // Log the new winning stats in aligned block
    snprintf(logBuf, sizeof(logBuf), " %-20s : %u", "New Streak", g_sessionStats.streaks);
    logKeyValue("Session", logBuf);
    snprintf(logBuf, sizeof(logBuf), " %-20s : %u", "Total Completed", g_sessionStats.completed);
    logKeyValue("Session", logBuf);
  } else if (previousState == ABORTED) {
    // --- PENALTY SERVED PATH ---
    // The user finished the penalty box.
    if (g_deterrentConfig.enablePaybackTime) {
      logKeyValue("Session", "Penalty time served. Payback debt retained.");
    } else {
      logKeyValue("Session", "Penalty time served.");
    }
  }

  // Clear all timers
  g_sessionTimers.lockRemaining = 0;
  g_sessionTimers.penaltyRemaining = 0;
  g_sessionTimers.testRemaining = 0;
  g_sessionTimers.triggerTimeout = 0;

  // Clear delays in config AND timers
  for (int i = 0; i < MAX_CHANNELS; i++) {
    g_activeSessionConfig.channelDelays[i] = 0;
    g_sessionTimers.channelDelays[i] = 0;
  }

  saveState(true);

  char tBuf1[50];

  if (g_deterrentConfig.enableStreaks) {
    snprintf(logBuf, sizeof(logBuf), "Streak Count: %u", g_sessionStats.streaks);
    logKeyValue("Session", logBuf);
    snprintf(logBuf, sizeof(logBuf), "Completed Sessions: %u", g_sessionStats.completed);
    logKeyValue("Session", logBuf);
    snprintf(logBuf, sizeof(logBuf), "Aborted Sessions: %u", g_sessionStats.aborted);
    logKeyValue("Session", logBuf);
  }

  formatSeconds(g_sessionStats.paybackAccumulated, tBuf1, sizeof(tBuf1));
  snprintf(logBuf, sizeof(logBuf), "Accumulated Debt: %s", tBuf1);
  logKeyValue("Session", logBuf);

  formatSeconds(g_sessionStats.totalLockedTime, tBuf1, sizeof(tBuf1));
  snprintf(logBuf, sizeof(logBuf), "Lifetime Locked: %s", tBuf1);
  logKeyValue("Session", logBuf);
}

/**
 * Aborts an active session (LOCKED, ARMED, or TESTING).
 */
void abortSession(const char *source) {
  char logBuf[100];

  if (g_currentState == LOCKED) {

    // LOGGING VISUALS: Abort Block
    snprintf(logBuf, sizeof(logBuf), "%sABORTED", LOG_PREFIX_STATE);
    logKeyValue("Session", logBuf);
    snprintf(logBuf, sizeof(logBuf), "Source: %s", source);
    logKeyValue("Session", logBuf);

    if (g_deterrentConfig.enableStreaks) {
      g_sessionStats.streaks = 0; // 1. Reset streak
      g_sessionStats.aborted++;   // 2. Increment counter
    }

    // 3. Add Payback (if enabled)
    if (g_deterrentConfig.enablePaybackTime) {
      g_sessionStats.paybackAccumulated += g_deterrentConfig.paybackTime;

      // Use helper to format payback time
      char timeStr[64];
      formatSeconds(g_sessionStats.paybackAccumulated, timeStr, sizeof(timeStr));

      // Log formatted payback stats
      snprintf(logBuf, sizeof(logBuf), "%-20s : +%u s", "Payback Added", g_deterrentConfig.paybackTime);
      logKeyValue("Session", logBuf);
      snprintf(logBuf, sizeof(logBuf), "%-20s : %s", "Total Debt", timeStr);
      logKeyValue("Session", logBuf);
    } else {
      logKeyValue("Session", "Disabled (No time added)");
    }

    // 4. Handle Reward Code / Penalty
    if (g_deterrentConfig.enableRewardCode) {
      // Penalty Box
      g_currentState = ABORTED;
      g_sessionTimers.lockRemaining = 0;
      g_sessionTimers.penaltyRemaining = g_sessionTimers.penaltyDuration; // Use stored penalty duration

      // Update Timers (Disarm Failsafe/KA, set Watchdog to Critical for Penalty)
      setTimersForCurrentState();
    } else {
      // No Reward Code = No Penalty Box
      logKeyValue("Session", "Reward Code disabled. Skipping penalty.");

      g_currentState = COMPLETED;
      g_sessionTimers.lockRemaining = 0;
      g_sessionTimers.penaltyRemaining = 0;

      snprintf(logBuf, sizeof(logBuf), "%COMPLETED", LOG_PREFIX_STATE);
      logKeyValue("Session", logBuf);

      // Update Timers (Disarm everything)
      setTimersForCurrentState();
    }

    saveState(true);

  } else if (g_currentState == ARMED) {
    snprintf(logBuf, sizeof(logBuf), "%s: Aborting session (ARMED).", source);
    logKeyValue("Session", logBuf);
    resetToReady(false);

  } else if (g_currentState == TESTING) {
    snprintf(logBuf, sizeof(logBuf), "%s: Aborting test session.", source);
    logKeyValue("Session", logBuf);
    stopTestSession();

  } else if (g_currentState == READY) {
    g_currentState = VALIDATING;
    extButtonSignalStartTime = 0;

  } else if (g_currentState == VALIDATING) {
    // Do nothing
  } else {
    snprintf(logBuf, sizeof(logBuf), "%s: Abort ignored, device not in abortable state.", source);
    logKeyValue("Session", logBuf);
    return;
  }
}

// =================================================================================
// SECTION: PERIODIC LOGIC & WATCHDOGS
// =================================================================================

/**
 * Arms the keep-alive watchdog for UI pings
 */
void armKeepAliveWatchdog() {
  g_lastKeepAliveTime = millis(); // Arm keep-alive watchdog
  g_currentKeepAliveStrikes = 0;  // Reset strikes

  logKeyValue("Session", "Keep-Alive UI Watchdog ARMED");
}

/**
 * Disarms the keep-alive watchdog for UI pings
 */
void disarmKeepAliveWatchdog() {
  g_lastKeepAliveTime = 0;       // Disarm keep-alive watchdog
  g_currentKeepAliveStrikes = 0; // Reset strikes

  logKeyValue("Session", "Keep-Alive UI Watchdog DISARMED");
}

/**
 * Centralized logic to configure hardware timers and safety watchdogs
 * based on the active state.
 */
void setTimersForCurrentState() {
  // 1. Hardware Task Watchdog (ESP WDT)
  // CRITICAL states (Active Lock or Penalty) require aggressive watchdog
  if (g_currentState == ARMED || g_currentState == LOCKED || g_currentState == ABORTED || g_currentState == TESTING) {
    updateWatchdogTimeout(CRITICAL_WDT_TIMEOUT);
  } else {
    updateWatchdogTimeout(DEFAULT_WDT_TIMEOUT);
  }

  // 2. Failsafe (Death Grip) Timer
  // Only ARMED when physically locked.
  if (g_currentState == ARMED || g_currentState == LOCKED || g_currentState == TESTING) {
    armFailsafeTimer();
  } else {
    disarmFailsafeTimer();
  }

  // 3. Keep-Alive (UI) Watchdog
  // Only ARMED when session is active (LOCKED) or testing.
  if (g_currentState == ARMED || g_currentState == LOCKED || g_currentState == TESTING) {
    armKeepAliveWatchdog();
  } else {
    disarmKeepAliveWatchdog();
  }

  g_currentKeepAliveStrikes = 0; // Reset strike counter on any state change
}

/**
 * Helper to check the Keep-Alive Watchdog.
 * Returns true if the session was aborted.
 */
bool checkKeepAliveWatchdog() {
  if (g_lastKeepAliveTime == 0)
    return false;

  unsigned long elapsed = millis() - g_lastKeepAliveTime;

  // Use SYSTEM DEFAULT for interval
  int calculatedStrikes = elapsed / g_systemDefaults.keepAliveInterval;

  if (calculatedStrikes > g_currentKeepAliveStrikes) {
    g_currentKeepAliveStrikes = calculatedStrikes;
    char logBuf[100];

    // Use SYSTEM DEFAULT for tolerance
    if (g_currentKeepAliveStrikes >= g_systemDefaults.keepAliveMaxStrikes) {
      snprintf(logBuf, sizeof(logBuf), "Keep-Alive UI Watchdog: Strike %d/%d! ABORTING.", g_currentKeepAliveStrikes,
               g_systemDefaults.keepAliveMaxStrikes);
      logKeyValue("Session", logBuf);
      abortSession("UI Watchdog Strikeout");
      return true;
    } else {
      snprintf(logBuf, sizeof(logBuf), "Keep-Alive UI Watchdog Missed check. Strike %d/%d", g_currentKeepAliveStrikes,
               g_systemDefaults.keepAliveMaxStrikes);
      logKeyValue("Session", logBuf);
    }
  }
  return false;
}

/**
 * This is the main state-machine handler, called 1x/sec from loop().
 */
void handleOneSecondTick() {
  switch (g_currentState) {
  case ARMED: {
    if (g_activeSessionConfig.triggerStrategy == STRAT_AUTO_COUNTDOWN) {
      // STRATEGY: Auto Countdown
      bool allDelaysZero = true;
      char debugDelayStr[64] = "Delays: ";

      for (size_t i = 0; i < MAX_CHANNELS; i++) {
        // Log delay status (Check SESSION TIMERS)
        char tmp[16];
        snprintf(tmp, sizeof(tmp), "[%d]%lu ", (int)i + 1, g_sessionTimers.channelDelays[i]);
        strncat(debugDelayStr, tmp, sizeof(debugDelayStr) - strlen(debugDelayStr) - 1);

        // If a delay is configured in the session, we respect it regardless
        // of whether the physical channel is currently enabled.
        if (g_sessionTimers.channelDelays[i] > 0) {
          allDelaysZero = false;
          g_sessionTimers.channelDelays[i]--;
        }
      }

      logKeyValue("Session", debugDelayStr);

      if (allDelaysZero) {
        enterLockedState("Auto Sequence");
      }
    } else if (g_activeSessionConfig.triggerStrategy == STRAT_BUTTON_TRIGGER) {
      // STRATEGY: Wait for Button
      if (g_sessionTimers.triggerTimeout > 0) {
        g_sessionTimers.triggerTimeout--;
      } else {
        logKeyValue("Session", "Armed Timeout: Button not pressed in time. Aborting.");
        abortSession("Arm Timeout");
      }
    }
    break;
  }
  case LOCKED:
    if (checkKeepAliveWatchdog()) {
      return;
    }

    // Decrement lock timer
    if (g_sessionTimers.lockRemaining > 0) {
      // Increment total locked time in STATS
      g_sessionStats.totalLockedTime++;

      if (--g_sessionTimers.lockRemaining == 0) {
        completeSession();
      }
    }
    break;
  case ABORTED:
    // Decrement penalty timer
    if (g_sessionTimers.penaltyRemaining > 0 && --g_sessionTimers.penaltyRemaining == 0) {
      completeSession();
    }
    break;
  case TESTING:
    if (checkKeepAliveWatchdog()) {
      return;
    }
    if (g_sessionTimers.testRemaining > 0 && --g_sessionTimers.testRemaining == 0) {
      logKeyValue("Session", "Test session done.");
      stopTestSession();
    }
    break;
  case READY:
  case COMPLETED:
  default:
    break;
  }
}