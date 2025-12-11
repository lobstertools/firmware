/*
 * =================================================================================
 * Project:   Lobster Lock - Self-Bondage Session Manager
 * File:      src/WebManager.cpp
 *
 * Description:
 * Async HTTP Server implementation.
 * Refactor:
 * - Modular architecture (WebManager class).
 * - Full input validation and field mapping.
 * - Uses SettingsManager for NVS operations.
 * - Uses HAL for thread safety (Mutex).
 * =================================================================================
 */
#include <ArduinoJson.h>
#include <WiFi.h>
#include <string.h>

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

// Local helper to shorten HAL log calls
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
  html += "<ul>";
  html += "<li><b>GET /status</b> - Real-time metrics.</li>";
  html += "<li><b>GET /details</b> - Device configuration.</li>";
  html += "<li><b>GET /log</b> - Internal logs.</li>";
  html += "<li><b>POST /arm</b> - Begin session (JSON).</li>";
  html += "<li><b>POST /abort</b> - Emergency stop.</li>";
  html += "</ul></body></html>";
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
  // HAL locking ensures thread safety for the watchdog update
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

    // Send response before reboot
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
  if (index + len != total)
    return;

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, (const char *)data, len);
  if (error) {
    sendJsonError(request, 400, "Invalid JSON.");
    return;
  }

  SessionConfig intent;
  std::string err;
  uint8_t mask = Esp32SessionHAL::getInstance().getChannelMask(); // Get mask from HAL

  // Use Static Validator
  if (!WebValidators::parseSessionConfig(doc, mask, intent, err)) {
    sendJsonError(request, 400, err);
    return;
  }

  // Execution
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

    JsonDocument doc;
    DeviceState s = _engine->getState();
    doc["status"] = (s == ABORTED) ? "aborted" : (s == COMPLETED ? "completed" : "ready");
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

/**
 * Converts a DeviceState enum to its string representation.
 */
const char *stateToString(DeviceState s) {
  switch (s) {
  case READY:
    return "ready";
  case ARMED:
    return "armed";
  case LOCKED:
    return "locked";
  case ABORTED:
    return "aborted";
  case COMPLETED:
    return "completed";
  case TESTING:
    return "testing";
  default:
    return "unknown";
  }
}

void WebManager::handleStatus(AsyncWebServerRequest *request) {
  // Quick Lock to snapshot state
  if (!Esp32SessionHAL::getInstance().lockState()) {
    request->send(503, "text/plain", "Busy");
    return;
  }

  // Snapshot Data locally to minimize lock time
  DeviceState s = _engine->getState();
  SessionTimers t = _engine->getTimers();
  SessionStats stats = _engine->getStats();
  SessionConfig cfg = _engine->getActiveConfig();

  int rssi = WiFi.RSSI();
  uint32_t heap = ESP.getFreeHeap();
  float temp = temperatureRead();

  Esp32SessionHAL::getInstance().unlockState();

  // Build JSON (Heavy operation done unlocked)
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  JsonDocument doc;

  doc["status"] = stateToString(s);
  doc["lockDuration"] = t.lockDuration;

  JsonObject timers = doc["timers"].to<JsonObject>();
  timers["lockRemaining"] = t.lockRemaining;
  timers["rewardRemaining"] = t.penaltyRemaining;
  timers["testRemaining"] = t.testRemaining;

  JsonObject cObj = doc["config"].to<JsonObject>();
  cObj["hideTimer"] = cfg.hideTimer;

  // Config Echo
  if (cfg.triggerStrategy == STRAT_BUTTON_TRIGGER)
    cObj["triggerStrategy"] = "buttonTrigger";
  else
    cObj["triggerStrategy"] = "autoCountdown";

  JsonObject st = doc["stats"].to<JsonObject>();
  st["streaks"] = stats.streaks;
  st["completed"] = stats.completed;
  st["totalTimeLocked"] = stats.totalLockedTime;

  JsonObject hw = doc["hardware"].to<JsonObject>();
  hw["rssi"] = rssi;
  hw["freeHeap"] = heap;
  hw["internalTempC"] = isnan(temp) ? 0.0 : temp;

  serializeJson(doc, *response);
  request->send(response);
}

// =================================================================================
// SECTION: STATUS & INFO
// =================================================================================

void WebManager::handleDetails(AsyncWebServerRequest *request) {
  // 1. Prepare Identification Data
  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);
  char idBuf[32];
  snprintf(idBuf, sizeof(idBuf), "lobster-lock-%02X%02X%02X", mac[3], mac[4], mac[5]);

  JsonDocument doc;
  doc["id"] = idBuf;
  doc["name"] = DEVICE_NAME;
  doc["version"] = DEVICE_VERSION;
  doc["address"] = WiFi.localIP().toString();
  doc["mac"] = WiFi.macAddress();
  doc["port"] = 80;

