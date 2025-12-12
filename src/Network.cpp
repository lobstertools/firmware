/*
 * =================================================================================
 * Project:   Lobster Lock - Self-Bondage Session Manager
 * File:      src/Network.cpp
 *
 * Description:
 * Network management module. Handles Wi-Fi connection logic and BLE Provisioning.
 * =================================================================================
 */
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

#include "Config.h"
#include "Esp32SessionHAL.h"
#include "Globals.h"
#include "Network.h"
#include "SettingsManager.h"
#include "Types.h"

// =================================================================================
// SECTION: CONSTANTS & UUIDS
// =================================================================================

#define PROV_SERVICE_UUID "5a160000-8334-469b-a316-c340cf29188f"

// --- WiFi Credentials ---
#define PROV_SSID_CHAR_UUID "5a160001-8334-469b-a316-c340cf29188f"
#define PROV_PASS_CHAR_UUID "5a160002-8334-469b-a316-c340cf29188f"

// --- Hardware Config ---
#define PROV_CH1_ENABLE_UUID "5a16000a-8334-469b-a316-c340cf29188f"
#define PROV_CH2_ENABLE_UUID "5a16000b-8334-469b-a316-c340cf29188f"
#define PROV_CH3_ENABLE_UUID "5a16000c-8334-469b-a316-c340cf29188f"
#define PROV_CH4_ENABLE_UUID "5a16000d-8334-469b-a316-c340cf29188f"

// --- Global Safety Limits ---
#define PROV_MIN_SESSION_DURATION_UUID "5a160010-8334-469b-a316-c340cf29188f"
#define PROV_MAX_SESSION_DURATION_UUID "5a160011-8334-469b-a316-c340cf29188f"

// --- Duration Presets ---
// Short
#define PROV_SHORT_MIN_UUID "5a160020-8334-469b-a316-c340cf29188f"
#define PROV_SHORT_MAX_UUID "5a160021-8334-469b-a316-c340cf29188f"
// Medium
#define PROV_MEDIUM_MIN_UUID "5a160022-8334-469b-a316-c340cf29188f"
#define PROV_MEDIUM_MAX_UUID "5a160023-8334-469b-a316-c340cf29188f"
// Long
#define PROV_LONG_MIN_UUID "5a160024-8334-469b-a316-c340cf29188f"
#define PROV_LONG_MAX_UUID "5a160025-8334-469b-a316-c340cf29188f"

// --- Deterrents ---
#define PROV_ENABLE_STREAKS_CHAR_UUID "5a160004-8334-469b-a316-c340cf29188f"

#define PROV_ENABLE_REWARD_CODE_CHAR_UUID "5a160003-8334-469b-a316-c340cf29188f"
#define PROV_REWARD_STRATEGY_UUID "5a160015-8334-469b-a316-c340cf29188f"
#define PROV_REWARD_PENALTY_CHAR_UUID "5a160007-8334-469b-a316-c340cf29188f"
#define PROV_REWARD_MIN_DURATION_UUID "5a160016-8334-469b-a316-c340cf29188f"
#define PROV_REWARD_MAX_DURATION_UUID "5a160017-8334-469b-a316-c340cf29188f"

#define PROV_ENABLE_PAYBACK_TIME_CHAR_UUID "5a160005-8334-469b-a316-c340cf29188f"
#define PROV_PAYBACK_STRATEGY_UUID "5a160012-8334-469b-a316-c340cf29188f"
#define PROV_PAYBACK_TIME_CHAR_UUID "5a160006-8334-469b-a316-c340cf29188f"
#define PROV_PAYBACK_MIN_DURATION_UUID "5a160013-8334-469b-a316-c340cf29188f"
#define PROV_PAYBACK_MAX_DURATION_UUID "5a160014-8334-469b-a316-c340cf29188f"


// =================================================================================
// SECTION: CLASS IMPLEMENTATION
// =================================================================================

NetworkManager &NetworkManager::getInstance() {
  static NetworkManager instance;
  return instance;
}

NetworkManager::NetworkManager() : _wifiCredentialsExist(false), _triggerProvisioning(false), _wifiRetries(0), _wifiReconnectTimer(NULL) {
  memset(_wifiSSID, 0, sizeof(_wifiSSID));
  memset(_wifiPass, 0, sizeof(_wifiPass));
}

