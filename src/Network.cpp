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

// =================================================================================
// SECTION: CONSTANTS & UUIDS
// =================================================================================

// Base UUID: 5a160000-8334-469b-a316-c340cf29188f
#define PROV_SERVICE_UUID "5a160000-8334-469b-a316-c340cf29188f"

// WiFi Credentials
#define PROV_SSID_CHAR_UUID "5a160001-8334-469b-a316-c340cf29188f"
#define PROV_PASS_CHAR_UUID "5a160002-8334-469b-a316-c340cf29188f"

// Session Config
#define PROV_ENABLE_REWARD_CODE_CHAR_UUID "5a160003-8334-469b-a316-c340cf29188f"
#define PROV_ENABLE_STREAKS_CHAR_UUID "5a160004-8334-469b-a316-c340cf29188f"
#define PROV_ENABLE_PAYBACK_TIME_CHAR_UUID "5a160005-8334-469b-a316-c340cf29188f"
#define PROV_PAYBACK_TIME_CHAR_UUID "5a160006-8334-469b-a316-c340cf29188f"

// Hardware Config (Channel Enable Mask)
#define PROV_CH1_ENABLE_UUID "5a16000a-8334-469b-a316-c340cf29188f"
#define PROV_CH2_ENABLE_UUID "5a16000b-8334-469b-a316-c340cf29188f"
#define PROV_CH3_ENABLE_UUID "5a16000d-8334-469b-a316-c340cf29188f"
#define PROV_CH4_ENABLE_UUID "5a16000c-8334-469b-a316-c340cf29188f"

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

  logMessage("WiFi: Connecting...");
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
    snprintf(logBuf, sizeof(logBuf), "WiFi: Connected. IP: %s", WiFi.localIP().toString().c_str());
    logMessage(logBuf);

    g_wifiRetries = 0; // Reset counter on success
    if (wifiReconnectTimer != NULL) {
      xTimerStop(wifiReconnectTimer, 0);
    }
    break;
  }
  case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
    // Check if we have exceeded max retries
    if (g_wifiRetries >= g_systemConfig.wifiMaxRetries) {
      logMessage("WiFi: Max retries exceeded. Falling back to BLE Provisioning...");

      // Stop any pending reconnect timers
      if (wifiReconnectTimer != NULL) {
        xTimerStop(wifiReconnectTimer, 0);
      }

      // Set flag for the main loop to handle (safest way to switch contexts from ISR/Event)
      g_triggerProvisioning = true;
    } else {
      char logBuf[64];
      snprintf(logBuf, sizeof(logBuf), "WiFi: Disconnected (Attempt %d/%d). Retrying...", g_wifiRetries + 1, g_systemConfig.wifiMaxRetries);
      logMessage(logBuf);

      g_wifiRetries++;
      // Wait 2 seconds before retrying
      if (wifiReconnectTimer != NULL) {
        xTimerStart(wifiReconnectTimer, 0);
      }
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
  char uniqueHostname[30];
  snprintf(uniqueHostname, sizeof(uniqueHostname), "lobster-lock-%02X%02X%02X", mac[3], mac[4], mac[5]);

  // Set the unique hostname (e.g., lobster-lock-AABBCC.local)
  if (!MDNS.begin(uniqueHostname)) {
    logMessage("ERROR: Failed to set up mDNS responder!");
    return;
  }
  // Announce the service your NodeJS app will look for
  MDNS.addService("lobster-lock", "tcp", 80);
  MDNS.addServiceTxt("lobster-lock", "tcp", "mac", WiFi.macAddress().c_str());

  char logBuf[64];
  snprintf(logBuf, sizeof(logBuf), "mDNS active: %s.local", uniqueHostname);
  logMessage(logBuf);
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

    // --- 1. WiFi Credentials ---
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
      g_credentialsReceived = true; // Trigger restart
    }

    // --- 2. Global Session Config ---
    else if (uuid == PROV_ENABLE_REWARD_CODE_CHAR_UUID) {
      sessionState.begin("session", false);
      sessionState.putBool("enableCode", (bool)data[0]);
      sessionState.end();
      logMessage("BLE: Received Enable Reward Code");
    } else if (uuid == PROV_ENABLE_STREAKS_CHAR_UUID) {
      sessionState.begin("session", false);
      sessionState.putBool("enableStreaks", (bool)data[0]);
      sessionState.end();
      logMessage("BLE: Received Enable Streaks");
    } else if (uuid == PROV_ENABLE_PAYBACK_TIME_CHAR_UUID) {
      sessionState.begin("session", false);
      sessionState.putBool("enablePayback", (bool)data[0]);
      sessionState.end();
      logMessage("BLE: Received Enable Payback Time");
    } else if (uuid == PROV_PAYBACK_TIME_CHAR_UUID) {
      sessionState.begin("session", false);
      uint32_t val = bytesToUint32(data);
      // Clamp value
      if (val < g_systemConfig.minPaybackTimeSeconds)
        val = g_systemConfig.minPaybackTimeSeconds;
      if (val > g_systemConfig.maxPaybackTimeSeconds)
        val = g_systemConfig.maxPaybackTimeSeconds;
      sessionState.putUInt("paybackSeconds", val);
      sessionState.end();
      logMessage("BLE: Received Payback Time");
    }

    // --- 3. Hardware Config (Channel Mask) ---
    else {
      provisioningPrefs.begin("provisioning", false);
      uint8_t currentMask = provisioningPrefs.getUChar("chMask", 0x0F);
      bool enable = (bool)data[0];

      if (uuid == PROV_CH1_ENABLE_UUID)
        enable ? currentMask |= (1 << 0) : currentMask &= ~(1 << 0);
      else if (uuid == PROV_CH2_ENABLE_UUID)
        enable ? currentMask |= (1 << 1) : currentMask &= ~(1 << 1);
      else if (uuid == PROV_CH3_ENABLE_UUID)
        enable ? currentMask |= (1 << 2) : currentMask &= ~(1 << 2);
      else if (uuid == PROV_CH4_ENABLE_UUID)
        enable ? currentMask |= (1 << 3) : currentMask &= ~(1 << 3);

      provisioningPrefs.putUChar("chMask", currentMask);
      provisioningPrefs.end();
      g_enabledChannelsMask = currentMask; // Update runtime global
      logMessage("BLE: Updated Channel Mask");
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

    statusLed.Update();   // Keep the LED pattern running
    esp_task_wdt_reset(); // Feed the watchdog
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
void waitForNetwork() {
  // Create software timer for reconnect logic
  wifiReconnectTimer = xTimerCreate("wifiTimer", pdMS_TO_TICKS(2000), pdFALSE, (void *)0, [](TimerHandle_t t) { connectToWiFi(); });

  // 1. Load Credentials
  if (!wifiPreferences.begin("wifi-creds", true)) {
    logMessage("NVS Warning: 'wifi-creds' namespace not found (First boot?)");
  }
  String ssidTemp = wifiPreferences.getString("ssid", "");
  String passTemp = wifiPreferences.getString("pass", "");
  wifiPreferences.end();

  strncpy(g_wifiSSID, ssidTemp.c_str(), sizeof(g_wifiSSID));
  strncpy(g_wifiPass, passTemp.c_str(), sizeof(g_wifiPass));

  // 2. Decide: Connect or Provision
  if (strlen(g_wifiSSID) > 0) {
    logMessage("Found Wi-Fi credentials. Registering events.");
    g_wifiCredentialsExist = true;
    WiFi.onEvent(WiFiEvent);
    connectToWiFi();
  } else {
    logMessage("No Wi-Fi credentials found.");
    // Fallthrough to BLE Provisioning below
  }

  // 3. Wait for IP (Blocking with Timeout)
  if (g_wifiCredentialsExist) {
    logMessage("Boot: Waiting for IP address...");
    unsigned long wifiWaitStart = millis();

    WiFi.setSleep(false); // Stay awake

    // Wait up to 30 seconds specifically for the IP
    while (WiFi.status() != WL_CONNECTED && (millis() - wifiWaitStart < 30000)) {
      processLogQueue();
      esp_task_wdt_reset();
      delay(100);
    }

    if (WiFi.status() == WL_CONNECTED) {
      startMDNS();
      return; // Success! Return to main setup.
    } else {
      logMessage("Boot: WiFi connection timed out.");
      // Fallthrough to BLE Provisioning below
    }
  }

  // 4. Fallback: Blocking BLE Provisioning
  // If we reach here, we either had no creds, or timed out.
  logMessage("Entering BLE Provisioning Fallback.");
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  startBLEProvisioning(); // This function does not return
}