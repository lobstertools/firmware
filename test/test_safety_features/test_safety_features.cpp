/*
 * File: test/test_safety_features/test_safety_features.cpp
 * Description: Verifies Safety Interlock disconnects, Reboot recovery, and Input Watchdogs.
 */
#include <unity.h>
#include "Session.h"
#include "MockSessionHAL.h"
#include "StandardRules.h"

// --- Constants ---
const SystemDefaults defaults = { 5, 10, 240, 10000, 4, 5, 30000, 3, 60 };

const SessionPresets presets = { 
    300, 600,    // Short Range
    900, 1800,   // Medium Range
    3600, 7200,  // Long Range
    14400,       // maxSessionDuration
    10           // minSessionDuration
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
// EXISTING TESTS (Safety Interlock)
// ============================================================================

void test_interlock_disconnect_during_lock(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engageSafetyInterlock(hal, engine);

    // Start and Lock
    SessionConfig cfg = {};
    cfg.durationType = DUR_FIXED;
    cfg.durationFixed = 100;
    cfg.triggerStrategy = STRAT_AUTO_COUNTDOWN;
    engine.startSession(cfg);
    engine.tick(); // Lock it

    TEST_ASSERT_EQUAL(LOCKED, engine.getState());

    // ACT: Disconnect Safety
    hal.setSafetyInterlock(false);
    engine.tick(); // Engine detects state change

    // ASSERT: Immediate Abort
    TEST_ASSERT_EQUAL(ABORTED, engine.getState());
    TEST_ASSERT_FALSE(engine.isHardwarePermitted());
    TEST_ASSERT_EQUAL_HEX8(0x00, hal.lastSafetyMask); // All Pins OFF
}

void test_start_fails_without_interlock(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);

    // Do NOT engage safety.
    
    SessionConfig cfg = {};
    cfg.durationType = DUR_FIXED;
    cfg.durationFixed = 60;

    int result = engine.startSession(cfg);

    TEST_ASSERT_EQUAL(412, result); // Precondition Failed
    TEST_ASSERT_EQUAL(READY, engine.getState());
}

// ============================================================================
// NEW TESTS: INPUT & WATCHDOGS
// ============================================================================

/**
 * Scenario: Hardware Long Press (Abort) is triggered during Lock.
 * Verifies logic: if (_hal.checkAbortAction()) { abort("Manual Long-Press"); }
 */
void test_hardware_abort_trigger(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engageSafetyInterlock(hal, engine);

    // 1. Start and Lock
    SessionConfig cfg = {};
    cfg.durationType = DUR_FIXED;
    cfg.durationFixed = 600;
    cfg.triggerStrategy = STRAT_AUTO_COUNTDOWN;
    engine.startSession(cfg);
    engine.tick(); 
    TEST_ASSERT_EQUAL(LOCKED, engine.getState());

    // 2. Act: Simulate Long Press (Hardware Abort)
    // This mocks the button ISR setting the internal flag
    hal.simulateLongPress(); 
    engine.tick(); // Engine processes checkAbortAction()

    // 3. Assert
    TEST_ASSERT_EQUAL(ABORTED, engine.getState());
}

/**
 * Scenario: User interacts with the device (petWatchdog) before timeout.
 * Verifies logic: void SessionEngine::petWatchdog() resets strikes.
 */
void test_watchdog_petting_prevents_timeout(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engageSafetyInterlock(hal, engine);

    // 1. Start and Lock
    SessionConfig cfg = {};
    cfg.durationType = DUR_FIXED;
    cfg.durationFixed = 600; 
    cfg.triggerStrategy = STRAT_AUTO_COUNTDOWN;
    engine.startSession(cfg);
    engine.tick(); 

    // 2. Run for a duration that WOULD cause a timeout if ignored.
    // Config: 4 strikes * 10000ms = 40s timeout.
    // We run for 60s total, but we pet the dog every 9s.
    
    for(int i=0; i<6; i++) { 
        hal.advanceTime(9000); // 9 seconds passed
        engine.tick();         // Process time
        
        // ACT: Pet the watchdog
        engine.petWatchdog();
        
        TEST_ASSERT_EQUAL(LOCKED, engine.getState()); // Should stay locked
    }
}

/**
 * Scenario: User interacts with the device (petWatchdog) AFTER a strike has occurred.
 */
