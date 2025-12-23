/*
 * File: test/test_session_cycle/test_session_cycle.cpp
 * Description: Full integration test for a standard Session lifecycle.
 * Covers Auto-Countdown, Button-Trigger, Duration Resolution, and Outcome logic.
 */
#include <unity.h>
#include "Session.h"
#include "MockSessionHAL.h"
#include "StandardRules.h"

// --- Constants ---
const SystemDefaults defaults = { 5, 10, 240, 10000, 4, 5, 30000, 3, 60 };

const SessionPresets presets = { 
    300, 600,   // Short Range
    900, 1800,  // Medium Range
    3600, 7200, // Long Range
    14400,      // maxSessionDuration
    10          // minSessionDuration
};

const DeterrentConfig deterrents = { 
    true,            // enableStreaks
    
    true,            // enableRewardCode
    DETERRENT_FIXED, // rewardPenaltyStrategy
    300, 900,        // rewardPenaltyMin, rewardPenaltyMax
    300,             // rewardPenalty
    
    true,            // enablePaybackTime
    DETERRENT_FIXED, // paybackTimeStrategy
    60, 120,         // paybackTimeMin, paybackTimeMax
    60               // paybackTime
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
// LIFECYCLE TESTS
// ============================================================================

void test_full_cycle_auto_countdown(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engageSafetyInterlock(hal, engine);

    SessionConfig cfg = {};
    cfg.durationType = DUR_FIXED;
    cfg.durationFixed = 60;
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
    
    // Verify Outcome Logic
    TEST_ASSERT_EQUAL(COMPLETED, engine.getState());
    TEST_ASSERT_EQUAL(OUTCOME_SUCCESS, engine.getOutcome()); 
}

void test_full_cycle_button_trigger(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engageSafetyInterlock(hal, engine);

    SessionConfig cfg = {};
    cfg.durationType = DUR_FIXED;
    cfg.durationFixed = 60;
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
    cfg.durationFixed = 60;
    cfg.triggerStrategy = STRAT_BUTTON_TRIGGER;
    engine.startSession(cfg);

    for(int i=0; i < defaults.armedTimeout + 5; i++) {
        engine.tick();
    }

    TEST_ASSERT_EQUAL(READY, engine.getState()); 
}

// ============================================================================
// DURATION RESOLUTION TESTS
// ============================================================================

void test_resolve_duration_rounds_up_to_minute(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engageSafetyInterlock(hal, engine);

    SessionConfig cfg = {};
    cfg.durationType = DUR_FIXED;
    cfg.triggerStrategy = STRAT_AUTO_COUNTDOWN;

    // Case 1: 61 seconds -> Should round up to 120
    cfg.durationFixed = 61;
    engine.startSession(cfg);
    TEST_ASSERT_EQUAL_UINT32(120, engine.getTimers().lockDuration);

    // Reset for next case
    engine.abort("Test Reset"); 
    engine.tick();

    // Case 2: 59 seconds -> Should round up to 60
    cfg.durationFixed = 59;
    engine.startSession(cfg);
    TEST_ASSERT_EQUAL_UINT32(60, engine.getTimers().lockDuration);
}

void test_resolve_duration_short_range(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engageSafetyInterlock(hal, engine);

    SessionConfig cfg = {};
    cfg.durationType = DUR_RANGE_SHORT; // 300 - 600
    cfg.triggerStrategy = STRAT_AUTO_COUNTDOWN;

    engine.startSession(cfg);
    // Mock HAL usually picks avg (450). 
    // 450 rounds up to nearest minute -> 480.
    TEST_ASSERT_EQUAL_UINT32(480, engine.getTimers().lockDuration);
}

void test_resolve_duration_medium_range(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engageSafetyInterlock(hal, engine);

    SessionConfig cfg = {};
    cfg.durationType = DUR_RANGE_MEDIUM; // 900 - 1800
    cfg.triggerStrategy = STRAT_AUTO_COUNTDOWN;

    engine.startSession(cfg);
    // Mock HAL avg: 1350. 
    // 1350 / 60 = 22.5. Rounds up to 23 mins -> 1380.
    TEST_ASSERT_EQUAL_UINT32(1380, engine.getTimers().lockDuration);
}

void test_resolve_duration_long_range(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engageSafetyInterlock(hal, engine);

    SessionConfig cfg = {};
    cfg.durationType = DUR_RANGE_LONG; // 3600 - 7200
    cfg.triggerStrategy = STRAT_AUTO_COUNTDOWN;

    engine.startSession(cfg);
    // Mock HAL avg: 5400. 
    // 5400 / 60 = 90.0. Exact minute, stays 5400.
    TEST_ASSERT_EQUAL_UINT32(5400, engine.getTimers().lockDuration);
}

void test_resolve_duration_random_custom(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engageSafetyInterlock(hal, engine);

    SessionConfig cfg = {};
    cfg.durationType = DUR_RANDOM; 
    cfg.durationMin = 100;
    cfg.durationMax = 200;
    cfg.triggerStrategy = STRAT_AUTO_COUNTDOWN;

    engine.startSession(cfg);
    // Mock HAL avg: 150.
    // 150 / 60 = 2.5. Rounds up to 3 mins -> 180.
    TEST_ASSERT_EQUAL_UINT32(180, engine.getTimers().lockDuration);
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
// EXTRA COVERAGE (API, Penalty, Rules, LED, OUTCOME)
// ============================================================================

void test_api_trigger_starts_locked_state(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engageSafetyInterlock(hal, engine);

    SessionConfig cfg = {};
    cfg.durationType = DUR_FIXED;
    cfg.durationFixed = 60;
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
    fastPenalty.rewardPenaltyMin = 10; 
    
    SessionEngine engine(hal, rules, defaults, presets, fastPenalty);
    engageSafetyInterlock(hal, engine);

    SessionConfig cfg = {};
    cfg.durationType = DUR_FIXED;
    cfg.durationFixed = 600;
    
    engine.startSession(cfg);
    engine.tick(); 
    engine.abort("Test"); // ABORTED

    // Verify Immediate Outcome in Penalty State
    TEST_ASSERT_EQUAL(ABORTED, engine.getState());
    TEST_ASSERT_EQUAL(OUTCOME_ABORTED, engine.getOutcome()); 

    for(int i=0; i<65; i++) engine.tick(); 
    
    // Verify Outcome AFTER Penalty is Served
    TEST_ASSERT_EQUAL(COMPLETED, engine.getState());
    TEST_ASSERT_EQUAL(OUTCOME_ABORTED, engine.getOutcome()); 
}

void test_start_rejected_by_rules_logic(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engageSafetyInterlock(hal, engine);

    SessionConfig cfg = {};
    cfg.durationType = DUR_FIXED;
    cfg.durationFixed = 0; 

    int res = engine.startSession(cfg);
    TEST_ASSERT_EQUAL(400, res); 
}

void test_start_auto_countdown_zeros_disabled_channels(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engageSafetyInterlock(hal, engine);

    hal.setChannelMask(0x0D); 

    SessionConfig cfg = {};
    cfg.durationType = DUR_FIXED;
    cfg.durationFixed = 60;
    cfg.triggerStrategy = STRAT_AUTO_COUNTDOWN;
    cfg.channelDelays[0] = 10; 
    cfg.channelDelays[1] = 20; 
    cfg.channelDelays[2] = 30; 

    engine.startSession(cfg);

    TEST_ASSERT_EQUAL_UINT32(10, engine.getTimers().channelDelays[0]);
    TEST_ASSERT_EQUAL_UINT32(0,  engine.getTimers().channelDelays[1]); 
    TEST_ASSERT_EQUAL_UINT32(30, engine.getTimers().channelDelays[2]);
}

void test_led_logic_with_disable_feature(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engageSafetyInterlock(hal, engine);

    SessionConfig cfg = {};
    cfg.durationType = DUR_FIXED;
    cfg.durationFixed = 60;
    cfg.triggerStrategy = STRAT_BUTTON_TRIGGER;
    cfg.disableLED = true; 

    engine.startSession(cfg);
    engine.tick(); 
    
    TEST_ASSERT_EQUAL(ARMED, engine.getState());
    TEST_ASSERT_TRUE(hal.ledEnabled);

    hal.simulateDoublePress();
    engine.tick();

    TEST_ASSERT_EQUAL(LOCKED, engine.getState());
    TEST_ASSERT_FALSE(hal.ledEnabled);

    for(int i=0; i<60; i++) engine.tick();
    
    TEST_ASSERT_EQUAL(COMPLETED, engine.getState());
    TEST_ASSERT_TRUE(hal.ledEnabled);
}

// Explicit test for Abort Immediate Outcome
void test_outcome_immediate_abort(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engageSafetyInterlock(hal, engine);

    SessionConfig cfg = {};
    cfg.durationType = DUR_FIXED;
    cfg.durationFixed = 60;
    cfg.triggerStrategy = STRAT_BUTTON_TRIGGER;

    engine.startSession(cfg);
    engine.trigger("API");
    engine.abort("Immediate");

    TEST_ASSERT_EQUAL(ABORTED, engine.getState());
    TEST_ASSERT_EQUAL(OUTCOME_ABORTED, engine.getOutcome()); 
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
    
    // Duration
    RUN_TEST(test_resolve_duration_rounds_up_to_minute);
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

    // Extras & Outcome
    RUN_TEST(test_api_trigger_starts_locked_state);
    RUN_TEST(test_completion_and_reset_generates_reward);
    RUN_TEST(test_penalty_box_auto_completion); 
    RUN_TEST(test_start_rejected_by_rules_logic);
    RUN_TEST(test_start_auto_countdown_zeros_disabled_channels);
    RUN_TEST(test_led_logic_with_disable_feature);
    RUN_TEST(test_outcome_immediate_abort);

    return UNITY_END();
}