/*
 * =================================================================================
 * Project:   Lobster Lock - Self-Bondage Session Manager
 * File:      Network.cpp
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Description:
 * Network management module. Handles Wi-Fi connection logic, mDNS advertising,
 * and the BLE Provisioning fallback mechanism.
 * =================================================================================
 */
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

#include "Globals.h"
#include "Logger.h"
#include "Network.h"
#include "Utils.h"
#include "Session.h"
#include "Hardware.h"

// =================================================================================
// SECTION: CONSTANTS & UUIDS
// =================================================================================

#define PROV_SERVICE_UUID "5a160000-8334-469b-a316-c340cf29188f"

// WiFi Credentials
#define PROV_SSID_CHAR_UUID "5a160001-8334-469b-a316-c340cf29188f"
#define PROV_PASS_CHAR_UUID "5a160002-8334-469b-a316-c340cf29188f"

// Deterrents
#define PROV_ENABLE_REWARD_CODE_CHAR_UUID "5a160003-8334-469b-a316-c340cf29188f"
#define PROV_ENABLE_STREAKS_CHAR_UUID "5a160004-8334-469b-a316-c340cf29188f"
#define PROV_ENABLE_PAYBACK_TIME_CHAR_UUID "5a160005-8334-469b-a316-c340cf29188f"
#define PROV_PAYBACK_TIME_CHAR_UUID "5a160006-8334-469b-a316-c340cf29188f"
#define PROV_REWARD_PENALTY_CHAR_UUID "5a160007-8334-469b-a316-c340cf29188f"

// Hardware Config
#define PROV_CH1_ENABLE_UUID "5a16000a-8334-469b-a316-c340cf29188f"
#define PROV_CH2_ENABLE_UUID "5a16000b-8334-469b-a316-c340cf29188f"
#define PROV_CH3_ENABLE_UUID "5a16000c-8334-469b-a316-c340cf29188f"
#define PROV_CH4_ENABLE_UUID "5a16000d-8334-469b-a316-c340cf29188f"

// =================================================================================
// SECTION: GLOBALS
// =================================================================================

volatile bool g_credentialsReceived = false;
TimerHandle_t wifiReconnectTimer = NULL;
int g_wifiRetries = 0;

// =================================================================================
// SECTION: WIFI LOGIC
// =================================================================================

/**
 * Trigger the WiFi connection process using stored credentials.
 */
void connectToWiFi() {
  if (!g_wifiCredentialsExist)
    return;
  if (WiFi.status() == WL_CONNECTED)
    return;

  logKeyValue("Network", "Connecting...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(g_wifiSSID, g_wifiPass);
}

/**
 * Handle WiFi connection events (connected, disconnected, etc).
 */
void WiFiEvent(WiFiEvent_t event) {
  switch (event) {
  case ARDUINO_EVENT_WIFI_STA_GOT_IP: {
    char logBuf[64];
    snprintf(logBuf, sizeof(logBuf), "Connected. IP: %s", WiFi.localIP().toString().c_str());
    logKeyValue("Network", logBuf);
    g_wifiRetries = 0;
    if (wifiReconnectTimer != NULL)
      xTimerStop(wifiReconnectTimer, 0);
    break;
  }
  case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
    if (g_wifiRetries >= g_systemDefaults.wifiMaxRetries) {
      logKeyValue("Network", "Max retries exceeded. Falling back to BLE Provisioning...");
      if (wifiReconnectTimer != NULL)
        xTimerStop(wifiReconnectTimer, 0);
      g_triggerProvisioning = true;
    } else {
      g_wifiRetries++;
      if (wifiReconnectTimer != NULL)
        xTimerStart(wifiReconnectTimer, 0);
    }
    break;
  default:
    break;
  }
}

/**
 * Starts the mDNS service to advertise 'lobster-lock.local'
 */
void startMDNS() {
  logKeyValue("Network", "Starting mDNS advertiser...");
  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);
  char uniqueHostname[30];
  snprintf(uniqueHostname, sizeof(uniqueHostname), "lobster-lock-%02X%02X%02X", mac[3], mac[4], mac[5]);

  // Set the unique hostname (e.g., lobster-lock-AABBCC.local)
  if (!MDNS.begin(uniqueHostname)) {
    logKeyValue("Network", "Failed to set up mDNS responder!");
    return;
  }
  // Announce the service your NodeJS app will look for
  MDNS.addService("lobster-lock", "tcp", 80);
  MDNS.addServiceTxt("lobster-lock", "tcp", "mac", WiFi.macAddress().c_str());

  char logBuf[64];
  snprintf(logBuf, sizeof(logBuf), "mDNS active: %s.local", uniqueHostname);
  logKeyValue("Network", logBuf);
}

