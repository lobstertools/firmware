/*
 * =================================================================================
 * Project:   Lobster Lock - Self-Bondage Session Manager
 * File:      src/WebManager.cpp
 *
 * Description:
 * Async HTTP Server implementation.
 * Updated to match TypeScript interfaces for SessionConfig, DeviceDetails, SessionStatus.
 * =================================================================================
 */
#include <ArduinoJson.h>
#include <WiFi.h>
#include <string.h>
#include <esp_timer.h> // For uptime

#include "Config.h"
#include "Esp32SessionHAL.h"
#include "SettingsManager.h"
#include "WebManager.h"
#include "WebValidators.h"

// =================================================================================
// SECTION: SINGLETON & INIT
// =================================================================================

WebManager &WebManager::getInstance() {
  static WebManager instance;
  return instance;
}

WebManager::WebManager() : _server(80), _engine(nullptr) {}

void WebManager::begin(SessionEngine *engine) {
  _engine = engine;
  registerEndpoints();
  _server.begin();
  log("WebAPI", "HTTP server started.");
}

void WebManager::log(const char *key, const char *value) { Esp32SessionHAL::getInstance().logKeyValue(key, value); }

// =================================================================================
// SECTION: HELPER FUNCTIONS
// =================================================================================

void WebManager::sendJsonError(AsyncWebServerRequest *request, int code, const std::string &message) {
  JsonDocument doc;
  doc["status"] = "error";
  doc["message"] = message;
  String response;
  serializeJson(doc, response);
  request->send(code, "application/json", response);
}

// =================================================================================
// SECTION: ROUTE REGISTRATION
// =================================================================================

void WebManager::registerEndpoints() {
  // 1. System & Health
  _server.on("/", HTTP_GET, [this](AsyncWebServerRequest *r) { handleRoot(r); });
  _server.on("/health", HTTP_GET, [this](AsyncWebServerRequest *r) { handleHealth(r); });
  _server.on("/keepalive", HTTP_POST, [this](AsyncWebServerRequest *r) { handleKeepAlive(r); });
  _server.on("/reboot", HTTP_POST, [this](AsyncWebServerRequest *r) { handleReboot(r); });
  _server.on("/factory-reset", HTTP_POST, [this](AsyncWebServerRequest *r) { handleFactoryReset(r); });

  // 2. Session Commands
  _server.on("/start-test", HTTP_POST, [this](AsyncWebServerRequest *r) { handleStartTest(r); });
  _server.on("/abort", HTTP_POST, [this](AsyncWebServerRequest *r) { handleAbort(r); });

  // 3. Status & Info
  _server.on("/status", HTTP_GET, [this](AsyncWebServerRequest *r) { handleStatus(r); });
  _server.on("/details", HTTP_GET, [this](AsyncWebServerRequest *r) { handleDetails(r); });
  _server.on("/log", HTTP_GET, [this](AsyncWebServerRequest *r) { handleLog(r); });
  _server.on("/reward", HTTP_GET, [this](AsyncWebServerRequest *r) { handleReward(r); });

  // 4. Body Handlers (Arm & WiFi)
  _server.on(
      "/arm", HTTP_POST, [](AsyncWebServerRequest *r) {}, NULL,
      [this](AsyncWebServerRequest *r, uint8_t *data, size_t len, size_t index, size_t total) { handleArm(r, data, len, index, total); });

  _server.on(
      "/update-wifi", HTTP_POST, [](AsyncWebServerRequest *r) {}, NULL,
      [this](AsyncWebServerRequest *r, uint8_t *data, size_t len, size_t index, size_t total) {
        handleUpdateWifi(r, data, len, index, total);
      });
}

// =================================================================================
// SECTION: SYSTEM HANDLERS
// =================================================================================

