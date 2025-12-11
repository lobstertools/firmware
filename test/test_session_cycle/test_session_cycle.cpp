/*
 * File: test/test_session_cycle/test_session_cycle.cpp
 * Description: Full integration test for a standard Session lifecycle.
 * Covers Auto-Countdown, Button-Trigger, and Duration Resolution logic.
 */
#include <unity.h>
#include "Session.h"
#include "MockSessionHAL.h"
#include "StandardRules.h"

// --- Constants ---
const SystemDefaults defaults = { 5, 10, 240, 10000, 4, 5, 30000, 3, 60 };

const SessionPresets presets = { 
    300, 600, 900, 1800, 3600, 7200, // Ranges (Short, Medium, Long)
    300, 900,                        // Penalty Range
    60, 120,                         // Payback Range
    14400, 14400, 3600,              // Limits (Ceilings)
    10, 10, 10                       // Absolute Minimums (Floors)
};

const DeterrentConfig deterrents = { true, true, DETERRENT_FIXED, 300, true, DETERRENT_FIXED, 60 };

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
// LIFECYCLE TESTS
// ============================================================================

void test_full_cycle_auto_countdown(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engageSafetyInterlock(hal, engine);

    SessionConfig cfg = {};
    cfg.durationType = DUR_FIXED;
    cfg.fixedDuration = 60;
    cfg.triggerStrategy = STRAT_AUTO_COUNTDOWN;
    cfg.channelDelays[0] = 2; 

    engine.startSession(cfg);
    TEST_ASSERT_EQUAL(ARMED, engine.getState());

    engine.tick(); // T=0
    engine.tick(); // T=1
    engine.tick(); // T=2 -> Locked
    
    TEST_ASSERT_EQUAL(LOCKED, engine.getState());
    TEST_ASSERT_EQUAL_UINT32(60, engine.getTimers().lockRemaining);

    for(int i=0; i<60; i++) engine.tick();
    TEST_ASSERT_EQUAL(COMPLETED, engine.getState());
}

void test_full_cycle_button_trigger(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engageSafetyInterlock(hal, engine);

    SessionConfig cfg = {};
    cfg.durationType = DUR_FIXED;
    cfg.fixedDuration = 60;
    cfg.triggerStrategy = STRAT_BUTTON_TRIGGER;

    engine.startSession(cfg);
    TEST_ASSERT_EQUAL(ARMED, engine.getState());

    hal.simulateDoublePress();
    engine.tick(); 

    TEST_ASSERT_EQUAL(LOCKED, engine.getState());
}

void test_armed_state_timeout(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engageSafetyInterlock(hal, engine);

    SessionConfig cfg = {};
    cfg.durationType = DUR_FIXED;
    cfg.fixedDuration = 60;
    cfg.triggerStrategy = STRAT_BUTTON_TRIGGER;
    engine.startSession(cfg);

    for(int i=0; i < defaults.armedTimeoutSeconds + 5; i++) {
        engine.tick();
    }

    TEST_ASSERT_EQUAL(READY, engine.getState()); 
}

// ============================================================================
// DURATION RESOLUTION TESTS (Coverage Gaps Fix)
// ============================================================================

void test_resolve_duration_short_range(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engageSafetyInterlock(hal, engine);

    SessionConfig cfg = {};
    cfg.durationType = DUR_RANGE_SHORT; // Target: 300 to 600
    cfg.triggerStrategy = STRAT_AUTO_COUNTDOWN;

    // (300+600)/2 = 450
    engine.startSession(cfg);
    TEST_ASSERT_EQUAL_UINT32(450, engine.getTimers().lockDuration);
}

void test_resolve_duration_medium_range(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engageSafetyInterlock(hal, engine);

    SessionConfig cfg = {};
    cfg.durationType = DUR_RANGE_MEDIUM; // Target: 900 to 1800
    cfg.triggerStrategy = STRAT_AUTO_COUNTDOWN;

    // (900+1800)/2 = 1350
    engine.startSession(cfg);
    TEST_ASSERT_EQUAL_UINT32(1350, engine.getTimers().lockDuration);
}

void test_resolve_duration_long_range(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engageSafetyInterlock(hal, engine);

    SessionConfig cfg = {};
    cfg.durationType = DUR_RANGE_LONG; // Target: 3600 to 7200
    cfg.triggerStrategy = STRAT_AUTO_COUNTDOWN;

    // (3600+7200)/2 = 5400
    engine.startSession(cfg);
    TEST_ASSERT_EQUAL_UINT32(5400, engine.getTimers().lockDuration);
}

void test_resolve_duration_random_custom(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engageSafetyInterlock(hal, engine);

    SessionConfig cfg = {};
    cfg.durationType = DUR_RANDOM; 
    cfg.minDuration = 100; // Custom range
    cfg.maxDuration = 200;
    cfg.triggerStrategy = STRAT_AUTO_COUNTDOWN;

    // (100+200)/2 = 150
    engine.startSession(cfg);
    TEST_ASSERT_EQUAL_UINT32(150, engine.getTimers().lockDuration);
}

// ============================================================================
// TEST MODE SCENARIOS
// ============================================================================

void test_start_test_mode_success(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engageSafetyInterlock(hal, engine);

    int res = engine.startTest();
    TEST_ASSERT_EQUAL(200, res);
    TEST_ASSERT_EQUAL(TESTING, engine.getState());
}

