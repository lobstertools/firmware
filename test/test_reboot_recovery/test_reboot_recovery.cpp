/*
 * File: test/test_reboot_recovery.cpp
 * Description: Deep-dive tests for unhappy path recovery after reboot.
 * Covers configuration persistence failures and complex hardware/state chains.
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
    return new SessionEngine(hal, rules, defaults, presets, deterrents);
}

void setUp(void) {}
void tearDown(void) {}

// ============================================================================
// TEST: ZERO-TIME CORRUPTION
// ============================================================================
void test_reboot_locked_with_zero_time_aborts(void) {
    MockSessionHAL hal; StandardRules rules;
    SessionEngine* engine = createEngine(hal, rules);

    // 1. Simulate NVS Data: LOCKED state, but 0 time remaining
    engine->loadState(LOCKED);
    SessionTimers t = {0};
    t.lockDuration = 600;
    t.lockRemaining = 0; // CORRUPTION / RACE CONDITION
    engine->loadTimers(t);

    // 2. Reboot
    engine->handleReboot();

    // 3. Assert: Should still Abort (Safe Fail) rather than Complete
    TEST_ASSERT_EQUAL(ABORTED, engine->getState());
    
    // Check penalty applied
    TEST_ASSERT_GREATER_THAN(0, engine->getTimers().penaltyRemaining);

    delete engine;
}

// ============================================================================
// TEST: BROKEN WIRE LOOP (CHAIN REACTION)
// ============================================================================
void test_reboot_chain_locked_to_paused_penalty(void) {
    MockSessionHAL hal; StandardRules rules;
    SessionEngine* engine = createEngine(hal, rules);

    // 1. Simulate NVS Data: LOCKED
    engine->loadState(LOCKED);
    SessionTimers t = {0};
    t.lockDuration = 600;
    t.lockRemaining = 300;
    engine->loadTimers(t);

    // 2. Hardware is BROKEN (Disconnected) on boot
    hal.setSafetyInterlock(false);

    // 3. Reboot Event
    // Should detect LOCKED -> Abort("Reboot") -> State ABORTED
    engine->handleReboot();

    TEST_ASSERT_EQUAL(ABORTED, engine->getState());
    uint32_t penaltyStart = engine->getTimers().penaltyRemaining;
    TEST_ASSERT_GREATER_THAN(0, penaltyStart);

    // 4. Tick (Process Time)
    // Hardware is INVALID, so time should pause.
    engine->tick();
    engine->tick();
    engine->tick();

    // 5. Assert: Timer did not move
    TEST_ASSERT_EQUAL_UINT32(penaltyStart, engine->getTimers().penaltyRemaining);
    
    // 6. Fix Hardware
    hal.setSafetyInterlock(true);
    engine->tick();

    // 7. Assert: Timer moves now
    TEST_ASSERT_EQUAL_UINT32(penaltyStart - 1, engine->getTimers().penaltyRemaining);

    delete engine;
}

/*
 * File: test/test_reboot_recovery/test_reboot_recovery.cpp
 * Add this test case to verify Fix 3 (Reboot = Zero Debt Paid)
 */
void test_reboot_results_in_zero_debt_paid(void) {
    MockSessionHAL hal; StandardRules rules;
    
    // Config: Max 4h. Payback Enabled.
    SessionPresets p = presets; p.maxSessionDuration = 14400;
    DeterrentConfig d = deterrents; d.enablePaybackTime = true;
    
    // Setup: 10h Debt
    SessionStats initialStats = {0};
    initialStats.paybackAccumulated = 36000;

    SessionEngine* engine = new SessionEngine(hal, rules, defaults, p, d);
    engine->loadStats(initialStats);
    hal.setSafetyInterlock(true);

    // 1. Start Session (1h Base + 3h Debt = 4h Total)
    SessionConfig req = { DUR_FIXED, 3600 }; 
    engine->startSession(req);
    engine->tick(); // ARMED -> LOCKED. This transition AUTOMATICALLY saves state to HAL.

    // Verify Debt Portion is calculated correctly (internal state)
    TEST_ASSERT_EQUAL_UINT32(10800, engine->getTimers().debtServed);

    // 2. Simulate Reboot (Persistence of STATE, but Failure of SESSION)
    // The mock HAL already has the latest state from the tick() call above.
    SessionTimers savedTimers = hal.savedTimers;
    SessionStats savedStats = hal.savedStats;
    DeviceState savedState = hal.savedState;
    SessionConfig savedConfig = hal.savedConfig;
    
    delete engine;
    engine = new SessionEngine(hal, rules, defaults, p, d);
    engine->loadState(savedState);
    engine->loadTimers(savedTimers);
    engine->loadStats(savedStats);
    engine->loadConfig(savedConfig);
    
    // 3. Handle Reboot -> Should ABORT (Reboot punishment)
    engine->handleReboot();

    TEST_ASSERT_EQUAL(ABORTED, engine->getState());

    // 4. Verify ZERO Debt Paid
    // The debt must NOT have decreased. It might have increased due to penalty.
    TEST_ASSERT_GREATER_OR_EQUAL(36000, engine->getStats().paybackAccumulated);
    
    delete engine;
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_reboot_locked_with_zero_time_aborts);
    RUN_TEST(test_reboot_chain_locked_to_paused_penalty);
    RUN_TEST(test_reboot_results_in_zero_debt_paid);
    return UNITY_END();
}