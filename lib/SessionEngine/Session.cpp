/*
 * =================================================================================
 * Project:   Lobster Lock - Self-Bondage Session Manager
 * File:      lib/SessionEngine/Session.cpp
 *
 * Description:
 * Core business logic. 
 * - Uses 'SessionRules' for Gamification/Consequences.
 * - Uses 'changeState' for robust State Transitions and Safety.
 * - Decoupled from Hardware via ISessionHAL (Polling Input).
 * - Enforces Safety Interlock (Pedal/Button) presence.
 * - Manages Network Provisioning Handover.
 * =================================================================================
 */
#include <stdio.h>
#include <string.h>

#include "Session.h"
#include "LogicUtils.h"

// =================================================================================
// SECTION: CONSTRUCTOR & INIT
// =================================================================================

SessionEngine::SessionEngine(ISessionHAL& hal, 
                             ISessionRules& rules, 
                             const SystemDefaults& sysDefaults,
                             const SessionPresets& presets, 
                             const DeterrentConfig& deterrents)
    : _hal(hal), 
      _rules(rules),
      _sysDefaults(sysDefaults), 
      _presets(presets), 
      _deterrents(deterrents) 
{
    _state = READY;
    memset(&_timers, 0, sizeof(_timers));
    memset(&_stats, 0, sizeof(_stats));
    memset(&_activeConfig, 0, sizeof(_activeConfig));
    memset(_rewardHistory, 0, sizeof(_rewardHistory));
    
    _lastKeepAliveTime = 0;
    _currentKeepAliveStrikes = 0;
    
    // Safety Interlock Init
    _isHardwarePermitted = false;
    _safetyStableStartTime = 0;
    _lastInterlockRawState = false;
}

// =================================================================================
// SECTION: INTERNAL HELPERS (Logging & Utils)
// =================================================================================

void SessionEngine::formatSecondsInternal(unsigned long totalSeconds, char *buffer, size_t size) {
    unsigned long hours = totalSeconds / 3600;
    unsigned long minutes = (totalSeconds % 3600) / 60;
    unsigned long seconds = totalSeconds % 60;
    snprintf(buffer, size, "%lu h, %lu min, %lu s", hours, minutes, seconds);
}

void SessionEngine::logKeyValue(const char *key, const char *value) {
    char tempBuf[128];
    // Format: " Key : Value"
    snprintf(tempBuf, sizeof(tempBuf), " %-8s : %s", key, value);
    _hal.log(tempBuf);
}

/* * In Session.cpp 
 * Add this method. (Ensure it is also declared in Session.h)
 */

