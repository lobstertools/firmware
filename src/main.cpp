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
 *
 * This version includes a two-stage setup:
 * 1. BLE Provisioning (Stage 1): For "out-of-the-box" setup
 * to send Wi-Fi credentials and device config.
 * 2. mDNS Discovery (Stage 2): For "day-to-day" use,
 * announcing itself as 'lobster-lock.local' on the network.
 * =================================================================
 */

#include <Arduino.h>

// --- Platform-Specific Includes ---
#include <WiFi.h>

// --- Library Includes ---
#include <ESPAsyncWebServer.h>
#include <Ticker.h>
#include <vector>
#include <array>
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
#define WDT_TIMEOUT_SECONDS 60

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

#define MIN_LOCK_MINUTES 15
#define MAX_LOCK_MINUTES 180
#define MIN_PENALTY_MINUTES 15
#define MAX_PENALTY_MINUTES 180

#define TEST_MODE_DURATION_SECONDS 120 // 2 minutes

// --- NTP Configuration ---
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 0;
const int   daylightOffset_sec = 3600;
#define NTP_SYNC_TIMEOUT_MS 15000 

// --- Globals ---
AsyncWebServer server(80);
Ticker oneSecondMasterTicker;
SessionState currentState = READY;

// NVS (Preferences) objects
Preferences wifiPreferences;
Preferences sessionState;

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
int NUMBER_OF_CHANNELS = 0;
bool g_time_initialized = false;
volatile bool g_oneSecondTick = false; // Flag set by 1s ISR

// --- Keep-Alive Watchdog (LOCKED/TESTING state only) ---
// IMPLEMENTATION: 3-Strike System
const unsigned long KEEP_ALIVE_EXPECTED_INTERVAL_MS = 10000; // Expect call every 10s
const int KEEP_ALIVE_MAX_STRIKES = 3; // Abort after 3 missed calls (30s)

unsigned long g_lastKeepAliveTime = 0; // For watchdog. 0 = disarmed.
int g_currentKeepAliveStrikes = 0;     // Counter for missed calls

// Vector holding countdowns for each channel.
std::vector<unsigned long> channelDelaysRemaining;

// Global Config (loaded from NVS)
uint32_t abortDelaySeconds = 3;     // Default to 3s
bool countStreaks = true;           // Default to true
bool enableTimePayback = true;      // Default to true
uint16_t abortPaybackMinutes = 15;  // Default to 15min

// Persistent Session Counters (loaded from NVS)
uint32_t sessionStreakCount = 0;
uint32_t completedSessions = 0;
uint32_t abortedSessions = 0;
uint32_t paybackAccumulated = 0; // In seconds
uint32_t totalLockedSessionSeconds = 0; // Total accumulated lock time

// Ring buffer for storing logs in memory.
const int LOG_BUFFER_SIZE = 50;
const int MAX_LOG_ENTRY_LENGTH = 150;
char logBuffer[LOG_BUFFER_SIZE][MAX_LOG_ENTRY_LENGTH];
int logBufferIndex = 0;
bool logBufferFull = false;

// =================================================================
// --- Function Prototypes ---
// =================================================================