// =================================================================================
// SECTION: BLE PROVISIONING LOGIC
// =================================================================================

/**
 * BLE callback class to handle writes to characteristics.
 * This receives the Wi-Fi credentials and config from the server/UI.
 */
class ProvisioningCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    std::string uuid = pCharacteristic->getUUID().toString();
    uint8_t *data = pCharacteristic->getData();
    size_t len = pCharacteristic->getLength();

    if (len == 0)
      return;

    char logBuf[100]; // Buffer for formatted log messages

    // --- 1. WiFi Credentials ---
    if (uuid == PROV_SSID_CHAR_UUID) {
      std::string value(data, data + len);
      wifiPreferences.begin("wifi-creds", false);
      wifiPreferences.putString("ssid", value.c_str());
      wifiPreferences.end();

      snprintf(logBuf, sizeof(logBuf), "SSID Set: %s", value.c_str());
      logKeyValue("BLE", logBuf);
    } else if (uuid == PROV_PASS_CHAR_UUID) {
      std::string value(data, data + len);
      wifiPreferences.begin("wifi-creds", false);
      wifiPreferences.putString("pass", value.c_str());
      wifiPreferences.end();

      // Do not log the actual password
      snprintf(logBuf, sizeof(logBuf), "Password Received (%u chars)", len);
      logKeyValue("BLE", logBuf);
      g_credentialsReceived = true;
    }

    // --- 2. Deterrent Config ---
    else if (uuid == PROV_ENABLE_REWARD_CODE_CHAR_UUID) {
      bool enable = (bool)data[0];
      provisioningPrefs.begin("provisioning", false);
      provisioningPrefs.putBool("enableCode", enable);
      provisioningPrefs.end();

      snprintf(logBuf, sizeof(logBuf), "Reward Code: %s", enable ? "ENABLED" : "DISABLED");
      logKeyValue("BLE", logBuf);
    } else if (uuid == PROV_ENABLE_STREAKS_CHAR_UUID) {
      bool enable = (bool)data[0];
      provisioningPrefs.begin("provisioning", false);
      provisioningPrefs.putBool("enableStreaks", enable);
      provisioningPrefs.end();

      snprintf(logBuf, sizeof(logBuf), "Streaks: %s", enable ? "ENABLED" : "DISABLED");
      logKeyValue("BLE", logBuf);
    } else if (uuid == PROV_ENABLE_PAYBACK_TIME_CHAR_UUID) {
      bool enable = (bool)data[0];
      provisioningPrefs.begin("provisioning", false);
      provisioningPrefs.putBool("enablePayback", enable);
      provisioningPrefs.end();

      snprintf(logBuf, sizeof(logBuf), "Payback Time: %s", enable ? "ENABLED" : "DISABLED");
      logKeyValue("BLE", logBuf);
    } else if (uuid == PROV_PAYBACK_TIME_CHAR_UUID) {
      provisioningPrefs.begin("provisioning", false);
      uint32_t rawVal = bytesToUint32(data);
      uint32_t finalVal = rawVal;
      const char *clampNote = "";

      // Logic: Clamping
      if (finalVal < g_sessionLimits.minPaybackTime) {
        finalVal = g_sessionLimits.minPaybackTime;
        clampNote = " (Clamped Min)";
      } else if (finalVal > g_sessionLimits.maxPaybackTime) {
        finalVal = g_sessionLimits.maxPaybackTime;
        clampNote = " (Clamped Max)";
      }

      provisioningPrefs.putUInt("paybackSeconds", finalVal);
      provisioningPrefs.end();

      if (strlen(clampNote) > 0) {
        snprintf(logBuf, sizeof(logBuf), "Payback Time: %u s%s (Req: %u)", finalVal, clampNote, rawVal);
      } else {
        snprintf(logBuf, sizeof(logBuf), "Payback Time: %u s", finalVal);
      }
      logKeyValue("BLE", logBuf);
    } else if (uuid == PROV_REWARD_PENALTY_CHAR_UUID) {
      provisioningPrefs.begin("provisioning", false);
      uint32_t rawVal = bytesToUint32(data);
      uint32_t finalVal = rawVal;
      const char *clampNote = "";

      // Logic: Clamping
      if (finalVal < g_sessionLimits.minRewardPenaltyDuration) {
        finalVal = g_sessionLimits.minRewardPenaltyDuration;
        clampNote = " (Clamped Min)";
      } else if (finalVal > g_sessionLimits.maxRewardPenaltyDuration) {
        finalVal = g_sessionLimits.maxRewardPenaltyDuration;
        clampNote = " (Clamped Max)";
      }

      provisioningPrefs.putUInt("rwdPenaltySec", finalVal);
      provisioningPrefs.end();

      if (strlen(clampNote) > 0) {
        snprintf(logBuf, sizeof(logBuf), "Reward Penalty: %u s%s (Req: %u)", finalVal, clampNote, rawVal);
      } else {
        snprintf(logBuf, sizeof(logBuf), "Reward Penalty: %u s", finalVal);
      }
      logKeyValue("BLE", logBuf);
    }

    // --- 3. Hardware Config (Channel Mask) ---
    else {
      provisioningPrefs.begin("provisioning", false);
      uint8_t currentMask = provisioningPrefs.getUChar("chMask", 0x0F);
      bool enable = (bool)data[0];
      int chIndex = -1;

      if (uuid == PROV_CH1_ENABLE_UUID)
        chIndex = 0;
      else if (uuid == PROV_CH2_ENABLE_UUID)
        chIndex = 1;
      else if (uuid == PROV_CH3_ENABLE_UUID)
        chIndex = 2;
      else if (uuid == PROV_CH4_ENABLE_UUID)
        chIndex = 3;

      if (chIndex >= 0) {
        if (enable)
          currentMask |= (1 << chIndex);
        else
          currentMask &= ~(1 << chIndex);

        snprintf(logBuf, sizeof(logBuf), "Ch%d Config: %s", chIndex + 1, enable ? "ENABLED" : "DISABLED");
        logKeyValue("BLE", logBuf);
      }

      provisioningPrefs.putUChar("chMask", currentMask);
      provisioningPrefs.end();
      g_enabledChannelsMask = currentMask;

      snprintf(logBuf, sizeof(logBuf), "New Channel Mask: 0x%02X", currentMask);
      logKeyValue("BLE", logBuf);
    }
  }
};