#ifdef DEBUG_MODE
  doc["buildType"] = "debug";
#else
  doc["buildType"] = "release";
#endif

  // 2. Features List
  JsonArray features = doc["features"].to<JsonArray>();
  features.add("footPedal");
  features.add("statusLed");
  features.add("wifiConfig");

  // 3. Retrieve Config from Engine (Thread Safe)
  if (Esp32SessionHAL::getInstance().lockState()) {
    const SessionPresets &presets = _engine->getPresets();
    const DeterrentConfig &det = _engine->getDeterrents();

    // --- Session Presets ---
    JsonObject p = doc["sessionPresets"].to<JsonObject>();
    // Generators
    p["shortMin"] = presets.shortMin;
    p["shortMax"] = presets.shortMax;
    p["mediumMin"] = presets.mediumMin;
    p["mediumMax"] = presets.mediumMax;
    p["longMin"] = presets.longMin;
    p["longMax"] = presets.longMax;
    // Ranges
    p["penaltyMin"] = presets.penaltyMin;
    p["penaltyMax"] = presets.penaltyMax;
    p["paybackMin"] = presets.paybackMin;
    p["paybackMax"] = presets.paybackMax;
    // Safety Ceilings
    p["limitLockMax"] = presets.limitLockMax;
    p["limitPenaltyMax"] = presets.limitPenaltyMax;
    p["limitPaybackMax"] = presets.limitPaybackMax;
    // Absolute Floors
    p["minLockDuration"] = presets.minLockDuration;
    p["minRewardPenaltyDuration"] = presets.minRewardPenaltyDuration;
    p["minPaybackTime"] = presets.minPaybackTime;

    // --- Deterrents ---
    JsonObject d = doc["deterrents"].to<JsonObject>();
    d["enableStreaks"] = det.enableStreaks;
    d["enableRewardCode"] = det.enableRewardCode;
    d["penaltyStrategy"] = (det.penaltyStrategy == DETERRENT_FIXED) ? "fixed" : "random";
    d["rewardPenalty"] = det.rewardPenalty;
    d["enablePaybackTime"] = det.enablePaybackTime;
    d["paybackStrategy"] = (det.paybackStrategy == DETERRENT_FIXED) ? "fixed" : "random";
    d["paybackTime"] = det.paybackTime;

    Esp32SessionHAL::getInstance().unlockState();
  } else {
    sendJsonError(request, 503, "System Busy");
    return;
  }

  // 4. Channel Configuration (from Globals)
  JsonObject c = doc["channels"].to<JsonObject>();
  c["ch1"] = Esp32SessionHAL::getInstance().isChannelEnabled(0);
  c["ch2"] = Esp32SessionHAL::getInstance().isChannelEnabled(1);
  c["ch3"] = Esp32SessionHAL::getInstance().isChannelEnabled(2);
  c["ch4"] = Esp32SessionHAL::getInstance().isChannelEnabled(3);

  String response;
  serializeJson(doc, response);
  request->send(200, "application/json", response);
}

void WebManager::handleLog(AsyncWebServerRequest *request) {
  AsyncResponseStream *response = request->beginResponseStream("text/plain");

  // HAL Interaction
  Esp32SessionHAL &hal = Esp32SessionHAL::getInstance();

  // Iterate and Stream
  // We lock briefly per line to ensure thread safety while streaming
  // without blocking the system for the entire transfer.
  for (int i = 0; i < LOG_BUFFER_SIZE; i++) {
    if (hal.lockState()) {
      const char *line = hal.getLogLine(i);
      // Copy line to stack to send outside lock
      char lineBuf[256];
      strncpy(lineBuf, line, sizeof(lineBuf));
      lineBuf[sizeof(lineBuf) - 1] = '\0';

      hal.unlockState();

      if (strlen(lineBuf) > 0) {
        response->print(lineBuf);
        response->print("\n");
      }
    } else {
      // Failed to get lock, skip line or retry
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
  if (index + len != total)
    return;

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, (const char *)data, len);

  if (error) {
    sendJsonError(request, 400, "Invalid JSON.");
    return;
  }

  const char *ssid = doc["ssid"];
  const char *pass = doc["pass"];
  std::string err;

  // Use Static Validator
  if (!WebValidators::validateWifiCredentials(ssid, pass, err)) {
    sendJsonError(request, 400, err);
    return;
  }

  SettingsManager::setWifiSSID(ssid);
  SettingsManager::setWifiPassword(pass);

  request->send(200, "application/json", "{\"status\":\"saved\", \"message\":\"Reboot to apply.\"}");
}