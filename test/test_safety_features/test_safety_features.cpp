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
    14400,       // maxSessionDuration (4 Hours)
    10           // minSessionDuration
};

// Define a permissive preset set for Failsafe testing (Max 2 weeks)
const SessionPresets permissivePresets = { 
    300, 600, 900, 1800, 3600, 7200, 
    1209600,     // Max (14 Days) - Allows testing large failsafe tiers
    10 
};

const DeterrentConfig deterrents = { 
    true, true, DETERRENT_FIXED, 300, 900, 300, 
    true, DETERRENT_FIXED, 60, 120, 60 
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
    MockSessionHAL hal; StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engageSafetyInterlock(hal, engine);

    SessionConfig cfg = {};
    cfg.durationType = DUR_FIXED;
    cfg.durationFixed = 100;
    cfg.triggerStrategy = STRAT_AUTO_COUNTDOWN;
    engine.startSession(cfg);
    engine.tick(); 

    TEST_ASSERT_EQUAL(LOCKED, engine.getState());

    hal.setSafetyInterlock(false);
    engine.tick(); 

    TEST_ASSERT_EQUAL(ABORTED, engine.getState());
    TEST_ASSERT_FALSE(engine.isHardwarePermitted());
    TEST_ASSERT_EQUAL_HEX8(0x00, hal.lastSafetyMask); 
}

void test_start_fails_without_interlock(void) {
    MockSessionHAL hal; StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    SessionConfig cfg = { DUR_FIXED, 60 };
    int result = engine.startSession(cfg);
    TEST_ASSERT_EQUAL(412, result); 
    TEST_ASSERT_EQUAL(READY, engine.getState());
}

// ============================================================================
// NEW TESTS: INPUT & WATCHDOGS
// ============================================================================

void test_hardware_abort_trigger(void) {
    MockSessionHAL hal; StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engageSafetyInterlock(hal, engine);

    SessionConfig cfg = { DUR_FIXED, 600, 0, 0, STRAT_AUTO_COUNTDOWN };
    engine.startSession(cfg);
    engine.tick(); 
    TEST_ASSERT_EQUAL(LOCKED, engine.getState());

    hal.simulateLongPress(); 
    engine.tick(); 
    TEST_ASSERT_EQUAL(ABORTED, engine.getState());
}

void test_watchdog_petting_prevents_timeout(void) {
    MockSessionHAL hal; StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engageSafetyInterlock(hal, engine);

    SessionConfig cfg = { DUR_FIXED, 600, 0, 0, STRAT_AUTO_COUNTDOWN };
    engine.startSession(cfg);
    engine.tick(); 

    for(int i=0; i<6; i++) { 
        hal.advanceTime(9000); 
        engine.tick();         
        engine.petWatchdog();
        TEST_ASSERT_EQUAL(LOCKED, engine.getState()); 
    }
}

void test_watchdog_petting_prevents_timeout_and_resets_strikes(void) {
    MockSessionHAL hal; StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engageSafetyInterlock(hal, engine);

    SessionConfig cfg = { DUR_FIXED, 600, 0, 0, STRAT_AUTO_COUNTDOWN };
    engine.startSession(cfg);
    engine.tick(); 

    hal.advanceTime(11000); 
    engine.tick(); 
    TEST_ASSERT_EQUAL(LOCKED, engine.getState());

    engine.petWatchdog();
    
    for(int i=0; i<3; i++) {
        hal.advanceTime(10100); 
        engine.tick();
    }
    TEST_ASSERT_EQUAL(LOCKED, engine.getState());
}

void test_ui_watchdog_timeout_aborts_session(void) {
    MockSessionHAL hal; StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engageSafetyInterlock(hal, engine);

    SessionConfig cfg = { DUR_FIXED, 600, 0, 0, STRAT_AUTO_COUNTDOWN };
    engine.startSession(cfg);
    engine.tick(); 

    uint32_t interval = defaults.keepAliveInterval; 
    uint32_t strikes = defaults.keepAliveMaxStrikes;
    
    for(uint32_t i=0; i <= strikes; i++) {
        hal.advanceTime(interval + 100); 
        engine.tick();
    }
    TEST_ASSERT_EQUAL(ABORTED, engine.getState());
}