/**
 * Starts the BLE provisioning service.
 * This function DOES NOT RETURN. It waits for credentials and reboots.
 */
void startBLEProvisioning() {
  logKeyValue("BLE", "Starting Provisioning Mode...");

  // Set a "pairing" pulse pattern
  statusLed.FadeOn(500).FadeOff(500).Forever();

  BLEDevice::init(DEVICE_NAME);
  BLEServer *pServer = BLEDevice::createServer();
  BLEService *pService = pServer->createService(BLEUUID(PROV_SERVICE_UUID), 35);

  ProvisioningCallbacks *callbacks = new ProvisioningCallbacks();

  auto createChar = [&](const char *uuid) {
    BLECharacteristic *p = pService->createCharacteristic(uuid, BLECharacteristic::PROPERTY_WRITE);
    p->setCallbacks(callbacks);
    return p;
  };

  createChar(PROV_SSID_CHAR_UUID);
  createChar(PROV_PASS_CHAR_UUID);
  createChar(PROV_ENABLE_REWARD_CODE_CHAR_UUID);
  createChar(PROV_ENABLE_STREAKS_CHAR_UUID);
  createChar(PROV_ENABLE_PAYBACK_TIME_CHAR_UUID);
  createChar(PROV_PAYBACK_TIME_CHAR_UUID);
  createChar(PROV_REWARD_PENALTY_CHAR_UUID);
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

    // Actively force pins LOW every cycle to prevent latch-up/glitches
    sendChannelOffAll();

    if (g_credentialsReceived) {
      logKeyValue("BLE", "Wifi Credentials received. Restarting...");

      processLogQueue();

      delay(3000);
      ESP.restart();
    }
    statusLed.Update();
    esp_task_wdt_reset();
    delay(100);
  }
}

