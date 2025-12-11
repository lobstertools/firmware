/*
 * =================================================================================
 * File:      lib/SessionEngine/SessionRules.h
 * Description: Interface for "Game Logic" policies. Decouples math/consequences
 * from the Session State Machine.
 * =================================================================================
 */
#pragma once
#include "Types.h"

// FIX: Forward declaration so the compiler knows this type exists
class ISessionHAL;

struct AbortConsequences {
    bool enterPenaltyBox;
    uint32_t penaltyDuration;
};

class ISessionRules {
public:
    virtual ~ISessionRules() {}

    /**
     * Called when starting a session.
     * Responsibility: Apply accumulated debt, clamp to safety limits, validate ranges.
     * @return Final duration in seconds, or 0 if the session start should be rejected.
     */
    virtual uint32_t processStartRequest(uint32_t baseDuration, 
                                         const SessionPresets& presets,
                                         const DeterrentConfig& deterrents,
                                         SessionStats& stats) = 0;

    /**
     * Called every second while LOCKED.
     * Responsibility: Update time tracking stats.
     */
    virtual void onTickLocked(SessionStats& stats) = 0;

    /**
     * Called upon successful completion (timer reached 0).
     * Responsibility: Update streaks, clear debt, increment counters.
     */
    virtual void onCompletion(SessionStats& stats, 
                              const DeterrentConfig& deterrents) = 0;

    /**
     * Called upon Abort/Emergency Stop.
     * Responsibility: Apply penalties (reset streak, add debt), decide if Penalty Box is required.
     */
    virtual AbortConsequences onAbort(SessionStats& stats, 
                                      const DeterrentConfig& deterrents,
                                      const SessionPresets& presets,
                                      ISessionHAL& hal) = 0;
};