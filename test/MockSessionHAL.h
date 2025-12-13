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
    uint32_t failsafeDuration = 0;
    bool keepAliveArmed = false;
    
    // Storage Spy
    DeviceState savedState;
    SessionTimers savedTimers;
    SessionStats savedStats;

    // Simulation Variables
    uint32_t currentMillis = 1000; 
    std::vector<std::string> logs;
    
    // Safety Switch State
    bool _mockSafetyRaw = false; 
    bool _mockSafetyValid = false;

    // Input Event States
    bool _triggerActionPending = false;
    bool _abortActionPending = false;
    bool _shortPressPending = false;

    // Network State
    bool _networkProvisioningRequested = false;
    bool _enteredProvisioningMode = false;

    // --- Helpers for Test Control ---

    void setSafetyInterlock(bool engaged) {
        _mockSafetyRaw = engaged;
        _mockSafetyValid = engaged;
    }

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

    // --- Safety Interlock ---
    
    bool isSafetyInterlockValid() override {
        return _mockSafetyValid;
    }

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
        failsafeDuration = seconds;
    }

    void disarmFailsafeTimer() override {
        failsafeArmed = false;
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

    uint32_t getRandom(uint32_t min, uint32_t max) override {
        return (min + max) / 2; // Deterministic average
    }
};