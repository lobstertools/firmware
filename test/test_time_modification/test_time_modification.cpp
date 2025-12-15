/*
 * File: test/test_time_modification/test_time_modification.cpp
 * Description: Verifies the Time Modification API logic.
 * Covers:
 * - Feature enablement/disablement
 * - State validity (ARMED/LOCKED/TESTING only)
 * - Adding time (Clamping to Max, Increasing Debt Payoff)
 * - Removing time (Clamping to Step Floor, Decreasing Debt Payoff)
 * - Safety constraints (Never reduce to 0)
 */

#include <unity.h>
#include "Session.h"
#include "MockSessionHAL.h"
#include "StandardRules.h"

// --- Standard Test Configuration ---
const SystemDefaults defaults = { 5, 10, 240, 10000, 4, 5, 30000, 3, 60 };

const SessionPresets presets = { 
    300, 600,    // Short
    900, 1800,   // Medium
    3600, 7200,  // Long
    14400,       // Max Session Duration (4 Hours)
    10           // Min
};

// Configuration: Feature ENABLED, Step = 300s (5 min)
const DeterrentConfig configEnabled = { 
    true,               // enableStreaks
    true,               // enableRewardCode (FIX: Must be true to test ABORTED state)
    DETERRENT_FIXED, 300, 900, 300, 
    false,              // enablePaybackTime (Default off, enabled per test)
    DETERRENT_FIXED, 60, 120, 60,
    
    true,               // enableTimeModification
    300                 // timeModificationStep (5 mins)
};

// Configuration: Feature DISABLED
const DeterrentConfig configDisabled = { 
    true, false, DETERRENT_FIXED, 300, 900, 300, 
    false, DETERRENT_FIXED, 60, 120, 60,
    
    false,              // enableTimeModification
    300
};

// --- Helper Functions ---

void engageSafetyInterlock(MockSessionHAL& hal, SessionEngine& engine) {
    hal.setSafetyInterlock(true);
    engine.tick(); 
    hal.advanceTime(11000); 
    engine.tick(); 
}

SessionEngine* createEngine(MockSessionHAL& hal, StandardRules& rules, const DeterrentConfig& cfg) {
    return new SessionEngine(hal, rules, defaults, presets, cfg);
}

void setUp(void) {}
void tearDown(void) {}

// ============================================================================
// SECTION 1: PERMISSIONS & STATE CHECKS
// ============================================================================

void test_mod_fails_if_feature_disabled(void) {
    MockSessionHAL hal; StandardRules rules;
    SessionEngine* engine = createEngine(hal, rules, configDisabled);
    engageSafetyInterlock(hal, *engine);
    
    SessionConfig cfg = { DUR_FIXED, 600 };
    engine->startSession(cfg);
    engine->tick(); // LOCKED

    // Attempt Add
    int res = engine->modifyTime(true); 
    TEST_ASSERT_EQUAL(403, res); // Forbidden
    TEST_ASSERT_EQUAL_UINT32(600, engine->getTimers().lockRemaining); // Unchanged

    delete engine;
}

void test_mod_fails_in_invalid_states(void) {
    MockSessionHAL hal; StandardRules rules;
    SessionEngine* engine = createEngine(hal, rules, configEnabled);
    engageSafetyInterlock(hal, *engine);

    // 1. READY State
    TEST_ASSERT_EQUAL(READY, engine->getState());
    TEST_ASSERT_EQUAL(409, engine->modifyTime(true));

    // 2. ABORTED State (Penalty Box)
    SessionConfig cfg = { DUR_FIXED, 600 };
    engine->startSession(cfg);
    engine->tick();
    engine->abort("Test"); 
    
    // Now asserts correctly because enableRewardCode is true in configEnabled
    TEST_ASSERT_EQUAL(ABORTED, engine->getState()); 
    
    // Modification strictly forbidden during penalty
    TEST_ASSERT_EQUAL(409, engine->modifyTime(false)); 

    delete engine;
}