void SessionEngine::printStartupDiagnostics() {
    char logBuf[128];
    const char* boolStr[] = { "NO", "YES" };

    _hal.log("==========================================================================");
    _hal.log("                        SESSION ENGINE DIAGNOSTICS                        ");
    _hal.log("==========================================================================");

    // -------------------------------------------------------------------------
    // SECTION: CURRENT STATE
    // -------------------------------------------------------------------------
    _hal.log("[ ENGINE STATE ]");

    // Device State
    const char* sStr = "UNKNOWN";
    switch(_state) {
        case READY:      sStr = "READY"; break;
        case ARMED:      sStr = "ARMED"; break;
        case LOCKED:     sStr = "LOCKED"; break;
        case ABORTED:    sStr = "ABORTED"; break;
        case COMPLETED:  sStr = "COMPLETED"; break;
        case TESTING:    sStr = "TESTING"; break;
    }
    snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Current Mode", sStr);
    _hal.log(logBuf);

    // Safety Interlock
    snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Interlock Permitted", boolStr[_isHardwarePermitted]);
    _hal.log(logBuf);

    // Watchdogs
    snprintf(logBuf, sizeof(logBuf), " %-25s : %d / %d", "Keep-Alive Strikes", 
             _currentKeepAliveStrikes, _sysDefaults.keepAliveMaxStrikes);
    _hal.log(logBuf);

    // -------------------------------------------------------------------------
    // SECTION: CONFIGURATION LIMITS (PRESETS)
    // -------------------------------------------------------------------------
    _hal.log(""); 
    _hal.log("[ CONFIGURATION LIMITS ]");

    snprintf(logBuf, sizeof(logBuf), " %-25s : %u s", "Absolute Min Lock", _presets.minLockDuration);
    _hal.log(logBuf);
    snprintf(logBuf, sizeof(logBuf), " %-25s : %u s", "Absolute Max Lock", _presets.limitLockMax);
    _hal.log(logBuf);

    // Ranges
    snprintf(logBuf, sizeof(logBuf), " %-25s : %u - %u s", "Short Range", _presets.shortMin, _presets.shortMax);
    _hal.log(logBuf);
    snprintf(logBuf, sizeof(logBuf), " %-25s : %u - %u s", "Medium Range", _presets.mediumMin, _presets.mediumMax);
    _hal.log(logBuf);
    snprintf(logBuf, sizeof(logBuf), " %-25s : %u - %u s", "Long Range", _presets.longMin, _presets.longMax);
    _hal.log(logBuf);

    // -------------------------------------------------------------------------
    // SECTION: DETERRENTS & RULES
    // -------------------------------------------------------------------------
    _hal.log(""); 
    _hal.log("[ DETERRENTS & RULES ]");

    snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Reward Codes", boolStr[_deterrents.enableRewardCode]);
    _hal.log(logBuf);

    if (_deterrents.enableRewardCode) {
        snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Penalty Strategy", 
            _deterrents.penaltyStrategy == DETERRENT_FIXED ? "FIXED" : "RANDOM");
        _hal.log(logBuf);
        snprintf(logBuf, sizeof(logBuf), " %-25s : %u s", "Base Penalty", _deterrents.rewardPenalty);
        _hal.log(logBuf);
    }

    snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Payback (Debt)", boolStr[_deterrents.enablePaybackTime]);
    _hal.log(logBuf);

    // -------------------------------------------------------------------------
    // SECTION: STATISTICS
    // -------------------------------------------------------------------------
    _hal.log(""); 
    _hal.log("[ HISTORY & STATS ]");

    snprintf(logBuf, sizeof(logBuf), " %-25s : %u", "Sessions Completed", _stats.completed);
    _hal.log(logBuf);
    snprintf(logBuf, sizeof(logBuf), " %-25s : %u", "Current Streak", _stats.streaks);
    _hal.log(logBuf);
    
    char timeStr[64];
    formatSecondsInternal(_stats.totalLockedTime, timeStr, sizeof(timeStr));
    snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Total Time Locked", timeStr);
    _hal.log(logBuf);

    if (_deterrents.enablePaybackTime) {
        formatSecondsInternal(_stats.paybackAccumulated, timeStr, sizeof(timeStr));
        snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Accumulated Debt", timeStr);
        _hal.log(logBuf);
    }

}

// =================================================================================
// SECTION: STATE TRANSITION SYSTEM (The Event Core)
// =================================================================================

bool SessionEngine::isCriticalState(DeviceState s) const {
    // These states are active/dangerous and need aggressive crash recovery
    return (s == ARMED || s == LOCKED || s == ABORTED || s == TESTING);
}

bool SessionEngine::requiresFailsafe(DeviceState s) const {
    // Only states where we are physically locked or potentially dangerous
    // ABORTED is excluded (it's a penalty box, but we don't need the 'Death Grip' hardware timer)
    return (s == ARMED || s == LOCKED || s == TESTING);
}

bool SessionEngine::requiresKeepAlive(DeviceState s) const {
    // We need UI connectivity confirmation during active use
    return (s == ARMED || s == LOCKED || s == TESTING);
}

