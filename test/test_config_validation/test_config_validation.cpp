/*
 * File: test/test_config_validation.cpp
 * Description: Verifies that the Session Engine rejects invalid configurations
 * including Presets and Deterrents logic.
 */
#include <unity.h>
#include "Session.h"
#include "MockSessionHAL.h"
#include "StandardRules.h"

// --- Defaults ---
const SystemDefaults defaults = { 5, 10, 240, 10000, 4, 5, 30000, 3, 60 };

// --- Valid Base Configs ---
const SessionPresets validPresets = { 
    300, 600,    // Short
    900, 1800,   // Medium
    3600, 7200,  // Long
    14400,       // Max (4h)
    10           // Min
};

const DeterrentConfig validDeterrents = { 
    true, true, DETERRENT_FIXED, 300, 900, 300, 
    true, DETERRENT_FIXED, 60, 120, 60 
};

// --- Helper ---
SessionEngine* createEngine(MockSessionHAL& hal, StandardRules& rules, 
                            const SessionPresets& p, const DeterrentConfig& d) {
    auto* engine = new SessionEngine(hal, rules, defaults, p, d);
    hal.setSafetyInterlock(true);
    return engine;
}

void setUp(void) {}
void tearDown(void) {}

// ============================================================================
// TEST GROUP: PRESET VALIDATION
// ============================================================================

void test_presets_min_greater_than_max_fails(void) {
    MockSessionHAL hal; StandardRules rules;
    SessionPresets p = validPresets;
    
    // Invalid: Min > Max
    p.minSessionDuration = 20000; 
    p.maxSessionDuration = 14400;

    SessionEngine* engine = createEngine(hal, rules, p, validDeterrents);
    SessionConfig req = { DUR_FIXED, 600 }; 

    TEST_ASSERT_EQUAL(400, engine->startSession(req));
    delete engine;
}

void test_presets_range_inverted_fails(void) {
    MockSessionHAL hal; StandardRules rules;
    SessionPresets p = validPresets;
    
    // Invalid: Short Min > Short Max
    p.shortMin = 600;
    p.shortMax = 300;

    SessionEngine* engine = createEngine(hal, rules, p, validDeterrents);
    SessionConfig req = { DUR_FIXED, 600 }; 

    TEST_ASSERT_EQUAL(400, engine->startSession(req));
    delete engine;
}

void test_presets_zero_min_fails(void) {
    MockSessionHAL hal; StandardRules rules;
    SessionPresets p = validPresets;
    
    // Invalid: Min is 0
    p.minSessionDuration = 0;

    SessionEngine* engine = createEngine(hal, rules, p, validDeterrents);
    SessionConfig req = { DUR_FIXED, 600 }; 

    TEST_ASSERT_EQUAL(400, engine->startSession(req));
    delete engine;
}

// ============================================================================
// TEST GROUP: DETERRENT VALIDATION
// ============================================================================

void test_reward_fixed_zero_fails(void) {
    MockSessionHAL hal; StandardRules rules;
    DeterrentConfig d = validDeterrents;
    d.rewardPenaltyStrategy = DETERRENT_FIXED;
    d.rewardPenalty = 0;

    SessionEngine* engine = createEngine(hal, rules, validPresets, d);
    SessionConfig req = { DUR_FIXED, 600 }; 

    TEST_ASSERT_EQUAL(400, engine->startSession(req));
    delete engine;
}

void test_reward_random_inverted_range_fails(void) {
    MockSessionHAL hal; StandardRules rules;
    DeterrentConfig d = validDeterrents;
    d.rewardPenaltyStrategy = DETERRENT_RANDOM;
    d.rewardPenaltyMin = 600;
    d.rewardPenaltyMax = 300;

    SessionEngine* engine = createEngine(hal, rules, validPresets, d);
    SessionConfig req = { DUR_FIXED, 600 }; 

    TEST_ASSERT_EQUAL(400, engine->startSession(req));
    delete engine;
}

void test_deterrent_fixed_exceeds_preset_max_fails(void) {
    MockSessionHAL hal; StandardRules rules;
    DeterrentConfig d = validDeterrents;
    
    // Invalid: Penalty (15000) > Global Max (14400)
    d.rewardPenaltyStrategy = DETERRENT_FIXED;
    d.rewardPenalty = 15000;

    SessionEngine* engine = createEngine(hal, rules, validPresets, d);
    SessionConfig req = { DUR_FIXED, 600 }; 

    TEST_ASSERT_EQUAL(400, engine->startSession(req));
    delete engine;
}

void test_deterrent_random_max_exceeds_preset_max_fails(void) {
    MockSessionHAL hal; StandardRules rules;
    DeterrentConfig d = validDeterrents;
    
    // Invalid: Random Max (15000) > Global Max (14400)
    d.rewardPenaltyStrategy = DETERRENT_RANDOM;
    d.rewardPenaltyMin = 300;
    d.rewardPenaltyMax = 15000;

    SessionEngine* engine = createEngine(hal, rules, validPresets, d);
    SessionConfig req = { DUR_FIXED, 600 }; 

    TEST_ASSERT_EQUAL(400, engine->startSession(req));
    delete engine;
}

void test_valid_combo_succeeds(void) {
    MockSessionHAL hal; StandardRules rules;
    SessionEngine* engine = createEngine(hal, rules, validPresets, validDeterrents);
    SessionConfig req = { DUR_FIXED, 600 }; 

    TEST_ASSERT_EQUAL(200, engine->startSession(req));
    TEST_ASSERT_EQUAL(ARMED, engine->getState());
    delete engine;
}

int main(void) {
    UNITY_BEGIN();
    
    // Presets
    RUN_TEST(test_presets_min_greater_than_max_fails);
    RUN_TEST(test_presets_range_inverted_fails);
    RUN_TEST(test_presets_zero_min_fails);

    // Deterrents
    RUN_TEST(test_reward_fixed_zero_fails);
    RUN_TEST(test_reward_random_inverted_range_fails);
    RUN_TEST(test_deterrent_fixed_exceeds_preset_max_fails);
    RUN_TEST(test_deterrent_random_max_exceeds_preset_max_fails);

    // Success
    RUN_TEST(test_valid_combo_succeeds);

    return UNITY_END();
}