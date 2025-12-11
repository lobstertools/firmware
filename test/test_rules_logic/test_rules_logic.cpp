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
    300, 900,   // Penalty
    60, 120,    // Payback Range
    14400,      // Limit Lock Max (The Clamp Target)
    14400,      // Limit Penalty Max
    3600,       // Limit Payback Max
    // --- Absolute Minimums (Floors) ---
    10,         // minLockDuration
    10,         // minRewardPenaltyDuration
    10          // minPaybackTime
};

// Base config: Fixed Strategy
const DeterrentConfig configFixed = { 
    true,               // enableStreaks
    true,               // enableRewardCode
    DETERRENT_FIXED,    // penaltyStrategy
    500,                // rewardPenalty (Fixed Value)
    true,               // enablePaybackTime
    DETERRENT_FIXED,    // paybackStrategy
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

    // Request 5s (Below minLockDuration of 10s)
    uint32_t duration = rules.processStartRequest(5, presets, configFixed, stats);

    TEST_ASSERT_EQUAL_UINT32(0, duration); // Should be rejected
}

void test_abort_strategy_fixed(void) {
    MockSessionHAL hal; // Spy
    StandardRules rules;
    SessionStats stats = {0};

    // Act
    AbortConsequences result = rules.onAbort(stats, configFixed, presets, hal);

    // Assert
    TEST_ASSERT_TRUE(result.enterPenaltyBox);
    TEST_ASSERT_EQUAL_UINT32(500, result.penaltyDuration); // Should match configFixed.rewardPenalty
}

void test_abort_strategy_random(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionStats stats = {0};

    // Setup: Random Strategy
    DeterrentConfig configRandom = configFixed;
    configRandom.penaltyStrategy = DETERRENT_RANDOM;

    // Presets Penalty Range: 300 - 900
    // MockSessionHAL.getRandom returns (min+max)/2 => (300+900)/2 = 600
    
    // Act
    AbortConsequences result = rules.onAbort(stats, configRandom, presets, hal);

    // Assert
    TEST_ASSERT_TRUE(result.enterPenaltyBox);
    TEST_ASSERT_EQUAL_UINT32(600, result.penaltyDuration); // Verified against Mock behavior
}

void test_abort_applies_random_payback(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionStats stats = {0};

    // Setup: Enable Random Payback Strategy
    DeterrentConfig configRandomPayback = configFixed; // Copy base
    configRandomPayback.enablePaybackTime = true;
    configRandomPayback.paybackStrategy = DETERRENT_RANDOM;

    // Note: 'presets' is defined at the top of test_rules_logic.cpp
    // Payback Range in presets is: 60 (min), 120 (max)
    // MockHAL calculates random as (min+max)/2 => (60+120)/2 = 90.
    
    // Act: Abort
    rules.onAbort(stats, configRandomPayback, presets, hal);

    // Assert: 90 seconds added to debt
    TEST_ASSERT_EQUAL_UINT32(90, stats.paybackAccumulated);
}

void test_completion_clears_payback_debt(void) {
    StandardRules rules;
    SessionStats stats = {0};
    stats.paybackAccumulated = 500; // Start with existing debt

    // Config doesn't matter for clearing logic, but we pass it anyway
    DeterrentConfig ignored = {0}; 
    ignored.enableStreaks = true; // Ensure other stats update too

    // Act: Complete the session
    rules.onCompletion(stats, ignored);

    // Assert: Debt must be wiped to 0
    TEST_ASSERT_EQUAL_UINT32(0, stats.paybackAccumulated);
    TEST_ASSERT_EQUAL_UINT32(1, stats.completed);
}

// --- Runner ---
int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_start_request_applies_debt);
    RUN_TEST(test_start_request_clamps_to_profile_max);
    RUN_TEST(test_start_request_rejects_below_minimum);
    RUN_TEST(test_abort_strategy_fixed);
    RUN_TEST(test_abort_strategy_random);
    RUN_TEST(test_completion_clears_payback_debt);
    RUN_TEST(test_abort_applies_random_payback);
    return UNITY_END();
}