void WebManager::handleRoot(AsyncWebServerRequest *request) {
  String html = "<html><head><title>" + String(DEVICE_NAME) + "</title></head><body>";
  html += "<h1>" + String(DEVICE_NAME) + " API</h1>";
  html += "<h2>" + String(DEVICE_VERSION) + "</h2>";
  request->send(200, "text/html", html);
}

void WebManager::handleHealth(AsyncWebServerRequest *request) {
  JsonDocument doc;
  doc["status"] = "ok";
  doc["message"] = "Device is reachable.";
  String response;
  serializeJson(doc, response);
  request->send(200, "application/json", response);
}

void WebManager::handleKeepAlive(AsyncWebServerRequest *request) {
  if (Esp32SessionHAL::getInstance().lockState()) {
    _engine->petWatchdog();
    Esp32SessionHAL::getInstance().unlockState();
    request->send(200);
  } else {
    sendJsonError(request, 503, "System Busy");
  }
}

void WebManager::handleReboot(AsyncWebServerRequest *request) {
  if (Esp32SessionHAL::getInstance().lockState()) {
    DeviceState s = _engine->getState();

    if (s != COMPLETED && s != READY) {
      Esp32SessionHAL::getInstance().unlockState();
      sendJsonError(request, 403, "Reboot denied. Device active.");
      return;
    }

    log("WebAPI", "Reboot requested via API.");
    request->send(200, "application/json", "{\"status\":\"rebooting\"}");

    Esp32SessionHAL::getInstance().unlockState();
    delay(1000);
    ESP.restart();
  } else {
    sendJsonError(request, 503, "System Busy");
  }
}

void WebManager::handleFactoryReset(AsyncWebServerRequest *request) {
  if (Esp32SessionHAL::getInstance().lockState()) {
    DeviceState s = _engine->getState();
    if (s != READY && s != COMPLETED) {
      Esp32SessionHAL::getInstance().unlockState();
      sendJsonError(request, 409, "Cannot reset while active.");
      return;
    }
    log("WebAPI", "Factory Reset initiated.");
    SettingsManager::wipeAll();
    request->send(200, "application/json", "{\"status\":\"resetting\"}");
    Esp32SessionHAL::getInstance().unlockState();
    delay(1000);
    ESP.restart();
  } else {
    sendJsonError(request, 503, "System Busy");
  }
}

// =================================================================================
// SECTION: SESSION CONTROL
// =================================================================================

void WebManager::handleArm(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
  if (index + len != total) return;

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, (const char *)data, len);
  if (error) {
    sendJsonError(request, 400, "Invalid JSON.");
    return;
  }

  SessionConfig intent;
  std::string err;
  uint8_t mask = Esp32SessionHAL::getInstance().getChannelMask();

  // Updated Validator handles the new TypeScript interface structure
  if (!WebValidators::parseSessionConfig(doc, mask, intent, err)) {
    sendJsonError(request, 400, err);
    return;
  }

  if (Esp32SessionHAL::getInstance().lockState()) {
    int result = _engine->startSession(intent);
    Esp32SessionHAL::getInstance().unlockState();

    if (result == 200) {
      request->send(200, "application/json", "{\"status\":\"armed\"}");
    } else {
      sendJsonError(request, result, "Session start failed (Engine rejected).");
    }
  } else {
    sendJsonError(request, 503, "System Busy");
  }
}

void WebManager::handleStartTest(AsyncWebServerRequest *request) {
  if (Esp32SessionHAL::getInstance().lockState()) {
    int result = _engine->startTest();
    Esp32SessionHAL::getInstance().unlockState();
    if (result == 200) {
      request->send(200, "application/json", "{\"status\":\"testing\"}");
    } else {
      sendJsonError(request, 409, "Cannot start test (Not Ready).");
    }
  } else {
    sendJsonError(request, 503, "System Busy");
  }
}

