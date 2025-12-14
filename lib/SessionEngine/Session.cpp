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

    // Generate the initial reward code upon startup.
    // This ensures getRewardHistory() returns a valid code immediately in the READY state.
    rotateAndGenerateReward();
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

/**
 * Validates a specific Session Request.
 * Performs sanity checks on user inputs (Duration and Delays).
 */
bool SessionEngine::validateSessionConfig(const SessionConfig& config) const {
    
    // 1. Duration Sanity Checks
    if (config.durationType == DUR_FIXED) {
        // Fixed duration must be non-zero
        if (config.durationFixed == 0) return false;
    } 
    else if (config.durationType == DUR_RANDOM) {
        // Min must be strictly less than Max
        if (config.durationMin >= config.durationMax) return false;
        
        // Implicitly, Max must be greater than 0, which is covered by Min < Max if Min >= 0.
        // (uint32_t is always >= 0).
    }

    // 2. Channel Delay Checks
    // Limit: 1 Hour (3600 seconds) per channel
    const uint32_t MAX_DELAY_SEC = 3600;

    for (int i = 0; i < MAX_CHANNELS; i++) {
        // uint32_t is always >= 0, so we only check the upper bound
        if (config.channelDelays[i] > MAX_DELAY_SEC) {
            return false;
        }
    }

    return true;
}

/**
 * Unified Configuration Validator.
 * 1. Checks that Presets are logically sound (Min <= Max, Non-zero).
 * 2. Checks that Deterrents respect the Global Safety Limits defined in Presets.
 */