void SessionEngine::applyStateSafetyProfile() {
    // 1. Hardware Watchdog (ESP Task WDT)
    // Critical states get a tight 5s timeout, others get relaxed 20s
    _hal.setWatchdogTimeout(isCriticalState(_state) ? 5 : 20);

    // 2. Failsafe (Death Grip)
    if (requiresFailsafe(_state)) {
        // Calculate the safety cap based on context
        uint32_t safetyDuration = _timers.lockDuration;
        if (_state == TESTING) safetyDuration = _sysDefaults.testModeDuration;
        
        // Arm the hardware timer
        _hal.armFailsafeTimer(safetyDuration);
    } else {
        _hal.disarmFailsafeTimer();
    }

    // 3. UI Keep-Alive Watchdog
    if (requiresKeepAlive(_state)) {
        armKeepAliveWatchdog();
    } else {
        disarmKeepAliveWatchdog();
    }
}

/**
 * Centralized State Machine Transition.
 * All state changes MUST go through this function to ensure safety side-effects run.
 */
void SessionEngine::changeState(DeviceState newState) {
    // Guard: Do not re-enter the same state (unless needed for refresh, but generally redundant)
    if (_state == newState) return;

    _state = newState;
    
    // 1. Log Visuals
    const char* sStr = "UNKNOWN";
    switch(_state) {
        case READY:      sStr = "READY"; break;
        case ARMED:      sStr = "ARMED"; break;
        case LOCKED:     sStr = "LOCKED"; break;
        case ABORTED:    sStr = "ABORTED"; break;
        case COMPLETED:  sStr = "COMPLETED"; break;
        case TESTING:    sStr = "TESTING"; break;
    }
    
    char logBuf[64];
    snprintf(logBuf, sizeof(logBuf), ">>> STATE CHANGE: %s", sStr);
    logKeyValue("Session", logBuf);

    // 2. Apply Safety Configuration (Side Effects)
    applyStateSafetyProfile();

    // 3. Persist to NVS
    _hal.saveState(_state, _timers, _stats);
}

// =================================================================================
// SECTION: SAFETY INTERLOCK LOGIC
// =================================================================================

void SessionEngine::updateSafetyInterlock() {
    // 1. Poll Hardware
    bool isConnected = _hal.isSafetyInterlockEngaged();

    // 2. Handle State Changes (Debounce / Stability Logic)
    if (isConnected) {
        if (!_lastInterlockRawState) {
            // Edge: Just connected. Start timer.
            _safetyStableStartTime = _hal.getMillis();
            logKeyValue("Safety", "Interlock Connected. Validating...");
        }
        
        // Convert seconds config to ms
        unsigned long requiredStableMs = _sysDefaults.extButtonSignalDuration * 1000; 

        // If not yet permitted, check if duration has passed
        if (!_isHardwarePermitted && (_hal.getMillis() - _safetyStableStartTime >= requiredStableMs)) {
            _isHardwarePermitted = true;
            logKeyValue("Safety", "Interlock Verified. Hardware Permitted.");
        }
    } else {
        // Disconnected
        if (_lastInterlockRawState) {
             logKeyValue("Safety", "Interlock Disconnected!");
        }
        // Immediate revocation of permission
        _isHardwarePermitted = false; 
        _safetyStableStartTime = 0;
    }

    _lastInterlockRawState = isConnected;

    // 3. ENFORCE SAFETY (The "Kill Switch")
    // If we are in an active state and permission is lost -> ABORT
    if (!_isHardwarePermitted) {
        if (_state == LOCKED || _state == ARMED || _state == TESTING) {
            logKeyValue("Safety", "Critical: Interlock lost during session.");
            abort("Safety Interlock Disconnected");
        }
    }
}

// =================================================================================
// SECTION: NETWORK HEALTH CHECK
// =================================================================================

void SessionEngine::checkNetworkHealth() {
    // 1. Ask HAL if the network is dead and requesting provisioning
    if (_hal.isNetworkProvisioningRequested()) {
        
        // 2. If we are in a critical session, we MUST abort first
        if (isCriticalState(_state)) {
            logKeyValue("Session", "Critical: Network Failure! Aborting Session.");
            abort("Network Failure");
            
            // We return here. The next tick will see we are ABORTED/COMPLETED 
            // and proceed to the next block below (provisioning logic).
            return; 
        }

        // 3. If we are safe (READY, ABORTED, COMPLETED), we allow the blocking provisioning
        logKeyValue("Session", "Network provisioning authorized. Handing over control.");
        
        // This is a blocking call (reboots device after).
        // Safety profile (Pins LOW) is enforced by Network module or HAL.
        _hal.enterNetworkProvisioning();
    }
}

