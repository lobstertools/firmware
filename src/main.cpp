/*
 * =================================================================
 * Project:   Lobster Lock - Self-Bondage Session Manager
 * File:      main.cpp
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Description:
 * Core firmware for the Lobster Lock ESP32 device. This code
 * manages all session state (e.g., READY, ARMED, LOCKED),
 * provides a JSON API for the web UI, and persists the
 * session to NVS (Preferences) to survive power loss.
 * =================================================================
 */

#include <Arduino.h>
#include <memory>

// --- Platform-Specific Includes ---
#include <WiFi.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h" // Required for Brownout handling
#include "esp_timer.h"        // Required for Death Grip Timer
#include <esp_system.h>       // Standard system calls

// --- Library Includes ---
#include <ESPAsyncWebServer.h>
#include <Ticker.h>
#include <vector>
#include <array>
#include <string>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include "time.h"
#include <OneButton.h>
#include <jled.h>

// --- LIBRARIES FOR STAGE 1, 2, & NVS ---
#include <Preferences.h>  // For Wi-Fi credentials AND session state
#include <BLEDevice.h>    // For Stage 1 Provisioning
#include <BLEUtils.h>
#include <BLEServer.h>
#include <ESPmDNS.h>      // For Stage 2 Discovery

using namespace ArduinoJson;

// =================================================================
// --- Hardware & Global Configuration ---
// =================================================================

#define SERIAL_BAUD_RATE 115200
#define DEFAULT_WDT_TIMEOUT 20 // Relaxed for READY state
#define CRITICAL_WDT_TIMEOUT 5 // Tight for LOCKED state

// --- Channel-Specific Configuration ---
const int HARDWARE_PINS[4] = {16, 17, 18, 19};
const int MAX_CHANNELS = 4;

// Bitmask for enabled channels (loaded from Provisioning NVS)
// Bit 0 = Ch1, Bit 1 = Ch2, etc.
uint8_t g_enabledChannelsMask = 0x0F; // Default: 1111 (All enabled)

// --- Button Configuration ---
OneButton button(ONE_BUTTON_PIN, true, true); // Pin, active low, internal pull-up

// --- Session Constants & NVS ---
#define REWARD_HISTORY_SIZE 10
#define REWARD_CODE_LENGTH 32
#define REWARD_CHECKSUM_LENGTH 16

// Struct for storing reward code history.
struct Reward {
  char code[REWARD_CODE_LENGTH + 1];
  char checksum[REWARD_CHECKSUM_LENGTH + 1]; 
};

// Main state machine enum.
enum SessionState : uint8_t { READY, ARMED, LOCKED, ABORTED, COMPLETED, TESTING };

// Trigger Strategy (How we move from ARMED -> LOCKED)
enum TriggerStrategy : uint8_t { STRAT_AUTO_COUNTDOWN, STRAT_BUTTON_TRIGGER };

// --- SYTEM PREFERENCES ---
struct SystemConfig {
    uint32_t longPressSeconds;
    uint32_t minLockSeconds;
    uint32_t maxLockSeconds;
    uint32_t minPenaltySeconds;
    uint32_t maxPenaltySeconds;
    uint32_t testModeDurationSeconds;
    uint32_t failsafeMaxLockSeconds;
    uint32_t keepAliveIntervalMs;
    uint32_t keepAliveMaxStrikes;
    uint32_t bootLoopThreshold;
    uint32_t stableBootTimeMs;
    uint32_t wifiMaxRetries;
    uint32_t armedTimeoutSeconds;
};

// Default values (used if NVS is empty)
const SystemConfig DEFAULT_SETTINGS = {
    5,          // longPressSeconds
    900,        // minLockSeconds (15 min)
    10800,      // maxLockSeconds (180 min)
    900,        // minPenaltySeconds (15 min)
    10800,      // maxPenaltySeconds (180 min)
    120,        // testModeDurationSeconds
    14400,      // failsafeMaxLockSeconds (4 hours)
    10000,      // keepAliveIntervalMs
    4,          // keepAliveMaxStrikes
    5,          // bootLoopThreshold (Default)
    120000,     // stableBootTimeMs (Default 2 Minutes)
    5,          // wifiMaxRetries (Default)
    600         // armedTimeoutSeconds (10 Minutes Default)
};

SystemConfig g_systemConfig = DEFAULT_SETTINGS;

// --- Globals ---
AsyncWebServer server(80);
Ticker oneSecondMasterTicker;

// --- Synchronization ---
// Mutex to guard shared state between Async Web Task and Main Loop Task
SemaphoreHandle_t stateMutex = NULL;
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED; // Critical section for tick counter

SessionState currentState = READY;
TriggerStrategy currentStrategy = STRAT_AUTO_COUNTDOWN;

// NVS (Preferences) objects
Preferences wifiPreferences;      // Namespace: "wifi-creds"
Preferences provisioningPrefs;    // Namespace: "provisioning" (Hardware Config)
Preferences sessionState;         // Namespace: "session" (Dynamic State)
Preferences bootPrefs;            // Namespace: "boot" (Crash tracking)

jled::JLed statusLed = jled::JLed(STATUS_LED_PIN);

// Global array to hold reward history.
Reward rewardHistory[REWARD_HISTORY_SIZE];

// --- Global State & Timers ---
unsigned long lockSecondsRemaining = 0;
unsigned long penaltySecondsRemaining = 0;
unsigned long testSecondsRemaining = 0;
unsigned long triggerTimeoutRemaining = 0;

unsigned long penaltySecondsConfig = 0;
unsigned long lockSecondsConfig = 0;
bool hideTimer = false;

// --- WiFi Retry Logic ---
int g_wifiRetries = 0;
volatile bool g_triggerProvisioning = false; // Flag to trigger blocking mode from loop

// Use a counter to track missed ticks (ISR Safe)
volatile uint32_t g_tickCounter = 0; 

// --- Keep-Alive Watchdog (LOCKED/TESTING) ---
unsigned long g_lastKeepAliveTime = 0; // For watchdog. 0 = disarmed.
int g_currentKeepAliveStrikes = 0;     // Counter for missed calls

// Health Check Timers
unsigned long g_lastHealthCheck = 0;
unsigned long g_bootStartTime = 0;
bool g_bootMarkedStable = false;

// --- Device and Sessing Configuration ---

// Global Config (loaded from NVS)
bool enableStreaks = true;          // Default to true
bool enablePaybackTime = true;      // Default to true
uint32_t paybackTimeSeconds = 900;  // Default to 900s (15min).

// Persistent Session Counters (loaded from NVS)
uint32_t sessionStreakCount = 0;
uint32_t completedSessions = 0;
uint32_t abortedSessions = 0;
uint32_t paybackAccumulated = 0; // In seconds
uint32_t totalLockedSessionSeconds = 0; // Total accumulated lock time

// Fixed array holding countdowns for each channel (Index 0-3).
unsigned long channelDelaysRemaining[MAX_CHANNELS] = {0, 0, 0, 0};

// --- Logging System ---
// Ring buffer for storing logs in memory.
const int LOG_BUFFER_SIZE = 150;
const int MAX_LOG_ENTRY_LENGTH = 150;
char logBuffer[LOG_BUFFER_SIZE][MAX_LOG_ENTRY_LENGTH]; // For WebUI
int logBufferIndex = 0;
bool logBufferFull = false;

// Serial Log Queue (To prevent Serial blocks inside Mutex)
const int SERIAL_QUEUE_SIZE = 50;
char serialLogQueue[SERIAL_QUEUE_SIZE][MAX_LOG_ENTRY_LENGTH];
volatile int serialQueueHead = 0;
volatile int serialQueueTail = 0;

// --- Fail-Safe Timer Handle ---
esp_timer_handle_t failsafeTimer = NULL;

// --- WiFi Management (Event Driven) ---
TimerHandle_t wifiReconnectTimer;
bool g_wifiCredentialsExist = false;

char g_wifiSSID[33] = {0};
char g_wifiPass[65] = {0};

// =================================================================
// --- Function Prototypes ---
// =================================================================

void logMessage(const char* message);
void processLogQueue(); 
void connectToWiFi();
void startBLEProvisioning();
void startMDNS();

void resetToReady(bool generateNewCode);
void completeSession();
void stopTestMode();
void abortSession(const char* source);
void handleOneSecondTick();
void saveState(bool force = false);
bool loadState();
void handleRebootState();
void startTimersForState(SessionState state);
void generateUniqueSessionCode(char* codeBuffer, char* checksumBuffer);
void calculateChecksum(const char* code, char* outString);
void initializeChannels();
void sendChannelOn(int channelIndex);
void sendChannelOff(int channelIndex);
void sendChannelOnAll();
void sendChannelOffAll();
void setLedPattern(SessionState state);
void sendJsonError(AsyncWebServerRequest *request, int code, const String& message);
void handleLongPress();
void handleDoublePress();
bool checkKeepAliveWatchdog();
void armFailsafeTimer();
void disarmFailsafeTimer();

// Byte conversion helpers
uint16_t bytesToUint16(uint8_t* data);
uint32_t bytesToUint32(uint8_t* data);
void formatSeconds(unsigned long totalSeconds, char* buffer, size_t size);
const char* stateToString(SessionState state);

// Health Checks
void checkSystemHealth();
void checkHeapHealth();
void updateWatchdogTimeout(uint32_t seconds);
void checkBootLoop();

// --- Web Server Prototypes ---
void setupWebServer();
void handleRoot(AsyncWebServerRequest *request);
void handleHealth(AsyncWebServerRequest *request);
void handleKeepAlive(AsyncWebServerRequest *request);
void handleArm(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total);
void handleStartTest(AsyncWebServerRequest *request);
void handleAbort(AsyncWebServerRequest *request);
void handleReward(AsyncWebServerRequest *request);
void handleStatus(AsyncWebServerRequest *request);
void handleDetails(AsyncWebServerRequest *request);
void handleLog(AsyncWebServerRequest *request);
void handleUpdateWifi(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total);
void handleFactoryReset(AsyncWebServerRequest *request);


// =================================================================
// --- BLE Provisioning (Stage 1) ---
// =================================================================

// Base UUID: 5a160000-8334-469b-a316-c340cf29188f
#define PROV_SERVICE_UUID                   "5a160000-8334-469b-a316-c340cf29188f"
#define PROV_SSID_CHAR_UUID                 "5a160001-8334-469b-a316-c340cf29188f"
#define PROV_PASS_CHAR_UUID                 "5a160002-8334-469b-a316-c340cf29188f"
#define PROV_ENABLE_STREAKS_CHAR_UUID       "5a160004-8334-469b-a316-c340cf29188f"
#define PROV_ENABLE_PAYBACK_TIME_CHAR_UUID  "5a160005-8334-469b-a316-c340cf29188f"
#define PROV_PAYBACK_TIME_CHAR_UUID         "5a160006-8334-469b-a316-c340cf29188f"
#define PROV_CH1_ENABLE_UUID                "5a16000A-8334-469b-a316-c340cf29188f"
#define PROV_CH2_ENABLE_UUID                "5a16000B-8334-469b-a316-c340cf29188f"
#define PROV_CH3_ENABLE_UUID                "5a16000C-8334-469b-a316-c340cf29188f"
#define PROV_CH4_ENABLE_UUID                "5a16000D-8334-469b-a316-c340cf29188f"

volatile bool g_credentialsReceived = false;

/**
 * BLE callback class to handle writes to characteristics.
 * This receives the Wi-Fi credentials and config from the server/UI.
 */
class ProvisioningCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
        std::string uuid = pCharacteristic->getUUID().toString();
        uint8_t* data = pCharacteristic->getData();
        size_t len = pCharacteristic->getLength();

        if (len == 0) {
            logMessage("BLE: Received empty write.");
            return;
        }

        // Handle Wi-Fi Credentials
        if (uuid == PROV_SSID_CHAR_UUID) {
            std::string value(data, data + len);
            wifiPreferences.begin("wifi-creds", false);
            wifiPreferences.putString("ssid", value.c_str());
            wifiPreferences.end();
            logMessage("BLE: Received SSID");
        } else if (uuid == PROV_PASS_CHAR_UUID) {
            std::string value(data, data + len);
            wifiPreferences.begin("wifi-creds", false);
            wifiPreferences.putString("pass", value.c_str());
            wifiPreferences.end();
            logMessage("BLE: Received Password");
            g_credentialsReceived = true; // Flag to restart
        } 
        // Handle Global Config (session namespace)
        else if (uuid == PROV_ENABLE_STREAKS_CHAR_UUID) {
            sessionState.begin("session", false);
            bool val = (bool)data[0];
            sessionState.putBool("enableStreaks", val);
            sessionState.end();
            logMessage("BLE: Received Enable Streaks");
        } else if (uuid == PROV_ENABLE_PAYBACK_TIME_CHAR_UUID) {
            sessionState.begin("session", false);
            bool val = (bool)data[0];
            sessionState.putBool("enablePayback", val);
            sessionState.end();
            logMessage("BLE: Received Enable Payback Time");
        } else if (uuid == PROV_PAYBACK_TIME_CHAR_UUID) {
            sessionState.begin("session", false);
            uint32_t val = bytesToUint32(data);
            sessionState.putUInt("paybackSeconds", val);
            sessionState.end();
            logMessage("BLE: Received Payback Time (Seconds)");
        }
        // Handle Hardware Config (provisioning namespace)
        else {
            provisioningPrefs.begin("provisioning", false);
            
            // Read current mask (default to 0x0F if not set)
            uint8_t currentMask = provisioningPrefs.getUChar("chMask", 0x0F);
            bool enable = (bool)data[0];

            if (uuid == PROV_CH1_ENABLE_UUID) {
                if(enable) currentMask |= (1 << 0); else currentMask &= ~(1 << 0);
                logMessage(enable ? "BLE: Enabled Ch1" : "BLE: Disabled Ch1");
            } else if (uuid == PROV_CH2_ENABLE_UUID) {
                if(enable) currentMask |= (1 << 1); else currentMask &= ~(1 << 1);
                logMessage(enable ? "BLE: Enabled Ch2" : "BLE: Disabled Ch2");
            } else if (uuid == PROV_CH3_ENABLE_UUID) {
                if(enable) currentMask |= (1 << 2); else currentMask &= ~(1 << 2);
                logMessage(enable ? "BLE: Enabled Ch3" : "BLE: Disabled Ch3");
            } else if (uuid == PROV_CH4_ENABLE_UUID) {
                if(enable) currentMask |= (1 << 3); else currentMask &= ~(1 << 3);
                logMessage(enable ? "BLE: Enabled Ch4" : "BLE: Disabled Ch4");
            }
            
            provisioningPrefs.putUChar("chMask", currentMask);
            provisioningPrefs.end();
            
            // Update global immediately for this boot session
            g_enabledChannelsMask = currentMask; 
        }
    }
};

/**
 * Starts the BLE provisioning service.
 * This function DOES NOT RETURN. It waits for credentials and reboots.
 */
void startBLEProvisioning() {
    logMessage("Starting BLE Provisioning Mode...");
    
    // Set a "pairing" pulse pattern
    statusLed.FadeOn(500).FadeOff(500).Forever();

    // Initialize BLE
    BLEDevice::init(DEVICE_NAME);
    BLEServer *pServer = BLEDevice::createServer();
    BLEService *pService = pServer->createService(BLEUUID(PROV_SERVICE_UUID), 30);
    
    ProvisioningCallbacks* callbacks = new ProvisioningCallbacks();

    // Helper to create char
    auto createChar = [&](const char* uuid) {
        BLECharacteristic* p = pService->createCharacteristic(uuid, BLECharacteristic::PROPERTY_WRITE);
        p->setCallbacks(callbacks);
        return p;
    };

    createChar(PROV_SSID_CHAR_UUID);
    createChar(PROV_PASS_CHAR_UUID);
    createChar(PROV_ENABLE_STREAKS_CHAR_UUID);
    createChar(PROV_ENABLE_PAYBACK_TIME_CHAR_UUID);
    createChar(PROV_PAYBACK_TIME_CHAR_UUID);
    createChar(PROV_CH1_ENABLE_UUID);
    createChar(PROV_CH2_ENABLE_UUID);
    createChar(PROV_CH3_ENABLE_UUID);
    createChar(PROV_CH4_ENABLE_UUID);

    pService->start();
    
    // Start advertising
    BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(PROV_SERVICE_UUID);
    pAdvertising->start();

    // --- Wait here forever until credentials are set ---
    while (1) {

        processLogQueue();

        if (g_credentialsReceived) {
            logMessage("Credentials received. Restarting to connect...");
            delay(1000);
            ESP.restart();
        }

        statusLed.Update(); // Keep the LED pattern running

        esp_task_wdt_reset(); // Feed the watchdog
        delay(100);
    }
}

// =================================================================
// --- mDNS Discovery (Stage 2) ---
// =================================================================

/**
 * Starts the mDNS service to advertise 'lobster-lock.local'
 */
void startMDNS() {
    logMessage("Starting mDNS advertiser...");
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    char uniqueHostname[20];
    snprintf(uniqueHostname, sizeof(uniqueHostname), "lobster-lock-%02X%02X%02X", mac[3], mac[4], mac[5]);
    
    // Set the unique hostname (e.g., lobster-lock-AABBCC.local)
    if (!MDNS.begin(uniqueHostname)) { 
        logMessage("ERROR: Failed to set up mDNS responder!");
        return;
    }
    // Announce the service your NodeJS app will look for
    MDNS.addService("lobster-lock", "tcp", 80);
    logMessage("mDNS service announced: _lobster-lock._tcp.local");
}

// =================================================================
// --- FAILSAFE / DEATH GRIP TIMER ---
// =================================================================

/**
 * Hardware Timer Callback for "Death Grip".
 * This runs in ISR context when the absolute maximum safety limit is hit.
 * Forces a reboot to ensure pins go low.
 */
void IRAM_ATTR failsafe_timer_callback(void* arg) {
    // EMERGENCY: Force all channels LOW immediately
    for (int i = 0; i < MAX_CHANNELS; i++) {
        digitalWrite(HARDWARE_PINS[i], LOW);
    }
    
    // While technically not 100% ISR-safe (can panic), a panic reset 
    // is exactly what we want for a "Failsafe" condition.
    esp_restart();
}

/**
 * Arms the independent hardware timer.
 */
void armFailsafeTimer() {
    if (failsafeTimer == NULL) return;
    
    // Start one-shot timer. FAILSAFE_MAX_LOCK_SECONDS converted to microseconds.
    // This is completely independent of the main FreeRTOS tasks.
    uint64_t timeout_us = (uint64_t)g_systemConfig.failsafeMaxLockSeconds * 1000000ULL;
    esp_timer_start_once(failsafeTimer, timeout_us);
    
    char timeStr[64];
    formatSeconds(g_systemConfig.failsafeMaxLockSeconds, timeStr, sizeof(timeStr));

    char logBuf[100];
    snprintf(logBuf, sizeof(logBuf), "Death Grip Timer ARMED: %s", timeStr);
    logMessage(logBuf);
}

/**
 * Disarms the independent hardware timer.
 */
void disarmFailsafeTimer() {
    if (failsafeTimer == NULL) return;
    esp_timer_stop(failsafeTimer);
    logMessage("Death Grip Timer DISARMED.");
}

// =================================================================
// --- WiFi Event Handling ---
// =================================================================

/**
 * Trigger the WiFi connection process using stored credentials.
 */
