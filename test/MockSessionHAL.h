/*
 * File: test/MockSessionHAL.h
 * Description: A "Spy" implementation of the HAL for Native Unit Tests. 
 */
#pragma once
#include "SessionContext.h"
#include <string>
#include <vector>
#include <stdio.h>
#include <cstring>

class MockSessionHAL : public ISessionHAL {
public:
    // --- Spy Variables ---
    uint8_t lastSafetyMask = 0xFF; 
    uint32_t lastWatchdogTimeout = 0;
    bool failsafeArmed = false;
    uint32_t lastFailsafeArmedSeconds = 0;
    uint32_t failsafeDuration = 0;
    bool keepAliveArmed = false;
    
    // Storage Spy
    DeviceState savedState;
    SessionTimers savedTimers;
    SessionStats savedStats;

    // Simulation Variables
    uint32_t currentMillis = 1000; 
    std::vector<std::string> logs;
    
    // Hardware 
    bool _mockSafetyRaw = false; 
    bool _mockSafetyValid = false;
    uint8_t _mockChannelMask = 0x0F;
    bool ledEnabled = true;

    // Input Event States
    bool _triggerActionPending = false;
    bool _abortActionPending = false;
    bool _shortPressPending = false;

    // Network State
    bool _networkProvisioningRequested = false;
    bool _enteredProvisioningMode = false;
    
    // RNG State
    uint32_t _rngSeed = 12345;

    // --- Helpers for Test Control ---

    void setSafetyInterlock(bool engaged) {
        _mockSafetyRaw = engaged;
        // Default behavior for simple tests: Logical state matches physical immediately
        _mockSafetyValid = engaged;
    }

    // New helper specifically for the "Grace Period" test or timing simulations
    void setSafetyRawButKeepValid(bool rawState, bool validState) {
        _mockSafetyRaw = rawState;
        _mockSafetyValid = validState;
    }

    void simulateDoublePress() {
        _triggerActionPending = true;
    }

    void simulateLongPress() {
        _abortActionPending = true;
    }

    void simulateShortPress() {
        _shortPressPending = true;
    }

    void setNetworkProvisioningRequest(bool requested) {
        _networkProvisioningRequested = requested;
    }
    
    void advanceTime(uint32_t ms) {
        currentMillis += ms;
    }

    // --- ISessionHAL Implementation ---

    void setHardwareSafetyMask(uint8_t mask) override {
        lastSafetyMask = mask;
    }

    bool isChannelEnabled(int channelIndex) const override {
        if (channelIndex < 0 || channelIndex >= 4) return false;
        return (_mockChannelMask >> channelIndex) & 1;
    }

    void setChannelMask(uint8_t mask) {
        _mockChannelMask = mask;
    }

    void setLedEnabled(bool enabled) override {
        ledEnabled = enabled;
    }

    // --- Safety Interlock ---
    
    // The Engine now checks THIS to see if operation is permitted
    bool isSafetyInterlockValid() override {
        return _mockSafetyValid;
    }

    // Represents the raw physical state (for diagnostics)
    bool isSafetyInterlockEngaged() override {
        return _mockSafetyRaw;
    }

    // --- Input Events ---
    bool checkTriggerAction() override {
        if (_triggerActionPending) {
            _triggerActionPending = false; // Consume
            return true;
        }
        return false;
    }

    bool checkAbortAction() override {
        if (_abortActionPending) {
            _abortActionPending = false; // Consume
            return true;
        }
        return false;
    }

    bool checkShortPressAction() override {
        if (_shortPressPending) {
            _shortPressPending = false; // Consume
            return true;
        }
        return false;
    }

    // --- Network Management ---
    bool isNetworkProvisioningRequested() override {
        return _networkProvisioningRequested;
    }

    void enterNetworkProvisioning() override {
        _enteredProvisioningMode = true;
    }

    // --- Watchdogs ---
    void setWatchdogTimeout(uint32_t seconds) override {
        lastWatchdogTimeout = seconds;
    }

    void armFailsafeTimer(uint32_t seconds) override {
        failsafeArmed = true;
        lastFailsafeArmedSeconds = seconds;
        char buf[64];
        snprintf(buf, sizeof(buf), "MOCK: Failsafe ARMED %u", seconds);
        log(buf);
    }

    void disarmFailsafeTimer() override {
        failsafeArmed = false;
        lastFailsafeArmedSeconds = 0;
        log("MOCK: Failsafe DISARMED");
    }

    // --- Storage ---
    void saveState(const DeviceState& state, const SessionTimers& timers, const SessionStats& stats) override {
        savedState = state;
        savedTimers = timers;
        savedStats = stats;
    }

    // --- Logging ---
    void log(const char* message) override {
        logs.push_back(std::string(message));
    }

    // --- Time & Random ---
    unsigned long getMillis() override {
        return currentMillis;
    }

    /**
       * Uses a simple Linear Congruential Generator (LCG) for the reward code range (0-3)
     * to ensure the generated code varies, preventing collision loops.
     * Falls back to "Average" for other ranges (durations) to preserve existing test logic.
     */
    uint32_t getRandom(uint32_t min, uint32_t max) override {
        // Specific case for Reward Code Generation
        if (min == 0 && max == 3) {
            _rngSeed = _rngSeed * 1103515245 + 12345;
            return (_rngSeed / 65536) % 4;
        }
        
        // Default behavior for durations (Deterministic Average)
        return (min + max) / 2; 
    }
};