// =================================================================================
// SECTION: TICK LOGIC HELPERS
// =================================================================================

/**
 * Handles the "Auto Countdown" strategy.
 * Decrements delays and triggers lock when complete.
 */
void SessionEngine::processAutoCountdown() {
  bool allDelaysZero = true;
  char debugDelayStr[64];
  
  // Initialize log string
  snprintf(debugDelayStr, sizeof(debugDelayStr), "Delays: ");

  for (size_t i = 0; i < MAX_CHANNELS; i++) {
    // 1. Format Status for Log
    char tmp[16];
    snprintf(tmp, sizeof(tmp), "[%d]%u ", (int)i + 1, _timers.channelDelays[i]);
    strncat(debugDelayStr, tmp, sizeof(debugDelayStr) - strlen(debugDelayStr) - 1);

    // 2. Decrement Logic
    if (_timers.channelDelays[i] > 0) {
      allDelaysZero = false;
      _timers.channelDelays[i]--;
    }
  }

  logKeyValue("Session", debugDelayStr);

  if (allDelaysZero) {
    enterLockedState("Auto Sequence");
  }
}

/**
 * Handles the "Button Trigger" strategy.
 * Checks for user input (Polling) or timeout.
 */
void SessionEngine::processButtonTriggerWait() {
  // 1. POLL THE HAL: Did the user double-click?
  // This replaces the old 'push' trigger() call from hardware.
  // The HAL implementation should return true ONCE per event (consume the flag).
  if (_hal.checkTriggerAction()) {
      enterLockedState("Button Double-Click");
      return;
  }

  // 2. Handle Timeout
  if (_timers.triggerTimeout > 0) {
    _timers.triggerTimeout--;
  } else {
    logKeyValue("Session", "Armed Timeout: Button not pressed in time. Aborting.");
    abort("Arm Timeout");
  }
}

// =================================================================================
// SECTION: MAIN TICK
// =================================================================================

/**
 * This is the main state-machine handler, called 1x/sec.
 * Logic is now delegated to specific private helpers.
 */
void SessionEngine::tick() {
  // 1. Priority Checks: Safety & Connectivity
  updateSafetyInterlock();
  checkNetworkHealth();

  if (_hal.checkAbortAction()) {
      logKeyValue("Session", "Universal Abort Triggered (Hardware Input)");
      abort("Manual Long-Press");
  }

  // 2. Process Logic based on State
  switch (_state) {
  case ARMED:
    if (_activeConfig.triggerStrategy == STRAT_AUTO_COUNTDOWN) {
      processAutoCountdown();
    } else {
      processButtonTriggerWait();
    }
    break;

  case LOCKED:
    if (checkKeepAliveWatchdog()) return;
    if (_timers.lockRemaining > 0) {
      
      // DELEGATE: Rules track time stats
      _rules.onTickLocked(_stats);

      if (--_timers.lockRemaining == 0) completeSession();
    }
    break;

  case ABORTED:
    if (_timers.penaltyRemaining > 0 && --_timers.penaltyRemaining == 0) completeSession();
    break;

  case TESTING:
    if (checkKeepAliveWatchdog()) return;
    if (_timers.testRemaining > 0 && --_timers.testRemaining == 0) {
      logKeyValue("Session", "Test session done.");
      stopTest();
    }
    break;

  case READY:
  case COMPLETED:
  default:
    break;
  }

  // 3. ENFORCE HARDWARE SAFETY (The "Continuous Enforcement")
  // Calculate what the pins *should* be right now based on logic
  uint8_t targetMask = calculateSafetyMask();
  _hal.setHardwareSafetyMask(targetMask);
}

// =================================================================================
// SECTION: LOGIC & HELPERS
// =================================================================================

/**
 * Resolves the "Intent" (Random/Fixed) into a concrete base duration.
 * This remains here because it involves RNG (Mechanism).
 */