void test_watchdog_petting_prevents_timeout_and_resets_strikes(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engageSafetyInterlock(hal, engine);

    // 1. Start and Lock
    SessionConfig cfg = {};
    cfg.durationType = DUR_FIXED;
    cfg.durationFixed = 600; 
    cfg.triggerStrategy = STRAT_AUTO_COUNTDOWN;
    engine.startSession(cfg);
    engine.tick(); 

    // 2. Intentionally allow ONE strike to accumulate.
    // Interval is 10s. We advance 11s.
    hal.advanceTime(11000); 
    engine.tick(); // _currentKeepAliveStrikes becomes 1.

    // Verify we haven't aborted yet (Max strikes = 4)
    TEST_ASSERT_EQUAL(LOCKED, engine.getState());

    // 3. ACT: Pet the watchdog
    engine.petWatchdog();
    
    // 4. Verify strikes were actually reset.
    // If they were NOT reset, accumulating 3 more strikes (total 4) would trigger ABORT.
    // If they WERE reset, accumulating 3 strikes (total 3) leaves us LOCKED.
    
    for(int i=0; i<3; i++) {
        hal.advanceTime(10100); // 10.1s -> 1 strike per loop
        engine.tick();
    }
    
    TEST_ASSERT_EQUAL(LOCKED, engine.getState());
}

void test_ui_watchdog_timeout_aborts_session(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engageSafetyInterlock(hal, engine);

    // 1. Start and Lock
    SessionConfig cfg = {};
    cfg.durationType = DUR_FIXED;
    cfg.durationFixed = 600; 
    cfg.triggerStrategy = STRAT_AUTO_COUNTDOWN;
    engine.startSession(cfg);
    engine.tick(); // Locked

    // 2. Advance time to simulate UI silence
    // Defaults: Interval 10000ms (10s), Max Strikes 4.
    uint32_t interval = defaults.keepAliveInterval; 
    uint32_t strikes = defaults.keepAliveMaxStrikes;
    
    // Simulate passing of time without UI interaction (strike accumulation)
    for(uint32_t i=0; i <= strikes; i++) {
        hal.advanceTime(interval + 100); 
        engine.tick();
    }

    // 3. Assert Abort
    TEST_ASSERT_EQUAL(ABORTED, engine.getState());
}

/**
 * Scenario: Validates that a hardware Long Press triggers an abort 
 * even if the Safety Interlock has not yet granted permission (e.g., during stabilization).
 */
void test_hardware_abort_works_without_validated_hardware(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);

    engine.loadState(LOCKED);
    
    // SETUP: Simulate the "Grace Period" state
    // Physical Switch is OPEN (False) -> User is pressing button
    // Logical Safety is TRUE (True) -> Grace period active in HAL
    hal.setSafetyRawButKeepValid(false, true); 

    // ACT: Simulate Long Press
    hal.simulateLongPress(); 
    engine.tick();

    // ASSERT
    // Should abort via Manual Long Press, NOT Safety Disconnect
    bool foundManualAbort = false;
    for (const auto& logLine : hal.logs) {
        if (logLine.find("Abort Source: Manual Long-Press") != std::string::npos) {
            foundManualAbort = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(foundManualAbort);
    TEST_ASSERT_EQUAL(ABORTED, engine.getState());
}

// ============================================================================
// EXTENDED REBOOT SCENARIOS
// ============================================================================

void test_reboot_from_locked_enforces_penalty(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);

    engine.loadState(LOCKED); 
    engine.handleReboot();

    TEST_ASSERT_EQUAL(ABORTED, engine.getState());
    TEST_ASSERT_EQUAL_UINT32(300, engine.getTimers().penaltyRemaining); 
}

void test_reboot_from_armed_resets_to_ready(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);

    engine.loadState(ARMED);
    engine.handleReboot();

    TEST_ASSERT_EQUAL(READY, engine.getState());
    TEST_ASSERT_EQUAL_UINT32(0, engine.getTimers().lockDuration);
}

void test_reboot_from_testing_resets_to_ready(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);

    engine.loadState(TESTING);
    engine.handleReboot();

    TEST_ASSERT_EQUAL(READY, engine.getState());
}

void test_reboot_from_completed_resets_to_ready(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);

    engine.loadState(COMPLETED);
    engine.handleReboot();

    TEST_ASSERT_EQUAL(READY, engine.getState());
}

void test_reboot_from_aborted_resumes_penalty(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);

    engine.loadState(ABORTED);
    SessionTimers t = {0};
    t.penaltyDuration = 300;
    t.penaltyRemaining = 150; 
    engine.loadTimers(t);

    engine.handleReboot();

    TEST_ASSERT_EQUAL(ABORTED, engine.getState());
    TEST_ASSERT_EQUAL_UINT32(150, engine.getTimers().penaltyRemaining);
}

void test_reboot_from_ready_stays_ready(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);

    engine.loadState(READY);
    engine.handleReboot();

    TEST_ASSERT_EQUAL(READY, engine.getState());
}

// ============================================================================
// HARDWARE & NETWORK LOGIC
// ============================================================================

