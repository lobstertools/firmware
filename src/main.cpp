/*
 * =================================================================
 * Project:   Lobster Lock - Self-Bondage Session Manager
 * File:      main.cpp
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Description:
 * Core firmware for the Lobster Lock ESP32 device. This code
 * manages all session state (e.g., READY, LOCKED, ABORTED),
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
// This firmware is built specifically for the diymore 2/4-Channel MOS Switch Module.
const int MOSFET_PINS_ARRAY[] = {16, 17};
// const int MOSFET_PINS_ARRAY[] = {16, 17, 18, 19};
std::vector<int> MOSFET_PINS;

// --- Button Configuration ---
#ifdef ONE_BUTTON_PIN
  OneButton button(ONE_BUTTON_PIN, true, true); // Pin, active low, internal pull-up
#endif

// --- Session Constants & NVS ---
#define REWARD_HISTORY_SIZE 10
#define SESSION_CODE_LENGTH 32
#define SESSION_TIMESTAMP_LENGTH 32
#define MAX_CHANNELS 4

// Struct for storing reward code history.
struct Reward {
  char code[SESSION_CODE_LENGTH + 1];
  char timestamp[SESSION_TIMESTAMP_LENGTH + 1];
};

// Main state machine enum.
enum SessionState : uint8_t { READY, COUNTDOWN, LOCKED, ABORTED, COMPLETED, TESTING };

// --- SYTEM PREFERENCES ---
struct SystemConfig {
    uint32_t abortDelaySeconds;
    uint32_t minLockMinutes;
    uint32_t maxLockMinutes;
    uint32_t minPenaltyMinutes;
    uint32_t maxPenaltyMinutes;
    uint32_t testModeDurationSeconds;
    uint32_t failsafeMaxLockSeconds;
    uint32_t keepAliveIntervalMs;
    uint32_t keepAliveMaxStrikes;
    uint32_t bootLoopThreshold;
    uint32_t stableBootTimeMs;
    uint32_t wifiMaxRetries;
};

// Default values (used if NVS is empty)
const SystemConfig DEFAULT_SETTINGS = {
    3,      // abortDelaySeconds
    15,     // minLockMinutes
    180,    // maxLockMinutes
    15,     // minPenaltyMinutes
    180,    // maxPenaltyMinutes
    120,    // testModeDurationSeconds
    14400,  // failsafeMaxLockSeconds (4 hours)
    10000,  // keepAliveIntervalMs
    4,      // keepAliveMaxStrikes
    5,      // bootLoopThreshold (Default)
    120000, // stableBootTimeMs (Default 2 Minutes)
    5       // wifiMaxRetries (Default)
};

SystemConfig g_systemConfig = DEFAULT_SETTINGS;

// --- NTP Configuration ---
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 0;
const int   daylightOffset_sec = 3600;
#define NTP_SYNC_TIMEOUT_MS 15000 

// --- Globals ---
AsyncWebServer server(80);
Ticker oneSecondMasterTicker;

// --- Synchronization ---
// Mutex to guard shared state between Async Web Task and Main Loop Task
SemaphoreHandle_t stateMutex = NULL;
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED; // Critical section for tick counter

SessionState currentState = READY;

// NVS (Preferences) objects
Preferences wifiPreferences;
Preferences sessionState;
Preferences bootPrefs; // For boot loop detection

#ifdef STATUS_LED_PIN
  jled::JLed statusLed = jled::JLed(STATUS_LED_PIN);
#endif

// Global array to hold reward history.
Reward rewardHistory[REWARD_HISTORY_SIZE];

// --- Global State & Timers ---
unsigned long lockSecondsRemaining = 0;
unsigned long penaltySecondsRemaining = 0;
unsigned long testSecondsRemaining = 0;
unsigned long penaltySecondsConfig = 0;
unsigned long lockSecondsConfig = 0;
bool hideTimer = false;

// Time Sync State
bool g_time_initialized = false;
unsigned long g_lastNtpRetry = 0;
const unsigned long NTP_RETRY_INTERVAL_MS = 60000; // Retry every 1 minute

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
uint16_t paybackTimeMinutes = 15;   // Default to 15min

// Persistent Session Counters (loaded from NVS)
uint32_t sessionStreakCount = 0;
uint32_t completedSessions = 0;
uint32_t abortedSessions = 0;
uint32_t paybackAccumulated = 0; // In seconds
uint32_t totalLockedSessionSeconds = 0; // Total accumulated lock time

// Vector holding countdowns for each channel.
std::vector<unsigned long> channelDelaysRemaining;

// --- Logging System ---
// Ring buffer for storing logs in memory.
const int LOG_BUFFER_SIZE = 50;
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
void generateSessionCode(char* buffer);
void initializeChannels();
void sendChannelOn(int channel);
void sendChannelOff(int channel);
void sendChannelOnAll();
void sendChannelOffAll();
void setLedPattern(SessionState state);
void sendJsonError(AsyncWebServerRequest *request, int code, const String& message);
#ifdef ONE_BUTTON_PIN
void handleLongPressStart();
#endif
bool checkKeepAliveWatchdog();
void armFailsafeTimer();
void disarmFailsafeTimer();

// Byte conversion helpers
uint16_t bytesToUint16(uint8_t* data);
uint32_t bytesToUint32(uint8_t* data);
void formatSeconds(unsigned long totalSeconds, char* buffer, size_t size);
const char* stateToString(SessionState state);
void getCurrentTimestamp(char* buffer, size_t bufferSize);


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
void handleStart(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total);
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
            uint16_t val = bytesToUint16(data);
            sessionState.putUShort("paybackTime", val);
            sessionState.end();
            logMessage("BLE: Received Payback Time");
        }
    }
};

/**
 * Starts the BLE provisioning service.
 * This function DOES NOT RETURN. It waits for credentials and reboots.
 */