void connectToWiFi() {
    if (!g_wifiCredentialsExist) return;
    if (WiFi.status() == WL_CONNECTED) return;

    logMessage("WiFi: Connecting...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(g_wifiSSID, g_wifiPass);
}

/**
 * Handle WiFi connection events (connected, disconnected, etc).
 */
void WiFiEvent(WiFiEvent_t event) {
    switch(event) {
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            
            char logBuf[64];
            snprintf(logBuf, sizeof(logBuf), "WiFi: Connected. IP: %s", WiFi.localIP().toString().c_str());
            logMessage(logBuf);

            g_wifiRetries = 0; // Reset counter on success
            xTimerStop(wifiReconnectTimer, 0);
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            // Check if we have exceeded max retries
            if (g_wifiRetries >= g_systemConfig.wifiMaxRetries) {
                logMessage("WiFi: Max retries exceeded. Falling back to BLE Provisioning...");
                // Stop any pending reconnect timers
                xTimerStop(wifiReconnectTimer, 0); 
                // Set flag for the main loop to handle (safest way to switch contexts)
                g_triggerProvisioning = true;
            } else {
                char logBuf[64];
                snprintf(logBuf, sizeof(logBuf), "WiFi: Disconnected (Attempt %d/%d). Retrying...", 
                         g_wifiRetries + 1, g_systemConfig.wifiMaxRetries);
                logMessage(logBuf);
                
                g_wifiRetries++;
                // Wait 2 seconds before retrying
                xTimerStart(wifiReconnectTimer, 0); 
            }
            break;
        default:
            break;
    }
}

// =================================================================
// --- Core Application Setup & Loop ---
// =================================================================

/**
 * Main Arduino setup function. Runs once on boot.
 */
void setup() {

  Serial.begin(SERIAL_BAUD_RATE);
  
  delay(3000);

  // Initialize the state Mutex before anything else
  stateMutex = xSemaphoreCreateRecursiveMutex();
  if (stateMutex == NULL) {
      Serial.println("Critical Error: Could not create Mutex.");
      ESP.restart();
  }

  // Safety First: Initialize all hardware pins to a safe state (LOW)
  // immediately, before any logic or config loading occurs.
  // This prevents floating pins while waiting for NVS.
  initializeChannels();

  // --- Boot Loop Detection ---
  checkBootLoop();
  g_bootStartTime = millis();

  // --- Basic hardware init ---
  randomSeed(esp_random());

  // --- Init Failsafe Timer ---
  const esp_timer_create_args_t failsafe_timer_args = {
            .callback = &failsafe_timer_callback,
            .name = "failsafe_wdt"
  };
  esp_timer_create(&failsafe_timer_args, &failsafeTimer);

  char logBuf[100];
  snprintf(logBuf, sizeof(logBuf), "%s starting up...", DEVICE_NAME);
  logMessage(logBuf);
  snprintf(logBuf, sizeof(logBuf), "Firmware Version: %s", DEVICE_VERSION);
  logMessage(logBuf);

  logMessage("Initializing hardware watchdog...");
  esp_task_wdt_init(DEFAULT_WDT_TIMEOUT, true);
  esp_task_wdt_add(NULL);

  logMessage("--- System Configuration ---");
  snprintf(logBuf, sizeof(logBuf), "Long Press: %lu s", g_systemConfig.longPressSeconds);
  logMessage(logBuf);
  snprintf(logBuf, sizeof(logBuf), "Lock Range: %lu-%lu s", g_systemConfig.minLockSeconds, g_systemConfig.maxLockSeconds);
  logMessage(logBuf);
  snprintf(logBuf, sizeof(logBuf), "Penalty Range: %lu-%lu s", g_systemConfig.minPenaltySeconds, g_systemConfig.maxPenaltySeconds);
  logMessage(logBuf);
  snprintf(logBuf, sizeof(logBuf), "Test Mode: %lu s", g_systemConfig.testModeDurationSeconds);
  logMessage(logBuf);
  snprintf(logBuf, sizeof(logBuf), "Failsafe: %lu s", g_systemConfig.failsafeMaxLockSeconds);
  logMessage(logBuf);
  snprintf(logBuf, sizeof(logBuf), "KeepAlive: %lu ms (Max Strikes: %lu)", g_systemConfig.keepAliveIntervalMs, g_systemConfig.keepAliveMaxStrikes);
  logMessage(logBuf);
  snprintf(logBuf, sizeof(logBuf), "Boot Loop: %lu (Stable: %lu ms)", g_systemConfig.bootLoopThreshold, g_systemConfig.stableBootTimeMs);
  logMessage(logBuf);
  logMessage("--- /System Configuration ---"  );

  logMessage("--- Device Features ---"); 

  // Load Enable Mask from Provisioning NVS
  provisioningPrefs.begin("provisioning", true); // Read-only
  g_enabledChannelsMask = provisioningPrefs.getUChar("chMask", 0x0F); // Default all on
  provisioningPrefs.end();
  
  // Log which channels are logically enabled
  for(int i=0; i<MAX_CHANNELS; i++) {
      if ((g_enabledChannelsMask >> i) & 1) {
          snprintf(logBuf, sizeof(logBuf), "Channel %d (GPIO %d): ENABLED", i+1, HARDWARE_PINS[i]);
      } else {
          snprintf(logBuf, sizeof(logBuf), "Channel %d (GPIO %d): DISABLED", i+1, HARDWARE_PINS[i]);
      }
      logMessage(logBuf);
  }

  logMessage("LED Status Indicator enabled");

  char btnLog[50];
  snprintf(btnLog, sizeof(btnLog), "Button (Pin %d) enabled", ONE_BUTTON_PIN);
  logMessage(btnLog);

  // Context-sensitive button logic.
  // Long Press: ALWAYS ABORT/CANCEL (Armed -> Ready, Locked -> Penalty)
  button.setLongPressIntervalMs(g_systemConfig.longPressSeconds * 1000);
  button.attachLongPressStart(handleLongPress);

  // Double Click: TRIGGER (Armed -> Locked)
  button.attachDoubleClick(handleDoublePress);

  logMessage("--- /Device Features ---"); 

  // Read credentials from NVS
  if (!wifiPreferences.begin("wifi-creds", true)) {
      logMessage("NVS Warning: 'wifi-creds' namespace not found (First boot?)");
  } else {
      logMessage("NVS: 'wifi-creds' loaded successfully.");
  }
  String ssidTemp = wifiPreferences.getString("ssid", "");
  String passTemp = wifiPreferences.getString("pass", "");
  wifiPreferences.end();

  strncpy(g_wifiSSID, ssidTemp.c_str(), sizeof(g_wifiSSID));
  strncpy(g_wifiPass, passTemp.c_str(), sizeof(g_wifiPass));

  // Create software timer for reconnect logic
  wifiReconnectTimer = xTimerCreate("wifiTimer", pdMS_TO_TICKS(2000), pdFALSE, (void*)0, 
    [](TimerHandle_t t){
        connectToWiFi();
    });

  if (strlen(g_wifiSSID) > 0) {
      logMessage("Found Wi-Fi credentials. Registering events.");
      g_wifiCredentialsExist = true;
      WiFi.onEvent(WiFiEvent);

      connectToWiFi();
  } else {
      // --- STAGE 1: PROVISIONING ---
      logMessage("No Wi-Fi credentials found.");
      startBLEProvisioning(); // Blocks here intentionally for first setup
  }
  
  startMDNS();

  if (g_wifiCredentialsExist) {
      logMessage("Boot: Waiting for IP address...");
      unsigned long wifiWaitStart = millis();

      // Stay awake WiFi
      WiFi.setSleep(false);

      // Wait up to 30 seconds specifically for the IP
      while (WiFi.status() != WL_CONNECTED && (millis() - wifiWaitStart < 30000)) {
          processLogQueue();    // Keep flushing logs so "Connected" appears instantly
          esp_task_wdt_reset(); // Feed the watchdog so we don't crash
          delay(100);
      }
      
      if (WiFi.status() != WL_CONNECTED) {
          logMessage("Boot: WiFi connection timed out.");
      }
  }

  // --- STAGE 3: OPERATIONAL MODE ---

  logMessage("Initializing Session State from NVS...");  
  
  if (loadState()) {
      // 1. Log the raw state we just recovered from flash
      char rawBuf[64];
      snprintf(rawBuf, sizeof(rawBuf), "Raw NVS State: %s", stateToString(currentState));
      logMessage(rawBuf);

      // 2. Decide what to do (e.g., abort if we rebooted during ARMED)
      handleRebootState();
      
      // 3. Log the final state after decision logic
      char finalBuf[64];
      snprintf(finalBuf, sizeof(finalBuf), "Final Boot State: %s", stateToString(currentState));
      logMessage(finalBuf);

  } else {
      // No valid data in NVS. Initialize a fresh state.
      logMessage("No valid session data in NVS. Initializing fresh state.");
      resetToReady(true); // This saves the new, fresh state
      
      // Log the final state (always ready)
      char finalBuf[64];
      snprintf(finalBuf, sizeof(finalBuf), "Final Boot State: %s", stateToString(currentState));
      logMessage(finalBuf);
  }

    // Helper buffers for string formatting
  char tBuf1[50];
  char tBuf2[50];

  logMessage("--- Session State ---");

  snprintf(logBuf, sizeof(logBuf), "State: %s (Hide Timer: %s)", 
             stateToString(currentState), hideTimer ? "Yes" : "No");
  logMessage(logBuf);

  // Format Lock Timer: "Remaining (Configured)"
  formatSeconds(lockSecondsRemaining, tBuf1, sizeof(tBuf1));
  formatSeconds(lockSecondsConfig, tBuf2, sizeof(tBuf2));
  snprintf(logBuf, sizeof(logBuf), "Lock Timer: %s (Cfg: %s)", tBuf1, tBuf2);
  logMessage(logBuf);

  // Format Penalty Timer
  formatSeconds(penaltySecondsRemaining, tBuf1, sizeof(tBuf1));
  formatSeconds(penaltySecondsConfig, tBuf2, sizeof(tBuf2));
  snprintf(logBuf, sizeof(logBuf), "Penalty Timer: %s (Cfg: %s)", tBuf1, tBuf2);
  logMessage(logBuf);

  // Payback Debt
  formatSeconds(paybackAccumulated, tBuf1, sizeof(tBuf1));
  snprintf(logBuf, sizeof(logBuf), "Payback: %s (Debt: %s)", 
           enablePaybackTime ? "Enabled" : "Disabled", tBuf1);
  logMessage(logBuf);

  // Statistics
  snprintf(logBuf, sizeof(logBuf), "Stats: Streak %u | Complete %u | Aborted %u", 
           sessionStreakCount, completedSessions, abortedSessions);
  logMessage(logBuf);

  // Lifetime
  formatSeconds(totalLockedSessionSeconds, tBuf1, sizeof(tBuf1));
  snprintf(logBuf, sizeof(logBuf), "Lifetime Locked: %s", tBuf1);
  logMessage(logBuf);

  logMessage("--- /Session State ---");  

  // --- Start web server and timers ---
  logMessage("Attaching master 1-second ticker.");
  
  // ISR-Safe Ticker Callback
  oneSecondMasterTicker.attach(1, [](){ 
      portENTER_CRITICAL_ISR(&timerMux);
      g_tickCounter++; 
      portEXIT_CRITICAL_ISR(&timerMux);
  });

  // Status led
  setLedPattern(currentState); // Set initial LED pattern

  setupWebServer();
  server.begin();  
  logMessage("HTTP server started. Device is operational.");
}

/**
 * Main Arduino loop function. Runs continuously.
 * Handles periodic tasks (NTP retry, LED updates, Button polling).
 */
void loop() {
  // 0. Check for WiFi Failure Fallback
  if (g_triggerProvisioning) {
      g_triggerProvisioning = false; // Clear flag
      
      // Shut down WiFi to save power/conflicts
      WiFi.disconnect(true); 
      WiFi.mode(WIFI_OFF);
      
      logMessage("!!! Connection Failed. Entering BLE Provisioning Mode !!!");
      
      // Enter blocking provisioning mode
      // Note: This function ends with ESP.restart(), so we never return here.
      startBLEProvisioning(); 
  }

  esp_task_wdt_reset(); // Feed the watchdog

  // Check if we have been running long enough to be considered "Stable"
  // Uses g_systemConfig.stableBootTimeMs
  if (!g_bootMarkedStable && (millis() - g_bootStartTime > g_systemConfig.stableBootTimeMs)) {
      g_bootMarkedStable = true;
      bootPrefs.begin("boot", false);
      bootPrefs.putInt("crashes", 0);
      bootPrefs.end();
      logMessage("System stable. Boot loop counter reset.");
  }

  // Interval-based Health Checks (Every 60s)
  if (millis() - g_lastHealthCheck > 60000) {
      checkHeapHealth();
      checkSystemHealth();
      g_lastHealthCheck = millis();
  }

  // Drain the log queue (Non-blocking I/O)
  processLogQueue();

  // --- 2. JLed ---
  // JLed is not thread safe.
  if (xSemaphoreTakeRecursive(stateMutex, 0) == pdTRUE) {
    statusLed.Update(); // Update JLed state machine
    xSemaphoreGiveRecursive(stateMutex);
  }

  // --- 3. Button ---
  button.tick(); // Poll the button

  // --- 4. One Second Tick (Accumulated) ---
  // ISR Safe Read of g_tickCounter
  uint32_t pendingTicks = 0;
  portENTER_CRITICAL(&timerMux);
  if (g_tickCounter > 0) {
      pendingTicks = g_tickCounter;
      g_tickCounter = 0;
  }
  portEXIT_CRITICAL(&timerMux);

  if (pendingTicks > 0) {
    // Only process ticks if we can get the lock.
    if (xSemaphoreTakeRecursive(stateMutex, (TickType_t)pdMS_TO_TICKS(50)) == pdTRUE) {
        while (pendingTicks > 0) {
            handleOneSecondTick();
            pendingTicks--;
        }
        xSemaphoreGiveRecursive(stateMutex);
    } else {
        // Mutex contention detected. 
        // For this application, dropping a second under extreme load is better than deadlock.
    }
  }
}

// =================================================================
// --- Health & Boot Logic ---
// =================================================================

/**
 * Check for rapid crashes and enter safe mode if detected.
 */
void checkBootLoop() {
    bootPrefs.begin("boot", false);
    int crashes = bootPrefs.getInt("crashes", 0);
    
    // Use provisioned threshold
    if (crashes >= g_systemConfig.bootLoopThreshold) {
        Serial.println("CRITICAL: Boot Loop Detected! Entering Safe Mode.");

        // Safe Mode: Delay startup, disarm everything.
        // This gives the power rail time to stabilize or user time to factory reset.
        initializeChannels();

        delay(5000);
        pinMode(STATUS_LED_PIN, OUTPUT);
        digitalWrite(STATUS_LED_PIN, HIGH);

        delay(30000); // 30 Second penalty box before attempting start
    }

    bootPrefs.putInt("crashes", crashes + 1);
    bootPrefs.end();
}

/**
 * Monitor heap memory and emergency stop if fragmentation is high.
 */
void checkHeapHealth() {
    size_t freeMem = ESP.getFreeHeap();
    size_t largestBlock = ESP.getMaxAllocHeap();
    
    // If fragmentation is high (plenty of free mem but no big blocks)
    if (largestBlock < 8192 && freeMem > 20000) {
        logMessage("WARNING: Heap fragmentation detected!");
    }
    
    // Critical low memory
    if (freeMem < 10000) {
        logMessage("CRITICAL: Low Heap! Executing failsafe.");
        sendChannelOffAll();
    }
}

/**
 * Monitor stack usage and emergency stop if overflow is imminent.
 */
void checkSystemHealth() {
    UBaseType_t loopStack = uxTaskGetStackHighWaterMark(NULL);
    // Note: This assumes a task named "async_tcp" exists. If the library changes task name, this returns null.
    TaskHandle_t asyncHandle = xTaskGetHandle("async_tcp");
    UBaseType_t asyncStack = 0;
    if (asyncHandle != NULL) {
        asyncStack = uxTaskGetStackHighWaterMark(asyncHandle);
    }
    
    if (loopStack < 512 || (asyncHandle != NULL && asyncStack < 512)) {
        logMessage("CRITICAL: Stack near overflow! Emergency Stop.");
        sendChannelOffAll();
    }
}

/**
 * Dynamically update the Task Watchdog Timeout period.
 */
void updateWatchdogTimeout(uint32_t seconds) {
    esp_task_wdt_init(seconds, true);
    char logBuf[50];
    snprintf(logBuf, sizeof(logBuf), "WDT Timeout set to %u s", seconds);
    logMessage(logBuf);
}


// =================================================================
// --- Channel Control (Hardware Abstraction) ---
// =================================================================

/**
 * Initializes all channel pins as outputs.
 * Sets them to LOW immediately to prevent floating states on boot.
 * This function does NOT check logical 'enabled' status, it is purely hardware init.
 */
void initializeChannels() {
  logMessage("Initializing Channels: Setting all 4 to Output LOW (Safe State)");
  
  // Iterate through all physical pins defined in hardware config
  for (int i = 0; i < MAX_CHANNELS; i++) {
      pinMode(HARDWARE_PINS[i], OUTPUT);
      digitalWrite(HARDWARE_PINS[i], LOW); // Ensure circuit is open
  }
}

/**
 * Turns a specific Channel channel ON (closes circuit).
 */
void sendChannelOn(int channelIndex) {
  if (channelIndex < 0 || channelIndex >= MAX_CHANNELS) return;

  // Hardware Safety Check: Is this channel enabled in config?
  if ( !((g_enabledChannelsMask >> channelIndex) & 1) ) {
      return; // Silently fail if channel is disabled in config
  }

  char logBuf[50];
  snprintf(logBuf, sizeof(logBuf), "Channel %d: ON ", channelIndex + 1);
  logMessage(logBuf);
  #ifndef DEBUG_MODE
    digitalWrite(HARDWARE_PINS[channelIndex], HIGH);
  #endif
}

/**
 * Turns a specific Channel channel OFF (opens circuit).
 */
void sendChannelOff(int channelIndex) {
  if (channelIndex < 0 || channelIndex >= MAX_CHANNELS) return;
  // We always allow turning off, even if disabled, for safety.
  char logBuf[50];
  snprintf(logBuf, sizeof(logBuf), "Channel %d: OFF", channelIndex + 1);
  logMessage(logBuf);
  #ifndef DEBUG_MODE
    digitalWrite(HARDWARE_PINS[channelIndex], LOW);
  #endif
}

/**
 * Turns all Channel channels ON.
 */
void sendChannelOnAll() {
  logMessage("Channels: ON (All Enabled)");
  for (int i=0; i < MAX_CHANNELS; i++) { sendChannelOn(i); }
}

/**
 * Turns all Channel channels OFF.
 */
void sendChannelOffAll() {
  logMessage("Channels: OFF (All)");
  for (int i=0; i < MAX_CHANNELS; i++) { sendChannelOff(i); }
}

// =================================================================
// --- Session State Management ---
// =================================================================
// NOTE: Functions in this section assume the caller (Loop or API) 
// HAS ALREADY LOCKED THE MUTEX unless otherwise noted.

/**
 * Stops the test mode and returns to READY.
 */
void stopTestMode() {
    logMessage("Stopping test mode.");
    sendChannelOffAll();
    currentState = READY;
    startTimersForState(READY); // Set WDT
    setLedPattern(READY);
    testSecondsRemaining = 0;
    g_lastKeepAliveTime = 0; // Disarm watchdog
    g_currentKeepAliveStrikes = 0; // Reset strikes
    saveState(true); // Force save
}

/**
 * Aborts an active session (LOCKED, ARMED, or TESTING).
 * Implements payback logic, resets streak, and starts the reward penalty timer.
 */
void abortSession(const char* source) {
    char logBuf[100];
    snprintf(logBuf, sizeof(logBuf), "%s: Aborting session (%s).", source, stateToString(currentState));
    
    if (currentState == LOCKED) {
        disarmFailsafeTimer();
        logMessage(logBuf);
        sendChannelOffAll();
        currentState = ABORTED;
        setLedPattern(ABORTED);
       
        // Implement Abort Logic:
        sessionStreakCount = 0; // 1. Reset streak
        abortedSessions++;      // 2. Increment counter

        if (enablePaybackTime) { // 3. Add payback
            paybackAccumulated += paybackTimeSeconds;
            
            // Use helper to format payback time
            char timeStr[64];
            formatSeconds(paybackAccumulated, timeStr, sizeof(timeStr));
            
            char paybackLog[150];
            snprintf(paybackLog, sizeof(paybackLog), "Payback enabled. Added %u s. Total pending: %s",
                     paybackTimeSeconds, timeStr);
            logMessage(paybackLog);
        }

        lockSecondsRemaining = 0;
        penaltySecondsRemaining = penaltySecondsConfig; // 4. Start REWARD penalty
        startTimersForState(ABORTED);
        
        g_lastKeepAliveTime = 0; // Disarm watchdog
        g_currentKeepAliveStrikes = 0; // Reset strikes
        saveState(true); // Force save
    
    } else if (currentState == ARMED) {
        // ARMED is a "Safety Off" state, but not yet "Point of No Return".
        // Aborting here returns to READY without penalty.
        logMessage(logBuf);
        sendChannelOffAll();
        resetToReady(false); // Cancel arming (this disarms watchdog)
    } else if (currentState == TESTING) {
        snprintf(logBuf, sizeof(logBuf), "%s: Aborting test mode.", source);
        logMessage(logBuf);
        stopTestMode(); // Cancel test (this disarms watchdog)
    } else {
         snprintf(logBuf, sizeof(logBuf), "%s: Abort ignored, device not in abortable state.", source);
         logMessage(logBuf);
         return;
    }
}

/**
 * Called when a session completes (either Lock timer OR Penalty timer ends).
 * If coming from LOCKED: Increments success stats, clears payback.
 * If coming from ABORTED: Transitions to COMPLETED but retains debt/stats.
 */
void completeSession() {
  SessionState previousState = currentState;

  logMessage("Timer finished. State is now COMPLETED.");
  if (previousState == LOCKED) disarmFailsafeTimer();
  
  sendChannelOffAll();
  currentState = COMPLETED;
  startTimersForState(COMPLETED); // Reset WDT
  setLedPattern(COMPLETED);

  if (previousState == LOCKED) {
      // --- SUCCESS PATH ---
      // On successful completion of a LOCK, we clear debt and reward streaks.
      if (paybackAccumulated > 0) {
          logMessage("Valid session complete. Clearing accumulated payback debt.");
          paybackAccumulated = 0;
      }

      completedSessions++;
      if (enableStreaks) {
          sessionStreakCount++;
      }

      // Log the new winning stats
      char statBuf[100];
      snprintf(statBuf, sizeof(statBuf), "Stats updated: Streak %u | Completed %u", 
               sessionStreakCount, completedSessions);
      logMessage(statBuf);
  } 
  else if (previousState == ABORTED) {
      // --- PENALTY SERVED PATH ---
      // The user finished the penalty box. 
      
      if (enablePaybackTime) {
          logMessage("Penalty time served. Payback debt retained.");
      } else {
          logMessage("Penalty time served.");
      }
  }

  // Clear all timers
  lockSecondsRemaining = 0;
  penaltySecondsRemaining = 0;
  testSecondsRemaining = 0;
  triggerTimeoutRemaining = 0;
  g_lastKeepAliveTime = 0; 
  g_currentKeepAliveStrikes = 0; 
  
  for(int i=0; i<MAX_CHANNELS; i++) channelDelaysRemaining[i] = 0;
  
  // Log Lifetime Total
  char timeBuf[64];
  formatSeconds(totalLockedSessionSeconds, timeBuf, sizeof(timeBuf));
  char totalLog[100];
  snprintf(totalLog, sizeof(totalLog), "Lifetime Locked: %s", timeBuf);
  logMessage(totalLog);

  saveState(true); // Force save
}

/**
 * Resets the device state to READY, generates a new reward code.
 * Does NOT reset counters or payback.
 */
void resetToReady(bool generateNewCode) {
  logMessage("Resetting state to READY.");
  if (currentState == LOCKED) disarmFailsafeTimer();
  sendChannelOffAll();

  currentState = READY;
  startTimersForState(READY);

  setLedPattern(READY);

  // Clear all timers and configs
  lockSecondsRemaining = 0;
  penaltySecondsRemaining = 0;
  testSecondsRemaining = 0;
  triggerTimeoutRemaining = 0;
  g_lastKeepAliveTime = 0; // Disarm watchdog
  g_currentKeepAliveStrikes = 0; // Reset strikes
  lockSecondsConfig = 0;
  hideTimer = false;
  
  for(int i=0; i<MAX_CHANNELS; i++) channelDelaysRemaining[i] = 0;

  // Only generate new code if requested.
  // If we abort a countdown, we generally want to keep the same code 
  // unless we specifically want to roll it.
  if (generateNewCode) {
      logMessage("Generating new reward code and updating history.");
      // Shift reward history
      for (int i = REWARD_HISTORY_SIZE - 1; i > 0; i--) {
          rewardHistory[i] = rewardHistory[i - 1];
      }
      
      // Generate new code with collision detection against the history we just shifted
      generateUniqueSessionCode(rewardHistory[0].code, rewardHistory[0].checksum);

      char logBuf[150];
      char codeSnippet[9];
      strncpy(codeSnippet, rewardHistory[0].code, 8);
      codeSnippet[8] = '\0';
      snprintf(logBuf, sizeof(logBuf), "New Code: %s... Checksum: %s", codeSnippet, rewardHistory[0].checksum);
      logMessage(logBuf);
  } else {
      logMessage("Preserving existing reward code.");
  }

  saveState(true); // Force save
}

// =================================================================
// --- Web Server & API Endpoints ---
// =================================================================

/**
 * Helper function to send a standardized JSON error response.
 */
void sendJsonError(AsyncWebServerRequest *request, int code, const String& message) {
    // RAII: Automatically freed on scope exit
    std::unique_ptr<JsonDocument> doc(new JsonDocument());
    (*doc)["status"] = "error";
    (*doc)["message"] = message;
    String response;
    serializeJson(*doc, response);
    // doc deleted automatically here
    request->send(code, "application/json", response);
}

/**
 * Sets up all API endpoints for the web server.
 * This function is now just a clean list of routes.
 */
void setupWebServer() {
  // Root endpoint, simple health check.
  server.on("/", HTTP_GET, handleRoot);

  // API: Lightweight health check (GET /health)
  server.on("/health", HTTP_GET, handleHealth);
  
  // API: Keep-alive endpoint (POST /keepalive)
  server.on("/keepalive", HTTP_POST, handleKeepAlive);

  // API: Arm/Start a session (POST /arm) - REPLACES /start
  server.on("/arm", HTTP_POST,
      [](AsyncWebServerRequest *request) { 
        // Empty onReq, processing happens in onBody
      },
      NULL, // onUpload
      handleArm // onBody
  );

  // API: Start a short test mode (POST /start-test)
  server.on("/start-test", HTTP_POST, handleStartTest);

  // API: Abort an active session (POST /abort)
  server.on("/abort", HTTP_POST, handleAbort);

  // API: Get the reward code history (GET /reward)
  server.on("/reward", HTTP_GET, handleReward);
  
  // API: Get the main device status (dynamic data, polled frequently). (GET /status)
  server.on("/status", HTTP_GET, handleStatus);

  // API: Get the main device details (static data, polled once). (GET /details)
  server.on("/details", HTTP_GET, handleDetails);

  // API: Get the in-memory log buffer (GET /log)
  server.on("/log", HTTP_GET, handleLog);

  // API: Update Wi-Fi Credentials (POST /update-wifi)
  server.on("/update-wifi", HTTP_POST,
      [](AsyncWebServerRequest *request) {},
      NULL,
      handleUpdateWifi
  );

  // API: Factory reset (POST /factory-reset)
  server.on("/factory-reset", HTTP_POST, handleFactoryReset);
}

// =================================================================
// --- Web Server Handlers ---
// =================================================================

/**
 * Handler for GET /
 * Returns an HTML list of all API endpoints.
 */
void handleRoot(AsyncWebServerRequest *request) {
    String html = "<html><head><title>" + String(DEVICE_NAME) + "</title></head><body>";
    html += "<h1>" + String(DEVICE_NAME) + " API</h1>";
    html += "<ul>";
    html += "<li><b>GET /status</b> - Real-time metrics (lock timer, state).</li>";
    html += "<li><b>GET /details</b> - Device configuration & capabilities.</li>";
    html += "<li><b>GET /log</b> - Internal system logs.</li>";
    html += "<li><b>GET /reward</b> - Retrieve past unlock codes.</li>";
    html += "<li><b>GET /health</b> - Simple connectivity check.</li>";
    html += "<li><b>POST /arm</b> - Begin session (JSON body required).</li>";
    html += "<li><b>POST /start-test</b> - Run 2-min hardware test.</li>";
    html += "<li><b>POST /abort</b> - Emergency stop (triggers penalty).</li>";
    html += "<li><b>POST /keepalive</b> - Reset connection watchdog.</li>";
    html += "<li><b>POST /update-wifi</b> - Update credentials (JSON).</li>";
    html += "<li><b>POST /factory-reset</b> - Erase data and reset.</li>";
    html += "</ul></body></html>";
    request->send(200, "text/html", html);
}

/**
 * Handler for GET /health
 */
void handleHealth(AsyncWebServerRequest *request) {
    // RAII Managed
    std::unique_ptr<JsonDocument> doc(new JsonDocument());
    (*doc)["status"] = "ok";
    (*doc)["message"] = "Device is reachable.";
    String response;
    serializeJson(*doc, response);
    request->send(200, "application/json", response);
}

/**
 * Handler for POST /keepalive
 * Resets the connection watchdog (only active in LOCKED mode).
 */
void handleKeepAlive(AsyncWebServerRequest *request) {
    // Guard access to variables
    if (xSemaphoreTakeRecursive(stateMutex, (TickType_t)pdMS_TO_TICKS(100)) == pdTRUE) {
        // "Pet" the watchdog only if the session is in the LOCKED state
        if (currentState == LOCKED || currentState == TESTING) {
            g_lastKeepAliveTime = millis();

            // Log if we are recovering from missed checks
            if (g_currentKeepAliveStrikes > 0) {
                char logBuf[100];
                snprintf(logBuf, sizeof(logBuf), "Keep-Alive Watchdog: Signal received. Resetting %d strikes.", g_currentKeepAliveStrikes);
                logMessage(logBuf);
            }

            g_currentKeepAliveStrikes = 0; // Reset strike counter
        }
        xSemaphoreGiveRecursive(stateMutex);
    }
    request->send(200); 
}

/**
 * Handler for POST /arm (body)
 * Validates JSON, determines Strategy, sets timers, and enters ARMED state.
 */
void handleArm(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
    // Handle JSON body
    if (index + len != total) {
        return; // Wait for more data
    }
    
    // Stack Safety - Check payload size
    if (len > 4096) {
        sendJsonError(request, 413, "Payload too large.");
        return;
    }

    // ALLOCATE ON HEAP via RAII (Prefer PSRAM if available via ArduinoJson internals)
    std::unique_ptr<JsonDocument> doc(new JsonDocument());
    
    DeserializationError error = deserializeJson(*doc, (const char*)data, len);
    if (error) {
        char logBuf[100];
        snprintf(logBuf, sizeof(logBuf), "Failed to parse /arm JSON: %s", error.c_str());
        logMessage(logBuf);
        sendJsonError(request, 400, "Invalid JSON body.");
        return;
    }

    if (!(*doc)["lockDurationSeconds"].is<JsonInteger>() || !(*doc)["penaltyDurationSeconds"].is<JsonInteger>()) {
        sendJsonError(request, 400, "Missing required fields: lockDurationSeconds, penaltyDurationSeconds.");
        return;
    }
    
    // Read session-specific data from the request
    unsigned long durationSeconds = (*doc)["lockDurationSeconds"];
    unsigned long penaltySeconds = (*doc)["penaltyDurationSeconds"];
    bool newHideTimer = (*doc)["hideTimer"] | false; // Default to false if not present
    
    // Parse Strategy
    String stratStr = (*doc)["triggerStrategy"] | "autoCountdown";
    TriggerStrategy requestedStrat = STRAT_AUTO_COUNTDOWN;
    if (stratStr == "buttonTrigger") {
        requestedStrat = STRAT_BUTTON_TRIGGER;
    }

    unsigned long tempDelays[4] = {0};
    
    // Parse channels from nested 'channelDelaysSeconds' object
    if ((*doc)["channelDelaysSeconds"].is<JsonObject>()) {
        JsonObject delaysObj = (*doc)["channelDelaysSeconds"];
        if (delaysObj["ch1"].is<unsigned long>()) tempDelays[0] = delaysObj["ch1"].as<unsigned long>();
        if (delaysObj["ch2"].is<unsigned long>()) tempDelays[1] = delaysObj["ch2"].as<unsigned long>();
        if (delaysObj["ch3"].is<unsigned long>()) tempDelays[2] = delaysObj["ch3"].as<unsigned long>();
        if (delaysObj["ch4"].is<unsigned long>()) tempDelays[3] = delaysObj["ch4"].as<unsigned long>();
    }
    
    // Validate ranges against System Config
    unsigned long minLockSec = g_systemConfig.minLockSeconds;
    unsigned long maxLockSec = g_systemConfig.maxLockSeconds;
    unsigned long minPenaltySec = g_systemConfig.minPenaltySeconds;
    unsigned long maxPenaltySec = g_systemConfig.maxPenaltySeconds;

    if (durationSeconds < minLockSec || durationSeconds > maxLockSec) {
        sendJsonError(request, 400, "Invalid lockDurationSeconds."); return;
    }
    if (penaltySeconds < minPenaltySec || penaltySeconds > maxPenaltySec) {
        sendJsonError(request, 400, "Invalid penaltyDurationSeconds."); return;
    }

    // Filter delays by enabled mask
    for(int i=0; i<4; i++) {
        if (!((g_enabledChannelsMask >> i) & 1)) {
            tempDelays[i] = 0; 
        }
    }

    // Prepare response buffer (stack allocated String)
    String responseJson; 

    // We lock briefly ONLY to update the state machine.
    if (xSemaphoreTakeRecursive(stateMutex, (TickType_t)pdMS_TO_TICKS(1000)) == pdTRUE) {
        // State check inside mutex
        if (currentState != READY) {
            xSemaphoreGiveRecursive(stateMutex);
            sendJsonError(request, 409, "Device is not ready.");
            return;
        }

        // Copy validated data to State
        for(int i=0; i<MAX_CHANNELS; i++) channelDelaysRemaining[i] = tempDelays[i];
        
        hideTimer = newHideTimer;
        currentStrategy = requestedStrat;

        // Apply any pending payback time from previous sessions
        unsigned long paybackInSeconds = paybackAccumulated;
        if (paybackInSeconds > 0) {
            logMessage("Applying pending payback time to this session.");
        }

        // Save configs
        lockSecondsConfig = durationSeconds + paybackInSeconds;
        penaltySecondsConfig = penaltySeconds;
        
        // Logging (Inside mutex for thread safety of buffer)
        char logBuf[256];
        snprintf(logBuf, sizeof(logBuf), "API: /arm. Mode: %s. Lock: %lu s. Hide: %s",
                (currentStrategy == STRAT_BUTTON_TRIGGER ? "BUTTON" : "AUTO"),
                durationSeconds, (hideTimer ? "Yes" : "No"));
        logMessage(logBuf);

        // Logic - Response Doc on Heap via RAII
        std::unique_ptr<JsonDocument> responseDoc(new JsonDocument());

        // Enter ARMED state
        currentState = ARMED;
        setLedPattern(ARMED);
        
        // Setup based on strategy
        if (currentStrategy == STRAT_BUTTON_TRIGGER) {
            // Wait for Button: Set Timeout
            triggerTimeoutRemaining = g_systemConfig.armedTimeoutSeconds;
            logMessage("   -> Waiting for Manual Trigger.");
        } else {
            // Auto: Timers start ticking immediately in handleOneSecondTick
            logMessage("   -> Auto Sequence Started.");
        }

        startTimersForState(ARMED);
        // NOTE: Watchdog is NOT armed here, only when LOCKED (or in TEST)

        (*responseDoc)["status"] = "armed";
        (*responseDoc)["triggerStrategy"] = (currentStrategy == STRAT_BUTTON_TRIGGER) ? "buttonTrigger" : "autoCountdown";
        
        saveState(true);
        
        serializeJson(*responseDoc, responseJson);
        // responseDoc deleted here automatically
        
        xSemaphoreGiveRecursive(stateMutex); // RELEASE LOCK
    } else {
        sendJsonError(request, 503, "System Busy");
        return;
    }

    // Send response OUTSIDE of lock
    request->send(200, "application/json", responseJson);
}

/**
 * Handler for POST /start-test
 */
void handleStartTest(AsyncWebServerRequest *request) {
    String responseJson;

    if (xSemaphoreTakeRecursive(stateMutex, (TickType_t)pdMS_TO_TICKS(1000)) == pdTRUE) {
        if (currentState != READY) {
          xSemaphoreGiveRecursive(stateMutex);
          sendJsonError(request, 409, "Device must be in READY state to run test.");
          return;
        }
        logMessage("API: /start-test received. Engaging Channels for 2 min.");
        sendChannelOnAll();
        currentState = TESTING;
        setLedPattern(TESTING);
        testSecondsRemaining = g_systemConfig.testModeDurationSeconds;
        startTimersForState(TESTING);
        g_lastKeepAliveTime = millis(); // Arm keep-alive watchdog for test mode
        g_currentKeepAliveStrikes = 0;  // Reset strikes
        saveState(true);
        
        std::unique_ptr<JsonDocument> doc(new JsonDocument());
        (*doc)["status"] = "testing";
        (*doc)["testSecondsRemaining"] = testSecondsRemaining;
        serializeJson(*doc, responseJson);
        
        xSemaphoreGiveRecursive(stateMutex); 
    } else {
        sendJsonError(request, 503, "System Busy");
        return;
    }
    request->send(200, "application/json", responseJson);
}

/**
 * Handler for POST /abort
 */
void handleAbort(AsyncWebServerRequest *request) {
    String responseJson;
    if (xSemaphoreTakeRecursive(stateMutex, (TickType_t)pdMS_TO_TICKS(1000)) == pdTRUE) {
        if (currentState != LOCKED && currentState != ARMED && currentState != TESTING) {
          xSemaphoreGiveRecursive(stateMutex);
          sendJsonError(request, 409, "Device is not in an abortable state."); return;
        }
        
        String statusMsg = "ready";
        if (currentState == LOCKED) statusMsg = "aborted"; // Aborting a lock goes to penalty
        
        abortSession("API"); // This handles all logic
        
        std::unique_ptr<JsonDocument> doc(new JsonDocument());
        (*doc)["status"] = statusMsg;
        serializeJson(*doc, responseJson);
        
        xSemaphoreGiveRecursive(stateMutex); 
    } else {
        sendJsonError(request, 503, "System Busy");
        return;
    }
    request->send(200, "application/json", responseJson);
}

/**
 * Handler for GET /reward
 */
void handleReward(AsyncWebServerRequest *request) {
    // To be safe, we just take the lock for the whole operation here since it's read-only and fast enough.
    if (xSemaphoreTakeRecursive(stateMutex, (TickType_t)pdMS_TO_TICKS(1000)) == pdTRUE) {
        if (currentState == LOCKED || currentState == ABORTED || currentState == ARMED) {
            xSemaphoreGiveRecursive(stateMutex);
            sendJsonError(request, 403, "Reward is not yet available.");
        } else {
            // Send history (READY or COMPLETED)
            logMessage("API: /reward GET success. Releasing code history.");
            std::unique_ptr<JsonDocument> doc(new JsonDocument());
            JsonArray arr = (*doc).to<JsonArray>();
            for(int i = 0; i < REWARD_HISTORY_SIZE; i++) {
                if (strlen(rewardHistory[i].code) == REWARD_CODE_LENGTH) {
                    JsonObject reward = arr.add<JsonObject>();
                    reward["code"] = rewardHistory[i].code;
                    reward["checksum"] = rewardHistory[i].checksum;
                }
            }
            String response;
            serializeJson(*doc, response);
            xSemaphoreGiveRecursive(stateMutex); 
            request->send(200, "application/json", response);
        }
    } else {
        sendJsonError(request, 503, "System Busy");
    }
}

/**
 * Handler for GET /status
 */
void handleStatus(AsyncWebServerRequest *request) {
    // 1. Create the Snapshot
    struct StateSnapshot {
        SessionState state;
        TriggerStrategy strategy;
        unsigned long lockRemain;
        unsigned long penaltyRemain;
        unsigned long testRemain;
        unsigned long delays[4];
        bool hideTimer;
        uint32_t streaks;
        uint32_t aborted;
        uint32_t completed;
        uint32_t totalLocked;
        uint32_t payback;
    } snapshot;

    // Quick Lock & Copy
    if (xSemaphoreTakeRecursive(stateMutex, (TickType_t)pdMS_TO_TICKS(200)) == pdTRUE) {
        snapshot.state = currentState;
        snapshot.strategy = currentStrategy;
        snapshot.lockRemain = lockSecondsRemaining;
        snapshot.penaltyRemain = penaltySecondsRemaining;
        snapshot.testRemain = testSecondsRemaining;
        for(int i=0; i<4; i++) snapshot.delays[i] = channelDelaysRemaining[i];
        snapshot.hideTimer = hideTimer;
        snapshot.streaks = sessionStreakCount;
        snapshot.aborted = abortedSessions;
        snapshot.completed = completedSessions;
        snapshot.totalLocked = totalLockedSessionSeconds;
        snapshot.payback = paybackAccumulated;
        xSemaphoreGiveRecursive(stateMutex);
    } else {
        request->send(503, "text/plain", "System Busy");
        return;
    }

    // 2. Stream directly to TCP buffer
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    
    // Use Stack memory. 
    JsonDocument doc; 

    // Timers
    doc["status"] = stateToString(snapshot.state);
    doc["lockSecondsRemaining"] = snapshot.lockRemain;
    doc["penaltySecondsRemaining"] = snapshot.penaltyRemain;
    doc["testSecondsRemaining"] = snapshot.testRemain;
    
    // Arming Context
    if (snapshot.state == ARMED) {
        doc["triggerStrategy"] = (snapshot.strategy == STRAT_BUTTON_TRIGGER) ? "buttonTrigger" : "autoCountdown";
        if (snapshot.strategy == STRAT_BUTTON_TRIGGER) {
            doc["triggerTimeoutRemainingSeconds"] = triggerTimeoutRemaining;
        }
    }

    JsonObject channels = doc["channelDelaysRemainingSeconds"].to<JsonObject>();
    channels["ch1"] = snapshot.delays[0];
    channels["ch2"] = snapshot.delays[1];
    channels["ch3"] = snapshot.delays[2];
    channels["ch4"] = snapshot.delays[3];

    doc["hideTimer"] = snapshot.hideTimer;

    // Accumulated stats
    JsonObject stats = doc["stats"].to<JsonObject>();
    stats["streaks"] = snapshot.streaks;
    stats["aborted"] = snapshot.aborted;
    stats["completed"] = snapshot.completed;
    stats["totalTimeLockedSeconds"] = snapshot.totalLocked;
    stats["pendingPaybackSeconds"] = snapshot.payback;

    // Serialize directly into the response stream
    serializeJson(doc, *response);
    
    request->send(response);
}

/**
 * Handler for GET /details
 */
void handleDetails(AsyncWebServerRequest *request) {
    String response;

    if (xSemaphoreTakeRecursive(stateMutex, (TickType_t)pdMS_TO_TICKS(1000)) == pdTRUE) {
        uint8_t mac[6];
        esp_efuse_mac_get_default(mac);
        char uniqueHostname[20];
        snprintf(uniqueHostname, sizeof(uniqueHostname), "lobster-lock-%02X%02X%02X", mac[3], mac[4], mac[5]);

        std::unique_ptr<JsonDocument> doc(new JsonDocument());
        (*doc)["name"] = DEVICE_NAME;
        (*doc)["id"] = uniqueHostname;
        (*doc)["version"] = DEVICE_VERSION;
        (*doc)["numberOfChannels"] = 4;
        (*doc)["address"] = WiFi.localIP().toString();

        // Channels Configuration
        JsonObject channels = (*doc)["channels"].to<JsonObject>();
        channels["ch1"] = (bool)((g_enabledChannelsMask >> 0) & 1);
        channels["ch2"] = (bool)((g_enabledChannelsMask >> 1) & 1);
        channels["ch3"] = (bool)((g_enabledChannelsMask >> 2) & 1);
        channels["ch4"] = (bool)((g_enabledChannelsMask >> 3) & 1);

        // Deterrent Configuration
        JsonObject config = (*doc)["deterrents"].to<JsonObject>();
        config["enableStreaks"] = enableStreaks;
        config["enablePaybackTime"] = enablePaybackTime;
        config["paybackDurationSeconds"] = paybackTimeSeconds; 
        
        // Add features array
        JsonArray features = (*doc)["features"].to<JsonArray>();
        features.add("abortLongPress");
        features.add("startLongPress");
        features.add("startCountdown");
        features.add("statusLed");

        serializeJson(*doc, response);
        xSemaphoreGiveRecursive(stateMutex); 
    } else {
        sendJsonError(request, 503, "System Busy");
        return;
    }
    request->send(200, "application/json", response);
}

/**
 * Handler for GET /log
 */
void handleLog(AsyncWebServerRequest *request) {
    // Streaming logs needs to NOT lock the main loop for the whole transmission.
    // We use Chunked Response implicitly via AsyncResponseStream.
    // We lock briefly per line (or small chunk) to print.
    
    AsyncResponseStream *response = request->beginResponseStream("text/plain");
    
    // 1. Capture buffer state (indices) safely
    int start = 0;
    int count = 0;
    
    if (xSemaphoreTakeRecursive(stateMutex, (TickType_t)pdMS_TO_TICKS(100)) == pdTRUE) {
        // Handle ring buffer logic
        if (logBufferFull) {
            start = logBufferIndex; // Start at the oldest entry
            count = LOG_BUFFER_SIZE;
        } else {
            start = 0;
            count = logBufferIndex; // Only read up to where we've written
        }
        xSemaphoreGiveRecursive(stateMutex);
    } else {
        request->send(503, "text/plain", "Busy");
        return;
    }

    // 2. Iterate and send, locking briefly for each copy
    for (int i = 0; i < count; i++) {
        char lineBuffer[MAX_LOG_ENTRY_LENGTH];
        bool success = false;
        
        // Brief lock to copy one line
        if (xSemaphoreTakeRecursive(stateMutex, (TickType_t)pdMS_TO_TICKS(50)) == pdTRUE) {
            int index = (start + i) % LOG_BUFFER_SIZE;
            strncpy(lineBuffer, logBuffer[index], MAX_LOG_ENTRY_LENGTH);
            lineBuffer[MAX_LOG_ENTRY_LENGTH-1] = '\0'; // safety
            success = true;
            xSemaphoreGiveRecursive(stateMutex);
        }
        
        if (success) {
            response->print(lineBuffer);
            response->print("\r\n");
        } else {
            response->print("[Log Line Dropped - Mutex Contention]\r\n");
        }
        // Yielding happens automatically in AsyncWebServer if we stream
    }
    
    request->send(response);
}

/**
 * Handler for POST /update-wifi (body)
 */
void handleUpdateWifi(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
    // Handle JSON body
    if (index + len != total) return;
    
    std::unique_ptr<JsonDocument> doc(new JsonDocument());
    
    DeserializationError error = deserializeJson(*doc, (const char*)data, len);
    if (error) { 
        char logBuf[100];
        snprintf(logBuf, sizeof(logBuf), "Failed to parse /update-wifi JSON: %s", error.c_str());
        logMessage(logBuf);
        sendJsonError(request, 400, "Invalid JSON body."); 
        return; 
    }

    // Validate JSON body
    if (!(*doc)["ssid"].is<const char*>() || !(*doc)["pass"].is<const char*>()) {
        sendJsonError(request, 400, "Missing required fields: ssid, pass.");
        return;
    }

    const char* ssid = (*doc)["ssid"];
    const char* pass = (*doc)["pass"];

    if (xSemaphoreTakeRecursive(stateMutex, (TickType_t)pdMS_TO_TICKS(1000)) == pdTRUE) {
        // State is already checked in the on() handler, but as a safeguard:
        if (currentState != READY) {
            xSemaphoreGiveRecursive(stateMutex);
            logMessage("API: /update-wifi failed. Device is not in READY state.");
            sendJsonError(request, 409, "Device must be in READY state to update Wi-Fi.");
            return;
        }
        
        logMessage("API: /update-wifi received. Saving new credentials to NVS.");
        
        // Save new credentials to NVS
        wifiPreferences.begin("wifi-creds", false); // Open read/write
        wifiPreferences.putString("ssid", ssid);
        wifiPreferences.putString("pass", pass);
        wifiPreferences.end(); // Commit changes

        logMessage("New Wi-Fi credentials saved.");

        xSemaphoreGiveRecursive(stateMutex); 
    } else {
        sendJsonError(request, 503, "System Busy");
        return;
    }

    // Send response
    std::unique_ptr<JsonDocument> responseDoc(new JsonDocument());
    String response;
    (*responseDoc)["status"] = "success";
    (*responseDoc)["message"] = "Wi-Fi credentials updated. Please reboot the device to apply.";
    serializeJson(*responseDoc, response);
    request->send(200, "application/json", response);
}

/**
 * Handler for POST /factory-reset
 */
void handleFactoryReset(AsyncWebServerRequest *request) {

    if (xSemaphoreTakeRecursive(stateMutex, (TickType_t)pdMS_TO_TICKS(1000)) == pdTRUE) {
        // Do not allow forgetting during an active session
        if (currentState != READY && currentState != COMPLETED) {
            xSemaphoreGiveRecursive(stateMutex);
            logMessage("API: /factory-reset failed. Device is currently in an active session.");
            sendJsonError(request, 409, "Device is in an active session. Cannot reset while locked, in countdown, or in penalty.");
            return;
        }

        logMessage("API: /factory-reset received. Erasing credentials and session data.");
        
        // Erase Wi-Fi
        wifiPreferences.begin("wifi-creds", false); 
        wifiPreferences.clear();
        wifiPreferences.end(); 

        logMessage("Wi-Fi credentials erased.");

        // Erase all session state and counters
        sessionState.begin("session", false);
        sessionState.clear();
        sessionState.end();
        logMessage("Session state and config erased.");
        
        // Erase all device provisioning settings
        provisioningPrefs.begin("provisioning", false); 
        provisioningPrefs.clear();
        provisioningPrefs.end(); 
       
        // Clear boot loop stats
        bootPrefs.begin("boot", false);
        bootPrefs.clear();
        bootPrefs.end();

        xSemaphoreGiveRecursive(stateMutex); 
    } else {
        sendJsonError(request, 503, "System Busy");
        return;
    }

    // Send the response *before* we restart
    std::unique_ptr<JsonDocument> doc(new JsonDocument());
    (*doc)["status"] = "resetting";
    (*doc)["message"] = "Device credentials and state erased. Rebooting into provisioning mode.";
    String response;
    serializeJson(*doc, response);
    request->send(200, "application/json", response);

    // Give the response time to send, then restart
    delay(1000);
    ESP.restart();
}

// =================================================================
// --- Button Callbacks ---
// =================================================================

/**
 * DOUBLE PRESS: Used to TRIGGER the session when ARMED.
 */
void handleDoublePress() {
    if (xSemaphoreTakeRecursive(stateMutex, (TickType_t)pdMS_TO_TICKS(100)) == pdTRUE) {
        
        // TRIGGER Logic
        if (currentState == ARMED) {
            if (currentStrategy == STRAT_BUTTON_TRIGGER) {
                logMessage("Button: Double Click confirmed! Locking session.");
                
                currentState = LOCKED;
                setLedPattern(LOCKED);
                
                lockSecondsRemaining = lockSecondsConfig;
                startTimersForState(LOCKED);
                armFailsafeTimer(); 
                g_lastKeepAliveTime = millis(); 
                g_currentKeepAliveStrikes = 0;  
                
                // Engage hardware immediately
                sendChannelOnAll();

                saveState(true);
            }
        }

        xSemaphoreGiveRecursive(stateMutex);
    }
}

/**
 * LONG PRESS: Universal Cancel/Abort.
 * 1. If ARMED: Cancels arming (Returns to READY, no penalty).
 * 2. If LOCKED: Triggers ABORT (Emergency Stop / Penalty).
 */
void handleLongPress() {
    if (xSemaphoreTakeRecursive(stateMutex, (TickType_t)pdMS_TO_TICKS(100)) == pdTRUE) {
        
        // Cancel Arming (Safety reset)
        if (currentState == ARMED) {
            logMessage("Button: Long Press. Cancelling Arming.");
            abortSession("Button LongPress");
        } 
        // Abort the session (Emergency Stop)
        else if (currentState == LOCKED) {
            logMessage("Button: Long press in LOCKED state. Emergency Abort.");
            abortSession("Button LongPress");
        }
        
        xSemaphoreGiveRecursive(stateMutex);
    }
}

// =================================================================
// --- Timer Callbacks ---
// =================================================================

/**
 * Resets timers when entering a new state.
 */
void startTimersForState(SessionState state) {
    // Adjust Watchdog based on state
    if (state == LOCKED || state == ABORTED || state == TESTING) {
        updateWatchdogTimeout(CRITICAL_WDT_TIMEOUT); // Tight loop check
    } else {
        updateWatchdogTimeout(DEFAULT_WDT_TIMEOUT); // Loose loop check (network delays)
    }

    if (state == ARMED) {
        logMessage("Starting arming logic.");
    } else if (state == LOCKED) {
        logMessage("Starting lock logic.");
    } else if (state == ABORTED) {
        logMessage("Starting penalty logic.");
    } else if (state == TESTING) {
        logMessage("Starting test logic.");
    }
    g_currentKeepAliveStrikes = 0; // Reset strikes on state change
}

/**
 * Helper to check the Keep-Alive Watchdog.
 * Returns true if the session was aborted.
 */
bool checkKeepAliveWatchdog() {
    // Check if watchdog is armed
    if (g_lastKeepAliveTime == 0) return false; 

    unsigned long elapsed = millis() - g_lastKeepAliveTime;
    
    // Integer division finds how many 10s intervals have passed
    // Uses Configured Interval
    int calculatedStrikes = elapsed / g_systemConfig.keepAliveIntervalMs;

    // Only log/act if the strike count has increased
    if (calculatedStrikes > g_currentKeepAliveStrikes) {
        g_currentKeepAliveStrikes = calculatedStrikes;
        char logBuf[100];
        
        // Uses Configured Strike Count
        if (g_currentKeepAliveStrikes >= g_systemConfig.keepAliveMaxStrikes) {
            snprintf(logBuf, sizeof(logBuf), "Keep-Alive Watchdog: Strike %d/%d! ABORTING.", g_currentKeepAliveStrikes, g_systemConfig.keepAliveMaxStrikes);
            logMessage(logBuf);
            abortSession("Watchdog Strikeout");
            return true; // Signal that we aborted
        } else {
            snprintf(logBuf, sizeof(logBuf), "Keep-Alive Watchdog: Missed check. Strike %d/%d", g_currentKeepAliveStrikes, g_systemConfig.keepAliveMaxStrikes);
            logMessage(logBuf);
        }
    }
    return false;
}

/**
 * This is the main state-machine handler, called 1x/sec from loop().
 * It safely updates all timers and handles state transitions.
 */
void handleOneSecondTick() {
  switch (currentState) {
    case ARMED:
    { 
        if (currentStrategy == STRAT_AUTO_COUNTDOWN) {
            // STRATEGY: Auto Countdown
            // Decrement active channels immediately
            bool allDelaysZero = true;

            // Decrement all active channel delays
            for (size_t i = 0; i < MAX_CHANNELS; i++) {
                // Check if enabled in mask
                if ((g_enabledChannelsMask >> i) & 1) {
                    if (channelDelaysRemaining[i] > 0) {
                        allDelaysZero = false;
                        if (--channelDelaysRemaining[i] == 0) {
                            sendChannelOn(i); // Turn on Channel when its timer hits zero
                            char logBuf[100];
                            snprintf(logBuf, sizeof(logBuf), "Channel %d delay finished. Channel closed.", (int)i + 1);
                            logMessage(logBuf);
                        }
                    }
                }
            }

            // If all delays are done, transition to LOCKED
            if (allDelaysZero) {
                logMessage("Auto-Start: All delays finished. Transitioning to LOCKED state.");
                currentState = LOCKED;
                setLedPattern(LOCKED);
                lockSecondsRemaining = lockSecondsConfig;
                startTimersForState(LOCKED);
                armFailsafeTimer(); // DEATH GRIP
                g_lastKeepAliveTime = millis(); // Arm keep-alive watchdog
                g_currentKeepAliveStrikes = 0;  // Reset strikes
                saveState(true); // Force save
            }
        } 
        else if (currentStrategy == STRAT_BUTTON_TRIGGER) {
            // STRATEGY: Wait for Button
            // Channels do NOT tick down here. We just wait for the button event.
            // Check for timeout.
            if (triggerTimeoutRemaining > 0) {
                triggerTimeoutRemaining--;
            } else {
                logMessage("Armed Timeout: Button not pressed in time. Aborting.");
                abortSession("Arm Timeout");
            }
        }
        break;
    }
    case LOCKED:
      // --- Keep-Alive Watchdog Check ---
      if (checkKeepAliveWatchdog()) {
          return; // Aborted inside helper
      }
      // Decrement lock timer
      if (lockSecondsRemaining > 0) {
          if (--lockSecondsRemaining == 0) {
              completeSession(); // Timer finished
          }
          // Increment total locked time only when session is active
          totalLockedSessionSeconds++;
      }
      break;
    case ABORTED:
      // Decrement penalty timer
      if (penaltySecondsRemaining > 0 && --penaltySecondsRemaining == 0) {
        completeSession(); // Timer finished
      }
      break;
    case TESTING:
      // --- Keep-Alive Watchdog Check ---
      if (checkKeepAliveWatchdog()) {
          return; // Aborted inside helper
      }
      // Decrement test timer
      if (testSecondsRemaining > 0 && --testSecondsRemaining == 0) {
        logMessage("Test mode timer expired.");
        stopTestMode(); // Timer finished
      }
      break;
    case READY:
    case COMPLETED:
    default:
      // Do nothing
      break;
  }
}

// =================================================================
// --- NVS (Preferences) Persistence ---
// =================================================================

/**
 * Loads the entire session state and config from NVS (Preferences)
 * Uses key-value pairs for robustness and flash longevity.
 * @return true if a valid session was loaded, false otherwise.
 */
bool loadState() {
    sessionState.begin("session", true); // Read-only
    unsigned long magic = sessionState.getULong("magic", 0);
    
    // Check magic value first. If it's not present or correct,
    // we assume the rest of the data is invalid.
    if (magic != MAGIC_VALUE) {
        // No valid data.
        sessionState.end();
        return false; // Report failure
    }

    logMessage("Valid session data found in NVS.");
    
    // Load all values from NVS, providing a default for each
    currentState = (SessionState)sessionState.getUChar("state", (uint8_t)READY);
    currentStrategy = (TriggerStrategy)sessionState.getUChar("strategy", (uint8_t)STRAT_AUTO_COUNTDOWN);

    lockSecondsRemaining = sessionState.getULong("lockRemain", 0);
    penaltySecondsRemaining = sessionState.getULong("penaltyRemain", 0);
    penaltySecondsConfig = sessionState.getULong("penaltyConfig", 0);
    lockSecondsConfig = sessionState.getULong("lockConfig", 0);
    testSecondsRemaining = sessionState.getULong("testRemain", 0);
    
    hideTimer = sessionState.getBool("hideTimer", false);

    // Load device configuration (Session Specific)
    enableStreaks = sessionState.getBool("enableStreaks", true);
    enablePaybackTime = sessionState.getBool("enablePayback", true);
    paybackTimeSeconds = sessionState.getUInt("paybackSeconds", 900);

    // Load persistent session counters
    sessionStreakCount = sessionState.getUInt("streak", 0);
    completedSessions = sessionState.getUInt("completed", 0);
    abortedSessions = sessionState.getUInt("aborted", 0);
    paybackAccumulated = sessionState.getUInt("paybackAccum", 0);
    totalLockedSessionSeconds = sessionState.getUInt("totalLocked", 0);

    // Load arrays (as binary blobs)
    sessionState.getBytes("delays", channelDelaysRemaining, sizeof(channelDelaysRemaining));
    sessionState.getBytes("rewards", rewardHistory, sizeof(rewardHistory));

    sessionState.end(); // Done reading
    
    return true; // Report success
}

/**
 * Analyzes the loaded state after a reboot and performs
 * the necessary transitions (e.g., aborting, resetting).
 */
void handleRebootState() {

    switch (currentState) {
        case LOCKED:
        case ARMED:
        case TESTING:
            // These are active states. A reboot during them is an abort.
            logMessage("Reboot detected during session. Aborting session...");
            abortSession("Reboot");
            break;

        case COMPLETED:
            // Session was finished. Reset to ready for a new one.
            logMessage("Loaded COMPLETED state. Resetting to READY.");
            resetToReady(true); // This saves the new state
            break;

        case READY:
        case ABORTED:
        default:
            // These states are safe to resume.
            // ABORTED will resume its penalty timer.
            // The watchdog is NOT armed, per the "LOCKED only" rule.
            logMessage("Resuming in-progress state.");
            startTimersForState(currentState); // Resume timers
            break;
    }
}

/**
 * Saves the entire session state and config to NVS (Preferences)
 * Uses key-value pairs for robustness and flash longevity.
 */
void saveState(bool force) {
    if (!force) return;

    char logBuf[100];
    snprintf(logBuf, sizeof(logBuf), "Saving state to NVS: %s", stateToString(currentState));
    logMessage(logBuf);
    
    esp_task_wdt_reset(); // Feed before potentially slow commit

    sessionState.begin("session", false); // Open namespace in read/write
    
    // Save all dynamic state variables
    sessionState.putUChar("state", (uint8_t)currentState);
    sessionState.putUChar("strategy", (uint8_t)currentStrategy);
    
    sessionState.putULong("lockRemain", lockSecondsRemaining);
    sessionState.putULong("penaltyRemain", penaltySecondsRemaining);
    sessionState.putULong("penaltyConfig", penaltySecondsConfig);
    sessionState.putULong("lockConfig", lockSecondsConfig);
    sessionState.putULong("testRemain", testSecondsRemaining);
    
    // Save device configuration
    sessionState.putBool("hideTimer", hideTimer);
    sessionState.putBool("enableStreaks", enableStreaks);
    sessionState.putBool("enablePayback", enablePaybackTime);
    sessionState.putUInt("paybackSeconds", paybackTimeSeconds);

    // Save persistent counters
    sessionState.putUInt("streak", sessionStreakCount);
    sessionState.putUInt("completed", completedSessions);
    sessionState.putUInt("aborted", abortedSessions);
    sessionState.putUInt("paybackAccum", paybackAccumulated);
    sessionState.putUInt("totalLocked", totalLockedSessionSeconds);

    // Save arrays as binary "blobs"
    sessionState.putBytes("delays", channelDelaysRemaining, sizeof(channelDelaysRemaining));
    sessionState.putBytes("rewards", rewardHistory, sizeof(rewardHistory));
    
    // Save magic value
    sessionState.putULong("magic", MAGIC_VALUE);

    sessionState.end(); // This commits the changes
    
    esp_task_wdt_reset(); // And feed after
}

// =================================================================
// --- Utilities & Helpers ---
// =================================================================

/**
 * Helper to convert a 2-byte LE array to uint16_t
 */
uint16_t bytesToUint16(uint8_t* data) {
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

/**
 * Helper to convert a 4-byte LE array to uint32_t
 */
uint32_t bytesToUint32(uint8_t* data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

/**
 * Formats seconds into "H h, M min, S s"
 */
void formatSeconds(unsigned long totalSeconds, char* buffer, size_t size) {
    unsigned long hours = totalSeconds / 3600;
    unsigned long minutes = (totalSeconds % 3600) / 60;
    unsigned long seconds = totalSeconds % 60;
    snprintf(buffer, size, "%lu h, %lu min, %lu s", hours, minutes, seconds);
}

/**
 * Thread-safe logging. NO SERIAL IO IN THIS FUNCTION.
 * Adds a message to the in-memory log buffer and pushes to the Serial Queue.
 */
void logMessage(const char* message) {
    // Used shorter timeout here to prevent loop starvation
    if (xSemaphoreTakeRecursive(stateMutex, (TickType_t)pdMS_TO_TICKS(100)) == pdTRUE) {
    
    // Update RAM ring buffer (for API)
    // Removed timestamping per request. Just store the message.
    snprintf(logBuffer[logBufferIndex], MAX_LOG_ENTRY_LENGTH, "%s", message);
    logBufferIndex++;
    if (logBufferIndex >= LOG_BUFFER_SIZE) {
        logBufferIndex = 0;
        logBufferFull = true;
    }

    // Push to Serial Queue
    int nextHead = (serialQueueHead + 1) % SERIAL_QUEUE_SIZE;
    
    if (nextHead != serialQueueTail) {
        // Buffer not full
        snprintf(serialLogQueue[serialQueueHead], MAX_LOG_ENTRY_LENGTH, "%s", message);
        serialQueueHead = nextHead;
    } else {
        // Buffer full
    }
    
    xSemaphoreGiveRecursive(stateMutex);
  } else {
    // Emergency fallback
  }
}

/**
 * Called in main loop to drain log queue safely to Serial port.
 * Allows printing without blocking critical sections.
 * Drains up to 10 messages per call to prevent lag/dropped logs.
 */
void processLogQueue() {
    // Process up to 10 lines at once so we don't fall behind
    int maxLinesToProcess = 10; 

    while (maxLinesToProcess > 0) {
        char msgCopy[MAX_LOG_ENTRY_LENGTH];
        bool hasMessage = false;

        // 1. Quick lock to check/pop a message
        // We use a short timeout; if we can't get the lock, we skip printing this cycle
        if (xSemaphoreTakeRecursive(stateMutex, (TickType_t)pdMS_TO_TICKS(5)) == pdTRUE) {
            if (serialQueueHead != serialQueueTail) {
                // Copy message out
                strncpy(msgCopy, serialLogQueue[serialQueueTail], MAX_LOG_ENTRY_LENGTH);
                msgCopy[MAX_LOG_ENTRY_LENGTH-1] = '\0'; // safety null
                
                // Advance tail
                serialQueueTail = (serialQueueTail + 1) % SERIAL_QUEUE_SIZE;
                hasMessage = true;
            }
            // RELEASE LOCK IMMEDIATELY
            xSemaphoreGiveRecursive(stateMutex);
        } else {
            // If we couldn't get the lock, stop trying for this cycle
            break; 
        }

        // 2. Print OUTSIDE the lock (Serial is slow!)
        if (hasMessage) {
            Serial.println(msgCopy);
            maxLinesToProcess--;
        } else {
            // Queue is empty, we are done
            break;
        }
    }
}

/**
 * NATO Phonetic Alphabet Lookup
 */
const char* getNatoWord(char c) {
    switch(c) {
        case 'A': return "Alpha"; case 'B': return "Bravo"; case 'C': return "Charlie";
        case 'D': return "Delta"; case 'E': return "Echo"; case 'F': return "Foxtrot";
        case 'G': return "Golf"; case 'H': return "Hotel"; case 'I': return "India";
        case 'J': return "Juliett"; case 'K': return "Kilo"; case 'L': return "Lima";
        case 'M': return "Mike"; case 'N': return "November"; case 'O': return "Oscar";
        case 'P': return "Papa"; case 'Q': return "Quebec"; case 'R': return "Romeo";
        case 'S': return "Sierra"; case 'T': return "Tango"; case 'U': return "Uniform";
        case 'V': return "Victor"; case 'W': return "Whiskey"; case 'X': return "X-ray";
        case 'Y': return "Yankee"; case 'Z': return "Zulu";
        default: return "";
    }
}

/**
 * Calculates the Alpha-Numeric Checksum (NATO-00)
 * Output Format: "Alpha-92"
 */
void calculateChecksum(const char* code, char* outString) {
    int weightedSum = 0;
    int rollingVal = 0;
    int len = strlen(code);

    for (int i = 0; i < len; i++) {
        char c = code[i];
        int val = 0;
        if (c == 'U') val = 1;
        else if (c == 'D') val = 2;
        else if (c == 'L') val = 3;
        else if (c == 'R') val = 4;

        // Alpha-Tag Logic (Weighted Sum)
        weightedSum += val * (i + 1);

        // Numeric Logic (Rolling Hash)
        rollingVal = (rollingVal * 3 + val) % 100;
    }

    // Map to A-Z
    int alphaIndex = weightedSum % 26;
    char alphaChar = (char)('A' + alphaIndex);

    // Format string: "NATO-NUM"
    snprintf(outString, REWARD_CHECKSUM_LENGTH, "%s-%02d", getNatoWord(alphaChar), rollingVal);
}

/**
 * Fills buffers with a new random session code AND its checksum.
 * Ensures the checksum does NOT collide with any historical checksums.
 */
void generateUniqueSessionCode(char* codeBuffer, char* checksumBuffer) {
    const char chars[] = "UDLR";
    bool collision = true;

    while (collision) {
        // 1. Generate Candidate Code
        for (int i = 0; i < REWARD_CODE_LENGTH; ++i) {
            codeBuffer[i] = chars[esp_random() % 4];
        }
        codeBuffer[REWARD_CODE_LENGTH] = '\0';

        // 2. Calculate Candidate Checksum
        calculateChecksum(codeBuffer, checksumBuffer);

        // 3. Check for collisions against existing history
        // Note: rewardHistory[0] is the slot we are currently filling, so check 1..Size-1
        collision = false;
        for (int i = 1; i < REWARD_HISTORY_SIZE; i++) {
            // We compare the Checksum Strings (stored in the timestamp field)
            // If history entry is empty, skip it
            if (strlen(rewardHistory[i].checksum) > 0) {
                if (strncmp(checksumBuffer, rewardHistory[i].checksum, REWARD_CHECKSUM_LENGTH) == 0) {
                    collision = true; // Duplicate found!
                    break;
                }
            }
        }
    }
}

/**
 * Converts a SessionState enum to its string representation.
 */
const char* stateToString(SessionState s) {
    switch (s) {
        case READY: return "ready";
        case ARMED: return "armed";
        case LOCKED: return "locked";
        case ABORTED: return "aborted";
        case COMPLETED: return "completed";
        case TESTING: return "testing";
        default: return "unknown";
    }
}

/**
 * Sets the JLed pattern based on the current session state.
 * This is called every time the state changes.
 */
void setLedPattern(SessionState state) {
    char logBuf[50];
    snprintf(logBuf, sizeof(logBuf), "Setting LED pattern for: %s", stateToString(state));
    logMessage(logBuf);

    switch (state) {
        case READY:
            // Slow Breath - Waiting for session configuration
            statusLed.FadeOn(2000).FadeOff(2000).Forever();
            break;
        case ARMED:
            // Fast Blink (2Hz) - Waiting for trigger or counting down
            statusLed.Blink(250, 250).Forever();
            break;
        case LOCKED:
            // Solid On - Solid as a lock
            statusLed.On().Forever();
            break;
        case ABORTED:
            // Standard Blink (1Hz) - Steady "Penalty Wait" pacing
            statusLed.Blink(500, 500).Forever();
            break;
        case COMPLETED:
            // Slow Double-Blink notification - Ready for more?
            statusLed.Blink(200, 200).Repeat(2).DelayAfter(3000).Forever();
            break;
        case TESTING:
            // Medium Pulse - I'm alive and all is well
            statusLed.FadeOn(750).FadeOff(750).Forever();
            break;
        default:
            // Default to off
            statusLed.Off().Forever();
            break;
    }
}