void WebManager::handleAbort(AsyncWebServerRequest *request) {
  if (Esp32SessionHAL::getInstance().lockState()) {
    _engine->abort("API Request");
    DeviceState s = _engine->getState();
    // TS Expects DeviceState enum string: 'ABORTED', 'COMPLETED', 'READY'
    JsonDocument doc;
    if (s == ABORTED) doc["status"] = "ABORTED";
    else if (s == COMPLETED) doc["status"] = "COMPLETED";
    else doc["status"] = "READY";
    
    String rJson;
    serializeJson(doc, rJson);
    Esp32SessionHAL::getInstance().unlockState();
    request->send(200, "application/json", rJson);
  } else {
    sendJsonError(request, 503, "System Busy");
  }
}

// =================================================================================
// SECTION: STATUS & INFO
// =================================================================================

const char *stateToString(DeviceState s) {
  switch (s) {
    case READY: return "READY";
    case ARMED: return "ARMED";
    case LOCKED: return "LOCKED";
    case ABORTED: return "ABORTED";
    case COMPLETED: return "COMPLETED";
    case TESTING: return "TESTING";
    default: return "READY";
  }
}

const char *durTypeToString(DurationType d) {
    switch(d) {
        case DUR_RANDOM: return "DUR_RANDOM";
        case DUR_RANGE_SHORT: return "DUR_RANGE_SHORT";
        case DUR_RANGE_MEDIUM: return "DUR_RANGE_MEDIUM";
        case DUR_RANGE_LONG: return "DUR_RANGE_LONG";
        default: return "DUR_FIXED";
    }
}

void WebManager::handleStatus(AsyncWebServerRequest *request) {
  if (!Esp32SessionHAL::getInstance().lockState()) {
    request->send(503, "text/plain", "Busy");
    return;
  }

  // Snapshot Data
  DeviceState s = _engine->getState();
  SessionTimers t = _engine->getTimers();
  SessionStats stats = _engine->getStats();
  SessionConfig cfg = _engine->getActiveConfig();
  
  // Hardware reading
  bool btnPressed = Esp32SessionHAL::getInstance().isButtonPressed();
  int rssi = WiFi.RSSI();
  uint32_t heap = ESP.getFreeHeap();
  float temp = temperatureRead();
  int64_t uptime = esp_timer_get_time() / 1000; // micro to milli
  bool verified = Esp32SessionHAL::getInstance().isSafetyInterlockEngaged();
  uint32_t currentPressDurationMs = Esp32SessionHAL::getInstance().getCurrentPressDurationMs();

  Esp32SessionHAL::getInstance().unlockState();

  AsyncResponseStream *response = request->beginResponseStream("application/json");
  JsonDocument doc;

  // 1. Root Status
  doc["state"] = stateToString(s);
  doc["verified"] = verified;

  // 2. Config Echo (Matching SessionConfig Interface)
  JsonObject cObj = doc["config"].to<JsonObject>();
  cObj["durationType"] = durTypeToString(cfg.durationType);
  cObj["durationFixed"] = cfg.durationFixed;
  cObj["durationMin"] = cfg.durationMin;
  cObj["durationMax"] = cfg.durationMax;
  cObj["triggerStrategy"] = (cfg.triggerStrategy == STRAT_BUTTON_TRIGGER) ? "STRAT_BUTTON_TRIGGER" : "STRAT_AUTO_COUNTDOWN";
  cObj["hideTimer"] = cfg.hideTimer;
  cObj["disableLED"] = cfg.disableLED;
  
  JsonArray cDelays = cObj["channelDelays"].to<JsonArray>();
  for(int i=0; i<4; i++) cDelays.add(cfg.channelDelays[i]);

  // 3. Timers (Matching SessionTimers Interface)
  JsonObject tObj = doc["timers"].to<JsonObject>();
  tObj["lockDuration"] = t.lockDuration;
  tObj["penaltyDuration"] = t.penaltyDuration;
  tObj["lockRemaining"] = t.lockRemaining;
  tObj["penaltyRemaining"] = t.penaltyRemaining;
  tObj["testRemaining"] = t.testRemaining;
  tObj["triggerTimeout"] = t.triggerTimeout;
  
  JsonArray tDelays = tObj["channelDelays"].to<JsonArray>();
  for(int i=0; i<4; i++) tDelays.add(t.channelDelays[i]);

  // 4. Stats
  JsonObject sObj = doc["stats"].to<JsonObject>();
  sObj["streaks"] = stats.streaks;
  sObj["completed"] = stats.completed;
  sObj["aborted"] = stats.aborted;
  sObj["paybackAccumulated"] = stats.paybackAccumulated;
  sObj["totalLockedTime"] = stats.totalLockedTime;

  // 5. Telemetry
  JsonObject tel = doc["telemetry"].to<JsonObject>();
  tel["buttonPressed"] = btnPressed;
  tel["currentPressDurationMs"] = currentPressDurationMs;
  tel["rssi"] = rssi;
  tel["freeHeap"] = heap;
  tel["uptime"] = uptime;
  if(isnan(temp)) tel["internalTempC"] = "N/A";
  else tel["internalTempC"] = temp;

  serializeJson(doc, *response);
  request->send(response);
}

