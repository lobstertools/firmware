/*
 * =================================================================================
 * Project:   Lobster Lock - Self-Bondage Session Manager
 * File:      Globals.cpp
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Description:
 * Definitions of shared global state variables, synchronization primitives (Mutex),
 * hardware object instances (Button, LED), and persistent configuration flags.
 * =================================================================================
 */
#include "Globals.h"

// =================================================================================
// SECTION: HARDWARE & SYSTEM OBJECTS
// =================================================================================

AsyncWebServer server(80);

// Initialize split configurations
SystemDefaults g_systemDefaults = DEFAULT_SYSTEM_DEFS;
SessionLimits g_sessionLimits = DEFAULT_SESSION_LIMITS;

// --- Button Configuration ---

// 1. PCB Button: Always Present (GPIO 0 / Boot Button). Active LOW.
OneButton pcbButton(PCB_BUTTON_PIN, true, true);

// 2. External Button: Only in Release (defined in Config.h)
#ifdef EXT_BUTTON_PIN
// Production: Uses a Normally Closed (NC) abort switch.
// Active State = HIGH (Open Circuit).
OneButton extButton(EXT_BUTTON_PIN, false, true);
#else
// Placeholder for Debug mode to prevent compilation errors,
// though it won't be ticked or attached.
OneButton extButton(-1, true, true);
#endif

jled::JLed statusLed = jled::JLed(STATUS_LED_PIN);

// --- Synchronization ---
SemaphoreHandle_t stateMutex = NULL;
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED; // Critical section for tick counter

unsigned long extButtonSignalStartTime = 0;

volatile unsigned long g_buttonPressStartTime = 0;

// =================================================================================
// SECTION: STATE MANAGEMENT
// =================================================================================

DeviceState g_currentState = VALIDATING;

ActiveSessionConfig g_activeSessionConfig = {
    DUR_FIXED,            // durationType
    900,                  // durationMin (15m)
    2700,                 // durationMax (45m)
    STRAT_AUTO_COUNTDOWN, // triggerStrategy
    {30, 30, 30, 30},     // channelDelays (30s default)
    false                 // hideTimer
};

uint8_t g_enabledChannelsMask = 0x0F; // Default: 1111 (All enabled)

// =================================================================================
// SECTION: SESSION TIMERS & DELAYS
// =================================================================================

// Updated to match the 6 fields in SessionTimers struct
SessionTimers g_sessionTimers = {
    0,           // lockDuration
    0,           // penaltyDuration
    0,           // lockRemaining
    0,           // penaltyRemaining
    0,           // testRemaining
    0,           // triggerTimeout
    {0, 0, 0, 0} // channelDelays
};

// =================================================================================
// SECTION: FEATURE PROVISIONING
// =================================================================================

DeterrentConfig g_deterrentConfig = {
    true, // enableStreaks
    true, // enableRewardCode
    2700, // rewardPenalty (45 min)
    true, // enablePaybackTime
    900   // paybackTime (15 min)
};

// =================================================================================
// SECTION: STATISTICS & HISTORY
// =================================================================================

// Persistent Session Counters (loaded from NVS)
SessionStats g_sessionStats = {
    0, // streaks
    0, // completed
    0, // aborted
    0, // paybackAccumulated
    0  // totalLockedTime
};

Reward rewardHistory[REWARD_HISTORY_SIZE];

// =================================================================================
// SECTION: WATCHDOGS & INPUT TRACKING
// =================================================================================

// --- Keep-Alive Session Watchdog ---
unsigned long g_lastKeepAliveTime = 0; // 0 = disarmed.
int g_currentKeepAliveStrikes = 0;     // Counter for missed calls

// =================================================================================
// SECTION: STORAGE (PREFERENCES)
// =================================================================================

Preferences wifiPreferences;   // Namespace: "wifi-creds"
Preferences provisioningPrefs; // Namespace: "provisioning" (Hardware and Feature Config)
Preferences sessionState;      // Namespace: "session" (Dynamic State)
Preferences bootPrefs;         // Namespace: "boot" (Crash tracking)

// =================================================================================
// SECTION: NETWORK STATE
// =================================================================================

char g_wifiSSID[33] = {0};
char g_wifiPass[65] = {0};
bool g_wifiCredentialsExist = false;
volatile bool g_triggerProvisioning = false;