void logMessage(const char* message);
bool connectToWiFi(const char* ssid, const char* pass);
const char* stateToString(SessionState state);
void resetToReady();
void completeSession();
void stopTestMode();
void abortSession(const char* source);
void handleOneSecondTick();
void saveState();
bool loadState();
void handleRebootState();
void startTimersForState(SessionState state);
void generateSessionCode(char* buffer);
void initializeTime();
void getCurrentTimestamp(char* buffer, size_t bufferSize);
void initializeChannels();
void sendChannelOn(int channel);
void sendChannelOff(int channel);
void sendChannelOnAll();
void sendChannelOffAll();
void setLedPattern(SessionState state);
void sendJsonError(AsyncWebServerRequest *request, int code, const String& message);
void handleLongPressStart();
void startBLEProvisioning();
void startMDNS();
// Byte conversion helpers
uint16_t bytesToUint16(uint8_t* data);
uint32_t bytesToUint32(uint8_t* data);
// Watchdog helper
bool checkKeepAliveWatchdog();

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
#define PROV_SERVICE_UUID           "5a160000-8334-469b-a316-c340cf29188f"
#define PROV_SSID_CHAR_UUID         "5a160001-8334-469b-a316-c340cf29188f"
#define PROV_PASS_CHAR_UUID         "5a160002-8334-469b-a316-c340cf29188f"
#define PROV_ABORT_DELAY_CHAR_UUID  "5a160003-8334-469b-a316-c340cf29188f"
#define PROV_COUNT_STREAKS_CHAR_UUID "5a160004-8334-469b-a316-c340cf29188f"
#define PROV_ENABLE_PAYBACK_CHAR_UUID "5a160005-8334-469b-a316-c340cf29188f"
#define PROV_ABORT_PAYBACK_CHAR_UUID "5a160006-8334-469b-a316-c340cf29188f"

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
            // Data is a string
            std::string value(data, data + len);
            wifiPreferences.begin("wifi-creds", false);
            wifiPreferences.putString("ssid", value.c_str());
            wifiPreferences.end();
            logMessage("BLE: Received SSID");
        } else if (uuid == PROV_PASS_CHAR_UUID) {
            // Data is a string
            std::string value(data, data + len);
            wifiPreferences.begin("wifi-creds", false);
            wifiPreferences.putString("pass", value.c_str());
            wifiPreferences.end();
            logMessage("BLE: Received Password");
            g_credentialsReceived = true; // Flag to restart
        } 
        
        // Handle Configuration Data (written to "session" NVS)
        else {
            sessionState.begin("session", false);

            if (uuid == PROV_ABORT_DELAY_CHAR_UUID) {
                // Expects 4-byte (uint32_t) LE
                uint32_t val = bytesToUint32(data);
                sessionState.putUInt("abortDelay", val);
                logMessage("BLE: Received Abort Delay");
            } else if (uuid == PROV_COUNT_STREAKS_CHAR_UUID) {
                // Expects 1-byte (bool)
                bool val = (bool)data[0];
                sessionState.putBool("countStreaks", val);
                logMessage("BLE: Received Count Streaks");
            } else if (uuid == PROV_ENABLE_PAYBACK_CHAR_UUID) {
                // Expects 1-byte (bool)
                bool val = (bool)data[0];
                sessionState.putBool("enablePayback", val);
                logMessage("BLE: Received Enable Payback");
            } else if (uuid == PROV_ABORT_PAYBACK_CHAR_UUID) {
                // Expects 2-byte (uint16_t) LE
                uint16_t val = bytesToUint16(data);
                sessionState.putUShort("abortPayback", val);
                logMessage("BLE: Received Abort Payback");
            }

            sessionState.end();
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
    BLEDevice::init(DEVICE_NAME); // Advertise with your device ID
    BLEServer *pServer = BLEDevice::createServer();
    BLEService *pService = pServer->createService(PROV_SERVICE_UUID);

    // Create all characteristics for the provisioning service
    ProvisioningCallbacks* callbacks = new ProvisioningCallbacks();

    // Create SSID Characteristic
    BLECharacteristic *pSsidChar = pService->createCharacteristic(
                                         PROV_SSID_CHAR_UUID,
                                         BLECharacteristic::PROPERTY_WRITE
                                       );
    pSsidChar->setCallbacks(callbacks);

    // Create Password Characteristic
    BLECharacteristic *pPassChar = pService->createCharacteristic(
                                         PROV_PASS_CHAR_UUID,
                                         BLECharacteristic::PROPERTY_WRITE
                                       );
    pPassChar->setCallbacks(callbacks);

    // Create Config Characteristics
    BLECharacteristic *pAbortDelayChar = pService->createCharacteristic(
                                         PROV_ABORT_DELAY_CHAR_UUID,
                                         BLECharacteristic::PROPERTY_WRITE
                                       );
    pAbortDelayChar->setCallbacks(callbacks);

    BLECharacteristic *pCountStreaksChar = pService->createCharacteristic(
                                         PROV_COUNT_STREAKS_CHAR_UUID,
                                         BLECharacteristic::PROPERTY_WRITE
                                       );
    pCountStreaksChar->setCallbacks(callbacks);

    BLECharacteristic *pEnablePaybackChar = pService->createCharacteristic(
                                         PROV_ENABLE_PAYBACK_CHAR_UUID,
                                         BLECharacteristic::PROPERTY_WRITE
                                       );
    pEnablePaybackChar->setCallbacks(callbacks);

    BLECharacteristic *pAbortPaybackChar = pService->createCharacteristic(
                                         PROV_ABORT_PAYBACK_CHAR_UUID,
                                         BLECharacteristic::PROPERTY_WRITE
                                       );
    pAbortPaybackChar->setCallbacks(callbacks);


    pService->start();

    // Start advertising
    BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(PROV_SERVICE_UUID);
    pAdvertising->start();

    // --- Wait here forever until credentials are set ---
    while (1) {
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
// --- Core Application Setup & Loop ---
// =================================================================

/**
 * Main Arduino setup function. Runs once on boot.
 */
void setup() {
  
  Serial.begin(SERIAL_BAUD_RATE);
  delay(3000);

  // --- Basic hardware init ---
  randomSeed(esp_random());

  char logBuf[100];
  snprintf(logBuf, sizeof(logBuf), "%s starting up...", DEVICE_NAME);
  logMessage(logBuf);
  snprintf(logBuf, sizeof(logBuf), "Firmware Version: %s", DEVICE_VERSION);
  logMessage(logBuf);

  logMessage("Initializing hardware watchdog...");
  esp_task_wdt_init(WDT_TIMEOUT_SECONDS, true);
  esp_task_wdt_add(NULL);

  logMessage("--- Device Features ---"); 

  initializeChannels();

  snprintf(logBuf, sizeof(logBuf), "Device has %d channel(s).", NUMBER_OF_CHANNELS);
  logMessage(logBuf);

  #ifdef STATUS_LED_PIN
    logMessage("[Enabled] LED Status Indicator");
    pinMode(STATUS_LED_PIN, OUTPUT);
    setLedPattern(currentState); // Set initial LED pattern
  #else
    logMessage("[Disabled] LED Status Indicator");
  #endif

  #ifdef ONE_BUTTON_PIN
    char btnLog[50];
    snprintf(btnLog, sizeof(btnLog), "[Enabled] Abort Pedal (Pin %d)", ONE_BUTTON_PIN);
    logMessage(btnLog);

    // Use configurable abort delay from NVS
    snprintf(logBuf, sizeof(logBuf), "Long press (%lu sec) to abort session.", abortDelaySeconds);
    logMessage(logBuf);
    button.setLongPressIntervalMs(abortDelaySeconds * 1000);
    button.attachLongPressStart(handleLongPressStart);
  #else
    logMessage("[Disabled] Abort Pedal");
  #endif

  logMessage("--- /Device Features ---"); 

  // --- Wi-Fi Connection Attempt ---
  wifiPreferences.begin("wifi-creds", true); // Open NVS (read-only)
  String ssid = wifiPreferences.getString("ssid", "");
  String pass = wifiPreferences.getString("pass", "");
  wifiPreferences.end();

  bool wifiConnected = false;
  if (ssid.length() > 0) {
      logMessage("Found Wi-Fi credentials in NVS.");
      wifiConnected = connectToWiFi(ssid.c_str(), pass.c_str());
  } 

  if (!wifiConnected) {
      // --- STAGE 1: PROVISIONING ---
      if (ssid.length() > 0) {
          logMessage("Wi-Fi connection failed. Credentials may be bad.");
          // Erase the bad credentials
          wifiPreferences.begin("wifi-creds", false);
          wifiPreferences.clear();
          wifiPreferences.end();
          logMessage("Erased bad Wi-Fi credentials.");
      } else {
          logMessage("No Wi-Fi credentials found.");
      }

      // Start provisioning. This function blocks forever and
      // will reboot the device when done.
      startBLEProvisioning();
  }

  // Start network services
  initializeTime();
  startMDNS();

  // --- STAGE 2: OPERATIONAL MODE ---

  // --- Load session state from NVS ---
  logMessage("Initializing Session State from NVS...");  
  
  if (loadState()) {
      // A valid state was loaded from NVS.
      // Decide what to do based on the state we were in when rebooted.
      handleRebootState();
  } else {
      // No valid data in NVS. Initialize a fresh state.
      logMessage("No valid session data in NVS. Initializing fresh state.");
      resetToReady(); // This saves the new, fresh state
  }

  // --- Start web server and timers ---
  logMessage("Attaching master 1-second ticker.");
  oneSecondMasterTicker.attach(1, [](){ g_oneSecondTick = true; });

  setupWebServer();
  server.begin();  
  logMessage("HTTP server started. Device is operational.");
}

/**
 * Main Arduino loop function. Runs continuously.
 */
void loop() {
  esp_task_wdt_reset(); // Feed the watchdog

  #ifdef STATUS_LED_PIN
    statusLed.Update(); // Update JLed state machine
  #endif

  #ifdef ONE_BUTTON_PIN
    button.tick(); // Poll the button
  #endif

  // Check the volatile flag set by the 1s Ticker (ISR)
  if (g_oneSecondTick) {
    g_oneSecondTick = false;
    handleOneSecondTick();   // Handle all timer logic
  }
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
  MOSFET_PINS.assign(MOSFET_PINS_ARRAY, MOSFET_PINS_ARRAY + (sizeof(MOSFET_PINS_ARRAY) / sizeof(MOSFET_PINS_ARRAY[0])));
  NUMBER_OF_CHANNELS = MOSFET_PINS.size();

  char logBuf[50];
  for (int pin : MOSFET_PINS) {
      pinMode(pin, OUTPUT);
      digitalWrite(pin, LOW); // Default to off
      snprintf(logBuf, sizeof(logBuf), "Initialized GPIO %d", pin);
      logMessage(logBuf);
  }
}

/**
 * Turns a specific Channel channel ON (closes circuit).
 */
void sendChannelOn(int channel) {
  if (channel < 0 || channel >= NUMBER_OF_CHANNELS) return;
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
  if (channel < 0 || channel >= NUMBER_OF_CHANNELS) return;
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
  for (int i=0; i < NUMBER_OF_CHANNELS; i++) { sendChannelOn(i); }
}

/**
 * Turns all Channel channels OFF.
 */
void sendChannelOffAll() {
  logMessage("Channels: OFF (All)");
  for (int i=0; i < NUMBER_OF_CHANNELS; i++) { sendChannelOff(i); }
}

// =================================================================
// --- Session State Management ---
// =================================================================

/**
 * Stops the test mode and returns to READY.
 */
void stopTestMode() {
    logMessage("Stopping test mode.");
    sendChannelOffAll();
    currentState = READY;
    #ifdef STATUS_LED_PIN
      setLedPattern(READY);
    #endif
    testSecondsRemaining = 0;
    g_lastKeepAliveTime = 0; // Disarm watchdog
    g_currentKeepAliveStrikes = 0; // Reset strikes
    saveState();
}

/**
 * Aborts an active session (LOCKED, COUNTDOWN, or TESTING).
 * Implements payback logic, resets streak, and starts the reward penalty timer.
 */
void abortSession(const char* source) {
    char logBuf[100];
    snprintf(logBuf, sizeof(logBuf), "%s: Aborting session (%s).", source, stateToString(currentState));
    
    if (currentState == LOCKED) {
        logMessage(logBuf);
        sendChannelOffAll();
        currentState = ABORTED;
        #ifdef STATUS_LED_PIN
          setLedPattern(ABORTED);
        #endif
        
        // Implement Abort Logic:
        sessionStreakCount = 0; // 1. Reset streak
        abortedSessions++;      // 2. Increment counter

        if (enableTimePayback) { // 3. Add payback
            paybackAccumulated += (abortPaybackMinutes * 60);
            uint32_t totalSeconds = paybackAccumulated;
            uint32_t hours = totalSeconds / 3600;
            uint32_t minutes = (totalSeconds % 3600) / 60;
            uint32_t seconds = totalSeconds % 60;
            
            char paybackLog[100];
            snprintf(paybackLog, sizeof(paybackLog), "Payback enabled. Added %u min. Total pending: %lu h, %lu min, %lu s",
                     abortPaybackMinutes, hours, minutes, seconds);
            logMessage(paybackLog);
        }

        lockSecondsRemaining = 0;
        penaltySecondsRemaining = penaltySecondsConfig; // 4. Start REWARD penalty
        startTimersForState(ABORTED);
        
        g_lastKeepAliveTime = 0; // Disarm watchdog
        g_currentKeepAliveStrikes = 0; // Reset strikes
        saveState();
    
    } else if (currentState == COUNTDOWN) {
        logMessage(logBuf);
        sendChannelOffAll();
        resetToReady(); // Cancel countdown (this disarms watchdog)
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
  sendChannelOffAll();
  currentState = COMPLETED;
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
  if (countStreaks) {
      sessionStreakCount++;
  }
  
  // Clear all timers
  lockSecondsRemaining = 0;
  penaltySecondsRemaining = 0;
  testSecondsRemaining = 0;
  g_lastKeepAliveTime = 0; // Disarm watchdog
  g_currentKeepAliveStrikes = 0; // Reset strikes
  
  channelDelaysRemaining.assign(NUMBER_OF_CHANNELS, 0);
  
  saveState(); 
}

/**
 * Resets the device state to READY, generates a new reward code.
 * Does NOT reset counters or payback.
 */
void resetToReady() {
  logMessage("Resetting state to READY.");
  sendChannelOffAll();

  currentState = READY;
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
  
  channelDelaysRemaining.assign(NUMBER_OF_CHANNELS, 0);

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

  saveState(); 
}

// =================================================================
// --- Web Server & API Endpoints ---
// =================================================================

/**
 * Helper function to send a standardized JSON error response.
 */
void sendJsonError(AsyncWebServerRequest *request, int code, const String& message) {
    JsonDocument doc;
    doc["status"] = "error";
    doc["message"] = message;
    String response;
    serializeJson(doc, response);
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
      [](AsyncWebServerRequest *request) { // onReq
          if (request->contentType() != "application/json") {
              sendJsonError(request, 400, "Invalid Content-Type. Expected application/json");
              return;
          }
          if (currentState != READY) {
              sendJsonError(request, 409, "Device is not ready.");
              return;
          }
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
      [](AsyncWebServerRequest *request) { // onReq
          if (request->contentType() != "application/json") {
              sendJsonError(request, 400, "Invalid Content-Type. Expected application/json");
              return;
          }
          if (currentState != READY) {
              logMessage("API: /update-wifi failed. Device is not in READY state.");
              sendJsonError(request, 409, "Device must be in READY state to update Wi-Fi.");
              return;
          }
      },
      NULL, // onUpload
      handleUpdateWifi // onBody
  );

  // API: Factory reset (POST /factory-reset)
  server.on("/factory-reset", HTTP_POST, handleFactoryReset);
}

// =================================================================
// --- Web Server Handlers ---
// =================================================================

/**
 * Handler for GET /
 */
void handleRoot(AsyncWebServerRequest *request) {
    request->send(200, "text/plain", String(DEVICE_NAME));
}

/**
 * Handler for GET /health
 */
void handleHealth(AsyncWebServerRequest *request) {
    JsonDocument doc;
    doc["status"] = "ok";
    doc["message"] = "Device is reachable.";
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

/**
 * Handler for POST /keepalive
 */
void handleKeepAlive(AsyncWebServerRequest *request) {
    // "Pet" the watchdog only if the session is in the LOCKED state
    if (currentState == LOCKED || currentState == TESTING) {
        g_lastKeepAliveTime = millis();
        g_currentKeepAliveStrikes = 0; // Reset strike counter
    }
    request->send(200); 
}

/**
 * Handler for POST /start (body)
 */
void handleStart(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
    // Handle JSON body
    if (index + len != total) {
        return; // Wait for more data
    }
    
    // State is already checked in onReq, but as a safeguard:
    if (currentState != READY) {
        sendJsonError(request, 409, "Device is not ready.");
        return;
    }
    
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, (const char*)data, len);
    if (error) {
        char logBuf[100];
        snprintf(logBuf, sizeof(logBuf), "Failed to parse /start JSON: %s", error.c_str());
        logMessage(logBuf);
        sendJsonError(request, 400, "Invalid JSON body.");
        return;
    }
    // Validate JSON body using camelCase keys
    if (!doc["duration"].is<JsonInteger>() || !doc["penaltyDuration"].is<JsonInteger>() || !doc["delays"].is<JsonArray>()) {
          sendJsonError(request, 400, "Missing required fields: duration, penaltyDuration, delays.");
        return;
    }
    
    // Read session-specific data from the request
    unsigned long durationMinutes = doc["duration"];
    int penaltyMin = doc["penaltyDuration"];
    hideTimer = doc["hideTimer"] | false; // Default to false if not present
    
    // Validate ranges
    if (durationMinutes < MIN_LOCK_MINUTES || durationMinutes > MAX_LOCK_MINUTES) {
        sendJsonError(request, 400, "Invalid duration."); return;
    }
    if (penaltyMin < MIN_PENALTY_MINUTES || penaltyMin > MAX_PENALTY_MINUTES) {
        sendJsonError(request, 400, "Invalid penaltyDuration."); return;
    }
    JsonArray delays = doc["delays"].as<JsonArray>();
    if (delays.isNull()) {
        sendJsonError(request, 400, "Invalid 'delays' field. Must be an array."); return;
    }
    if (delays.size() != NUMBER_OF_CHANNELS) {
        char errBuf[100];
        snprintf(errBuf, sizeof(errBuf), "Incorrect number of delays. Expected %d, got %d.", NUMBER_OF_CHANNELS, delays.size());
        sendJsonError(request, 400, errBuf);
        return;
    }

    // Apply any pending payback time from previous sessions
    unsigned long paybackInSeconds = paybackAccumulated;
    if (paybackInSeconds > 0) {
        logMessage("Applying pending payback time to this session.");
    }

    // Save configs
    lockSecondsConfig = (durationMinutes * 60) + paybackInSeconds;
    penaltySecondsConfig = penaltyMin * 60;
    unsigned long maxDelay = 0;
    
    // Log and set delays
    char delayLog[MAX_CHANNELS * 7 + 3];
    char tempNum[8];
    strcpy(delayLog, "[");
    int i = 0;
    for(JsonVariant d : delays) {
        unsigned long delay = d.as<unsigned long>();
        channelDelaysRemaining[i] = delay;
        if (delay > maxDelay) maxDelay = delay;
        snprintf(tempNum, sizeof(tempNum), "%lu", delay);
        strcat(delayLog, tempNum);
        if (i < NUMBER_OF_CHANNELS - 1) {
            strcat(delayLog, ", ");
        }
        i++;
    }
    strcat(delayLog, "]");
    
    char logBuf[200];
    snprintf(logBuf, sizeof(logBuf), "API: /start. Lock: %lu min (+%lu s payback). Penalty: %d min. Hide: %s",
            durationMinutes, paybackInSeconds, penaltyMin, (hideTimer ? "Yes" : "No"));
    logMessage(logBuf);
    snprintf(logBuf, sizeof(logBuf), "   -> Delays (s): %s. Max Delay: %lu s.", delayLog, maxDelay);
    logMessage(logBuf);
    
    JsonDocument responseDoc;
    String response;
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
        g_lastKeepAliveTime = millis(); // Arm keep-alive watchdog
        g_currentKeepAliveStrikes = 0;  // Reset strikes
        saveState();
        responseDoc["status"] = "locked";
        responseDoc["durationSeconds"] = lockSecondsRemaining;
        serializeJson(responseDoc, response);
        request->send(200, "application/json", response);
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
        saveState();
        responseDoc["status"] = "countdown";
        serializeJson(responseDoc, response);
        request->send(200, "application/json", response);
    }
}

/**
 * Handler for POST /start-test
 */
void handleStartTest(AsyncWebServerRequest *request) {
    if (currentState != READY) {
      sendJsonError(request, 409, "Device must be in READY state to run test.");
      return;
    }
    logMessage("API: /start-test received. Engaging Channels for 2 min.");
    sendChannelOnAll();
    currentState = TESTING;
    #ifdef STATUS_LED_PIN
      setLedPattern(TESTING);
    #endif
    testSecondsRemaining = TEST_MODE_DURATION_SECONDS;
    startTimersForState(TESTING);
    g_lastKeepAliveTime = millis(); // Arm keep-alive watchdog for test mode
    g_currentKeepAliveStrikes = 0;  // Reset strikes
    saveState();
    
    JsonDocument doc;
    doc["status"] = "testing";
    doc["testTimeRemainingSeconds"] = testSecondsRemaining;
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

/**
 * Handler for POST /abort
 */
void handleAbort(AsyncWebServerRequest *request) {
    if (currentState != LOCKED && currentState != COUNTDOWN && currentState != TESTING) {
      sendJsonError(request, 409, "Device is not in an abortable state."); return;
    }
    String statusMsg = "ready";
    if (currentState == LOCKED) statusMsg = "aborted"; // Aborting a lock goes to penalty
    
    abortSession("API"); // This handles all logic
    
    // Send response
    JsonDocument doc;
    doc["status"] = statusMsg;
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

/**
 * Handler for GET /reward
 */
void handleReward(AsyncWebServerRequest *request) {
    if (currentState == LOCKED || currentState == ABORTED || currentState == COUNTDOWN) {
        sendJsonError(request, 403, "Reward is not yet available.");
    } else {
        // Send history (READY or COMPLETED)
        logMessage("API: /reward GET success. Releasing code history.");
        JsonDocument doc;
        JsonArray arr = doc.to<JsonArray>();
        for(int i = 0; i < REWARD_HISTORY_SIZE; i++) {
            if (strlen(rewardHistory[i].code) == SESSION_CODE_LENGTH) {
                JsonObject reward = arr.add<JsonObject>();
                reward["code"] = rewardHistory[i].code;
                reward["timestamp"] = rewardHistory[i].timestamp;
            }
        }
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    }
}

/**
 * Handler for GET /status
 */
void handleStatus(AsyncWebServerRequest *request) {
    // Build the JSON status document
    JsonDocument doc;
    doc["status"] = stateToString(currentState);

    // Timers
    doc["lockSecondsRemaining"] = lockSecondsRemaining;
    doc["penaltySecondsRemaining"] = penaltySecondsRemaining;
    doc["testSecondsRemaining"] = testSecondsRemaining;
    
    JsonArray delays = doc["countdownSecondsRemaining"].to<JsonArray>();
    for(int i = 0; i < NUMBER_OF_CHANNELS; i++) {
        delays.add(channelDelaysRemaining[i]);
    }

    doc["hideTimer"] = hideTimer;

    // Accumulated stats
    doc["streaks"] = sessionStreakCount;
    doc["abortedSessions"] = abortedSessions;
    doc["completedSessions"] = completedSessions;
    doc["totalLockedSessionSeconds"] = totalLockedSessionSeconds;
    doc["pendingPaybackSeconds"] = paybackAccumulated;
    
    // Send response
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

/**
 * Handler for GET /details
 */
void handleDetails(AsyncWebServerRequest *request) {
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    char uniqueHostname[20];
    snprintf(uniqueHostname, sizeof(uniqueHostname), "lobster-lock-%02X%02X%02X", mac[3], mac[4], mac[5]);

    JsonDocument doc;
    doc["name"] = DEVICE_NAME;
    doc["id"] = uniqueHostname;
    doc["version"] = DEVICE_VERSION;
    doc["numberOfChannels"] = NUMBER_OF_CHANNELS;
    doc["address"] = WiFi.localIP().toString();
    
    // Device Configuration
    JsonObject config = doc["config"].to<JsonObject>();
    config["abortDelaySeconds"] = abortDelaySeconds;
    config["countStreaks"] = countStreaks;
    config["enableTimePayback"] = enableTimePayback;
    config["abortPaybackMinutes"] = abortPaybackMinutes;
    
    // Add features array
    JsonArray features = doc["features"].to<JsonArray>();
    #ifdef STATUS_LED_PIN
        features.add("LED_Indicator");
    #endif
    #ifdef ONE_BUTTON_PIN
        features.add("Abort_Pedal");
    #endif
    
    // Send response
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

/**
 * Handler for GET /log
 */
void handleLog(AsyncWebServerRequest *request) {
    AsyncResponseStream *response = request->beginResponseStream("text/plain");
    int start = 0;
    int count = 0;
    // Handle ring buffer logic
    if (logBufferFull) {
        start = logBufferIndex; // Start at the oldest entry
        count = LOG_BUFFER_SIZE;
    } else {
        start = 0;
        count = logBufferIndex; // Only read up to where we've written
    }
    // Print logs line by line
    for (int i = 0; i < count; i++) {
        int index = (start + i) % LOG_BUFFER_SIZE;
        response->print(logBuffer[index]);
        response->print("\r\n");
    }
    request->send(response);
}

/**
 * Handler for POST /update-wifi (body)
 */
void handleUpdateWifi(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
    // Handle JSON body
    if (index + len != total) {
        return; // Wait for more data
    }
    
    // State is already checked in the on() handler, but as a safeguard:
    if (currentState != READY) {
        sendJsonError(request, 409, "Device must be in READY state to update Wi-Fi.");
        return;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, (const char*)data, len);
    if (error) {
        char logBuf[100];
        snprintf(logBuf, sizeof(logBuf), "Failed to parse /update-wifi JSON: %s", error.c_str());
        logMessage(logBuf);
        sendJsonError(request, 400, "Invalid JSON body.");
        return;
    }

    // Validate JSON body
    if (!doc["ssid"].is<const char*>() || !doc["pass"].is<const char*>()) {
        sendJsonError(request, 400, "Missing required fields: ssid, pass.");
        return;
    }

    const char* ssid = doc["ssid"];
    const char* pass = doc["pass"];

    logMessage("API: /update-wifi received. Saving new credentials to NVS.");

    // Save new credentials to NVS
    wifiPreferences.begin("wifi-creds", false); // Open read/write
    wifiPreferences.putString("ssid", ssid);
    wifiPreferences.putString("pass", pass);
    wifiPreferences.end(); // Commit changes

    logMessage("New Wi-Fi credentials saved.");

    // Send response
    JsonDocument responseDoc;
    String response;
    responseDoc["status"] = "success";
    responseDoc["message"] = "Wi-Fi credentials updated. Please reboot the device to apply.";
    serializeJson(responseDoc, response);
    request->send(200, "application/json", response);
}

/**
 * Handler for POST /factory-reset
 */
void handleFactoryReset(AsyncWebServerRequest *request) {
    // Do not allow forgetting during an active session
    if (currentState != READY && currentState != COMPLETED) {
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


    // Send the response *before* we restart
    JsonDocument doc;
    doc["status"] = "resetting";
    doc["message"] = "Device credentials and state erased. Rebooting into provisioning mode.";
    String response;
    serializeJson(doc, response);
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
    logMessage("Button: Long press detected. Aborting session.");
    abortSession("Button");
}
#endif

// =================================================================
// --- Timer Callbacks ---
// =================================================================

/**
 * Resets timers when entering a new state.
 */
void startTimersForState(SessionState state) {
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
    int calculatedStrikes = elapsed / KEEP_ALIVE_EXPECTED_INTERVAL_MS;

    // Only log/act if the strike count has increased
    if (calculatedStrikes > g_currentKeepAliveStrikes) {
        g_currentKeepAliveStrikes = calculatedStrikes;
        char logBuf[100];
        
        if (g_currentKeepAliveStrikes >= KEEP_ALIVE_MAX_STRIKES) {
            snprintf(logBuf, sizeof(logBuf), "Keep-Alive Watchdog: Strike %d/%d! ABORTING.", g_currentKeepAliveStrikes, KEEP_ALIVE_MAX_STRIKES);
            logMessage(logBuf);
            abortSession("Watchdog Strikeout");
            return true; // Signal that we aborted
        } else {
            snprintf(logBuf, sizeof(logBuf), "Keep-Alive Watchdog: Missed check. Strike %d/%d", g_currentKeepAliveStrikes, KEEP_ALIVE_MAX_STRIKES);
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
    { // Add scope for new variable
        bool allDelaysZero = true;
        // Decrement all active channel delays
        for (int i = 0; i < NUMBER_OF_CHANNELS; i++) {
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
            g_lastKeepAliveTime = millis(); // Arm keep-alive watchdog
            g_currentKeepAliveStrikes = 0;  // Reset strikes
            saveState(); // Save state on transition
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
        logMessage("Test mode (2 min) timer expired.");
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
    
    // Check magic value first. If it's not present or correct,
    // we assume the rest of the data is invalid.
    unsigned long magic = sessionState.getULong("magic", 0);
    
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

    // Load device configuration
    abortDelaySeconds = sessionState.getUInt("abortDelay", 3);
    countStreaks = sessionState.getBool("countStreaks", true);
    enableTimePayback = sessionState.getBool("enablePayback", true);
    abortPaybackMinutes = sessionState.getUShort("abortPayback", 15);

    // Load persistent session counters
    sessionStreakCount = sessionState.getUInt("streak", 0);
    completedSessions = sessionState.getUInt("completed", 0);
    abortedSessions = sessionState.getUInt("aborted", 0);
    paybackAccumulated = sessionState.getUInt("paybackAccum", 0);
    totalLockedSessionSeconds = sessionState.getUInt("totalLocked", 0);

    // Load arrays (as binary blobs)
    channelDelaysRemaining.resize(NUMBER_OF_CHANNELS, 0);
    sessionState.getBytes("delays", channelDelaysRemaining.data(), sizeof(unsigned long) * NUMBER_OF_CHANNELS);
    sessionState.getBytes("rewards", rewardHistory, sizeof(rewardHistory));

    sessionState.end(); // Done reading

    char logBuf[150];
    snprintf(logBuf, sizeof(logBuf), "Loaded State: %s, Lock: %lu s, Streak: %lu",
             stateToString(currentState), lockSecondsRemaining, sessionStreakCount);
    logMessage(logBuf);
    
    return true; // Report success
}

/**
 * Analyzes the loaded state after a reboot and performs
 * the necessary transitions (e.g., aborting, resetting).
 */
void handleRebootState() {
    // This is the logic moved from the old loadState()
    
    switch (currentState) {
        case LOCKED:
        case COUNTDOWN:
        case TESTING:
            // These are active states. A reboot during them is an abort.
            logMessage("Reboot detected during active session. Aborting session...");
            // abortSession() will handle the correct transition and save the new state.
            abortSession("Reboot");
            break;

        case COMPLETED:
            // Session was finished. Reset to ready for a new one.
            logMessage("Loaded COMPLETED state. Resetting to READY.");
            resetToReady(); // This saves the new state
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
void saveState() {
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
    sessionState.putUInt("abortDelay", abortDelaySeconds);
    sessionState.putBool("countStreaks", countStreaks);
    sessionState.putBool("enablePayback", enableTimePayback);
    sessionState.putUShort("abortPayback", abortPaybackMinutes);

    // Save persistent counters
    sessionState.putUInt("streak", sessionStreakCount);
    sessionState.putUInt("completed", completedSessions);
    sessionState.putUInt("aborted", abortedSessions);
    sessionState.putUInt("paybackAccum", paybackAccumulated);
    sessionState.putUInt("totalLocked", totalLockedSessionSeconds);

    // Save arrays as binary "blobs"
    channelDelaysRemaining.resize(NUMBER_OF_CHANNELS, 0); 
    sessionState.putBytes("delays", channelDelaysRemaining.data(), sizeof(unsigned long) * NUMBER_OF_CHANNELS);
    sessionState.putBytes("rewards", rewardHistory, sizeof(rewardHistory));
    
    // Save magic value
    sessionState.putULong("magic", MAGIC_VALUE);

    sessionState.end(); // This commits the changes
    
    esp_task_wdt_reset(); // And feed after
}

// =================================================================
// --- Network & Time Management ---
// =================================================================

/**
 * Attempts to connect to WiFi with a 15-second timeout.
 * @return true on success, false on failure.
 */
bool connectToWiFi(const char* ssid, const char* pass) {
  char logBuf[100];
  snprintf(logBuf, sizeof(logBuf), "Connecting to WiFi: %s", ssid);
  logMessage(logBuf);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);

  int retries = 0;
  // Try for 15 seconds (30 * 500ms)
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    esp_task_wdt_reset(); // Feed the dog
    if (++retries > 30) {
      logMessage("ERROR: Failed to connect to WiFi.");
      WiFi.disconnect();
      return false;
    }
  }

  snprintf(logBuf, sizeof(logBuf), "WiFi connected! IP: %s", WiFi.localIP().toString().c_str());
  logMessage(logBuf);
  return true;
}

/**
 * Connects to NTP server to get the current time.
 */
void initializeTime() {
  logMessage("Initializing NTP time (15s timeout)...");
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  struct tm timeinfo;
  unsigned long start = millis();
  char logBuf[100];

  // Poll for time sync with a timeout
  while (millis() - start < NTP_SYNC_TIMEOUT_MS) {
    esp_task_wdt_reset(); // Feed the dog
    if (getLocalTime(&timeinfo, 10)) { // 10ms non-blocking check
      g_time_initialized = true;
      char* timeStr = asctime(&timeinfo);
      timeStr[strlen(timeStr)-1] = '\0'; // Remove trailing newline
      snprintf(logBuf, sizeof(logBuf), "NTP time initialized: %s", timeStr);
      logMessage(logBuf);
      return;
    }
    delay(500);
  }

  logMessage("ERROR: Failed to obtain time from NTP. Using uptime for logs.");
}

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
 * Adds a message to the in-memory log buffer and prints to Serial.
 */
void logMessage(const char* message) {
  char timestamp[SESSION_TIMESTAMP_LENGTH + 4]; // Room for "[+HH:MM:SS]: "
  getCurrentTimestamp(timestamp, sizeof(timestamp)); // Pass buffer to write into

  // Use snprintf for safe, bounded string formatting
  snprintf(logBuffer[logBufferIndex], MAX_LOG_ENTRY_LENGTH, "%s: %s", timestamp, message);

  Serial.println(logBuffer[logBufferIndex]);

  // Handle ring buffer logic
  logBufferIndex++;
  if (logBufferIndex >= LOG_BUFFER_SIZE) {
      logBufferIndex = 0;
      logBufferFull = true;
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