uint32_t SessionEngine::resolveBaseDuration(const SessionConfig &config) {
  uint32_t baseDuration = 0;

  if (config.durationType == DUR_FIXED) {
    baseDuration = config.fixedDuration;
  } else {
    uint32_t minVal = config.minDuration;
    uint32_t maxVal = config.maxDuration;

    switch (config.durationType) {
      case DUR_RANGE_SHORT:
        minVal = _presets.shortMin;
        maxVal = _presets.shortMax;
        break;
      case DUR_RANGE_MEDIUM:
        minVal = _presets.mediumMin;
        maxVal = _presets.mediumMax;
        break;
      case DUR_RANGE_LONG:
        minVal = _presets.longMin;
        maxVal = _presets.longMax;
        break;
      default: break;
    }

    // Basic sanity clamps before RNG
    if (maxVal > _presets.limitLockMax) maxVal = _presets.limitLockMax;
    if (minVal > maxVal) minVal = maxVal; 
    if (maxVal < minVal) { uint32_t temp = maxVal; maxVal = minVal; minVal = temp; }
    if (minVal < _presets.minLockDuration) minVal = _presets.minLockDuration;
    if (maxVal == 0) maxVal = minVal + 60;

    baseDuration = _hal.getRandom(minVal, maxVal);
    
    // Logging intent
    const char* typeLabel = "Range";
    if (config.durationType == DUR_RANGE_SHORT) typeLabel = "Short";
    else if (config.durationType == DUR_RANGE_MEDIUM) typeLabel = "Medium";
    else if (config.durationType == DUR_RANGE_LONG) typeLabel = "Long";
    else if (config.durationType == DUR_RANDOM) typeLabel = "Random";

    char logBuf[128];
    snprintf(logBuf, sizeof(logBuf), "Resolved %s: %u s (Limits: %u-%u)", typeLabel, baseDuration, minVal, maxVal);
    logKeyValue("Session", logBuf);
  }

  return baseDuration;
}

// =================================================================================
// SECTION: ACTIONS & TRANSITIONS
// =================================================================================

/**
 * Validates configuration and starts a new session in ARMED state.
 */
int SessionEngine::startSession(const SessionConfig &config) {
  // 1. Safety Check First
  if (!_isHardwarePermitted) {
    logKeyValue("Session", "Start Failed: Safety Interlock not engaged.");
    return 412; // HTTP Precondition Failed
  }

  // 2. Network Stability Check
  if (_hal.isNetworkProvisioningRequested()) {
      logKeyValue("Session", "Start Failed: Network unstable/provisioning required.");
      return 503; 
  }

  if (_state != READY) {
    logKeyValue("Session", "Start Failed: Device not READY");
    return 409;
  }

  // 3. Determine Base Duration (Mechanism)
  uint32_t baseDuration = resolveBaseDuration(config);

  // 4. Delegate Policy Check to Rules Engine (Debt, Limits)
  // The Rules Engine determines the final duration (e.g., adding debt).
  uint32_t finalLockDuration = _rules.processStartRequest(baseDuration, _presets, _deterrents, _stats);

  if (finalLockDuration == 0) {
    logKeyValue("Session", "Start Failed: Duration Rejected by Rules (Out of Range)");
    return 400;
  }

  // 5. Validate Penalty against Profile
  // Only validate specific value if strategy is FIXED.
  if (_deterrents.enableRewardCode) {
    if (_deterrents.penaltyStrategy == DETERRENT_FIXED) {
      uint32_t penaltyDuration = _deterrents.rewardPenalty;
      if (penaltyDuration < _presets.penaltyMin || penaltyDuration > _presets.penaltyMax) {
        logKeyValue("Session", "Start Failed: Penalty Out of Range");
        return 400;
      }
    }
  }

  // 6. Commit State
  _activeConfig = config;
  _timers.lockDuration = finalLockDuration;
  
  // Initialize penalty duration.
  // If FIXED, we know it now. If RANDOM, it's calculated on Abort.
  if (_deterrents.enableRewardCode && _deterrents.penaltyStrategy == DETERRENT_FIXED) {
    _timers.penaltyDuration = _deterrents.rewardPenalty;
  } else {
    _timers.penaltyDuration = 0; 
  }
  
  _timers.lockRemaining = 0; 
  _timers.penaltyRemaining = 0;

  for (int i = 0; i < MAX_CHANNELS; i++) {
    _timers.channelDelays[i] = _activeConfig.channelDelays[i];
  }

  char logBuf[256];
  char timeStr[64];
  formatSecondsInternal(finalLockDuration, timeStr, sizeof(timeStr));
  snprintf(logBuf, sizeof(logBuf), "Total Lock Time: %s (Base: %u + Rules)", timeStr, baseDuration);
  logKeyValue("Session", logBuf);

  if (config.triggerStrategy == STRAT_BUTTON_TRIGGER) {
    _timers.triggerTimeout = _sysDefaults.armedTimeoutSeconds;
    logKeyValue("Session", "Waiting for Trigger...");
  } else {
    logKeyValue("Session", "Auto Sequence Started.");
  }

  // 7. Transition
  // This handles Logging, Safety Profile, and Saving
  changeState(ARMED); 

  return 200;
}

