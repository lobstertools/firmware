/* =================================================================================
 * File:      lib/SessionEngine/StandardRules.h
 * =================================================================================
 */
#pragma once
#include "SessionRules.h"
#include "SessionContext.h"
#include <stdio.h> 

class StandardRules : public ISessionRules {
public:
    // --- 1. Start Request Logic ---
    uint32_t processStartRequest(uint32_t baseDuration, 
                                 const SessionPresets& presets,
                                 const DeterrentConfig& deterrents,
                                 SessionStats& stats) override 
    {
        // A. Validate Input against Profile Minimums (Sanity)
        // This prevents 0 or tiny values that might glitch the timer logic
        if (baseDuration < presets.minSessionDuration) {
            return 0; // Invalid
        }

        // B. Apply Payback Debt
        uint32_t finalDuration = baseDuration;
        if (deterrents.enablePaybackTime && stats.paybackAccumulated > 0) {
            finalDuration += stats.paybackAccumulated;
        }

        // C. Clamp to Profile Maximum
        if (finalDuration > presets.maxSessionDuration) {
             finalDuration = presets.maxSessionDuration;
        }
        
        return finalDuration;
    }

    // --- 2. Tick Logic ---
    void onTickLocked(SessionStats& stats) override {
        stats.totalLockedTime++;
    }

    // --- 3. Completion Logic ---
    void onCompletion(SessionStats& stats, const SessionTimers& timers, const DeterrentConfig& deterrents) override {
        if (stats.paybackAccumulated >= timers.debtServed) {
            stats.paybackAccumulated -= timers.debtServed;
        } else {
            stats.paybackAccumulated = 0; // Safety clamp
        }
        stats.completed++;
        if (deterrents.enableStreaks) {
            stats.streaks++;
        }
    }

    // --- 4. Abort Logic ---
    AbortConsequences onAbort(SessionStats& stats, 
                              const DeterrentConfig& deterrents,
                              const SessionPresets& presets,
                              ISessionHAL& hal) override 
    {
        AbortConsequences result = { false, 0 };

        if (deterrents.enableStreaks) {
            stats.streaks = 0;
            stats.aborted++;
        }

        if (deterrents.enablePaybackTime) {
            uint32_t paybackToAdd = 0;
            if (deterrents.paybackTimeStrategy == DETERRENT_FIXED) {
                paybackToAdd = deterrents.paybackTime;
            } else {
                uint32_t minP = deterrents.paybackTimeMin;
                uint32_t maxP = deterrents.paybackTimeMax;
                if (minP > maxP) { uint32_t t = minP; minP = maxP; maxP = t; }
                paybackToAdd = hal.getRandom(minP, maxP);
            }
            stats.paybackAccumulated += paybackToAdd;
        }

        if (deterrents.enableRewardCode) {
            result.enterPenaltyBox = true;
            if (deterrents.rewardPenaltyStrategy == DETERRENT_FIXED) {
                result.penaltyDuration = deterrents.rewardPenalty;
            } else {
                uint32_t minP = deterrents.rewardPenaltyMin;
                uint32_t maxP = deterrents.rewardPenaltyMax;
                if (minP > maxP) { uint32_t t = minP; minP = maxP; maxP = t; }
                result.penaltyDuration = hal.getRandom(minP, maxP);
            }
        }
        return result;
    }
};