/*
 * File: test/test_reward_security.cpp
 * Description: Verifies that Reward Codes are hidden during active/penalty states
 * and correctly preserved upon completion and reset.
 */
#include <unity.h>
#include "Session.h"
#include "MockSessionHAL.h"
#include "StandardRules.h"

// --- Defaults ---
const SystemDefaults defaults = { 5, 10, 240, 10000, 4, 5, 30000, 3, 60 };
const SessionPresets presets = { 300, 600, 900, 1800, 3600, 7200, 14400, 10 };
const DeterrentConfig deterrents = { 
    true, true, DETERRENT_FIXED, 300, 900, 300, 
    true, DETERRENT_FIXED, 60, 120, 60 
};

// --- Helper ---
SessionEngine* createEngine(MockSessionHAL& hal, StandardRules& rules) {
    auto* engine = new SessionEngine(hal, rules, defaults, presets, deterrents);
    hal.setSafetyInterlock(true);
    return engine;
}

void setUp(void) {}
void tearDown(void) {}

// ============================================================================
// VISIBILITY TESTS
// ============================================================================

void test_reward_visible_in_ready_state(void) {
    MockSessionHAL hal; StandardRules rules;
    SessionEngine* engine = createEngine(hal, rules);

    // Initial State: READY
    TEST_ASSERT_EQUAL(READY, engine->getState());
    
    // Assert: Visible
    const Reward* history = engine->getRewardHistory();
    TEST_ASSERT_NOT_NULL(history);
    
    // Check code generation occurred
    TEST_ASSERT_NOT_EQUAL(0, strlen(history[0].code));
    
    delete engine;
}

void test_reward_hidden_in_active_states(void) {
    MockSessionHAL hal; StandardRules rules;
    SessionEngine* engine = createEngine(hal, rules);

    // 1. Start Session -> ARMED
    SessionConfig cfg = { DUR_FIXED, 600, 0, 0, STRAT_AUTO_COUNTDOWN, {2,0,0,0} };
    engine->startSession(cfg);
    TEST_ASSERT_EQUAL(ARMED, engine->getState());
    TEST_ASSERT_NULL(engine->getRewardHistory()); // Hidden in ARMED

    // 2. Tick -> LOCKED
    engine->tick(); engine->tick(); engine->tick();
    TEST_ASSERT_EQUAL(LOCKED, engine->getState());
    TEST_ASSERT_NULL(engine->getRewardHistory()); // Hidden in LOCKED

    delete engine;
}

void test_reward_hidden_in_penalty_box(void) {
    MockSessionHAL hal; StandardRules rules;
    SessionEngine* engine = createEngine(hal, rules);

    // 1. Start and Lock
    SessionConfig cfg = { DUR_FIXED, 600 };
    engine->startSession(cfg);
    engine->tick();
    
    // 2. Abort -> ABORTED (Penalty)
    engine->abort("Test");
    TEST_ASSERT_EQUAL(ABORTED, engine->getState());
    
    // Assert: Hidden during penalty
    TEST_ASSERT_NULL(engine->getRewardHistory());

    delete engine;
}

// ============================================================================
// INTEGRITY TESTS
// ============================================================================

void test_reward_visible_and_correct_on_completion(void) {
    MockSessionHAL hal; StandardRules rules;
    SessionEngine* engine = createEngine(hal, rules);

    // Capture the code generated at READY
    const Reward* initialHistory = engine->getRewardHistory();
    char expectedCode[REWARD_CODE_LENGTH + 1];
    strcpy(expectedCode, initialHistory[0].code);

    // Run Session
    SessionConfig cfg = { DUR_FIXED, 10 }; // Short duration
    engine->startSession(cfg);
    for(int i=0; i<15; i++) engine->tick(); // Complete it

    TEST_ASSERT_EQUAL(COMPLETED, engine->getState());

    // Assert: Visible again
    const Reward* endHistory = engine->getRewardHistory();
    TEST_ASSERT_NOT_NULL(endHistory);

    // Assert: The code is the SAME as what was locked (no rotation yet)
    TEST_ASSERT_EQUAL_STRING(expectedCode, endHistory[0].code);

    delete engine;
}

void test_reward_preserved_after_penalty_and_reboot(void) {
    MockSessionHAL hal; 
    StandardRules rules;
    SessionEngine* engine = createEngine(hal, rules);

    // Engage Safety Interlock (Required to arm/start)
    hal.setSafetyInterlock(true);
    engine->tick(); // Process safety state

    // 1. Capture Code A
    // At startup, the engine generates the first code at [0].
    char codeA[REWARD_CODE_LENGTH + 1];
    const Reward* initialHistory = engine->getRewardHistory();
    TEST_ASSERT_NOT_NULL(initialHistory);
    strcpy(codeA, initialHistory[0].code);

    // 2. Lock and Abort
    // Use zero-init to ensure channelDelays are clean
    SessionConfig cfg = {}; 
    cfg.durationType = DUR_FIXED;
    cfg.durationFixed = 600;

    int res = engine->startSession(cfg);
    TEST_ASSERT_EQUAL(200, res); // Ensure start succeeded
    
    engine->tick(); // Tick to process transition ARMED -> LOCKED (assuming 0 delay)
    TEST_ASSERT_EQUAL(LOCKED, engine->getState());

    engine->abort("Penalty Test"); // Enters Penalty Box (ABORTED state)
    TEST_ASSERT_EQUAL(ABORTED, engine->getState());

    // 3. Serve Penalty
    // Advance enough ticks to clear penalty (assuming 300s default in deterrents)
    for(int i=0; i<305; i++) engine->tick();
    
    // 4. Verify Completion
    TEST_ASSERT_EQUAL(COMPLETED, engine->getState());
    
    // Check Visibility: Should be visible now
    const Reward* compHistory = engine->getRewardHistory();
    TEST_ASSERT_NOT_NULL(compHistory);
    // Code A should still be at index 0 (The user just paid for this session)
    TEST_ASSERT_EQUAL_STRING(codeA, compHistory[0].code);

    // 5. Simulate Reboot / Reset to READY
    // This triggers rotateAndGenerateReward(): [0]->[1], New->[0]
    engine->handleReboot();
    TEST_ASSERT_EQUAL(READY, engine->getState());

    // 6. Verify History Shift
    const Reward* readyHistory = engine->getRewardHistory();
    TEST_ASSERT_NOT_NULL(readyHistory);

    // Index 0 should be NEW Code B
    TEST_ASSERT_NOT_EQUAL(0, strcmp(codeA, readyHistory[0].code));
    
    // Index 1 should be OLD Code A (Preserved)
    TEST_ASSERT_EQUAL_STRING(codeA, readyHistory[1].code);

    delete engine;
}

int main(void) {
    UNITY_BEGIN();
    
    RUN_TEST(test_reward_visible_in_ready_state);
    RUN_TEST(test_reward_hidden_in_active_states);
    RUN_TEST(test_reward_hidden_in_penalty_box);
    RUN_TEST(test_reward_visible_and_correct_on_completion);
    RUN_TEST(test_reward_preserved_after_penalty_and_reboot);

    return UNITY_END();
}