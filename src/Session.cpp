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
#include "Session.h"
#include "Globals.h"
#include "Hardware.h"
#include "Logger.h"
#include "Storage.h"
#include "Utils.h"

// =================================================================================
// SECTION: LIFECYCLE & RECOVERY
// =================================================================================

/**
 * Analyzes the loaded state after a reboot and performs
 * the necessary transitions (e.g., aborting, resetting).
 */
void handleRebootState() {

  switch (currentState) {
  case LOCKED:
  case ARMED:
  case TESTING:
    // These are active states. A reboot during them is an abort.
    logMessage("Reboot detected during session. Aborting session...");
    abortSession("Reboot");
    break;

  case COMPLETED:
    // Session was finished. Reset to ready for a new one.
    logMessage("Loaded COMPLETED state. Resetting to READY.");
    resetToReady(true); // This saves the new state
    break;

  case READY:
  case ABORTED:
  default:
    // These states are safe to resume.
    // ABORTED will resume its penalty timer.
    // The watchdog is NOT armed, per the "LOCKED only" rule.
    logMessage("Resuming in-progress state.");
    startTimersForState(currentState); // Resume timers
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
  logMessage(logBuf);

  if (currentState == LOCKED)
    disarmFailsafeTimer();

  currentState = READY;
  startTimersForState(READY);

  setLedPattern(READY);

  // Clear all timers and configs
  lockSecondsRemaining = 0;
  penaltySecondsRemaining = 0;
  testSecondsRemaining = 0;
  triggerTimeoutRemaining = 0;
  g_lastKeepAliveTime = 0;       // Disarm watchdog
  g_currentKeepAliveStrikes = 0; // Reset strikes
  lockSecondsConfig = 0;
  hideTimer = false;

  for (int i = 0; i < MAX_CHANNELS; i++)
    channelDelaysRemaining[i] = 0;

  // Only generate new code if requested.
  // If we abort a countdown, we generally want to keep the same code
  // unless we specifically want to roll it.
  if (generateNewCode) {
    // Shift reward history
    for (int i = REWARD_HISTORY_SIZE - 1; i > 0; i--) {
      rewardHistory[i] = rewardHistory[i - 1];
    }

    // Generate new code with collision detection against the history we just
    // shifted
    generateUniqueSessionCode(rewardHistory[0].code, rewardHistory[0].checksum);

    char codeSnippet[9];
    strncpy(codeSnippet, rewardHistory[0].code, 8);
    codeSnippet[8] = '\0';

    snprintf(logBuf, sizeof(logBuf), " New Reward Code Generated");
    logMessage(logBuf);
    snprintf(logBuf, sizeof(logBuf), " %-20s : %s...", "Code Snippet", codeSnippet);
    logMessage(logBuf);
    snprintf(logBuf, sizeof(logBuf), " %-20s : %s", "Checksum", rewardHistory[0].checksum);
    logMessage(logBuf);
  } else {
    logMessage(" > Preserving existing reward code.");
  }

  saveState(true); // Force save
}

// =================================================================================
// SECTION: SESSION INITIATION
// =================================================================================

// NOTE: Functions in this section assume the caller (Loop or API)
// HAS ALREADY LOCKED THE MUTEX unless otherwise noted.

/**
 * Validates configuration and starts a new session in ARMED state.
 * Returns HTTP-style error codes: 200 (OK), 409 (Not Ready), 400 (Invalid
 * Config).
 */
int startSession(unsigned long duration, unsigned long penalty, TriggerStrategy strategy, unsigned long *delays, bool hide) {
  // State check
  if (currentState != READY) {
    return 409;
  }

  // Validate ranges against System Config
  unsigned long minLockSec = g_systemConfig.minLockSeconds;
  unsigned long maxLockSec = g_systemConfig.maxLockSeconds;
  unsigned long minPenaltySec = g_systemConfig.minPenaltySeconds;
  unsigned long maxPenaltySec = g_systemConfig.maxPenaltySeconds;

  if (duration < minLockSec || duration > maxLockSec)
    return 400;

  // Only enforce penalty range if Reward Code is enabled
  if (enableRewardCode) {
    if (penalty < minPenaltySec || penalty > maxPenaltySec)
      return 400;
  }

  // Apply configuration
  for (int i = 0; i < MAX_CHANNELS; i++)
    channelDelaysRemaining[i] = delays[i];

  hideTimer = hide;
  currentStrategy = strategy;

  // Apply any pending payback time from previous sessions
  unsigned long paybackInSeconds = paybackAccumulated;
  if (paybackInSeconds > 0) {
    logMessage("Applying pending payback time to this session.");
  }

  // Save configs
  lockSecondsConfig = duration + paybackInSeconds;
  penaltySecondsConfig = penalty;

  // LOGGING VISUALS: Arming Block
  char logBuf[256];
  snprintf(logBuf, sizeof(logBuf), "%sARMED", LOG_PREFIX_STATE);
  logMessage(logBuf);

  snprintf(logBuf, sizeof(logBuf), " %-20s : %s", "Strategy",
           (currentStrategy == STRAT_BUTTON_TRIGGER ? "Manual Button" : "Auto Countdown"));
  logMessage(logBuf);

  char timeStr[64];
  formatSeconds(duration, timeStr, sizeof(timeStr));
  snprintf(logBuf, sizeof(logBuf), " %-20s : %s", "Lock Time", timeStr);
  logMessage(logBuf);

  snprintf(logBuf, sizeof(logBuf), " %-20s : %s", "Visibility", (hideTimer ? "Hidden" : "Visible"));
  logMessage(logBuf);

  // Enter ARMED state
  currentState = ARMED;
  setLedPattern(ARMED);

  // Setup based on strategy
  if (currentStrategy == STRAT_BUTTON_TRIGGER) {
    // Wait for Button: Set Timeout
    triggerTimeoutRemaining = g_systemConfig.armedTimeoutSeconds;
    logMessage("   -> Waiting for Manual Trigger.");
  } else {
    // Auto: Timers start ticking immediately in handleOneSecondTick
    logMessage("   -> Auto Sequence Started.");
  }

  startTimersForState(ARMED);
  // NOTE: Watchdog is NOT armed here, only when LOCKED (or in TEST)

  saveState(true);

  return 200;
}

/**
 * Starts the hardware test mode.
 * Returns: 200 (OK), 409 (Not Ready).
 */
int startTestMode() {
  if (currentState != READY) {
    return 409;
  }

  logMessage("Logic: Engaging Channels for 2 min test.");

  currentState = TESTING;
  setLedPattern(TESTING);
  testSecondsRemaining = g_systemConfig.testModeDurationSeconds;
  startTimersForState(TESTING);
  g_lastKeepAliveTime = millis(); // Arm keep-alive watchdog for test mode
  g_currentKeepAliveStrikes = 0;  // Reset strikes
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
  if (currentState == ARMED) {
    if (currentStrategy == STRAT_BUTTON_TRIGGER) {
      enterLockedState(source);
    }
  } else if (currentState == TESTING) {
    logMessage("Trigger ignored: Currently in Hardware Test.");
  }
}

/**
 * Transition from ARMED to LOCKED.
 * Used by Button Press or Auto Countdown completion.
 */
void enterLockedState(const char *source) {
  char logBuf[100];
  snprintf(logBuf, sizeof(logBuf), "%sLOCKED", LOG_PREFIX_STATE);
  logMessage(logBuf);
  snprintf(logBuf, sizeof(logBuf), " Source: %s", source);
  logMessage(logBuf);

  currentState = LOCKED;
  setLedPattern(LOCKED);

  lockSecondsRemaining = lockSecondsConfig;
  startTimersForState(LOCKED);
  armFailsafeTimer();             // DEATH GRIP
  g_lastKeepAliveTime = millis(); // Arm keep-alive watchdog
  g_currentKeepAliveStrikes = 0;  // Reset strikes

  saveState(true); // Force save
}

/**
 * Stops the test mode and returns to READY.
 */
void stopTestMode() {
  logMessage("Stopping test mode.");
  currentState = READY;
  startTimersForState(READY); // Set WDT
  setLedPattern(READY);
  testSecondsRemaining = 0;
  g_lastKeepAliveTime = 0;       // Disarm watchdog
  g_currentKeepAliveStrikes = 0; // Reset strikes
  saveState(true);               // Force save
}

// =================================================================================
// SECTION: SESSION TERMINATION (Complete & Abort)
// =================================================================================

/**
 * Called when a session completes (either Lock timer OR Penalty timer ends).
 * If coming from LOCKED: Increments success stats, clears payback.
 * If coming from ABORTED: Transitions to COMPLETED but retains debt/stats.
 */
void completeSession() {
  SessionState previousState = currentState;

  // LOGGING VISUALS: Complete Block
  char logBuf[100];
  snprintf(logBuf, sizeof(logBuf), "%sCOMPLETED", LOG_PREFIX_STATE);
  logMessage(logBuf);

  if (previousState == LOCKED)
    disarmFailsafeTimer();

  currentState = COMPLETED;
  startTimersForState(COMPLETED); // Reset WDT
  setLedPattern(COMPLETED);

  if (previousState == LOCKED) {
    // --- SUCCESS PATH ---
    // On successful completion of a LOCK, we clear debt and reward streaks.
    if (paybackAccumulated > 0) {
      logMessage(" > Valid session complete. Clearing accumulated payback debt.");
      paybackAccumulated = 0;
    }

    completedSessions++;
    if (enableStreaks) {
      sessionStreakCount++;
    }

    // Log the new winning stats in aligned block
    snprintf(logBuf, sizeof(logBuf), " %-20s : %u", "New Streak", sessionStreakCount);
    logMessage(logBuf);
    snprintf(logBuf, sizeof(logBuf), " %-20s : %u", "Total Completed", completedSessions);
    logMessage(logBuf);
  } else if (previousState == ABORTED) {
    // --- PENALTY SERVED PATH ---
    // The user finished the penalty box.
    if (enablePaybackTime) {
      logMessage(" > Penalty time served. Payback debt retained.");
    } else {
      logMessage(" > Penalty time served.");
    }
  }

  // Clear all timers
  lockSecondsRemaining = 0;
  penaltySecondsRemaining = 0;
  testSecondsRemaining = 0;
  triggerTimeoutRemaining = 0;
  g_lastKeepAliveTime = 0;
  g_currentKeepAliveStrikes = 0;

  for (int i = 0; i < MAX_CHANNELS; i++)
    channelDelaysRemaining[i] = 0;

  // Log Lifetime Total
  char timeBuf[64];
  formatSeconds(totalLockedSessionSeconds, timeBuf, sizeof(timeBuf));

  snprintf(logBuf, sizeof(logBuf), " %-20s : %s", "Lifetime Locked", timeBuf);
  logMessage(logBuf);

  saveState(true); // Force save
}

/**
 * Aborts an active session (LOCKED, ARMED, or TESTING).
 * Implements payback logic, resets streak, and starts the reward penalty timer.
 * If Reward Code is disabled, skips penalty and goes to COMPLETED.
 */
void abortSession(const char *source) {
  char logBuf[100];

  if (currentState == LOCKED) {
    disarmFailsafeTimer();

    // LOGGING VISUALS: Abort Block
    snprintf(logBuf, sizeof(logBuf), "%sABORTED", LOG_PREFIX_STATE);
    logMessage(logBuf);
    snprintf(logBuf, sizeof(logBuf), " Source: %s", source);
    logMessage(logBuf);

    // Implement Abort Logic:
    sessionStreakCount = 0; // 1. Reset streak
    abortedSessions++;      // 2. Increment counter

    if (enablePaybackTime) { // 3. Add payback
      paybackAccumulated += paybackTimeSeconds;

      // Use helper to format payback time
      char timeStr[64];
      formatSeconds(paybackAccumulated, timeStr, sizeof(timeStr));

      // Log formatted payback stats
      snprintf(logBuf, sizeof(logBuf), " %-20s : +%u s", "Payback Added", paybackTimeSeconds);
      logMessage(logBuf);
      snprintf(logBuf, sizeof(logBuf), " %-20s : %s", "Total Debt", timeStr);
      logMessage(logBuf);
    } else {
      logMessage(" Payback: Disabled (No time added)");
    }

    // 4. Handle Reward Code / Penalty
    if (enableRewardCode) {
      // Standard behavior: Penalty Box
      currentState = ABORTED;
      setLedPattern(ABORTED);

      lockSecondsRemaining = 0;
      penaltySecondsRemaining = penaltySecondsConfig;
      startTimersForState(ABORTED);
    } else {
      // New behavior: No Reward Code = No Penalty Box
      logMessage(" Reward Code disabled. Skipping penalty. Transitioning to "
                 "COMPLETED.");

      currentState = COMPLETED;
      setLedPattern(COMPLETED);

      lockSecondsRemaining = 0;
      penaltySecondsRemaining = 0;
      startTimersForState(COMPLETED);
    }

    g_lastKeepAliveTime = 0;       // Disarm watchdog
    g_currentKeepAliveStrikes = 0; // Reset strikes
    saveState(true);               // Force save

  } else if (currentState == ARMED) {
    // ARMED is a "Safety Off" state, but not yet "Point of No Return".
    // Aborting here returns to READY without penalty.
    snprintf(logBuf, sizeof(logBuf), "%s: Aborting session (ARMED).", source);
    logMessage(logBuf);
    resetToReady(false); // Cancel arming (this disarms watchdog)

  } else if (currentState == TESTING) {
    snprintf(logBuf, sizeof(logBuf), "%s: Aborting test mode.", source);
    logMessage(logBuf);
    stopTestMode(); // Cancel test (this disarms watchdog)

  } else {
    snprintf(logBuf, sizeof(logBuf), "%s: Abort ignored, device not in abortable state.", source);
    logMessage(logBuf);
    return;
  }
}

// =================================================================================
// SECTION: PERIODIC LOGIC & WATCHDOGS
// =================================================================================

/**
 * Resets timers when entering a new state.
 */
void startTimersForState(SessionState state) {
  // Adjust Watchdog based on state
  if (state == LOCKED || state == ABORTED || state == TESTING) {
    updateWatchdogTimeout(CRITICAL_WDT_TIMEOUT); // Tight loop check
  } else {
    updateWatchdogTimeout(DEFAULT_WDT_TIMEOUT); // Loose loop check (network delays)
  }

  if (state == ARMED) {
    logMessage("Starting arming logic.");
  } else if (state == LOCKED) {
    logMessage("Starting lock logic.");
  } else if (state == ABORTED) {
    logMessage("Starting penalty logic.");
  } else if (state == TESTING) {
    logMessage("Starting test logic.");
  }
  g_currentKeepAliveStrikes = 0; // Reset strikes on state change
}

/**
 * Helper to check the Keep-Alive Watchdog.
 * Returns true if the session was aborted.
 */
bool checkSessionKeepAliveWatchdog() {
  // Check if watchdog is armed
  if (g_lastKeepAliveTime == 0)
    return false;

  unsigned long elapsed = millis() - g_lastKeepAliveTime;

  // Integer division finds how many 10s intervals have passed
  // Uses Configured Interval
  int calculatedStrikes = elapsed / g_systemConfig.keepAliveIntervalMs;

  // Only log/act if the strike count has increased
  if (calculatedStrikes > g_currentKeepAliveStrikes) {
    g_currentKeepAliveStrikes = calculatedStrikes;
    char logBuf[100];

    // Uses Configured Strike Count
    if (g_currentKeepAliveStrikes >= g_systemConfig.keepAliveMaxStrikes) {
      snprintf(logBuf, sizeof(logBuf), "Keep-Alive Watchdog: Strike %d/%d! ABORTING.", g_currentKeepAliveStrikes,
               g_systemConfig.keepAliveMaxStrikes);
      logMessage(logBuf);
      abortSession("Watchdog Strikeout");
      return true; // Signal that we aborted
    } else {
      snprintf(logBuf, sizeof(logBuf), "Keep-Alive Watchdog: Missed check. Strike %d/%d", g_currentKeepAliveStrikes,
               g_systemConfig.keepAliveMaxStrikes);
      logMessage(logBuf);
    }
  }
  return false;
}

/**
 * This is the main state-machine handler, called 1x/sec from loop().
 * It safely updates all timers and handles state transitions.
 */
void handleOneSecondTick() {
  switch (currentState) {
  case ARMED: {
    if (currentStrategy == STRAT_AUTO_COUNTDOWN) {
      // STRATEGY: Auto Countdown
      // Decrement active channels immediately
      bool allDelaysZero = true;

      // Decrement all active channel delays
      for (size_t i = 0; i < MAX_CHANNELS; i++) {
        // Check if enabled in mask
        if ((g_enabledChannelsMask >> i) & 1) {
          if (channelDelaysRemaining[i] > 0) {
            allDelaysZero = false;
            channelDelaysRemaining[i]--;
          }
        }
      }

      // If all delays are done, transition to LOCKED
      if (allDelaysZero) {
        // Using shared logic function to transition
        enterLockedState("Auto Sequence");
      }
    } else if (currentStrategy == STRAT_BUTTON_TRIGGER) {
      // STRATEGY: Wait for Button
      // Channels do NOT tick down here. We just wait for the button event.
      // Check for timeout.
      if (triggerTimeoutRemaining > 0) {
        triggerTimeoutRemaining--;
      } else {
        logMessage("Armed Timeout: Button not pressed in time. Aborting.");
        abortSession("Arm Timeout");
      }
    }
    break;
  }
  case LOCKED:
    // --- Keep-Alive Session Watchdog Check ---
    if (checkSessionKeepAliveWatchdog()) {
      return; // Aborted inside helper
    }
    // Decrement lock timer
    if (lockSecondsRemaining > 0) {
      // Increment total locked time only when session is active
      totalLockedSessionSeconds++;

      if (--lockSecondsRemaining == 0) {
        completeSession(); // Timer finished
      }
    }
    break;
  case ABORTED:
    // Decrement penalty timer
    if (penaltySecondsRemaining > 0 && --penaltySecondsRemaining == 0) {
      completeSession(); // Timer finished
    }
    break;
  case TESTING:
    // --- Keep-Alive Watchdog Check ---
    if (checkSessionKeepAliveWatchdog()) {
      return; // Aborted inside helper
    }
    // Decrement test timer
    if (testSecondsRemaining > 0 && --testSecondsRemaining == 0) {
      logMessage("Test mode timer expired.");
      stopTestMode(); // Timer finished
    }
    break;
  case READY:
  case COMPLETED:
  default:
    // Do nothing
    break;
  }
}