void test_hardware_abort_works_without_validated_hardware(void) {
    MockSessionHAL hal; StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engine.loadState(LOCKED);
    
    hal.setSafetyRawButKeepValid(false, true); 
    hal.simulateLongPress(); 
    engine.tick();

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
    MockSessionHAL hal; StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engine.loadState(LOCKED); 
    engine.handleReboot();
    TEST_ASSERT_EQUAL(ABORTED, engine.getState());
    TEST_ASSERT_EQUAL_UINT32(300, engine.getTimers().penaltyRemaining); 
}

void test_reboot_from_armed_resets_to_ready(void) {
    MockSessionHAL hal; StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engine.loadState(ARMED);
    engine.handleReboot();
    TEST_ASSERT_EQUAL(READY, engine.getState());
    TEST_ASSERT_EQUAL_UINT32(0, engine.getTimers().lockDuration);
}

void test_reboot_from_testing_resets_to_ready(void) {
    MockSessionHAL hal; StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engine.loadState(TESTING);
    engine.handleReboot();
    TEST_ASSERT_EQUAL(READY, engine.getState());
}

void test_reboot_from_completed_resets_to_ready(void) {
    MockSessionHAL hal; StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engine.loadState(COMPLETED);
    engine.handleReboot();
    TEST_ASSERT_EQUAL(READY, engine.getState());
}

void test_reboot_from_aborted_resumes_penalty(void) {
    MockSessionHAL hal; StandardRules rules;
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
    MockSessionHAL hal; StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engine.loadState(READY);
    engine.handleReboot();
    TEST_ASSERT_EQUAL(READY, engine.getState());
}

// ============================================================================
// NEW FAILSAFE TIER TESTS (UPDATED WITH PERMISSIVE PRESETS)
// ============================================================================

void test_failsafe_tier_minimum_floor(void) {
    MockSessionHAL hal; StandardRules rules;
    // Uses permissivePresets to avoid clamping if we test edge cases, 
    // though for 60s the default 'presets' would also work.
    SessionEngine engine(hal, rules, defaults, permissivePresets, deterrents);
    engageSafetyInterlock(hal, engine);

    // 1. Start a very short session (60s)
    // Should bump up to the minimum tier (4 hours = 14400s)
    SessionConfig cfg = { DUR_FIXED, 60, 0, 0, STRAT_AUTO_COUNTDOWN };
    
    engine.startSession(cfg); 
    engine.tick(); 

    // Verify
    TEST_ASSERT_TRUE(hal.failsafeArmed);
    TEST_ASSERT_EQUAL_UINT32(14400, hal.lastFailsafeArmedSeconds);
}

void test_failsafe_tier_rounding_up(void) {
    MockSessionHAL hal; StandardRules rules;
    // MUST use permissivePresets, otherwise 18000s is clamped to 14400s by Rules
    SessionEngine engine(hal, rules, defaults, permissivePresets, deterrents);
    engageSafetyInterlock(hal, engine);

    // 1. Start a session of 5 hours (18000s)
    // Should round up to next tier (8 hours = 28800s)
    SessionConfig cfg = { DUR_FIXED, 18000, 0, 0, STRAT_AUTO_COUNTDOWN };
    
    engine.startSession(cfg);
    engine.tick(); 

    // Verify
    TEST_ASSERT_TRUE(hal.failsafeArmed);
    TEST_ASSERT_EQUAL_UINT32(28800, hal.lastFailsafeArmedSeconds);
}

void test_failsafe_tier_exact_match(void) {
    MockSessionHAL hal; StandardRules rules;
    // MUST use permissivePresets
    SessionEngine engine(hal, rules, defaults, permissivePresets, deterrents);
    engageSafetyInterlock(hal, engine);

    // 1. Start a session exactly 12 hours (43200s)
    // Should stay at 12 hours
    SessionConfig cfg = { DUR_FIXED, 43200, 0, 0, STRAT_AUTO_COUNTDOWN };
    
    engine.startSession(cfg);
    engine.tick(); 

    // Verify
    TEST_ASSERT_TRUE(hal.failsafeArmed);
    TEST_ASSERT_EQUAL_UINT32(43200, hal.lastFailsafeArmedSeconds);
}