void test_mod_valid_in_test_mode(void) {
    MockSessionHAL hal; StandardRules rules;
    SessionEngine* engine = createEngine(hal, rules, configEnabled);
    engageSafetyInterlock(hal, *engine);
    
    engine->startTest(); // Default test duration is 240s
    TEST_ASSERT_EQUAL(TESTING, engine->getState());

    // Add 300s
    int res = engine->modifyTime(true); 
    TEST_ASSERT_EQUAL(200, res);
    
    // 240 + 300 = 540
    TEST_ASSERT_EQUAL_UINT32(540, engine->getTimers().testRemaining);

    delete engine;
}

// ============================================================================
// SECTION 2: ADDING TIME (Safety & Limits)
// ============================================================================

void test_mod_add_time_basic_success(void) {
    MockSessionHAL hal; StandardRules rules;
    SessionEngine* engine = createEngine(hal, rules, configEnabled);
    engageSafetyInterlock(hal, *engine);
    
    SessionConfig cfg = { DUR_FIXED, 600 };
    engine->startSession(cfg);
    engine->tick(); // LOCKED at 600

    // Add 5 mins (300s)
    int res = engine->modifyTime(true); 
    
    TEST_ASSERT_EQUAL(200, res);
    TEST_ASSERT_EQUAL_UINT32(900, engine->getTimers().lockRemaining);
    TEST_ASSERT_EQUAL_UINT32(900, engine->getTimers().lockDuration); // Total record updates too

    delete engine;
}

void test_mod_add_clamps_to_global_max(void) {
    MockSessionHAL hal; StandardRules rules;
    SessionEngine* engine = createEngine(hal, rules, configEnabled);
    engageSafetyInterlock(hal, *engine);
    
    // Max is 14400 (4h). 
    // Start at 14200. Step is 300.
    // 14200 + 300 = 14500 > 14400.
    SessionConfig cfg = { DUR_FIXED, 14200 };
    engine->startSession(cfg);
    engine->tick();

    // Logic Choice: Does it clamp or reject? 
    // Implementation: Rejects (returns 400) to be safe/clear.
    int res = engine->modifyTime(true); 
    
    TEST_ASSERT_EQUAL(400, res);
    TEST_ASSERT_EQUAL_UINT32(14200, engine->getTimers().lockRemaining);

    delete engine;
}

// ============================================================================
// SECTION 3: REMOVING TIME (The Floor Constraint)
// ============================================================================

void test_mod_remove_time_basic_success(void) {
    MockSessionHAL hal; StandardRules rules;
    SessionEngine* engine = createEngine(hal, rules, configEnabled);
    engageSafetyInterlock(hal, *engine);
    
    // Start 10 mins (600s)
    SessionConfig cfg = { DUR_FIXED, 600 };
    engine->startSession(cfg);
    engine->tick();

    // Remove 5 mins (300s)
    int res = engine->modifyTime(false); 
    
    TEST_ASSERT_EQUAL(200, res);
    TEST_ASSERT_EQUAL_UINT32(300, engine->getTimers().lockRemaining);

    delete engine;
}

void test_mod_remove_rejected_at_step_floor(void) {
    MockSessionHAL hal; StandardRules rules;
    SessionEngine* engine = createEngine(hal, rules, configEnabled);
    engageSafetyInterlock(hal, *engine);
    
    // Config Step is 300s.
    // Start Session: 300s.
    SessionConfig cfg = { DUR_FIXED, 300 };
    engine->startSession(cfg);
    engine->tick();

    // Try to remove 300s. Result would be 0.
    // Constraint: Cannot reduce if Remaining <= Step.
    int res = engine->modifyTime(false);
    
    TEST_ASSERT_EQUAL(409, res); // Conflict
    TEST_ASSERT_EQUAL_UINT32(300, engine->getTimers().lockRemaining); // Unchanged
    TEST_ASSERT_EQUAL(LOCKED, engine->getState()); // Did not complete

    delete engine;
}

void test_mod_remove_rejected_if_below_step_floor(void) {
    MockSessionHAL hal; StandardRules rules;
    SessionEngine* engine = createEngine(hal, rules, configEnabled);
    engageSafetyInterlock(hal, *engine);
    
    // Config Step is 300s.
    // Start Session: 100s.
    SessionConfig cfg = { DUR_FIXED, 100 };
    engine->startSession(cfg);
    engine->tick();

    // Try to remove 300s.
    int res = engine->modifyTime(false);
    
    TEST_ASSERT_EQUAL(409, res); 
    TEST_ASSERT_EQUAL_UINT32(100, engine->getTimers().lockRemaining);

    delete engine;
}

