/*
 * Globals.cpp
 * Definitions of shared global variables.
 */

#include "Globals.h"

// --- Globals ---
AsyncWebServer server(80);
SystemConfig g_systemConfig = DEFAULT_SETTINGS;

// --- Button Configuration ---
OneButton button(ONE_BUTTON_PIN, true, true); // Pin, active low, internal pull-up

jled::JLed statusLed = jled::JLed(STATUS_LED_PIN);

// --- Synchronization ---
SemaphoreHandle_t stateMutex = NULL;
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED; // Critical section for tick counter

SessionState currentState = READY;
TriggerStrategy currentStrategy = STRAT_AUTO_COUNTDOWN;

uint8_t g_enabledChannelsMask = 0x0F; // Default: 1111 (All enabled)

// --- Global State & Timers ---
unsigned long lockSecondsRemaining = 0;
unsigned long penaltySecondsRemaining = 0;
unsigned long testSecondsRemaining = 0;
unsigned long triggerTimeoutRemaining = 0;

unsigned long penaltySecondsConfig = 0;
unsigned long lockSecondsConfig = 0;
bool hideTimer = false;

unsigned long channelDelaysRemaining[MAX_CHANNELS] = {0, 0, 0, 0};

// --- Device and Sessing Configuration ---
bool enableStreaks = true;         // Default to true
bool enablePaybackTime = true;     // Default to true
bool enableRewardCode = true;      // Default to true
uint32_t paybackTimeSeconds = 900; // Default to 900s (15min).

// Persistent Session Counters (loaded from NVS)
uint32_t sessionStreakCount = 0;
uint32_t completedSessions = 0;
uint32_t abortedSessions = 0;
uint32_t paybackAccumulated = 0;        // In seconds
uint32_t totalLockedSessionSeconds = 0; // Total accumulated lock time

Reward rewardHistory[REWARD_HISTORY_SIZE];

volatile unsigned long g_buttonPressStartTime = 0;

// --- Keep-Alive Watchdog (LOCKED/TESTING) ---
unsigned long g_lastKeepAliveTime = 0; // For watchdog. 0 = disarmed.
int g_currentKeepAliveStrikes = 0;     // Counter for missed calls

// NVS (Preferences) objects
Preferences wifiPreferences;   // Namespace: "wifi-creds"
Preferences provisioningPrefs; // Namespace: "provisioning" (Hardware Config)
Preferences sessionState;      // Namespace: "session" (Dynamic State)
Preferences bootPrefs;         // Namespace: "boot" (Crash tracking)

char g_wifiSSID[33] = {0};
char g_wifiPass[65] = {0};
bool g_wifiCredentialsExist = false;
volatile bool g_triggerProvisioning = false;