void test_failsafe_disarms_on_completion(void) {
    MockSessionHAL hal; StandardRules rules;
    SessionEngine engine(hal, rules, defaults, permissivePresets, deterrents);
    engageSafetyInterlock(hal, engine);

    // Start
    SessionConfig cfg = { DUR_FIXED, 60 };
    engine.startSession(cfg);
    engine.tick();
    
    // Assert Armed
    TEST_ASSERT_TRUE(hal.failsafeArmed);

    // Force completion
    SessionTimers t = engine.getTimers();
    t.lockRemaining = 1; 
    engine.loadTimers(t);
    engine.tick(); // Process decrement -> 0 -> Complete

    // Assert Disarmed
    TEST_ASSERT_EQUAL(COMPLETED, engine.getState());
    TEST_ASSERT_FALSE(hal.failsafeArmed);
}

// ============================================================================
// HARDWARE & NETWORK LOGIC
// ============================================================================

void test_channel_delay_masking(void) {
    MockSessionHAL hal; StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engageSafetyInterlock(hal, engine);

    SessionConfig cfg = { DUR_FIXED, 60, 0, 0, STRAT_AUTO_COUNTDOWN };
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
    MockSessionHAL hal; StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engageSafetyInterlock(hal, engine);

    SessionConfig cfg = { DUR_FIXED, 60, 0, 0, STRAT_AUTO_COUNTDOWN };
    engine.startSession(cfg);
    engine.tick(); 
    TEST_ASSERT_EQUAL(LOCKED, engine.getState());

    int result = engine.startSession(cfg);
    TEST_ASSERT_EQUAL(409, result);
    TEST_ASSERT_EQUAL(LOCKED, engine.getState());
}

void test_network_failure_while_ready(void) {
    MockSessionHAL hal; StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engageSafetyInterlock(hal, engine);
    
    hal.setNetworkProvisioningRequest(true);
    engine.tick();
    TEST_ASSERT_TRUE(hal._enteredProvisioningMode);
}

void test_network_failure_while_locked_aborts_session(void) {
    MockSessionHAL hal; StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engageSafetyInterlock(hal, engine);

    SessionConfig cfg = { DUR_FIXED, 600, 0, 0, STRAT_AUTO_COUNTDOWN };
    engine.startSession(cfg);
    engine.tick(); 
    TEST_ASSERT_EQUAL(LOCKED, engine.getState());

    hal.setNetworkProvisioningRequest(true);
    engine.tick();
    
    TEST_ASSERT_EQUAL(ABORTED, engine.getState());
    TEST_ASSERT_FALSE(hal._enteredProvisioningMode);
}

void test_network_provisioning_blocked_until_penalty_complete(void) {
    MockSessionHAL hal; StandardRules rules;
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
    MockSessionHAL hal; StandardRules rules;
    SessionEngine engine(hal, rules, defaults, presets, deterrents);
    engageSafetyInterlock(hal, engine);
    
    hal.setNetworkProvisioningRequest(true);
    SessionConfig cfg = { DUR_FIXED, 60 };
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

    // 3. Failsafe Tier Logic
    RUN_TEST(test_failsafe_tier_minimum_floor);
    RUN_TEST(test_failsafe_tier_rounding_up);
    RUN_TEST(test_failsafe_tier_exact_match);
    RUN_TEST(test_failsafe_disarms_on_completion);

    // 4. Hardware & Network
    RUN_TEST(test_channel_delay_masking);
    RUN_TEST(test_start_rejected_if_already_locked);
    RUN_TEST(test_network_failure_while_ready);
    RUN_TEST(test_network_failure_while_locked_aborts_session);
    RUN_TEST(test_network_provisioning_blocked_until_penalty_complete);
    RUN_TEST(test_start_session_fails_if_network_unstable);

    return UNITY_END();
}