void startBLEProvisioning() {
    logMessage("Starting BLE Provisioning Mode...");
    #ifdef STATUS_LED_PIN
      // Set a "pairing" pulse pattern
      statusLed.FadeOn(500).FadeOff(500).Forever();
    #endif

    // Initialize BLE
    BLEDevice::init(DEVICE_NAME);
    BLEServer *pServer = BLEDevice::createServer();
    BLEService *pService = pServer->createService(PROV_SERVICE_UUID);
    
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
        #ifdef STATUS_LED_PIN
          statusLed.Update(); // Keep the LED pattern running
        #endif
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
    // We iterate via raw array size to be safe in ISR context
    int numPins = sizeof(MOSFET_PINS_ARRAY) / sizeof(MOSFET_PINS_ARRAY[0]);
    for (int i = 0; i < numPins; i++) {
        digitalWrite(MOSFET_PINS_ARRAY[i], LOW);
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
  snprintf(logBuf, sizeof(logBuf), "Abort Delay: %lu s", g_systemConfig.abortDelaySeconds);
  logMessage(logBuf);
  snprintf(logBuf, sizeof(logBuf), "Lock Range: %lu-%lu min", g_systemConfig.minLockMinutes, g_systemConfig.maxLockMinutes);
  logMessage(logBuf);
  snprintf(logBuf, sizeof(logBuf), "Penalty Range: %lu-%lu min", g_systemConfig.minPenaltyMinutes, g_systemConfig.maxPenaltyMinutes);
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
  
  initializeChannels();

  #ifdef STATUS_LED_PIN
    logMessage("LED Status Indicator enabled");
    pinMode(STATUS_LED_PIN, OUTPUT);
    setLedPattern(currentState); // Set initial LED pattern
  #else
    logMessage("LED Status Indicator disabled");
  #endif

  #ifdef ONE_BUTTON_PIN
    char btnLog[50];
    snprintf(btnLog, sizeof(btnLog), "Abort Pedal (Pin %d) enabled", ONE_BUTTON_PIN);
    logMessage(btnLog);

    // Use configurable abort delay from NVS (g_systemConfig)
    snprintf(logBuf, sizeof(logBuf), "Long press (%lu sec) to abort session.", g_systemConfig.abortDelaySeconds);
    logMessage(logBuf);
    button.setLongPressIntervalMs(g_systemConfig.abortDelaySeconds * 1000);
    button.attachLongPressStart(handleLongPressStart);
  #else
    logMessage("Abort Pedal disabled");
  #endif

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

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

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

      // Wait up to 30 seconds specifically for the IP
      while (WiFi.status() != WL_CONNECTED && (millis() - wifiWaitStart < 30000)) {
          processLogQueue();    // Keep flushing logs so "Connected" appears instantly
          esp_task_wdt_reset(); // Feed the watchdog so we don't crash
          delay(100);
      }
      
      if (WiFi.status() != WL_CONNECTED) {
          logMessage("Boot: WiFi connection timed out. Skipping NTP.");
      }
  }

  // --- STAGE 2b: WAIT FOR TIME SYNC (BLOCKING BOOT) ---
  // We block here because the device is useless without correct time.
  // We only enter this if WiFi is actually connected.
  if (g_wifiCredentialsExist && WiFi.status() == WL_CONNECTED) {

      logMessage("Boot: Waiting for NTP time sync...");
      unsigned long waitStart = millis();
      bool timeSynced = false;

      // Wait up to 30 seconds for time
      // NOTE: We must feed WDT in this loop or it might trigger (20s timeout)
      while ((millis() - waitStart < 30000)) {
          
          processLogQueue(); // Flush logs so we see "Connected"

          // Only check NTP if we actually have a connection
          if (WiFi.status() == WL_CONNECTED) {
              struct tm timeinfo;
              // Check time manually here (configTime is async in background)
              if (getLocalTime(&timeinfo, 0)) {
                  g_time_initialized = true;
                  timeSynced = true;
                  break;
              }
          }
          
          esp_task_wdt_reset(); // Feed watchdog
          delay(100); // Yield to allow WiFi stack to run
      }

      if (timeSynced) {
          struct tm timeinfo;
          getLocalTime(&timeinfo);
          char timeBuf[64];
          strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &timeinfo);
          snprintf(logBuf, sizeof(logBuf), "NTP Sync Success: %s", timeBuf);
          logMessage(logBuf);
      } else {
          logMessage("NTP sync timed out. Proceeding with 1970 timestamp.");
      }
  }

  // --- STAGE 3: OPERATIONAL MODE ---

  logMessage("Initializing Session State from NVS...");  
  
  if (loadState()) {
      // 1. Log the raw state we just recovered from flash
      char rawBuf[64];
      snprintf(rawBuf, sizeof(rawBuf), "Raw NVS State: %s", stateToString(currentState));
      logMessage(rawBuf);

      // 2. Decide what to do (e.g., abort if we rebooted during COUNTDOWN)
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

  // If time is not set, and we are connected to WiFi, try again every 60s
  if (!g_time_initialized && WiFi.status() == WL_CONNECTED) {
      unsigned long now = millis();
      if (now - g_lastNtpRetry > NTP_RETRY_INTERVAL_MS) {
          g_lastNtpRetry = now;
          // Non-blocking attempt (configTime is async, but we check status)
          struct tm timeinfo;
          if (getLocalTime(&timeinfo, 10)) { // Quick 10ms check
              g_time_initialized = true;
              char logBuf[100];
              char* timeStr = asctime(&timeinfo);
              timeStr[strlen(timeStr)-1] = '\0'; // Remove trailing newline
              snprintf(logBuf, sizeof(logBuf), "NTP time recovered: %s", timeStr);
              
              if (xSemaphoreTakeRecursive(stateMutex, (TickType_t)100) == pdTRUE) {
                  logMessage(logBuf); 
                  xSemaphoreGiveRecursive(stateMutex); 
              }
          }
      }
  }

  // --- 2. JLed ---
  #ifdef STATUS_LED_PIN
    // JLed is not thread safe.
    if (xSemaphoreTakeRecursive(stateMutex, 0) == pdTRUE) {
      statusLed.Update(); // Update JLed state machine
      xSemaphoreGiveRecursive(stateMutex);
    }
  #endif

  // --- 3. Button ---
  #ifdef ONE_BUTTON_PIN
    button.tick(); // Poll the button
  #endif

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
        #ifdef STATUS_LED_PIN
            pinMode(STATUS_LED_PIN, OUTPUT);
            digitalWrite(STATUS_LED_PIN, HIGH); // Solid on for error
        #endif
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
 * This MUST be called before loadState() or resetToReady().
 */
void initializeChannels() {
  logMessage("Channel Module: diymore MOS module");

  MOSFET_PINS.reserve(MAX_CHANNELS);
  MOSFET_PINS.assign(MOSFET_PINS_ARRAY, MOSFET_PINS_ARRAY + (sizeof(MOSFET_PINS_ARRAY) / sizeof(MOSFET_PINS_ARRAY[0])));
  
  channelDelaysRemaining.reserve(MAX_CHANNELS);

  char logBuf[50];
  snprintf(logBuf, sizeof(logBuf), "Device has %d channel(s).", (int)MOSFET_PINS.size());
  logMessage(logBuf);

  for (int pin : MOSFET_PINS) {
      pinMode(pin, OUTPUT);
      digitalWrite(pin, LOW); // Default to off
      snprintf(logBuf, sizeof(logBuf), "Initialized GPIO %d (OFF)", pin);
      logMessage(logBuf);
  }
}

/**
 * Turns a specific Channel channel ON (closes circuit).
 */
void sendChannelOn(int channel) {
  if (channel < 0 || channel >= (int)MOSFET_PINS.size()) return;
  char logBuf[50];
  snprintf(logBuf, sizeof(logBuf), "Channel %d: ON ", channel);
  logMessage(logBuf);
  #ifndef DEBUG_MODE
    digitalWrite(MOSFET_PINS[channel], HIGH);
  #endif
}

/**
 * Turns a specific Channel channel OFF (opens circuit).
 */
void sendChannelOff(int channel) {
  if (channel < 0 || channel >= (int)MOSFET_PINS.size()) return;
  char logBuf[50];
  snprintf(logBuf, sizeof(logBuf), "Channel %d: OFF", channel);
  logMessage(logBuf);
  #ifndef DEBUG_MODE
    digitalWrite(MOSFET_PINS[channel], LOW);
  #endif
}

/**
 * Turns all Channel channels ON.
 */
void sendChannelOnAll() {
  logMessage("Channels: ON (All)");
  for (size_t i=0; i < MOSFET_PINS.size(); i++) { sendChannelOn(i); }
}

/**
 * Turns all Channel channels OFF.
 */
void sendChannelOffAll() {
  logMessage("Channels: OFF (All)");
  for (size_t i=0; i < MOSFET_PINS.size(); i++) { sendChannelOff(i); }
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
    #ifdef STATUS_LED_PIN
      setLedPattern(READY);
    #endif
    testSecondsRemaining = 0;
    g_lastKeepAliveTime = 0; // Disarm watchdog
    g_currentKeepAliveStrikes = 0; // Reset strikes
    saveState(true); // Force save
}

/**
 * Aborts an active session (LOCKED, COUNTDOWN, or TESTING).
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
        #ifdef STATUS_LED_PIN
          setLedPattern(ABORTED);
        #endif
        
        // Implement Abort Logic:
        sessionStreakCount = 0; // 1. Reset streak
        abortedSessions++;      // 2. Increment counter

        if (enablePaybackTime) { // 3. Add payback
            paybackAccumulated += (paybackTimeMinutes * 60);
            
            // Use helper to format payback time
            char timeStr[64];
            formatSeconds(paybackAccumulated, timeStr, sizeof(timeStr));
            
            char paybackLog[150];
            snprintf(paybackLog, sizeof(paybackLog), "Payback enabled. Added %u min. Total pending: %s",
                     paybackTimeMinutes, timeStr);
            logMessage(paybackLog);
        }

        lockSecondsRemaining = 0;
        penaltySecondsRemaining = penaltySecondsConfig; // 4. Start REWARD penalty
        startTimersForState(ABORTED);
        
        g_lastKeepAliveTime = 0; // Disarm watchdog
        g_currentKeepAliveStrikes = 0; // Reset strikes
        saveState(true); // Force save
    
    } else if (currentState == COUNTDOWN) {
        logMessage(logBuf);
        sendChannelOffAll();
        resetToReady(false); // Cancel countdown (this disarms watchdog)
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
 * Called when a session completes successfully (lock/penalty timer ends).
 * Increments counters, updates streak, and clears payback debt.
 */
void completeSession() {
  logMessage("Session complete. State is now COMPLETED.");
  if (currentState == LOCKED) disarmFailsafeTimer();
  
  sendChannelOffAll();
  currentState = COMPLETED;
  startTimersForState(COMPLETED); // Reset WDT
  #ifdef STATUS_LED_PIN
    setLedPattern(COMPLETED);
  #endif

  // --- THIS IS THE "COMMIT" STEP ---
  // On successful completion, clear any payback that was "paid"
  if (paybackAccumulated > 0) {
      logMessage("Session complete. Clearing accumulated payback debt.");
      paybackAccumulated = 0;
  }

  // Update session counters
  completedSessions++;
  if (enableStreaks) {
      sessionStreakCount++;
  }
  
  // Clear all timers
  lockSecondsRemaining = 0;
  penaltySecondsRemaining = 0;
  testSecondsRemaining = 0;
  g_lastKeepAliveTime = 0; // Disarm watchdog
  g_currentKeepAliveStrikes = 0; // Reset strikes
  
  channelDelaysRemaining.assign(MOSFET_PINS.size(), 0);
  
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

  #ifdef STATUS_LED_PIN
    setLedPattern(READY);
  #endif
  // Clear all timers and configs
  lockSecondsRemaining = 0;
  penaltySecondsRemaining = 0;
  testSecondsRemaining = 0;
  g_lastKeepAliveTime = 0; // Disarm watchdog
  g_currentKeepAliveStrikes = 0; // Reset strikes
  lockSecondsConfig = 0;
  hideTimer = false;
  
  channelDelaysRemaining.assign(MOSFET_PINS.size(), 0);

  // Only generate new code if requested.
  // If we abort a countdown, we generally want to keep the same code 
  // unless we specifically want to roll it.
  if (generateNewCode) {
      logMessage("Generating new reward code and updating history.");
      // Shift reward history
      for (int i = REWARD_HISTORY_SIZE - 1; i > 0; i--) {
          rewardHistory[i] = rewardHistory[i - 1];
      }
      // Generate new code and timestamp for the first entry
      generateSessionCode(rewardHistory[0].code);
      getCurrentTimestamp(rewardHistory[0].timestamp, SESSION_TIMESTAMP_LENGTH + 1);

      char logBuf[150];
      char codeSnippet[9];
      strncpy(codeSnippet, rewardHistory[0].code, 8);
      codeSnippet[8] = '\0';
      snprintf(logBuf, sizeof(logBuf), "New Code: %s... at %s", codeSnippet, rewardHistory[0].timestamp);
      logMessage(logBuf);
  } else {
      logMessage("Preserving existing reward code (Countdown Abort).");
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

  // API: Start a new lock session (POST /start)
  server.on("/start", HTTP_POST,
      [](AsyncWebServerRequest *request) { 
        // Empty onReq, processing happens in onBody
      },
      NULL, // onUpload
      handleStart // onBody
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
    html += "<li><b>POST /start</b> - Begin session (JSON body required).</li>";
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
 * Handler for POST /start (body)
 * Validates JSON, sets timers, and locks channels.
 */
void handleStart(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
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
        snprintf(logBuf, sizeof(logBuf), "Failed to parse /start JSON: %s", error.c_str());
        logMessage(logBuf);
        sendJsonError(request, 400, "Invalid JSON body.");
        return;
    }

    // Validate JSON body using camelCase keys
    if (!(*doc)["duration"].is<JsonInteger>() || !(*doc)["penaltyDuration"].is<JsonInteger>() || !(*doc)["delays"].is<JsonArray>()) {
        sendJsonError(request, 400, "Missing required fields: duration, penaltyDuration, delays.");
        return;
    }
    
    // Read session-specific data from the request
    unsigned long durationMinutes = (*doc)["duration"];
    int penaltyMin = (*doc)["penaltyDuration"];
    bool newHideTimer = (*doc)["hideTimer"] | false; // Default to false if not present
    JsonArray delays = (*doc)["delays"].as<JsonArray>();

    // Validate ranges using Provisioned Settings
    if (durationMinutes < g_systemConfig.minLockMinutes || durationMinutes > g_systemConfig.maxLockMinutes) {
        sendJsonError(request, 400, "Invalid duration."); return;
    }
    if (penaltyMin < g_systemConfig.minPenaltyMinutes || penaltyMin > g_systemConfig.maxPenaltyMinutes) {
        sendJsonError(request, 400, "Invalid penaltyDuration."); return;
    }
    if (delays.isNull()) {
        sendJsonError(request, 400, "Invalid 'delays' field. Must be an array."); return;
    }
    if (delays.size() != MOSFET_PINS.size()) {
        char errBuf[100];
        snprintf(errBuf, sizeof(errBuf), "Incorrect number of delays. Expected %d, got %d.", (int)MOSFET_PINS.size(), delays.size());
        sendJsonError(request, 400, errBuf);
        return;
    }

    unsigned long maxDelay = 0;
    std::vector<unsigned long> tempDelays; // Temporary storage
    tempDelays.reserve(MOSFET_PINS.size());
    
    // Store delays
    for(JsonVariant d : delays) {
        unsigned long delay = d.as<unsigned long>();
        tempDelays.push_back(delay);
        if (delay > maxDelay) maxDelay = delay;
    }

    // Clean up the input JSON document - automatic via RAII
    // doc will be deleted here or we can force it by reset() if we needed memory immediately.

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
        channelDelaysRemaining = tempDelays;
        hideTimer = newHideTimer;

        // Apply any pending payback time from previous sessions
        unsigned long paybackInSeconds = paybackAccumulated;
        if (paybackInSeconds > 0) {
            logMessage("Applying pending payback time to this session.");
        }

        // Save configs
        lockSecondsConfig = (durationMinutes * 60) + paybackInSeconds;
        penaltySecondsConfig = penaltyMin * 60;
        
        // Logging (Inside mutex for thread safety of buffer)
        char timeBuf[50];
        formatSeconds(paybackInSeconds, timeBuf, sizeof(timeBuf));
        char logBuf[256];
        snprintf(logBuf, sizeof(logBuf), "API: /start. Lock: %lu min (+%s payback). Hide: %s",
                durationMinutes, timeBuf, (hideTimer ? "Yes" : "No"));
        logMessage(logBuf);
        logMessage("Delays configured via API.");

        // Logic - Response Doc on Heap via RAII
        std::unique_ptr<JsonDocument> responseDoc(new JsonDocument());

        if (maxDelay == 0) {
            // No delays, start lock immediately
            logMessage("   -> No delay. Locking immediately.");
            currentState = LOCKED;
            #ifdef STATUS_LED_PIN
              setLedPattern(LOCKED);
            #endif
            lockSecondsRemaining = lockSecondsConfig;
            sendChannelOnAll();
            startTimersForState(LOCKED);
            armFailsafeTimer(); // DEATH GRIP
            g_lastKeepAliveTime = millis(); // Arm keep-alive watchdog
            g_currentKeepAliveStrikes = 0;  // Reset strikes
            (*responseDoc)["status"] = "locked";
            (*responseDoc)["durationSeconds"] = lockSecondsRemaining;
        } else {
            // Start countdown
            logMessage("   -> Starting countdown...");
            currentState = COUNTDOWN;
            #ifdef STATUS_LED_PIN
              setLedPattern(COUNTDOWN);
            #endif
            lockSecondsRemaining = 0;
            startTimersForState(COUNTDOWN);
            // NOTE: Watchdog is NOT armed here, only when LOCKED
            (*responseDoc)["status"] = "countdown";
        }
        
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
        #ifdef STATUS_LED_PIN
          setLedPattern(TESTING);
        #endif
        testSecondsRemaining = g_systemConfig.testModeDurationSeconds;
        startTimersForState(TESTING);
        g_lastKeepAliveTime = millis(); // Arm keep-alive watchdog for test mode
        g_currentKeepAliveStrikes = 0;  // Reset strikes
        saveState(true);
        
        std::unique_ptr<JsonDocument> doc(new JsonDocument());
        (*doc)["status"] = "testing";
        (*doc)["testTimeRemainingSeconds"] = testSecondsRemaining;
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
        if (currentState != LOCKED && currentState != COUNTDOWN && currentState != TESTING) {
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
        if (currentState == LOCKED || currentState == ABORTED || currentState == COUNTDOWN) {
            xSemaphoreGiveRecursive(stateMutex);
            sendJsonError(request, 403, "Reward is not yet available.");
        } else {
            // Send history (READY or COMPLETED)
            logMessage("API: /reward GET success. Releasing code history.");
            std::unique_ptr<JsonDocument> doc(new JsonDocument());
            JsonArray arr = (*doc).to<JsonArray>();
            for(int i = 0; i < REWARD_HISTORY_SIZE; i++) {
                if (strlen(rewardHistory[i].code) == SESSION_CODE_LENGTH) {
                    JsonObject reward = arr.add<JsonObject>();
                    reward["code"] = rewardHistory[i].code;
                    reward["timestamp"] = rewardHistory[i].timestamp;
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
    // 1. Create the Snapshot (Keep your existing struct logic)
    struct StateSnapshot {
        SessionState state;
        unsigned long lockRemain;
        unsigned long penaltyRemain;
        unsigned long testRemain;
        std::vector<unsigned long> delays;
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
        snapshot.lockRemain = lockSecondsRemaining;
        snapshot.penaltyRemain = penaltySecondsRemaining;
        snapshot.testRemain = testSecondsRemaining;
        snapshot.delays = channelDelaysRemaining; // Vector copy (allocates, but on stack/temp)
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

    // 2. Stream directly to TCP buffer (No String objects, No 'new' Heap allocs)
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    
    // Use Stack memory. ArduinoJson v7 calculates size automatically.
    // For this specific payload, ~512-1024 bytes is plenty.
    JsonDocument doc; 

    // Timers
    doc["status"] = stateToString(snapshot.state);
    doc["lockSecondsRemaining"] = snapshot.lockRemain;
    doc["penaltySecondsRemaining"] = snapshot.penaltyRemain;
    doc["testSecondsRemaining"] = snapshot.testRemain;
    
    JsonArray delays = doc["countdownSecondsRemaining"].to<JsonArray>();
    for(unsigned long d : snapshot.delays) {
        delays.add(d);
    }

    doc["hideTimer"] = snapshot.hideTimer;

    // Accumulated stats
    doc["streaks"] = snapshot.streaks;
    doc["abortedSessions"] = snapshot.aborted;
    doc["completedSessions"] = snapshot.completed;
    doc["totalLockedSessionSeconds"] = snapshot.totalLocked;
    doc["pendingPaybackSeconds"] = snapshot.payback;

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
        (*doc)["numberOfChannels"] = (int)MOSFET_PINS.size();
        (*doc)["address"] = WiFi.localIP().toString();
        
        // Device Configuration
        JsonObject config = (*doc)["config"].to<JsonObject>();
        config["abortDelaySeconds"] = g_systemConfig.abortDelaySeconds;
        config["enableStreaks"] = enableStreaks;
        config["enablePaybackTime"] = enablePaybackTime;
        config["paybackTimeMinutes"] = paybackTimeMinutes;
        
        // Add features array
        JsonArray features = (*doc)["features"].to<JsonArray>();
        #ifdef STATUS_LED_PIN
            features.add("LED_Indicator");
        #endif
        #ifdef ONE_BUTTON_PIN
            features.add("Abort_Pedal");
        #endif
        
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

        logMessage("API: /factory-reset received. Erasing all credentials and session data.");
        
        // Erase Wi-Fi
        wifiPreferences.begin("wifi-creds", false); // Open read/write
        wifiPreferences.clear();
        wifiPreferences.end(); // Commit changes

        logMessage("Wi-Fi credentials erased.");

        // Erase all session state and counters
        sessionState.begin("session", false);
        sessionState.clear();
        sessionState.end();
        logMessage("Session state and config erased.");
       
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

#ifdef ONE_BUTTON_PIN
/**
 * Called by OneButton when a long press starts.
 * This will trigger an abort/cancel of the current session.
 */
void handleLongPressStart() {
    if (xSemaphoreTakeRecursive(stateMutex, (TickType_t)pdMS_TO_TICKS(100)) == pdTRUE) {
        logMessage("Button: Long press detected. Aborting session.");
        abortSession("Button");
        xSemaphoreGiveRecursive(stateMutex);
    }
}
#endif

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

    if (state == COUNTDOWN) {
        logMessage("Starting countdown logic.");
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
    case COUNTDOWN:
    { 
        // Add scope for new variable
        bool allDelaysZero = true;

        // Decrement all active channel delays
        size_t count = std::min(MOSFET_PINS.size(), channelDelaysRemaining.size());        
        for (size_t i = 0; i < count; i++) {
            if (channelDelaysRemaining[i] > 0) {
                allDelaysZero = false;
                if (--channelDelaysRemaining[i] == 0) {
                    sendChannelOn(i); // Turn on Channel when its timer hits zero
                    char logBuf[100];
                    snprintf(logBuf, sizeof(logBuf), "Channel %d delay finished. Channel closed.", i);
                    logMessage(logBuf);
                }
            }
        }

        // If all delays are done, transition to LOCKED
        if (allDelaysZero) {
            logMessage("All channel delays finished. Transitioning to LOCKED state.");
            currentState = LOCKED;
            #ifdef STATUS_LED_PIN
              setLedPattern(LOCKED);
            #endif
            lockSecondsRemaining = lockSecondsConfig;
            startTimersForState(LOCKED);
            armFailsafeTimer(); // DEATH GRIP
            g_lastKeepAliveTime = millis(); // Arm keep-alive watchdog
            g_currentKeepAliveStrikes = 0;  // Reset strikes
            saveState(true); // Force save
        }
        break;
    }
    case LOCKED:
      // --- Keep-Alive Watchdog Check (3-Strike) ---
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
      // --- Keep-Alive Watchdog Check (3-Strike) ---
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
    lockSecondsRemaining = sessionState.getULong("lockRemain", 0);
    penaltySecondsRemaining = sessionState.getULong("penaltyRemain", 0);
    penaltySecondsConfig = sessionState.getULong("penaltyConfig", 0);
    lockSecondsConfig = sessionState.getULong("lockConfig", 0);
    testSecondsRemaining = sessionState.getULong("testRemain", 0);
    hideTimer = sessionState.getBool("hideTimer", false);

    // Load device configuration (Session Specific) - RENAMED KEYS
    enableStreaks = sessionState.getBool("enableStreaks", true);
    enablePaybackTime = sessionState.getBool("enablePayback", true);
    paybackTimeMinutes = sessionState.getUShort("paybackTime", 15);

    // Load persistent session counters
    sessionStreakCount = sessionState.getUInt("streak", 0);
    completedSessions = sessionState.getUInt("completed", 0);
    abortedSessions = sessionState.getUInt("aborted", 0);
    paybackAccumulated = sessionState.getUInt("paybackAccum", 0);
    totalLockedSessionSeconds = sessionState.getUInt("totalLocked", 0);

    // Load arrays (as binary blobs)
    // Important: Resize to current hardware config before loading
    channelDelaysRemaining.resize(MOSFET_PINS.size(), 0);
    sessionState.getBytes("delays", channelDelaysRemaining.data(), sizeof(unsigned long) * MOSFET_PINS.size());
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
        case COUNTDOWN:
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
    sessionState.putULong("lockRemain", lockSecondsRemaining);
    sessionState.putULong("penaltyRemain", penaltySecondsRemaining);
    sessionState.putULong("penaltyConfig", penaltySecondsConfig);
    sessionState.putULong("lockConfig", lockSecondsConfig);
    sessionState.putULong("testRemain", testSecondsRemaining);
    
    // Save device configuration
    sessionState.putBool("hideTimer", hideTimer);
    sessionState.putBool("enableStreaks", enableStreaks);
    sessionState.putBool("enablePayback", enablePaybackTime);
    sessionState.putUShort("paybackTime", paybackTimeMinutes);

    // Save persistent counters
    sessionState.putUInt("streak", sessionStreakCount);
    sessionState.putUInt("completed", completedSessions);
    sessionState.putUInt("aborted", abortedSessions);
    sessionState.putUInt("paybackAccum", paybackAccumulated);
    sessionState.putUInt("totalLocked", totalLockedSessionSeconds);

    // Save arrays as binary "blobs"
    // Save vectors based on current hardware size
    channelDelaysRemaining.resize(MOSFET_PINS.size(), 0); 
    sessionState.putBytes("delays", channelDelaysRemaining.data(), sizeof(unsigned long) * MOSFET_PINS.size());
    sessionState.putBytes("rewards", rewardHistory, sizeof(rewardHistory));
    
    // Save magic value
    sessionState.putULong("magic", MAGIC_VALUE);

    sessionState.end(); // This commits the changes
    
    esp_task_wdt_reset(); // And feed after
}

// =================================================================
// --- Network & Time Management ---
// =================================================================
// Note: WiFi reconnect logic is now handled via WiFiEvent and wifiReconnectTimer

/**
 * Fills a buffer with the current timestamp (ISO 8601 or uptime).
 */
void getCurrentTimestamp(char* buffer, size_t bufferSize) {
  if (g_time_initialized) {
    // Got NTP time, use ISO 8601 format
    struct tm timeinfo;
    if(!getLocalTime(&timeinfo)){
      // Fallback 1: NTP was initialized but failed to get time struct
      strncpy(buffer, "1970-01-01T00:00:00Z", bufferSize); 
      return;
    }
    strftime(buffer, bufferSize, "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
  } else {
    // Fallback 2: NTP never initialized (e.g., boot w/o Wi-Fi).
    // Instead of an unparsable uptime string "[+...]", send a
    // valid, parsable "null" date (the Unix epoch).
    strncpy(buffer, "1970-01-01T00:00:00Z", bufferSize);
  }
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
    char timestamp[SESSION_TIMESTAMP_LENGTH + 4]; // Room for "[+HH:MM:SS]: "
    getCurrentTimestamp(timestamp, sizeof(timestamp)); // Pass buffer to write into

    // Update RAM ring buffer (for API)
    // Use snprintf for safe, bounded string formatting
    snprintf(logBuffer[logBufferIndex], MAX_LOG_ENTRY_LENGTH, "%s: %s", timestamp, message);
    logBufferIndex++;
    if (logBufferIndex >= LOG_BUFFER_SIZE) {
        logBufferIndex = 0;
        logBufferFull = true;
    }

    // Push to Serial Queue
    // Calculate next head index
    int nextHead = (serialQueueHead + 1) % SERIAL_QUEUE_SIZE;
    
    if (nextHead != serialQueueTail) {
        // Buffer not full
        snprintf(serialLogQueue[serialQueueHead], MAX_LOG_ENTRY_LENGTH, "%s: %s", timestamp, message);
        serialQueueHead = nextHead;
    } else {
        // Buffer full - drop message or overwrite?
        // For safety, we drop message to avoid confusing the tail reader.
    }
    
    xSemaphoreGiveRecursive(stateMutex);
  } else {
    // Emergency fallback if locked out - do nothing to avoid crash
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
 * Fills a buffer with a new random session code.
 */
void generateSessionCode(char* buffer) {
    const char chars[] = "UDLR";
    for (int i = 0; i < SESSION_CODE_LENGTH; ++i) {
        buffer[i] = chars[esp_random() % 4];
    }
    buffer[SESSION_CODE_LENGTH] = '\0';
}

/**
 * Converts a SessionState enum to its string representation.
 */
const char* stateToString(SessionState s) {
    switch (s) {
        case READY: return "ready";
        case COUNTDOWN: return "countdown";
        case LOCKED: return "locked";
        case ABORTED: return "aborted";
        case COMPLETED: return "completed";
        case TESTING: return "testing";
        default: return "unknown";
    }
}

#ifdef STATUS_LED_PIN
/**
 * Sets the JLed pattern based on the current session state.
 * This is called every time the state changes.
 */
void setLedPattern(SessionState state) {
    char logBuf[50];
    snprintf(logBuf, sizeof(logBuf), "Setting LED pattern for %s", stateToString(state));
    logMessage(logBuf);

    switch (state) {
        case READY:
            // Slow Breath
            statusLed.FadeOn(2000).FadeOff(2000).Forever();
            break;
        case COUNTDOWN:
            // Fast Blink
            statusLed.Blink(250, 250).Forever();
            break;
        case LOCKED:
            // Solid On
            statusLed.On().Forever();
            break;
        case ABORTED:
            // SOS Signal
            statusLed.Blink(150, 150).Repeat(3).DelayAfter(1000).Forever();
            break;
        case COMPLETED:
            // Slow Double-Blink notification
            statusLed.Blink(200, 200).Repeat(2).DelayAfter(3000).Forever();
            break;
        case TESTING:
            // Medium Pulse
            statusLed.FadeOn(750).FadeOff(750).Forever();
            break;
        default:
            // Default to off
            statusLed.Off().Forever();
            break;
    }
}
#endif