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

// --- LIBRARIES FOR STAGE 1, 2 ---
// Base UUID: 5a160000-8334-469b-a316-c340cf29188f
#define PROV_SERVICE_UUID "5a160000-8334-469b-a316-c340cf29188f"
#define PROV_SSID_CHAR_UUID "5a160001-8334-469b-a316-c340cf29188f"
#define PROV_PASS_CHAR_UUID "5a160002-8334-469b-a316-c340cf29188f"
#define PROV_ENABLE_REWARD_CODE_CHAR_UUID "5a160003-8334-469b-a316-c340cf29188f"
#define PROV_ENABLE_STREAKS_CHAR_UUID "5a160004-8334-469b-a316-c340cf29188f"
#define PROV_ENABLE_PAYBACK_TIME_CHAR_UUID "5a160005-8334-469b-a316-c340cf29188f"
#define PROV_PAYBACK_TIME_CHAR_UUID "5a160006-8334-469b-a316-c340cf29188f"
#define PROV_CH1_ENABLE_UUID "5a16000a-8334-469b-a316-c340cf29188f"
#define PROV_CH2_ENABLE_UUID "5a16000b-8334-469b-a316-c340cf29188f"
#define PROV_CH3_ENABLE_UUID "5a16000d-8334-469b-a316-c340cf29188f"
#define PROV_CH4_ENABLE_UUID "5a16000c-8334-469b-a316-c340cf29188f"

volatile bool g_credentialsReceived = false;
TimerHandle_t wifiReconnectTimer;
int g_wifiRetries = 0;

/**
 * Trigger the WiFi connection process using stored credentials.
 */
void connectToWiFi() {
  if (!g_wifiCredentialsExist)
    return;
  if (WiFi.status() == WL_CONNECTED)
    return;

  logMessage("WiFi: Connecting...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(g_wifiSSID, g_wifiPass);
}

/**
 * Handle WiFi connection events (connected, disconnected, etc).
 */
void WiFiEvent(WiFiEvent_t event) {
  switch (event) {
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
      snprintf(logBuf, sizeof(logBuf), "WiFi: Disconnected (Attempt %d/%d). Retrying...", g_wifiRetries + 1, g_systemConfig.wifiMaxRetries);
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
  MDNS.addServiceTxt("lobster-lock", "tcp", "mac", WiFi.macAddress().c_str());
  logMessage("mDNS service announced: _lobster-lock._tcp.local");
}

/**
 * BLE callback class to handle writes to characteristics.
 * This receives the Wi-Fi credentials and config from the server/UI.
 */
class ProvisioningCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    std::string uuid = pCharacteristic->getUUID().toString();
    uint8_t *data = pCharacteristic->getData();
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
    else if (uuid == PROV_ENABLE_REWARD_CODE_CHAR_UUID) {
      sessionState.begin("session", false);
      bool val = (bool)data[0];
      sessionState.putBool("enableCode", val);
      sessionState.end();
      logMessage("BLE: Received Enable Reward Code");
    } else if (uuid == PROV_ENABLE_STREAKS_CHAR_UUID) {
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

      // Clamp value against System Config limits
      if (val < g_systemConfig.minPaybackTimeSeconds) {
        val = g_systemConfig.minPaybackTimeSeconds;
      } else if (val > g_systemConfig.maxPaybackTimeSeconds) {
        val = g_systemConfig.maxPaybackTimeSeconds;
      }

      sessionState.putUInt("paybackSeconds", val);
      sessionState.end();

      char logBuf[64];
      snprintf(logBuf, sizeof(logBuf), "BLE: Received Payback Time (%u s)", val);
      logMessage(logBuf);
    }
    // Handle Hardware Config (provisioning namespace)
    else {
      provisioningPrefs.begin("provisioning", false);

      // Read current mask (default to 0x0F if not set)
      uint8_t currentMask = provisioningPrefs.getUChar("chMask", 0x0F);
      bool enable = (bool)data[0];

      if (uuid == PROV_CH1_ENABLE_UUID) {
        if (enable)
          currentMask |= (1 << 0);
        else
          currentMask &= ~(1 << 0);
        logMessage(enable ? "BLE: Enabled Ch1" : "BLE: Disabled Ch1");
      } else if (uuid == PROV_CH2_ENABLE_UUID) {
        if (enable)
          currentMask |= (1 << 1);
        else
          currentMask &= ~(1 << 1);
        logMessage(enable ? "BLE: Enabled Ch2" : "BLE: Disabled Ch2");
      } else if (uuid == PROV_CH3_ENABLE_UUID) {
        if (enable)
          currentMask |= (1 << 2);
        else
          currentMask &= ~(1 << 2);
        logMessage(enable ? "BLE: Enabled Ch3" : "BLE: Disabled Ch3");
      } else if (uuid == PROV_CH4_ENABLE_UUID) {
        if (enable)
          currentMask |= (1 << 3);
        else
          currentMask &= ~(1 << 3);
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

  ProvisioningCallbacks *callbacks = new ProvisioningCallbacks();

  // Helper to create char
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

/**
 * Setup function to wrap the wifi init logic
 */
void waitForNetwork() {
  // Create software timer for reconnect logic
  wifiReconnectTimer = xTimerCreate("wifiTimer", pdMS_TO_TICKS(2000), pdFALSE, (void *)0, [](TimerHandle_t t) { connectToWiFi(); });

  if (!wifiPreferences.begin("wifi-creds", true)) {
    logMessage("NVS Warning: 'wifi-creds' namespace not found (First boot?)");
  }

  String ssidTemp = wifiPreferences.getString("ssid", "");
  String passTemp = wifiPreferences.getString("pass", "");
  wifiPreferences.end();

  strncpy(g_wifiSSID, ssidTemp.c_str(), sizeof(g_wifiSSID));
  strncpy(g_wifiPass, passTemp.c_str(), sizeof(g_wifiPass));

  if (strlen(g_wifiSSID) > 0) {
    logMessage("Found Wi-Fi credentials. Registering events.");
    g_wifiCredentialsExist = true;
    WiFi.onEvent(WiFiEvent);

    connectToWiFi();
  } else {
    // Enter provisioning mode
    logMessage("No Wi-Fi credentials found.");
    startBLEProvisioning();
  }

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

  startMDNS();
}