// =================================================================================
// SECTION: PUBLIC API
// =================================================================================

/**
 * Setup function to wrap the wifi init logic.
 * If WiFi fails or credentials are missing, this falls through to BLE Provisioning (Blocking).
 */
void connectWiFiOrProvision() {
  wifiReconnectTimer = xTimerCreate("wifiTimer", pdMS_TO_TICKS(2000), pdFALSE, (void *)0, [](TimerHandle_t t) { connectToWiFi(); });

  if (!wifiPreferences.begin("wifi-creds", true)) {
    logKeyValue("Prefs", "'wifi-creds' missing.");
  }
  String ssidTemp = wifiPreferences.getString("ssid", "");
  String passTemp = wifiPreferences.getString("pass", "");
  wifiPreferences.end();

  strncpy(g_wifiSSID, ssidTemp.c_str(), sizeof(g_wifiSSID));
  strncpy(g_wifiPass, passTemp.c_str(), sizeof(g_wifiPass));

  if (strlen(g_wifiSSID) > 0) {
    logKeyValue("Network", "Found Wi-Fi credentials.");
    g_wifiCredentialsExist = true;
    WiFi.onEvent(WiFiEvent);
    connectToWiFi();
  } else {
    // Fallthrough
  }

  if (g_wifiCredentialsExist) {
    unsigned long wifiWaitStart = millis();
    WiFi.setSleep(false);
    while (WiFi.status() != WL_CONNECTED && (millis() - wifiWaitStart < 30000)) {
      processLogQueue();
      esp_task_wdt_reset();
      delay(100);
    }
    if (WiFi.status() == WL_CONNECTED) {
      startMDNS();
      return;
    }

    if (g_currentState == LOCKED || g_currentState == ARMED || g_currentState == TESTING) {
      // Just return. The main loop will run, buttons will work, 
      // and the "Network Fallback" in loop() will catch it later 
      // (triggering the Runtime Abort logic you already wrote).
      return; 
      logKeyValue("Network", "Startup WiFi Failed. Skipping BLE to preserve Session Safety.");
      g_triggerProvisioning = true;
      return;
    }
  }

  logKeyValue("BLE", "Entering BLE Provisioning Fallback.");
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  startBLEProvisioning();
}

void handleNetworkFallback() {
  if (g_triggerProvisioning) {
    
    // If WiFi fails, we treat it like a Keep-Alive failure.
    // 1. Abort the session (Records the stats, applies penalty, saves to NVS).
    // 2. Unlock hardware (User is free).
    // 3. Enter Provisioning (Device is effectively disabled until fixed).    
    if (g_currentState == LOCKED || g_currentState == ARMED || g_currentState == TESTING) {
      logKeyValue("Network", "Critical: WiFi Stack Failure during session.");
      
      processLogQueue();

      // Trigger the logic abort (State -> ABORTED/COMPLETED)
      // This ensures the failure is recorded in stats and penalties are armed.
      abortSession("WiFi Connection Lost");
      
      // Force Hardware Unlock immediately (Safety)
      // We call this explicitly because the blocking call below prevents 
      // the main loop's 'enforceHardwareState' from running.
      sendChannelOffAll();
    }

    // 3. Clear flag and enter blocking BLE mode
    g_triggerProvisioning = false;
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    
    // This function blocks forever until credentials are provided + Reboot.
    startBLEProvisioning();
  }
}