void SessionEngine::enterLockedState(const char *source) {
  char logBuf[100];
  snprintf(logBuf, sizeof(logBuf), "Source: %s", source);
  logKeyValue("Session", logBuf);
  
  _timers.lockRemaining = _timers.lockDuration;
  
  // Transition
  changeState(LOCKED);
}

void SessionEngine::completeSession() {
  DeviceState previousState = _state;

  // Transition FIRST to disarm watchdogs/failsafes immediately
  changeState(COMPLETED);

  // Then process business logic
  if (previousState == LOCKED) {
    // DELEGATE: Rules update Stats (Streaks, Debt clear)
    _rules.onCompletion(_stats, _deterrents);

    char logBuf[100];
    snprintf(logBuf, sizeof(logBuf), "%-20s : %u", "New Streak", _stats.streaks);
    logKeyValue("Session", logBuf);
    snprintf(logBuf, sizeof(logBuf), "%-20s : %u", "Total Completed", _stats.completed);
    logKeyValue("Session", logBuf);
  } else if (previousState == ABORTED) {
    logKeyValue("Session", "Penalty time served.");
  }

  // Reset Timers
  _timers.lockRemaining = 0;
  _timers.penaltyRemaining = 0;
  _timers.testRemaining = 0;
  _timers.triggerTimeout = 0;
  for (int i = 0; i < MAX_CHANNELS; i++) {
    _activeConfig.channelDelays[i] = 0;
    _timers.channelDelays[i] = 0;
  }
  
  // Save again to capture the stats update
  _hal.saveState(_state, _timers, _stats);
}

void SessionEngine::abort(const char *source) {
  char logBuf[100];
  snprintf(logBuf, sizeof(logBuf), "Abort Source: %s", source);
  logKeyValue("Session", logBuf);

  if (_state == LOCKED) {
    // DELEGATE: Rules determine consequences
    AbortConsequences consequences = _rules.onAbort(_stats, _deterrents, _presets, _hal);

    // Log Consequences
    if (_deterrents.enablePaybackTime) {
         char timeStr[64];
         formatSecondsInternal(_stats.paybackAccumulated, timeStr, sizeof(timeStr));
         snprintf(logBuf, sizeof(logBuf), "Payback Added. Total Debt: %s", timeStr);
         logKeyValue("Rules", logBuf);
    }

    if (consequences.enterPenaltyBox) {
      _timers.penaltyDuration = consequences.penaltyDuration; // Ensure duration is set
      _timers.penaltyRemaining = consequences.penaltyDuration; 
      _timers.lockRemaining = 0;
      
      snprintf(logBuf, sizeof(logBuf), "Penalty Enforced: %u s", consequences.penaltyDuration);
      logKeyValue("Rules", logBuf);
      
      // Transition to Penalty
      changeState(ABORTED);
    } else {
      logKeyValue("Rules", "No Penalty Box enforced.");
      _timers.lockRemaining = 0;
      _timers.penaltyRemaining = 0;
      
      // Transition to Complete
      changeState(COMPLETED);
    }

  } else if (_state == ARMED) {
    resetToReady(false); // resetToReady calls changeState
  } else if (_state == TESTING) {
    stopTest(); // stopTest calls changeState
  }
}