bool SessionEngine::validateConfig(const DeterrentConfig& deterrents, const SessionPresets& presets) const {
    
    // --- 1. Session Presets Validation ---
    
    // Global Limits must be sane
    if (presets.minSessionDuration == 0) return false;
    if (presets.minSessionDuration >= presets.maxSessionDuration) return false;

    // Absolute Hard Limit (2 Weeks)
    // 14 days * 24 hours * 3600 seconds = 1209600 seconds
    const uint32_t ABSOLUTE_MAX_SESSION_SEC = 1209600;
    if (presets.maxSessionDuration > ABSOLUTE_MAX_SESSION_SEC) return false;
    
    // Generators: Min <= Max
    if (presets.shortMin > presets.shortMax) return false;
    if (presets.mediumMin > presets.mediumMax) return false;
    if (presets.longMin > presets.longMax) return false;

    // --- 2. Deterrent Validation ---
    
    uint32_t globalMax = presets.maxSessionDuration;

    // Reward Code
    if (deterrents.enableRewardCode) {
        if (deterrents.rewardPenaltyStrategy == DETERRENT_FIXED) {
            // Fixed: Duration must be non-zero AND <= Global Max
            if (deterrents.rewardPenalty == 0) return false;
            if (deterrents.rewardPenalty > globalMax) return false; 
        } else {
            // Random: Min must be non-zero AND Min < Max AND Max <= Global Max
            if (deterrents.rewardPenaltyMin == 0) return false;
            if (deterrents.rewardPenaltyMin >= deterrents.rewardPenaltyMax) return false;
            if (deterrents.rewardPenaltyMax > globalMax) return false;
        }
    }

    // Payback Time
    if (deterrents.enablePaybackTime) {
        if (deterrents.paybackTimeStrategy == DETERRENT_FIXED) {
            // Fixed: Duration must be non-zero AND <= Global Max
            if (deterrents.paybackTime == 0) return false;
            if (deterrents.paybackTime > globalMax) return false;
        } else {
            // Random: Min must be non-zero AND Min < Max AND Max <= Global Max
            if (deterrents.paybackTimeMin == 0) return false;
            if (deterrents.paybackTimeMin >= deterrents.paybackTimeMax) return false;
            if (deterrents.paybackTimeMax > globalMax) return false;
        }
    }

    return true;
}

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

    // Safety Interlock - Query HAL directly
    bool isPermitted = _hal.isSafetyInterlockValid();
    snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Interlock Permitted", boolStr[isPermitted]);
    _hal.log(logBuf);

    // Watchdogs
    snprintf(logBuf, sizeof(logBuf), " %-25s : %d / %d", "Keep-Alive Strikes", 
             _currentKeepAliveStrikes, _sysDefaults.keepAliveMaxStrikes);
    _hal.log(logBuf);

    // -------------------------------------------------------------------------
    // SECTION: CONFIGURATION STATUS
    // -------------------------------------------------------------------------
    _hal.log(""); 
    _hal.log("[ CONFIGURATION STATUS ]");

    // RUN VALIDATION
    bool configValid = validateConfig(_deterrents, _presets);
    snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Self-Check", configValid ? "PASS" : "FAIL (INVALID CONFIG)");
    _hal.log(logBuf);

    if (!configValid) {
        _hal.log(" WARNING: System will reject session starts until configuration is fixed.");
    }

    // -------------------------------------------------------------------------
    // SECTION: CONFIGURATION LIMITS (PRESETS)
    // -------------------------------------------------------------------------
    _hal.log(""); 
    _hal.log("[ CONFIGURATION LIMITS ]");

    snprintf(logBuf, sizeof(logBuf), " %-25s : %u s", "Absolute Min Lock", _presets.minSessionDuration);
    _hal.log(logBuf);
    snprintf(logBuf, sizeof(logBuf), " %-25s : %u s", "Absolute Max Lock", _presets.maxSessionDuration);
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
            _deterrents.rewardPenaltyStrategy == DETERRENT_FIXED ? "FIXED" : "RANDOM");
        _hal.log(logBuf);

        if (_deterrents.rewardPenaltyStrategy == DETERRENT_FIXED) {
            snprintf(logBuf, sizeof(logBuf), " %-25s : %u s", "Base Penalty", _deterrents.rewardPenalty);
            _hal.log(logBuf);
        } else {
            snprintf(logBuf, sizeof(logBuf), " %-25s : %u - %u s", "Penalty Range", _deterrents.rewardPenaltyMin, _deterrents.rewardPenaltyMax);
            _hal.log(logBuf);
        }
    }

    snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Payback (Debt)", boolStr[_deterrents.enablePaybackTime]);
    _hal.log(logBuf);

    if (_deterrents.enablePaybackTime) {
        snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Payback Strategy", 
            _deterrents.paybackTimeStrategy == DETERRENT_FIXED ? "FIXED" : "RANDOM");
        _hal.log(logBuf);

        if (_deterrents.paybackTimeStrategy == DETERRENT_FIXED) {
            snprintf(logBuf, sizeof(logBuf), " %-25s : %u s", "Base Payback", _deterrents.paybackTime);
            _hal.log(logBuf);
        } else {
            snprintf(logBuf, sizeof(logBuf), " %-25s : %u - %u s", "Payback Range", _deterrents.paybackTimeMin, _deterrents.paybackTimeMax);
            _hal.log(logBuf);
        }
    }

    // -------------------------------------------------------------------------
    // SECTION: STATISTICS
    // -------------------------------------------------------------------------
    _hal.log(""); 
    _hal.log("[ HISTORY & STATS ]");

    snprintf(logBuf, sizeof(logBuf), " %-25s : %u", "Sessions Completed", _stats.completed);
    _hal.log(logBuf);

    if (_deterrents.enableStreaks) {
        snprintf(logBuf, sizeof(logBuf), " %-25s : %u", "Sessions Aborted", _stats.aborted);
        _hal.log(logBuf);
        snprintf(logBuf, sizeof(logBuf), " %-25s : %u", "Current Streak", _stats.streaks);
        _hal.log(logBuf);
    }
    
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
    return (s == LOCKED || s == TESTING);
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
        // Determine the intended duration based on state
        uint32_t targetDuration = _timers.lockDuration;
        if (_state == TESTING) targetDuration = _sysDefaults.testModeDuration;
        
        // Calculate the hardware safety tier (Logic moved here from HAL)
        // This ensures the hardware timer is always >= session duration, 
        // snapped to specific "safe" intervals (4h, 8h, etc.) to prevent infinite lock.
        uint32_t failsafeSeconds = calculateFailsafeDuration(targetDuration);
        
        // Arm the hardware timer with the calculated tier
        _hal.armFailsafeTimer(failsafeSeconds);
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
    _hal.saveState(_state, _timers, _stats, _activeConfig);
}