void NetworkManager::log(const char *key, const char *val) { Esp32SessionHAL::getInstance().logKeyValue(key, val); }

// =================================================================================
// SECTION: WIFI LOGIC
// =================================================================================

void NetworkManager::connectToWiFi() {
  if (!_wifiCredentialsExist)
    return;
  if (WiFi.status() == WL_CONNECTED)
    return;

  log("Network", "Connecting...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(_wifiSSID, _wifiPass);
}

// --- Static Callbacks ---

void NetworkManager::onWiFiEvent(WiFiEvent_t event) { getInstance().handleWiFiEvent(event); }

void NetworkManager::onWifiTimer(TimerHandle_t t) { getInstance().handleWifiTimer(); }

// --- Member Handlers ---

void NetworkManager::handleWifiTimer() { connectToWiFi(); }

void NetworkManager::handleWiFiEvent(WiFiEvent_t event) {
  switch (event) {
  case ARDUINO_EVENT_WIFI_STA_GOT_IP:
    log("Network", "Connected.");
    _wifiRetries = 0;
    if (_wifiReconnectTimer != NULL)
      xTimerStop(_wifiReconnectTimer, 0);
    break;
  case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
    // If we fail too many times, we don't abort directly.
    // We just raise the flag. The Engine will decide what to do.
    if (_wifiRetries >= g_systemDefaults.wifiMaxRetries) {
      log("Network", "Max retries exceeded. Requesting Provisioning...");
      if (_wifiReconnectTimer != NULL)
        xTimerStop(_wifiReconnectTimer, 0);
      _triggerProvisioning = true;
    } else {
      _wifiRetries++;
      if (_wifiReconnectTimer != NULL)
        xTimerStart(_wifiReconnectTimer, 0);
    }
    break;
  default:
    break;
  }
}

void NetworkManager::startMDNS() {
  log("Network", "Starting mDNS advertiser...");
  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);
  char uniqueHostname[30];
  snprintf(uniqueHostname, sizeof(uniqueHostname), "lobster-lock-%02X%02X%02X", mac[3], mac[4], mac[5]);

  if (!MDNS.begin(uniqueHostname)) {
    log("Network", "Failed to set up mDNS responder!");
    return;
  }
  MDNS.addService("lobster-lock", "tcp", 80);

  char logBuf[64];
  snprintf(logBuf, sizeof(logBuf), "mDNS active: %s.local", uniqueHostname);
  log("Network", logBuf);
}

// =================================================================================
// SECTION: BLE PROVISIONING (Transport Only)
// =================================================================================