// ============================================================================
// SECTION 4: DEBT INTERPLAY (Payback Logic)
// ============================================================================

void test_decrease_reduces_debt_served_first(void) {
    MockSessionHAL hal; StandardRules rules;
    
    // Setup: 10 hours of debt
    SessionStats stats = {0};
    stats.paybackAccumulated = 36000; 
    
    // Config with Payback Enabled
    DeterrentConfig d = configEnabled; 
    d.enablePaybackTime = true;

    SessionEngine* engine = createEngine(hal, rules, d);
    engine->loadStats(stats);
    engageSafetyInterlock(hal, *engine);

    // 1. Start Session: 1h Base + 10h Debt -> Clamped to 4h (14400s) Max
    // potentialDebtServed will be 3h (10800s)
    SessionConfig cfg = { DUR_FIXED, 3600 }; 
    engine->startSession(cfg);
    engine->tick(); // LOCKED

    TEST_ASSERT_EQUAL_UINT32(14400, engine->getTimers().lockDuration);
    TEST_ASSERT_EQUAL_UINT32(10800, engine->getTimers().potentialDebtServed);

    // 2. Decrease Time by 5m (300s)
    int res = engine->modifyTime(false);
    TEST_ASSERT_EQUAL(200, res);

    // 3. Verify Debt Served reduced by 300s
    // 10800 - 300 = 10500
    TEST_ASSERT_EQUAL_UINT32(10500, engine->getTimers().potentialDebtServed);
    
    // 4. Verify Total Duration reduced
    TEST_ASSERT_EQUAL_UINT32(14100, engine->getTimers().lockDuration);

    delete engine;
}

void test_increase_adds_debt_served(void) {
    MockSessionHAL hal; StandardRules rules;
    
    // Setup: 35 mins debt (2100s)
    SessionStats stats = {0};
    stats.paybackAccumulated = 2100;
    
    DeterrentConfig d = configEnabled;
    d.enablePaybackTime = true;

    SessionEngine* engine = createEngine(hal, rules, d);
    engine->loadStats(stats);
    engageSafetyInterlock(hal, *engine);

    // 1. Start Session: 10m Base (600s) + 35m Debt (2100s) = 2700s Total
    // All accumulated debt is covered.
    SessionConfig cfg = { DUR_FIXED, 600 };
    engine->startSession(cfg);
    engine->tick(); 

    TEST_ASSERT_EQUAL_UINT32(2100, engine->getTimers().potentialDebtServed);

    // 2. Increase Time by 5m (300s)
    // We are already at max debt coverage (2100/2100). 
    // Adding time should NOT increase potentialDebtServed because we hit the stats cap.
    engine->modifyTime(true);
    TEST_ASSERT_EQUAL_UINT32(2100, engine->getTimers().potentialDebtServed); // Capped

    // 3. Manually simulate a scenario where not all debt was covered
    // (e.g. Session was clamped by Max Duration at start)
    // Let's force potentialDebtServed to be lower to test the increment logic.
    SessionTimers t = engine->getTimers();
    t.potentialDebtServed = 1000; 
    engine->loadTimers(t);

    // Add another 300s
    engine->modifyTime(true);
    
    // Should increase from 1000 to 1300
    TEST_ASSERT_EQUAL_UINT32(1300, engine->getTimers().potentialDebtServed);

    delete engine;
}

int main(void) {
    UNITY_BEGIN();
    
    // State & Perms
    RUN_TEST(test_mod_fails_if_feature_disabled);
    RUN_TEST(test_mod_fails_in_invalid_states);
    RUN_TEST(test_mod_valid_in_test_mode);

    // Add
    RUN_TEST(test_mod_add_time_basic_success);
    RUN_TEST(test_mod_add_clamps_to_global_max);

    // Remove
    RUN_TEST(test_mod_remove_time_basic_success);
    RUN_TEST(test_mod_remove_rejected_at_step_floor);
    RUN_TEST(test_mod_remove_rejected_if_below_step_floor);

    // Debt Logic
    RUN_TEST(test_decrease_reduces_debt_served_first);
    RUN_TEST(test_increase_adds_debt_served);

    return UNITY_END();
}