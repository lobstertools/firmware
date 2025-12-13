/*
 * =================================================================================
 * Project:   Lobster Lock - Self-Bondage Session Manager
 * File:      lib/SessionEngine/Session.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Description:
 * Header for the SessionEngine class.
 *
 * REFACTOR NOTES:
 * 1. Decoupled from Globals/Hardware via ISessionHAL.
 * 2. Decoupled from Game Logic via ISessionRules.
 * 3. Implements "Internal Event" pattern via changeState().
 * 4. Implements "Safety Interlock" to prevent operation when hardware is unsafe.
 * 5. Implements "Network Abstraction" to handle provisioning requests safely.
 * =================================================================================
 */
#pragma once
#include "Types.h"
#include "SessionContext.h"
#include "SessionRules.h"

class SessionEngine {
public:
    // Constructor requires HAL, Rules, and System Config
    SessionEngine(ISessionHAL& hal, 
                  ISessionRules& rules, 
                  const SystemDefaults& sysDefaults,
                  const SessionPresets& presets, 
                  const DeterrentConfig& deterrents);

    // --- Main Loop Tick ---
    void tick(); 

    // --- API Commands ---
    int startSession(const SessionConfig& config);
    int startTest();
    void stopTest();
    void abort(const char* source);
    void trigger(const char* source);
    void handleReboot(); 
    void petWatchdog();

    // --- State Accessors (Read-Only) ---
    DeviceState getState() const { return _state; }
    const SessionTimers& getTimers() const { return _timers; }
    const SessionStats& getStats() const { return _stats; }
    const SessionConfig& getActiveConfig() const { return _activeConfig; }
    const Reward* getRewardHistory() const { return _rewardHistory; }

    // --- Configuration Accessors ---
    const SessionPresets& getPresets() const { return _presets; }
    const DeterrentConfig& getDeterrents() const { return _deterrents; }

    // --- Safety Accessor ---
    // Returns true if the HAL reports the safety interlock is valid (physically safe or within grace period).
    bool isHardwarePermitted() const { return _hal.isSafetyInterlockValid(); }

    // --- State Setters (for Loading from NVS) ---
    void loadState(DeviceState s) { _state = s; }
    void loadTimers(SessionTimers t) { _timers = t; }
    void loadStats(SessionStats s) { _stats = s; }

    void printStartupDiagnostics();
    bool validateConfig(const DeterrentConfig& deterrents, const SessionPresets& presets) const;
    
private:
    // --- Dependencies ---
    ISessionHAL& _hal;
    ISessionRules& _rules;

    // --- Configuration ---
    SystemDefaults _sysDefaults;
    SessionPresets _presets;
    DeterrentConfig _deterrents;

    // --- Dynamic State ---
    DeviceState _state;
    SessionTimers _timers;
    SessionStats _stats;
    SessionConfig _activeConfig;
    Reward _rewardHistory[REWARD_HISTORY_SIZE];
    
    // --- Watchdog State ---
    unsigned long _lastKeepAliveTime;
    int _currentKeepAliveStrikes;

    // =========================================================================
    // SECTION: STATE TRANSITION SYSTEM (Internal Events)
    // =========================================================================
    
    void changeState(DeviceState newState);
    void applyStateSafetyProfile();

    // Declarative Properties
    bool isCriticalState(DeviceState s) const;   
    bool requiresFailsafe(DeviceState s) const;  
    bool requiresKeepAlive(DeviceState s) const; 

    // =========================================================================
    // SECTION: LOGIC HELPERS
    // =========================================================================
    
    void updateSafetyInterlock(); // Polls HAL for processed safety status
    void checkNetworkHealth();    // Polls network status

    void processAutoCountdown();
    void processButtonTriggerWait();
   
    uint32_t resolveBaseDuration(const SessionConfig &config);
    
    void enterLockedState(const char* source);
    void completeSession();
    
    void resetToReady(bool generateNewCode);

    void armKeepAliveWatchdog();
    void disarmKeepAliveWatchdog();
    bool checkKeepAliveWatchdog();

    void rotateAndGenerateReward();
    uint8_t calculateSafetyMask();

    // --- Formatting Utils ---
    void formatSecondsInternal(unsigned long totalSeconds, char *buffer, size_t size);
    void logKeyValue(const char *key, const char *value);
};