// =================================================================================
// SECTION: SAFETY INTERLOCK LOGIC
// =================================================================================

void SessionEngine::updateSafetyInterlock() {
    // 1. Delegate Logic to HAL
    // The HAL tracks stabilization and grace periods internally.
    bool isSafe = _hal.isSafetyInterlockValid();

    // 2. ENFORCE SAFETY (The "Kill Switch")
    // If HAL says "Invalid" (Timeout exceeded), we abort.
    if (!isSafe) {
        if (isCriticalState(_state)) {
            // Only log if this is a new event (prevent log spamming handled by state change)
            // The HAL's logging handles verbose debugging.
            logKeyValue("Safety", "Critical: Interlock invalid/disconnected.");
            abort("Safety Disconnect");
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
  
  for (size_t i = 0; i < MAX_CHANNELS; i++) {
    // Decrement Logic
    if (_timers.channelDelays[i] > 0) {
      allDelaysZero = false;
      _timers.channelDelays[i]--;
    }
  }

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

uint32_t SessionEngine::calculateFailsafeDuration(uint32_t baseSeconds) const {
  const uint32_t ONE_HOUR = 3600;
  // Safety Logic: Tiers
  // The timer MUST be longer than the session, but provide a hard ceiling
  // in case software crashes.
  const uint32_t SAFETY_TIERS[] = {
      4 * ONE_HOUR,   // Min safe tier
      8 * ONE_HOUR, 
      12 * ONE_HOUR, 
      24 * ONE_HOUR, 
      48 * ONE_HOUR, 
      168 * ONE_HOUR  // Max safe tier (1 week)
  };
  const int NUM_TIERS = sizeof(SAFETY_TIERS) / sizeof(SAFETY_TIERS[0]);

  // Select Tier (Smallest tier >= requested seconds)
  uint32_t armedSeconds = SAFETY_TIERS[NUM_TIERS - 1];
  for (int i = 0; i < NUM_TIERS; i++) {
    if (SAFETY_TIERS[i] >= baseSeconds) {
      armedSeconds = SAFETY_TIERS[i];
      break;
    }
  }
  return armedSeconds;
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
  updateSafetyInterlock(); // This will change state to ABORTED if safety is lost
  checkNetworkHealth();

  if (_hal.checkAbortAction()) {
      logKeyValue("Session", "Universal Abort Triggered (Hardware Input)");
      abort("Manual Long-Press");
  }

  // 2. Process Logic based on State (ONLY IF HARDWARE IS VALID)
  // If hardware is not permitted (disconnected/stabilizing), 
  // we pause all timer decrements.
  if (_hal.isSafetyInterlockValid()) {
      
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
        // Penalty only counts down if hardware is connected!
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
  } 

  // 3. ENFORCE HARDWARE SAFETY (The "Continuous Enforcement")
  // Calculate what the pins *should* be right now based on logic
  // If safety is invalid, calculateSafetyMask might return pins HIGH (if LOCKED), 
  // but ISessionHAL implementations should logically gate this. 
  // However, specifically setting it here ensures the logical intent is sent.
  uint8_t targetMask = calculateSafetyMask();
  _hal.setHardwareSafetyMask(targetMask);

  // 4. LED CONTROL
  bool shouldLedBeEnabled = true;
  if (_activeConfig.disableLED && _state == LOCKED) {
      shouldLedBeEnabled = false;
  }
  _hal.setLedEnabled(shouldLedBeEnabled);
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
    baseDuration = config.durationFixed;
  } else {
    uint32_t minVal = config.durationMin;
    uint32_t maxVal = config.durationMax;

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
    if (maxVal > _presets.maxSessionDuration) maxVal = _presets.maxSessionDuration;
    if (minVal > maxVal) minVal = maxVal; 
    if (maxVal < minVal) { uint32_t temp = maxVal; maxVal = minVal; minVal = temp; }
    if (minVal < _presets.minSessionDuration) minVal = _presets.minSessionDuration;
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
  if (!_hal.isSafetyInterlockValid()) {
    logKeyValue("Session", "Start Failed: Safety Interlock not valid.");
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

  // 3. Configuration Validation
  
  // A. Check System Settings (Presets/Deterrents)
  if (!validateConfig(_deterrents, _presets)) {
      logKeyValue("Session", "Start Failed: Invalid System Configuration (Presets or Deterrents).");
      return 400; // Bad Request
  }

  // B. Check Session Request (Input Sanity)
  if (!validateSessionConfig(config)) {
      logKeyValue("Session", "Start Failed: Invalid Session Config (Time/Delay limits).");
      return 400; // Bad Request
  }
  
  // 4. Determine Base Duration (Mechanism)
  uint32_t baseDuration = resolveBaseDuration(config);

  // 5. Delegate Policy Check to Rules Engine (Debt, Limits)
  uint32_t finalLockDuration = _rules.processStartRequest(baseDuration, _presets, _deterrents, _stats);

  if (finalLockDuration == 0) {
    logKeyValue("Session", "Start Failed: Duration Rejected by Rules (Out of Range)");
    return 400;
  }

  // 6. Commit State
  _activeConfig = config;
  _timers.lockDuration = finalLockDuration;
  
  // Initialize penalty duration.
  // If FIXED, we know it now. If RANDOM, it's calculated on Abort.
  if (_deterrents.enableRewardCode && _deterrents.rewardPenaltyStrategy == DETERRENT_FIXED) {
    _timers.penaltyDuration = _deterrents.rewardPenalty;
  } else {
    _timers.penaltyDuration = 0; 
  }
  
  _timers.lockRemaining = 0; 
  _timers.penaltyRemaining = 0;

  for (int i = 0; i < MAX_CHANNELS; i++) {
    if (config.triggerStrategy == STRAT_AUTO_COUNTDOWN && !_hal.isChannelEnabled(i)) {
         _timers.channelDelays[i] = 0;
    } else {
        _timers.channelDelays[i] = _activeConfig.channelDelays[i];
    }
  }

  char logBuf[256];
  char timeStr[64];
  formatSecondsInternal(finalLockDuration, timeStr, sizeof(timeStr));
  snprintf(logBuf, sizeof(logBuf), "Total Lock Time: %s (Base: %u + Rules)", timeStr, baseDuration);
  logKeyValue("Session", logBuf);

  if (config.triggerStrategy == STRAT_BUTTON_TRIGGER) {
    _timers.triggerTimeout = _sysDefaults.armedTimeout;
    logKeyValue("Session", "Waiting for Trigger...");
  } else {
    logKeyValue("Session", "Auto Sequence Started.");
    
    // Log initial delays once for enabled channels
    char debugDelayStr[64];
    snprintf(debugDelayStr, sizeof(debugDelayStr), "Delays: ");
    
    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (_hal.isChannelEnabled(i)) {
            char tmp[16];
            snprintf(tmp, sizeof(tmp), "[%d]%u ", i + 1, _timers.channelDelays[i]);
            strncat(debugDelayStr, tmp, sizeof(debugDelayStr) - strlen(debugDelayStr) - 1);
        }
    }
    logKeyValue("Session", debugDelayStr);
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
  _hal.saveState(_state, _timers, _stats, _activeConfig);
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
  if (!_hal.isSafetyInterlockValid()) {
      logKeyValue("Session", "Test Failed: Safety Interlock not valid.");
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
    snprintf(logBuf, sizeof(logBuf), "New Reward Code Generated: %s...", codeSnippet);
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