void SessionEngine::petWatchdog() {
  if (requiresKeepAlive(_state)) {
      _lastKeepAliveTime = _hal.getMillis();
      if (_currentKeepAliveStrikes > 0) {
           char logBuf[100];
           snprintf(logBuf, sizeof(logBuf), "Keep-Alive Signal. Resetting %d strikes.", _currentKeepAliveStrikes);
           logKeyValue("Session", logBuf);
      }
      _currentKeepAliveStrikes = 0;
  }
}

int SessionEngine::startTest() {
  // Safety Check
  if (!_isHardwarePermitted) {
      logKeyValue("Session", "Test Failed: Safety Interlock not engaged.");
      return 412;
  }

  if (_state != READY) return 409;
  _timers.testRemaining = _sysDefaults.testModeDuration;
  changeState(TESTING);
  return 200;
}

void SessionEngine::stopTest() {
  logKeyValue("Session", "Stopping test session.");
  _timers.testRemaining = 0;
  changeState(READY);
}

void SessionEngine::handleReboot() {
  // Special Case: Reboot Recovery.
  // We don't always use changeState() because we aren't necessarily *changing* state,
  // we are restoring it. However, we MUST ensure the safety profile is applied.

  switch (_state) {
  case LOCKED:
  case ARMED:
    logKeyValue("Session", "Reboot detected during active session. Aborting...");
    abort("Reboot"); // Abort triggers changeState internally
    break;
  case TESTING:
    logKeyValue("Session", "Loaded TESTING state. Resetting to READY.");
    resetToReady(true);
    break;
  case COMPLETED:
    logKeyValue("Session", "Loaded COMPLETED state. Resetting to READY.");
    resetToReady(true);
    break;
  default:
    logKeyValue("Session", "Resuming in-progress state.");
    // Manual re-application of safety profile for resumed states (e.g. ABORTED penalty)
    applyStateSafetyProfile();
    break;
  }
}

/**
 * Generates a unique reward code by checking against history.
 * Ensures strict uniqueness of the Checksum to prevent duplicates.
 */
void SessionEngine::rotateAndGenerateReward() {
    // 1. Shift History
    for (int i = REWARD_HISTORY_SIZE - 1; i > 0; i--) {
      _rewardHistory[i] = _rewardHistory[i - 1];
    }

    const char chars[] = "UDLR";
    bool collision = true;
    int safetyCounter = 0;

    // 2. Loop until a unique checksum is found
    while (collision && safetyCounter < 50) {
        safetyCounter++;
        collision = false;

        // Generate Code
        for (int i = 0; i < REWARD_CODE_LENGTH; ++i) {
            // getRandom(0, 3) implies inclusive range 0..3
            _rewardHistory[0].code[i] = chars[_hal.getRandom(0, 3)];
        }
        _rewardHistory[0].code[REWARD_CODE_LENGTH] = '\0';

        // Calculate Checksum
        LogicUtils::calculateChecksum(_rewardHistory[0].code, _rewardHistory[0].checksum);

        // Check History for collisions
        for (int i = 1; i < REWARD_HISTORY_SIZE; i++) {
            if (strlen(_rewardHistory[i].checksum) > 0) {
                if (strncmp(_rewardHistory[0].checksum, _rewardHistory[i].checksum, REWARD_CHECKSUM_LENGTH) == 0) {
                    collision = true;
                    break;
                }
            }
        }
    }

    if (safetyCounter >= 50) {
        logKeyValue("Session", "Warning: Reward Generation timed out (Potential collision accepted).");
    }

    // Log the result
    char codeSnippet[9];
    strncpy(codeSnippet, _rewardHistory[0].code, 8);
    codeSnippet[8] = '\0';

    char logBuf[128];
    snprintf(logBuf, sizeof(logBuf), "New Reward Code Generated");
    logKeyValue("Session", logBuf);
    snprintf(logBuf, sizeof(logBuf), " %-20s : %s...", "Code Snippet", codeSnippet);
    logKeyValue("Session", logBuf);
}

