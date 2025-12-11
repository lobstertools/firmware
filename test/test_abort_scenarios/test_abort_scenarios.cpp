/*
 * File: test/test_abort_scenarios/test_abort_scenarios.cpp
 * Description: Verifies Abort logic with and without deterrents enabled.
 */
#include <unity.h>
#include "Session.h"
#include "MockSessionHAL.h"
#include "StandardRules.h"

// --- Shared Constants ---
const SystemDefaults defaults = { 5, 10, 240, 10000, 4, 5, 30000, 3, 60 };

const SessionPresets presets = { 
    300, 600, 900, 1800, 3600, 7200, // Ranges
    300, 900,                        // Penalty Range
    60, 120,                         // Payback Range
    14400, 14400, 3600,              // Limits (Ceilings)
    10, 10, 10                       // Absolute Minimums (Floors)
};

// --- Helper ---
void engageSafetyInterlock(MockSessionHAL& hal, SessionEngine& engine) {
    hal.setSafetyInterlock(true);
    engine.tick(); 
    hal.advanceTime(11000); 
    engine.tick(); 
}

void setUp(void) {}
void tearDown(void) {}

// ============================================================================
// TEST CASES
// ============================================================================

void test_abort_with_reward_penalty_enforces_penalty_box(void) {
    // 1. Setup: Strict Deterrents
    DeterrentConfig strictConfig = { 
        true,               // enableStreaks
        true,               // enableRewardCode (Triggers Penalty Box)
        DETERRENT_FIXED,    // penaltyStrategy
        300,                // rewardPenalty (5 mins)
        true,               // enablePaybackTime
        DETERRENT_FIXED,    // paybackStrategy
        60                  // paybackTime
    };

    MockSessionHAL hal;
    StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, strictConfig);
    engageSafetyInterlock(hal, engine);

    // 2. Start and Lock Session
    SessionConfig cfg = {};
    cfg.durationType = DUR_FIXED;
    cfg.fixedDuration = 600; // 10 mins
    cfg.triggerStrategy = STRAT_AUTO_COUNTDOWN;
    
    engine.startSession(cfg);
    engine.tick(); // Locked
    TEST_ASSERT_EQUAL(LOCKED, engine.getState());

    // 3. Fake some progress (Streak accumulation would happen on completion, but we verify reset on abort)
    // We can manually inject a streak to ensure it gets wiped.
    SessionStats stats = {0};
    stats.streaks = 5;
    engine.loadStats(stats);

    // 4. ACT: Abort
    engine.abort("User Panic");

    // 5. ASSERT
    // A. State should be ABORTED (Penalty Box)
    TEST_ASSERT_EQUAL(ABORTED, engine.getState());

    // B. Penalty Timer should be set to 300s
    TEST_ASSERT_EQUAL_UINT32(300, engine.getTimers().penaltyRemaining);
    TEST_ASSERT_EQUAL_UINT32(300, engine.getTimers().penaltyDuration);

    // C. Payback Debt should increase by 60s
    TEST_ASSERT_EQUAL_UINT32(60, engine.getStats().paybackAccumulated);

    // D. Streak should be wiped
    TEST_ASSERT_EQUAL_UINT32(0, engine.getStats().streaks);
    
    // E. Abort counter incremented
    TEST_ASSERT_EQUAL_UINT32(1, engine.getStats().aborted);
}

void test_abort_without_reward_penalty_skips_penalty_box(void) {
    // 1. Setup: No Deterrents (Safe Mode)
    DeterrentConfig safeConfig = { 
        false,                 // enableStreaks (No reset)
        false,                 // enableRewardCode (NO Penalty Box)
        DETERRENT_FIXED, 300,  // Ignored
        false,                 // enablePaybackTime (No Debt)
        DETERRENT_FIXED, 60    // Ignored
    };

    MockSessionHAL hal;
    StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, safeConfig);
    engageSafetyInterlock(hal, engine);

    // 2. Start and Lock
    SessionConfig cfg = {};
    cfg.durationType = DUR_FIXED;
    cfg.fixedDuration = 600;
    cfg.triggerStrategy = STRAT_AUTO_COUNTDOWN;
    
    engine.startSession(cfg);
    engine.tick(); 
    TEST_ASSERT_EQUAL(LOCKED, engine.getState());

    // Inject stats to ensure they are preserved
    SessionStats stats = {0};
    stats.streaks = 10;
    stats.paybackAccumulated = 0;
    engine.loadStats(stats);

    // 3. ACT: Abort
    engine.abort("User Cancel");

    // 4. ASSERT
    // A. State should go directly to COMPLETED (No Penalty Box needed)
    TEST_ASSERT_EQUAL(COMPLETED, engine.getState());

    // B. Timers cleared
    TEST_ASSERT_EQUAL_UINT32(0, engine.getTimers().penaltyRemaining);
    TEST_ASSERT_EQUAL_UINT32(0, engine.getTimers().lockRemaining);

    // C. Stats Preserved (No Penalty)
    TEST_ASSERT_EQUAL_UINT32(0, engine.getStats().paybackAccumulated); // No Debt added
    TEST_ASSERT_EQUAL_UINT32(10, engine.getStats().streaks); // Streak NOT wiped
    
    // Note: StandardRules does NOT increment 'aborted' count if streaks are disabled,
    // as it treats it as a non-consequential event, or we can check implementation.
    // Looking at StandardRules.h: "if (deterrents.enableStreaks) { stats.aborted++; }"
    // So aborted count should NOT increment here.
    TEST_ASSERT_EQUAL_UINT32(0, engine.getStats().aborted);
}

// ============================================================================
// RUNNER
// ============================================================================
int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_abort_with_reward_penalty_enforces_penalty_box);
    RUN_TEST(test_abort_without_reward_penalty_skips_penalty_box);
    return UNITY_END();
}