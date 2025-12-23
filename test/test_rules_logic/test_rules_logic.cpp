/*
 * File: test/test_rules_logic/test_rules_logic.cpp
 * Description: Logic-only tests for StandardRules. 
 * Verifies Math, Debt application, and Random/Fixed strategies.
 */
#include <unity.h>
#include "StandardRules.h"
#include "MockSessionHAL.h"
#include "Types.h"

// --- Test Constants ---

const SessionPresets presets = { 
    300, 600,   // Short
    900, 1800,  // Medium
    3600, 7200, // Long
    14400,      // maxSessionDuration
    10          // minSessionDuration
};

// Base config: Fixed Strategy
const DeterrentConfig configFixed = { 
    true,               // enableStreaks
    
    true,               // enableRewardCode
    DETERRENT_FIXED,    // rewardPenaltyStrategy
    300, 900,           // rewardPenaltyMin, rewardPenaltyMax
    500,                // rewardPenalty (Fixed Value)
    
    true,               // enablePaybackTime
    DETERRENT_FIXED,    // paybackTimeStrategy
    60, 120,            // paybackTimeMin, paybackTimeMax
    60                  // paybackTime
};

// --- Setup ---
void setUp(void) {}
void tearDown(void) {}

// --- Tests ---

void test_start_request_applies_debt(void) {
    StandardRules rules;
    SessionStats stats = {0};
    stats.paybackAccumulated = 100;

    // Request 600s + 100s Debt = 700s
    // Note: processStartRequest does not force rounding on the sum sum itself;
    // it relies on inputs being valid or downstream handling.
    uint32_t duration = rules.processStartRequest(600, presets, configFixed, stats);
    
    TEST_ASSERT_EQUAL_UINT32(700, duration);
}

void test_start_request_clamps_to_profile_max(void) {
    StandardRules rules;
    SessionStats stats = {0};
    
    // Profile Limit is 14400.
    // Request is 20000.
    // Logic should clamp this to 14400.
    uint32_t duration = rules.processStartRequest(20000, presets, configFixed, stats);
    
    TEST_ASSERT_EQUAL_UINT32(14400, duration);
}

void test_start_request_rejects_below_minimum(void) {
    StandardRules rules;
    SessionStats stats = {0};

    // Request 5s (Below minSessionDuration of 10s)
    uint32_t duration = rules.processStartRequest(5, presets, configFixed, stats);

    TEST_ASSERT_EQUAL_UINT32(0, duration); // Should be rejected
}

void test_abort_strategy_fixed_rounds_up(void) {
    MockSessionHAL hal; // Spy
    StandardRules rules;
    SessionStats stats = {0};

    // Act
    // Config has 500s fixed penalty.
    // 500 / 60 = 8.33 minutes.
    // Logic should round UP to 9 minutes (540s).
    AbortConsequences result = rules.onAbort(stats, configFixed, presets, hal);

    // Assert
    TEST_ASSERT_TRUE(result.enterPenaltyBox);
    TEST_ASSERT_EQUAL_UINT32(540, result.penaltyDuration); 
}

void test_abort_strategy_random_rounds_up(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionStats stats = {0};

    // Setup: Random Strategy
    DeterrentConfig configRandom = configFixed;
    configRandom.rewardPenaltyStrategy = DETERRENT_RANDOM;

    // Set range to 300-400. 
    // MockHAL returns average: (300+400)/2 = 350.
    // 350 / 60 = 5.833 -> Rounds up to 6 mins (360s).
    configRandom.rewardPenaltyMin = 300;
    configRandom.rewardPenaltyMax = 400;
    
    // Act
    AbortConsequences result = rules.onAbort(stats, configRandom, presets, hal);

    // Assert
    TEST_ASSERT_TRUE(result.enterPenaltyBox);
    TEST_ASSERT_EQUAL_UINT32(360, result.penaltyDuration); 
}

void test_abort_applies_random_payback_rounds_up(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionStats stats = {0};

    // Setup: Enable Random Payback Strategy
    DeterrentConfig configRandomPayback = configFixed; 
    configRandomPayback.enablePaybackTime = true;
    configRandomPayback.paybackTimeStrategy = DETERRENT_RANDOM;

    // Payback Range in config: 60 (min), 120 (max)
    // MockHAL calculates random as (min+max)/2 => (60+120)/2 = 90.
    // 90s -> Rounds up to 120s (2 mins).
    
    // Act: Abort
    rules.onAbort(stats, configRandomPayback, presets, hal);

    // Assert: 120 seconds added to debt (Rounded up from 90)
    TEST_ASSERT_EQUAL_UINT32(120, stats.paybackAccumulated);
}

void test_abort_clamps_penalty_to_max(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionStats stats = {0};

    // Setup: Huge penalty config
    DeterrentConfig configHuge = configFixed;
    configHuge.rewardPenalty = 20000; // > 14400 Max

    // Act
    AbortConsequences result = rules.onAbort(stats, configHuge, presets, hal);

    // Assert
    TEST_ASSERT_EQUAL_UINT32(14400, result.penaltyDuration);
}

void test_completion_clamps_debt_at_zero(void) {
    StandardRules rules;
    SessionStats stats = {0};
    stats.paybackAccumulated = 100; // Small debt

    // Setup: Simulate a session where 200s was attributed to debt payment
    SessionTimers timers = {0};
    timers.potentialDebtServed = 200; 

    DeterrentConfig ignored = {0}; 
    ignored.enableStreaks = true; 

    // Act
    rules.onCompletion(stats, timers, ignored);

    // Assert: Clamped to 0 (No negative debt)
    TEST_ASSERT_EQUAL_UINT32(0, stats.paybackAccumulated);
}

void test_completion_reduces_debt_fairly(void) {
    StandardRules rules;
    SessionStats stats = {0};
    stats.paybackAccumulated = 36000; // 10h Debt

    // Simulate a timer where 3h (10800s) was attributed to debt
    SessionTimers timers = {0};
    timers.potentialDebtServed = 10800; 

    DeterrentConfig deterrents = {0}; 
    deterrents.enableStreaks = true; 

    // Act: Complete Successfully
    rules.onCompletion(stats, timers, deterrents);

    // Assert: Debt is 36000 - 10800 = 25200 (7h)
    TEST_ASSERT_EQUAL_UINT32(25200, stats.paybackAccumulated);
}

// --- Runner ---
int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_start_request_applies_debt);
    RUN_TEST(test_start_request_clamps_to_profile_max);
    RUN_TEST(test_start_request_rejects_below_minimum);
    
    RUN_TEST(test_abort_strategy_fixed_rounds_up);
    RUN_TEST(test_abort_strategy_random_rounds_up);
    RUN_TEST(test_abort_applies_random_payback_rounds_up);
    RUN_TEST(test_abort_clamps_penalty_to_max);
    
    RUN_TEST(test_completion_clamps_debt_at_zero);
    RUN_TEST(test_completion_reduces_debt_fairly);
    
    return UNITY_END();
}