uint16_t bytesToUint16(uint8_t *data) { return (uint16_t)data[0] | ((uint16_t)data[1] << 8); }
uint32_t bytesToUint32(uint8_t *data) {
  return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

class ProvisioningCallbacks : public BLECharacteristicCallbacks {
  // Flag to signal completion to the manager
  bool *_credentialsReceivedPtr;

private:
  void log(const char *key, const char *val) { Esp32SessionHAL::getInstance().logKeyValue(key, val); }

public:
  ProvisioningCallbacks(bool *flagPtr) : _credentialsReceivedPtr(flagPtr) {}

  void onWrite(BLECharacteristic *pCharacteristic) {
    std::string uuid = pCharacteristic->getUUID().toString();
    uint8_t *data = pCharacteristic->getData();
    size_t len = pCharacteristic->getLength();

    if (len == 0)
      return;

    // --- Credentials ---
    if (uuid == PROV_SSID_CHAR_UUID) {
      std::string val(data, data + len);
      SettingsManager::setWifiSSID(val.c_str());
      log("BLE", "SSID Received");
    } else if (uuid == PROV_PASS_CHAR_UUID) {
      std::string val(data, data + len);
      SettingsManager::setWifiPassword(val.c_str());
      log("BLE", "Password Received");
      // Signal completion - Triggers Reboot
      if (_credentialsReceivedPtr) *_credentialsReceivedPtr = true;
    } 
    
    // --- Toggles & Fixed Values ---
    else if (uuid == PROV_ENABLE_REWARD_CODE_CHAR_UUID) SettingsManager::setRewardCodeEnabled((bool)data[0]);
    else if (uuid == PROV_ENABLE_STREAKS_CHAR_UUID) SettingsManager::setStreaksEnabled((bool)data[0]);
    else if (uuid == PROV_ENABLE_PAYBACK_TIME_CHAR_UUID) SettingsManager::setPaybackEnabled((bool)data[0]);
    else if (uuid == PROV_PAYBACK_TIME_CHAR_UUID) SettingsManager::setPaybackDuration(bytesToUint32(data));
    else if (uuid == PROV_REWARD_PENALTY_CHAR_UUID) SettingsManager::setRewardPenaltyDuration(bytesToUint32(data));
    
    // --- Hardware ---
    else if (uuid == PROV_CH1_ENABLE_UUID) SettingsManager::setChannelEnabled(0, (bool)data[0]);
    else if (uuid == PROV_CH2_ENABLE_UUID) SettingsManager::setChannelEnabled(1, (bool)data[0]);
    else if (uuid == PROV_CH3_ENABLE_UUID) SettingsManager::setChannelEnabled(2, (bool)data[0]);
    else if (uuid == PROV_CH4_ENABLE_UUID) SettingsManager::setChannelEnabled(3, (bool)data[0]);

    // --- Strategies ---
    else if (uuid == PROV_PAYBACK_STRATEGY_UUID) SettingsManager::setPaybackStrategy((DeterrentStrategy)data[0]);
    else if (uuid == PROV_REWARD_STRATEGY_UUID) SettingsManager::setRewardStrategy((DeterrentStrategy)data[0]);

    // --- Ranges (Read-Modify-Write) ---
    // Since we receive Min/Max individually but save them as pairs, we must:
    // 1. Load the current config to get the "other" value of the pair.
    // 2. Update the specific value we received.
    // 3. Save the pair back using the SettingsManager.
    else {
        DeterrentConfig config;
        SessionPresets presets;
        uint8_t mask;
        SettingsManager::loadProvisioningConfig(config, presets, mask);
        uint32_t val = bytesToUint32(data);

        // 1. Global Session Safety Limits
        if (uuid == PROV_MIN_SESSION_DURATION_UUID) {
            SettingsManager::setSessionLimits(val, presets.maxSessionDuration);
        } else if (uuid == PROV_MAX_SESSION_DURATION_UUID) {
            SettingsManager::setSessionLimits(presets.minSessionDuration, val);
        }
        
        // 2. Deterrent: Payback Range
        else if (uuid == PROV_PAYBACK_MIN_DURATION_UUID) {
            SettingsManager::setPaybackRange(val, config.paybackTimeMax);
        } else if (uuid == PROV_PAYBACK_MAX_DURATION_UUID) {
            SettingsManager::setPaybackRange(config.paybackTimeMin, val);
        }

        // 3. Deterrent: Reward Penalty Range
        else if (uuid == PROV_REWARD_MIN_DURATION_UUID) {
            SettingsManager::setRewardRange(val, config.rewardPenaltyMax);
        } else if (uuid == PROV_REWARD_MAX_DURATION_UUID) {
            SettingsManager::setRewardRange(config.rewardPenaltyMin, val);
        }

        // 4. Session Presets: Short
        else if (uuid == PROV_SHORT_MIN_UUID) {
            SettingsManager::setDurationPreset(DUR_RANGE_SHORT, val, presets.shortMax);
        } else if (uuid == PROV_SHORT_MAX_UUID) {
            SettingsManager::setDurationPreset(DUR_RANGE_SHORT, presets.shortMin, val);
        }

        // 5. Session Presets: Medium
        else if (uuid == PROV_MEDIUM_MIN_UUID) {
            SettingsManager::setDurationPreset(DUR_RANGE_MEDIUM, val, presets.mediumMax);
        } else if (uuid == PROV_MEDIUM_MAX_UUID) {
            SettingsManager::setDurationPreset(DUR_RANGE_MEDIUM, presets.mediumMin, val);
        }

        // 6. Session Presets: Long
        else if (uuid == PROV_LONG_MIN_UUID) {
            SettingsManager::setDurationPreset(DUR_RANGE_LONG, val, presets.longMax);
        } else if (uuid == PROV_LONG_MAX_UUID) {
            SettingsManager::setDurationPreset(DUR_RANGE_LONG, presets.longMin, val);
        }
    }
  }
};

void NetworkManager::startBLEProvisioningBlocking() {
  log("BLE", "Entering Provisioning Mode (Blocking)...");

  // 1. Shutdown WiFi
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  // 2. Feedback
  Esp32SessionHAL::getInstance().getStatusLed().FadeOn(500).FadeOff(500).Forever();

  // 3. Init BLE
  bool localCredentialsReceived = false;

  BLEDevice::init(DEVICE_NAME);
  BLEServer *pServer = BLEDevice::createServer();
  BLEService *pService = pServer->createService(BLEUUID(PROV_SERVICE_UUID), 90); // Increased handles for new chars

  ProvisioningCallbacks *callbacks = new ProvisioningCallbacks(&localCredentialsReceived);

  auto createChar = [&](const char *uuid) {
    BLECharacteristic *p = pService->createCharacteristic(uuid, BLECharacteristic::PROPERTY_WRITE);
    p->setCallbacks(callbacks);
    return p;
  };

  // Credentials
  createChar(PROV_SSID_CHAR_UUID);
  createChar(PROV_PASS_CHAR_UUID);

  // Feature Toggles
  createChar(PROV_ENABLE_REWARD_CODE_CHAR_UUID);
  createChar(PROV_ENABLE_STREAKS_CHAR_UUID);
  createChar(PROV_ENABLE_PAYBACK_TIME_CHAR_UUID);
  
  // Base Values
  createChar(PROV_PAYBACK_TIME_CHAR_UUID);
  createChar(PROV_REWARD_PENALTY_CHAR_UUID);
  
  // Hardware
  createChar(PROV_CH1_ENABLE_UUID);
  createChar(PROV_CH2_ENABLE_UUID);
  createChar(PROV_CH3_ENABLE_UUID);
  createChar(PROV_CH4_ENABLE_UUID);

  // Global Limits
  createChar(PROV_MIN_SESSION_DURATION_UUID);
  createChar(PROV_MAX_SESSION_DURATION_UUID);
  
  // Deterrent Strategies & Ranges
  createChar(PROV_PAYBACK_STRATEGY_UUID);
  createChar(PROV_PAYBACK_MIN_DURATION_UUID);
  createChar(PROV_PAYBACK_MAX_DURATION_UUID);
  
  createChar(PROV_REWARD_STRATEGY_UUID);
  createChar(PROV_REWARD_MIN_DURATION_UUID);
  createChar(PROV_REWARD_MAX_DURATION_UUID);

  // New Duration Presets
  createChar(PROV_SHORT_MIN_UUID);
  createChar(PROV_SHORT_MAX_UUID);
  createChar(PROV_MEDIUM_MIN_UUID);
  createChar(PROV_MEDIUM_MAX_UUID);
  createChar(PROV_LONG_MIN_UUID);
  createChar(PROV_LONG_MAX_UUID);

  pService->start();

  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(PROV_SERVICE_UUID);
  pAdvertising->start();

  // 4. Blocking Loop (Hardware Safe)
  while (1) {
    Esp32SessionHAL::getInstance().tick();

    // CRITICAL: Ensure pins are OFF directly
    // The Session Engine is paused/blocked here, so we must enforce safety manually.
    for (int i = 0; i < MAX_CHANNELS; i++)
      digitalWrite(HARDWARE_PINS[i], LOW);

    if (localCredentialsReceived) {
      log("BLE", "Credentials received. Restarting...");
      Esp32SessionHAL::getInstance().tick();
      delay(3000);
      ESP.restart();
    }

    esp_task_wdt_reset();
    delay(100);
  }
}

// =================================================================================
// SECTION: PUBLIC API IMPLEMENTATION
// =================================================================================

void NetworkManager::connectOrRequestProvisioning() {
  // Create Timer
  _wifiReconnectTimer = xTimerCreate("wifiTimer", pdMS_TO_TICKS(2000), pdFALSE, (void *)0, NetworkManager::onWifiTimer);

  SettingsManager::getWifiSSID(_wifiSSID, sizeof(_wifiSSID));
  SettingsManager::getWifiPassword(_wifiPass, sizeof(_wifiPass));

  // Determine if we can connect
  if (strlen(_wifiSSID) > 0) {
    log("Network", "Found Wi-Fi credentials.");
    _wifiCredentialsExist = true;
    WiFi.onEvent(NetworkManager::onWiFiEvent);
    connectToWiFi();
  }

  // Blocking wait for initial connection
  if (_wifiCredentialsExist) {
    unsigned long wifiWaitStart = millis();
    WiFi.setSleep(false);

    // Wait for connection (Blocking briefly on startup)
    while (WiFi.status() != WL_CONNECTED && (millis() - wifiWaitStart < 30000)) {
      Esp32SessionHAL::getInstance().tick();
      esp_task_wdt_reset();
      delay(100);
    }

    if (WiFi.status() == WL_CONNECTED) {
      startMDNS();
      return;
    }

    // Failure: Just flag it.
    log("Network", "Startup WiFi Failed. Requesting Provisioning...");
    _triggerProvisioning = true;
  } else {
    // No creds? Flag immediately.
    _triggerProvisioning = true;
  }
}

void NetworkManager::printStartupDiagnostics() {
    char logBuf[128];
    const char* boolStr[] = { "NO", "YES" };
    
    // Get HAL instance for raw logging
    Esp32SessionHAL& hal = Esp32SessionHAL::getInstance();

    hal.log("==========================================================================");
    hal.log("                            NETWORK DIAGNOSTICS                           ");
    hal.log("==========================================================================");

    // -------------------------------------------------------------------------
    // SECTION: WI-FI STATE
    // -------------------------------------------------------------------------
    hal.log("[ WI-FI STATUS ]");

    // Connection State
    wl_status_t status = WiFi.status();
    const char* statusStr;
    switch(status) {
        case WL_CONNECTED:       statusStr = "CONNECTED"; break;
        case WL_NO_SSID_AVAIL:   statusStr = "SSID NOT FOUND"; break;
        case WL_CONNECT_FAILED:  statusStr = "FAILED"; break;
        case WL_IDLE_STATUS:     statusStr = "IDLE"; break;
        case WL_DISCONNECTED:    statusStr = "DISCONNECTED"; break;
        default:                 statusStr = "UNKNOWN"; break;
    }
    
    snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Connection State", statusStr);
    hal.log(logBuf);

    // SSID (Only show if we have one loaded)
    if (strlen(_wifiSSID) > 0) {
        snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Target SSID", _wifiSSID);
        hal.log(logBuf);
    } else {
        snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Target SSID", "-- NOT SET --");
        hal.log(logBuf);
    }
    
    // Signal Strength (RSSI) - Only valid if connected
    if (status == WL_CONNECTED) {
        snprintf(logBuf, sizeof(logBuf), " %-25s : %ld dBm", "Signal Strength", WiFi.RSSI());
        hal.log(logBuf);
    }

    // Hardware MAC (Always available)
    snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Device MAC", WiFi.macAddress().c_str());
    hal.log(logBuf);

    // -------------------------------------------------------------------------
    // SECTION: IP CONFIGURATION
    // -------------------------------------------------------------------------
    if (status == WL_CONNECTED) {
        hal.log(""); 
        hal.log("[ IP CONFIGURATION ]");

        snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Local IP", WiFi.localIP().toString().c_str());
        hal.log(logBuf);

        snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Subnet Mask", WiFi.subnetMask().toString().c_str());
        hal.log(logBuf);

        snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Gateway", WiFi.gatewayIP().toString().c_str());
        hal.log(logBuf);

        // mDNS Check
        // Note: MDNS.begin returns true/false, but there isn't a direct "isRunning()" 
        // getter exposed easily in standard ESP libraries, but we can imply it from success.
        snprintf(logBuf, sizeof(logBuf), " %-25s : %s.local", "mDNS Hostname", "lobster-lock-[MAC]"); 
        hal.log(logBuf);
    }

    // -------------------------------------------------------------------------
    // SECTION: INTERNAL FLAGS
    // -------------------------------------------------------------------------
    hal.log(""); 
    hal.log("[ LOGIC FLAGS ]");

    snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Credentials Loaded", boolStr[_wifiCredentialsExist]);
    hal.log(logBuf);

    snprintf(logBuf, sizeof(logBuf), " %-25s : %d / %d", "Retry Counter", _wifiRetries, g_systemDefaults.wifiMaxRetries);
    hal.log(logBuf);

    snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Provisioning Request", boolStr[_triggerProvisioning]);
    hal.log(logBuf);
}