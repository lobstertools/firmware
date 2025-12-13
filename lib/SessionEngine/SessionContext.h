/*
 * =================================================================================
 * File:      lib/SessionEngine/SessionContext.h
 * Description: Abstraction layer (HAL) for Hardware, Storage, and Logging.
 * =================================================================================
 */
#pragma once
#include "Types.h"

class ISessionHAL {
public:
    virtual ~ISessionHAL() {}

    // --- Hardware Control ---
    virtual void setHardwareSafetyMask(uint8_t mask) = 0;
    
    // Check if a specific channel is physically enabled (e.g. via DIP switch or Provisioning)
    virtual bool isChannelEnabled(int channelIndex) const = 0;

    // --- Input Events ---
    // Returns true if the physical trigger (e.g. Double Click) was activated
    // since the last check. Reads and clears the flag (Consume).
    virtual bool checkTriggerAction() = 0;

    // Returns true if the physical abort (e.g. Long Press) was activated.
    // Reads and clears the flag (Consume).
    virtual bool checkAbortAction() = 0;

    // Returns true if a short press (Click) was activated (e.g. for Test Mode)
    // Reads and clears the flag (Consume).
    virtual bool checkShortPressAction() = 0;

    // --- Safety Interlock ---
    
    // Returns true if hardware is physically safe OR within the allowed grace period.
    // Returns false if the interlock is definitely broken/disconnected (timeout exceeded).
    virtual bool isSafetyInterlockValid() = 0;

    // Returns true if the external safety/abort button is electrically connected (Safe).
    // Returns false if disconnected (Unsafe). (Raw State)
    virtual bool isSafetyInterlockEngaged() = 0;

    // --- Network ---
    // Returns true if the network layer has failed retries and needs reconfiguration.
    virtual bool isNetworkProvisioningRequested() = 0;

    // Commands the system to enter blocking provisioning mode.
    // This function is expected to NOT return (or block until reboot).
    virtual void enterNetworkProvisioning() = 0;

    // --- Safety Watchdogs ---
    virtual void setWatchdogTimeout(uint32_t seconds) = 0;
    virtual void armFailsafeTimer(uint32_t seconds) = 0;
    virtual void disarmFailsafeTimer() = 0;

    // --- Storage ---
    virtual void saveState(const DeviceState& state, const SessionTimers& timers, const SessionStats& stats) = 0;
    
    // --- Logging ---
    virtual void log(const char* message) = 0;
    
    // --- Utils ---
    virtual unsigned long getMillis() = 0; 
    virtual uint32_t getRandom(uint32_t min, uint32_t max) = 0;
};