// =================================================================================
// SECTION: DEVICE DETAILS
// =================================================================================

void WebManager::handleDetails(AsyncWebServerRequest *request) {
  // 1. Prepare Identification
  uint8_t macRaw[6];
  esp_efuse_mac_get_default(macRaw);
  char idBuf[32];
  snprintf(idBuf, sizeof(idBuf), "lobster-lock-%02X%02X%02X", macRaw[3], macRaw[4], macRaw[5]);

  JsonDocument doc;
  
  // -- Root ID
  doc["id"] = idBuf;

  // -- Identity Interface
  JsonObject identity = doc["identity"].to<JsonObject>();
  identity["name"] = DEVICE_NAME;
  identity["version"] = DEVICE_VERSION;
#ifdef DEBUG_MODE
  identity["buildType"] = "debug";
#else
  identity["buildType"] = "release";
#endif
  identity["buildDate"] = __DATE__;
  identity["buildTime"] = __TIME__;
  identity["cppStandard"] = __cplusplus;

  // -- Network Interface
  JsonObject net = doc["network"].to<JsonObject>();
  net["ssid"] = WiFi.SSID();
  net["rssi"] = WiFi.RSSI();
  net["mac"] = WiFi.macAddress();
  net["ip"] = WiFi.localIP().toString();
  net["subnetMask"] = WiFi.subnetMask().toString();
  net["gateway"] = WiFi.gatewayIP().toString();
  net["hostname"] = WiFi.getHostname();
  net["port"] = 80;

  // -- Features
  JsonArray features = doc["features"].to<JsonArray>();
  features.add("footPedal");
  features.add("startCountdown");
  features.add("statusLed");

  // -- Channels
  JsonObject chans = doc["channels"].to<JsonObject>();
  chans["ch1"] = Esp32SessionHAL::getInstance().isChannelEnabled(0);
  chans["ch2"] = Esp32SessionHAL::getInstance().isChannelEnabled(1);
  chans["ch3"] = Esp32SessionHAL::getInstance().isChannelEnabled(2);
  chans["ch4"] = Esp32SessionHAL::getInstance().isChannelEnabled(3);

  // -- Retrieve Config Data
  if (Esp32SessionHAL::getInstance().lockState()) {
    const SessionPresets &presets = _engine->getPresets();
    const DeterrentConfig &det = _engine->getDeterrents();

    // -- Presets Interface
    JsonObject p = doc["presets"].to<JsonObject>();
    p["shortMin"] = presets.shortMin;
    p["shortMax"] = presets.shortMax;
    p["mediumMin"] = presets.mediumMin;
    p["mediumMax"] = presets.mediumMax;
    p["longMin"] = presets.longMin;
    p["longMax"] = presets.longMax;
    p["minSessionDuration"] = presets.minSessionDuration; 
    p["maxSessionDuration"] = presets.maxSessionDuration;

    // -- DeterrentConfig Interface
    JsonObject d = doc["deterrentConfig"].to<JsonObject>();
    d["enableStreaks"] = det.enableStreaks;
    d["enableRewardCode"] = det.enableRewardCode;
    d["rewardPenaltyStrategy"] = (det.rewardPenaltyStrategy == DETERRENT_FIXED) ? "DETERRENT_FIXED" : "DETERRENT_RANDOM";
    d["rewardPenaltyMin"] = det.rewardPenaltyMin;
    d["rewardPenaltyMax"] = det.rewardPenaltyMax;
    d["rewardPenalty"] = det.rewardPenalty;
    d["enablePaybackTime"] = det.enablePaybackTime;
    d["paybackTimeStrategy"] = (det.paybackTimeStrategy == DETERRENT_FIXED) ? "DETERRENT_FIXED" : "DETERRENT_RANDOM";
    d["paybackTimeMin"] = det.paybackTimeMin; 
    d["paybackTimeMax"] = det.paybackTimeMax;
    d["paybackTime"] = det.paybackTime;

    Esp32SessionHAL::getInstance().unlockState();
  } else {
    sendJsonError(request, 503, "System Busy");
    return;
  }

  // -- System Defaults (Mocking or retrieving from constants if available)
  JsonObject def = doc["defaults"].to<JsonObject>();
  def["longPressDuration"] = 2000; 
  def["extButtonSignalDuration"] = 50;
  def["testModeDuration"] = 10000;
  def["keepAliveInterval"] = 5000;
  def["wifiMaxRetries"] = 10;
  def["armedTimeoutSeconds"] = 300;

  String response;
  serializeJson(doc, response);
  request->send(200, "application/json", response);
}

