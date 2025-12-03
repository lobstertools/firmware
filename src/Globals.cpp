/*
 * =================================================================================
 * Project:   Lobster Lock - Self-Bondage Session Manager
 * File:      Globals.h / Globals.cpp
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
SystemConfig g_systemConfig = DEFAULT_SETTINGS;

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

// =================================================================================
// SECTION: STATE MANAGEMENT
// =================================================================================

SessionState currentState = VALIDATING;
TriggerStrategy currentStrategy = STRAT_AUTO_COUNTDOWN;

uint8_t g_enabledChannelsMask = 0x0F; // Default: 1111 (All enabled)
unsigned long extButtonSignalStartTime = 0;

// =================================================================================
// SECTION: SESSION TIMERS & DELAYS
// =================================================================================

unsigned long lockSecondsRemaining = 0;
unsigned long penaltySecondsRemaining = 0;
unsigned long testSecondsRemaining = 0;
unsigned long triggerTimeoutRemaining = 0;

unsigned long penaltySecondsConfig = 0;
unsigned long lockSecondsConfig = 0;
bool hideTimer = false;

unsigned long channelDelaysRemaining[MAX_CHANNELS] = {0, 0, 0, 0};

// =================================================================================
// SECTION: FEATURE CONFIGURATION
// =================================================================================

bool enableStreaks = true;         // Default to true
bool enablePaybackTime = true;     // Default to true
bool enableRewardCode = true;      // Default to true
uint32_t paybackTimeSeconds = 900; // Default to 900s (15min).

// =================================================================================
// SECTION: STATISTICS & HISTORY
// =================================================================================

// Persistent Session Counters (loaded from NVS)
uint32_t sessionStreakCount = 0;
uint32_t completedSessions = 0;
uint32_t abortedSessions = 0;
uint32_t paybackAccumulated = 0;        // In seconds
uint32_t totalLockedSessionSeconds = 0; // Total accumulated lock time

Reward rewardHistory[REWARD_HISTORY_SIZE];

// =================================================================================
// SECTION: WATCHDOGS & INPUT TRACKING
// =================================================================================

volatile unsigned long g_buttonPressStartTime = 0;

// --- Keep-Alive Watchdog (LOCKED/TESTING) ---
unsigned long g_lastKeepAliveTime = 0; // For watchdog. 0 = disarmed.
int g_currentKeepAliveStrikes = 0;     // Counter for missed calls

// =================================================================================
// SECTION: STORAGE (PREFERENCES)
// =================================================================================

Preferences wifiPreferences;   // Namespace: "wifi-creds"
Preferences provisioningPrefs; // Namespace: "provisioning" (Hardware Config)
Preferences sessionState;      // Namespace: "session" (Dynamic State)
Preferences bootPrefs;         // Namespace: "boot" (Crash tracking)

// =================================================================================
// SECTION: NETWORK STATE
// =================================================================================

char g_wifiSSID[33] = {0};
char g_wifiPass[65] = {0};
bool g_wifiCredentialsExist = false;
volatile bool g_triggerProvisioning = false;