void test_test_mode_auto_completion(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engageSafetyInterlock(hal, engine);

    engine.startTest();
    uint32_t duration = defaults.testModeDuration; 
    for(uint32_t i=0; i < duration; i++) engine.tick();

    TEST_ASSERT_EQUAL(READY, engine.getState());
}

void test_test_mode_abort_resets_to_ready(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engageSafetyInterlock(hal, engine);

    engine.startTest();
    engine.abort("Manual Stop");
    TEST_ASSERT_EQUAL(READY, engine.getState());
}

void test_test_mode_ignores_triggers(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engageSafetyInterlock(hal, engine);

    engine.startTest();
    hal.simulateDoublePress();
    engine.trigger("External API"); 
    engine.tick(); 
    TEST_ASSERT_EQUAL(TESTING, engine.getState());
}

void test_start_test_mode_fails_unsafe(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    hal.setSafetyInterlock(false);
    engine.tick();

    int res = engine.startTest();
    TEST_ASSERT_EQUAL(412, res); 
}

// ============================================================================
// EXTRA COVERAGE (API, Penalty, Rules)
// ============================================================================

void test_api_trigger_starts_locked_state(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engageSafetyInterlock(hal, engine);

    SessionConfig cfg = {};
    cfg.durationType = DUR_FIXED;
    cfg.fixedDuration = 60;
    cfg.triggerStrategy = STRAT_BUTTON_TRIGGER;
    engine.startSession(cfg);

    engine.trigger("WebAPI");
    TEST_ASSERT_EQUAL(LOCKED, engine.getState());
}

void test_completion_and_reset_generates_reward(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engageSafetyInterlock(hal, engine);

    engine.loadState(COMPLETED);
    engine.handleReboot(); 
    TEST_ASSERT_EQUAL(READY, engine.getState());
    TEST_ASSERT_NOT_EQUAL(0, strlen(engine.getRewardHistory()[0].code));
}

void test_penalty_box_auto_completion(void) {
    MockSessionHAL hal;
    StandardRules rules;
    
    // Setup Custom Config (Allow short penalty)
    DeterrentConfig fastPenalty = deterrents;
    fastPenalty.rewardPenalty = 10; 
    
    // FIX: Create permissive presets because default minimum is 300s
    SessionPresets loosePresets = presets;
    loosePresets.penaltyMin = 10; 
    
    SessionEngine engine(hal, rules, defaults, loosePresets, fastPenalty);
    engageSafetyInterlock(hal, engine);

    SessionConfig cfg = {};
    cfg.durationType = DUR_FIXED;
    cfg.fixedDuration = 600;
    
    engine.startSession(cfg);
    engine.tick(); 
    engine.abort("Test"); // ABORTED

    for(int i=0; i<10; i++) engine.tick(); 
    TEST_ASSERT_EQUAL(COMPLETED, engine.getState());
}

void test_start_rejected_by_rules_logic(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engageSafetyInterlock(hal, engine);

    SessionConfig cfg = {};
    cfg.durationType = DUR_FIXED;
    cfg.fixedDuration = 5; 

    int res = engine.startSession(cfg);
    TEST_ASSERT_EQUAL(400, res); 
}

void test_start_fails_if_penalty_out_of_range(void) {
    MockSessionHAL hal;
    StandardRules rules;
    
    // 1. Setup: Fixed Penalty of 10s (Too Short!)
    DeterrentConfig badConfig = deterrents;
    badConfig.penaltyStrategy = DETERRENT_FIXED;
    badConfig.rewardPenalty = 10; 
    
    // Presets.penaltyMin is 300s by default. 10 < 300 -> Invalid.
    SessionEngine engine(hal, rules, defaults, presets, badConfig);
    engageSafetyInterlock(hal, engine);

    SessionConfig cfg = {};
    cfg.durationType = DUR_FIXED;
    cfg.fixedDuration = 600;

    // 2. Act
    int res = engine.startSession(cfg);

    // 3. Assert
    TEST_ASSERT_EQUAL(400, res); // Start Failed: Penalty Out of Range
    TEST_ASSERT_EQUAL(READY, engine.getState());
}

// ============================================================================
// MAIN RUNNER
// ============================================================================
int main(void) {
    UNITY_BEGIN();
    
    // Lifecycle
    RUN_TEST(test_full_cycle_auto_countdown);
    RUN_TEST(test_full_cycle_button_trigger);
    RUN_TEST(test_armed_state_timeout);
    
    // Duration Resolution (ALL Cases)
    RUN_TEST(test_resolve_duration_short_range);
    RUN_TEST(test_resolve_duration_medium_range);
    RUN_TEST(test_resolve_duration_long_range);
    RUN_TEST(test_resolve_duration_random_custom);

    // Test Mode
    RUN_TEST(test_start_test_mode_success);
    RUN_TEST(test_test_mode_auto_completion);
    RUN_TEST(test_test_mode_abort_resets_to_ready);
    RUN_TEST(test_test_mode_ignores_triggers);
    RUN_TEST(test_start_test_mode_fails_unsafe);

    // Extras
    RUN_TEST(test_api_trigger_starts_locked_state);
    RUN_TEST(test_completion_and_reset_generates_reward);
    RUN_TEST(test_penalty_box_auto_completion);
    RUN_TEST(test_start_rejected_by_rules_logic);
    RUN_TEST(test_start_fails_if_penalty_out_of_range);

    return UNITY_END();
}