void WebManager::handleLog(AsyncWebServerRequest *request) {
  AsyncResponseStream *response = request->beginResponseStream("text/plain");
  Esp32SessionHAL &hal = Esp32SessionHAL::getInstance();

  for (int i = 0; i < LOG_BUFFER_SIZE; i++) {
    if (hal.lockState()) {
      const char *line = hal.getLogLine(i);
      char lineBuf[256];
      strncpy(lineBuf, line, sizeof(lineBuf));
      lineBuf[sizeof(lineBuf) - 1] = '\0';
      hal.unlockState();

      if (strlen(lineBuf) > 0) {
        response->print(lineBuf);
        response->print("\n");
      }
    } else {
      response->print("[Busy]\n");
    }
  }
  request->send(response);
}

void WebManager::handleReward(AsyncWebServerRequest *request) {
  if (Esp32SessionHAL::getInstance().lockState()) {
    const Reward *history = _engine->getRewardHistory();
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    for (int i = 0; i < REWARD_HISTORY_SIZE; i++) {
      if (strlen(history[i].code) > 0) {
        JsonObject r = arr.add<JsonObject>();
        r["code"] = history[i].code;
        r["checksum"] = history[i].checksum;
      }
    }
    String rJson;
    serializeJson(doc, rJson);
    Esp32SessionHAL::getInstance().unlockState();
    request->send(200, "application/json", rJson);
  } else {
    sendJsonError(request, 503, "Busy");
  }
}

// =================================================================================
// SECTION: CONFIGURATION
// =================================================================================

void WebManager::handleUpdateWifi(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
  if (index + len != total) return;

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, (const char *)data, len);

  if (error) {
    sendJsonError(request, 400, "Invalid JSON.");
    return;
  }

  const char *ssid = doc["ssid"];
  const char *pass = doc["pass"];
  std::string err;

  if (!WebValidators::validateWifiCredentials(ssid, pass, err)) {
    sendJsonError(request, 400, err);
    return;
  }

  SettingsManager::setWifiSSID(ssid);
  SettingsManager::setWifiPassword(pass);

  request->send(200, "application/json", "{\"status\":\"saved\", \"message\":\"Reboot to apply.\"}");
}