void SessionEngine::resetToReady(bool generateNewCode) {
  // Reset Timers
  _timers.lockDuration = 0;
  _timers.penaltyDuration = 0;
  _timers.lockRemaining = 0;
  _timers.penaltyRemaining = 0;
  _timers.testRemaining = 0;
  _timers.triggerTimeout = 0;
  _activeConfig.hideTimer = false;

  for (int i = 0; i < MAX_CHANNELS; i++) {
    _activeConfig.channelDelays[i] = 0;
    _timers.channelDelays[i] = 0;
  }

  if (generateNewCode) {
    rotateAndGenerateReward();
  } else {
    logKeyValue("Session", "Preserving existing reward code.");
  }
  
  // Transition
  changeState(READY);
}

// Logic to trigger from external sources (e.g. API).
// Hardware triggering is done via Polling in tick().
void SessionEngine::trigger(const char *source) {
  if (_state == ARMED && _activeConfig.triggerStrategy == STRAT_BUTTON_TRIGGER) {
      enterLockedState(source);
  } else if (_state == TESTING) {
      logKeyValue("Session", "Trigger ignored: Currently in Hardware Test.");
  }
}

// =================================================================================
// SECTION: WATCHDOGS (Internal implementation)
// =================================================================================

void SessionEngine::armKeepAliveWatchdog() {
  _lastKeepAliveTime = _hal.getMillis();
  _currentKeepAliveStrikes = 0;
  logKeyValue("Session", "Keep-Alive UI Watchdog ARMED");
}

void SessionEngine::disarmKeepAliveWatchdog() {
  _lastKeepAliveTime = 0;       
  _currentKeepAliveStrikes = 0; 
  logKeyValue("Session", "Keep-Alive UI Watchdog DISARMED");
}

bool SessionEngine::checkKeepAliveWatchdog() {
  if (_lastKeepAliveTime == 0) return false;

  unsigned long elapsed = _hal.getMillis() - _lastKeepAliveTime;
  int calculatedStrikes = elapsed / _sysDefaults.keepAliveInterval;

  if (calculatedStrikes > _currentKeepAliveStrikes) {
    _currentKeepAliveStrikes = calculatedStrikes;
    char logBuf[100];
    if (_currentKeepAliveStrikes >= (int)_sysDefaults.keepAliveMaxStrikes) {
      snprintf(logBuf, sizeof(logBuf), "Keep-Alive UI Watchdog: Strike %d/%u! ABORTING.", _currentKeepAliveStrikes,
               _sysDefaults.keepAliveMaxStrikes);
      logKeyValue("Session", logBuf);
      abort("UI Watchdog Strikeout");
      return true;
    } else {
      snprintf(logBuf, sizeof(logBuf), "Keep-Alive UI Watchdog Missed check. Strike %d/%u", _currentKeepAliveStrikes,
               _sysDefaults.keepAliveMaxStrikes);
      logKeyValue("Session", logBuf);
    }
  }
  return false;
}

/**
 * Calculates the Safe Hardware Mask based on current state and timers.
 * This ensures continuous safety enforcement.
 */
uint8_t SessionEngine::calculateSafetyMask() {
    uint8_t mask = 0x00;
    if (_state == LOCKED || _state == TESTING) {
        // In fully locked/testing mode, we request ALL logical channels ON.
        // The Hardware layer will filter this against the Physical Installed Mask.
        mask = 0x0F; // 1111
    } 
    else if (_state == ARMED) {
        // In Countdown, specific channels turn on as their individual delays expire.
        for (int i = 0; i < MAX_CHANNELS; i++) {
            if (_timers.channelDelays[i] == 0) {
                mask |= (1 << i);
            }
        }
    }
    // READY, ABORTED, COMPLETED, VALIDATING -> Mask stays 0x00 (Safe)

    return mask;
}