void test_channel_delay_masking(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engageSafetyInterlock(hal, engine);

    SessionConfig cfg = {};
    cfg.durationType = DUR_FIXED;
    cfg.durationFixed = 60;
    cfg.triggerStrategy = STRAT_AUTO_COUNTDOWN;
    
    cfg.channelDelays[0] = 0; 
    cfg.channelDelays[1] = 5; 
    cfg.channelDelays[2] = 10;
    cfg.channelDelays[3] = 60; 

    engine.startSession(cfg); 
    engine.tick(); 
    TEST_ASSERT_EQUAL_HEX8(0x01, hal.lastSafetyMask);

    for(int i=0; i<5; i++) engine.tick();
    TEST_ASSERT_EQUAL_HEX8(0x03, hal.lastSafetyMask);

    for(int i=0; i<5; i++) engine.tick();
    TEST_ASSERT_EQUAL_HEX8(0x07, hal.lastSafetyMask);
}

void test_start_rejected_if_already_locked(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engageSafetyInterlock(hal, engine);

    SessionConfig cfg = {};
    cfg.durationType = DUR_FIXED;
    cfg.durationFixed = 60;
    cfg.triggerStrategy = STRAT_AUTO_COUNTDOWN;
    engine.startSession(cfg);
    engine.tick(); 
    TEST_ASSERT_EQUAL(LOCKED, engine.getState());

    int result = engine.startSession(cfg);
    TEST_ASSERT_EQUAL(409, result);
    TEST_ASSERT_EQUAL(LOCKED, engine.getState());
}

void test_network_failure_while_ready(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engageSafetyInterlock(hal, engine);
    
    hal.setNetworkProvisioningRequest(true);
    engine.tick();
    TEST_ASSERT_TRUE(hal._enteredProvisioningMode);
}

void test_network_failure_while_locked_aborts_session(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engageSafetyInterlock(hal, engine);

    SessionConfig cfg = {};
    cfg.durationType = DUR_FIXED;
    cfg.durationFixed = 600;
    cfg.triggerStrategy = STRAT_AUTO_COUNTDOWN;
    engine.startSession(cfg);
    engine.tick(); 
    TEST_ASSERT_EQUAL(LOCKED, engine.getState());

    hal.setNetworkProvisioningRequest(true);
    engine.tick();
    
    TEST_ASSERT_EQUAL(ABORTED, engine.getState());
    TEST_ASSERT_FALSE(hal._enteredProvisioningMode);
}

void test_network_provisioning_blocked_until_penalty_complete(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    
    engine.loadState(ABORTED);
    SessionTimers t = {0};
    t.penaltyRemaining = 1; 
    engine.loadTimers(t);
    
    hal.setNetworkProvisioningRequest(true);
    engine.tick(); 
    
    TEST_ASSERT_EQUAL(COMPLETED, engine.getState());
    TEST_ASSERT_FALSE(hal._enteredProvisioningMode); 
    
    engine.tick();
    TEST_ASSERT_TRUE(hal._enteredProvisioningMode);
}

void test_start_session_fails_if_network_unstable(void) {
    MockSessionHAL hal;
    StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engageSafetyInterlock(hal, engine);
    
    hal.setNetworkProvisioningRequest(true);
    
    SessionConfig cfg = {};
    cfg.durationType = DUR_FIXED;
    cfg.durationFixed = 60;
    
    int result = engine.startSession(cfg);
    
    TEST_ASSERT_EQUAL(503, result);
    TEST_ASSERT_EQUAL(READY, engine.getState());
}

int main(void) {
    UNITY_BEGIN();
    
    // 1. Safety & Inputs
    RUN_TEST(test_interlock_disconnect_during_lock);
    RUN_TEST(test_start_fails_without_interlock);
    RUN_TEST(test_hardware_abort_trigger);
    RUN_TEST(test_watchdog_petting_prevents_timeout);
    RUN_TEST(test_watchdog_petting_prevents_timeout_and_resets_strikes);
    RUN_TEST(test_ui_watchdog_timeout_aborts_session);
    RUN_TEST(test_hardware_abort_works_without_validated_hardware);

    // 2. Reboot Scenarios
    RUN_TEST(test_reboot_from_locked_enforces_penalty);
    RUN_TEST(test_reboot_from_armed_resets_to_ready);
    RUN_TEST(test_reboot_from_testing_resets_to_ready);
    RUN_TEST(test_reboot_from_completed_resets_to_ready);
    RUN_TEST(test_reboot_from_aborted_resumes_penalty);
    RUN_TEST(test_reboot_from_ready_stays_ready);

    // 3. Hardware & Network
    RUN_TEST(test_channel_delay_masking);
    RUN_TEST(test_start_rejected_if_already_locked);
    RUN_TEST(test_network_failure_while_ready);
    RUN_TEST(test_network_failure_while_locked_aborts_session);
    RUN_TEST(test_network_provisioning_blocked_until_penalty_complete);
    RUN_TEST(test_start_session_fails_if_network_unstable);

    return UNITY_END();
}