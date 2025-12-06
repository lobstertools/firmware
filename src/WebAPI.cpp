/*
 * =================================================================================
 * Project:   Lobster Lock - Self-Bondage Session Manager
 * File:      WebAPI.h / WebAPI.cpp
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Description:
 * Async HTTP Server implementation. Defines RESTful JSON endpoints for
 * controlling the device (Arm/Abort), retrieving status/logs, and
 * configuring settings via a web client.
 * =================================================================================
 */
#include <ArduinoJson.h>
#include <WiFi.h>

#include "Globals.h"
#include "Hardware.h"
#include "Logger.h"
#include "Session.h"
#include "Storage.h"
#include "Utils.h"
#include "WebAPI.h"

// =================================================================================
// SECTION: HELPER FUNCTIONS
// =================================================================================

/**
 * Helper function to send a standardized JSON error response.
 */
void sendJsonError(AsyncWebServerRequest *request, int code, const String &message) {
  // RAII: Automatically freed on scope exit
  std::unique_ptr<JsonDocument> doc(new JsonDocument());
  (*doc)["status"] = "error";
  (*doc)["message"] = message;
  String response;
  serializeJson(*doc, response);
  // doc deleted automatically here
  request->send(code, "application/json", response);
}

// =================================================================================
// SECTION: CORE SYSTEM HANDLERS (Root, Health, KeepAlive)
// =================================================================================

/**
 * Handler for GET /
 * Returns an HTML list of all API endpoints.
 */
void handleRoot(AsyncWebServerRequest *request) {
  String html = "<html><head><title>" + String(DEVICE_NAME) + "</title></head><body>";
  html += "<h1>" + String(DEVICE_NAME) + " API</h1>";
  html += "<h2>" + String(DEVICE_VERSION) + " API</h2>";
  html += "<ul>";
  html += "<li><b>GET /status</b> - Real-time metrics (SessionStatus).</li>";
  html += "<li><b>GET /details</b> - Device configuration (DeviceDetails).</li>";
  html += "<li><b>GET /log</b> - Internal system logs.</li>";
  html += "<li><b>GET /reward</b> - Retrieve past unlock codes.</li>";
  html += "<li><b>GET /health</b> - Simple connectivity check.</li>";
  html += "<li><b>POST /arm</b> - Begin session (SessionConfig required).</li>";
  html += "<li><b>POST /start-test</b> - Run hardware test.</li>";
  html += "<li><b>POST /abort</b> - Emergency stop (triggers penalty).</li>";
  html += "<li><b>POST /keepalive</b> - Reset connection watchdog.</li>";
  html += "<li><b>POST /update-wifi</b> - Update credentials.</li>";
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

    // "Pet" the watchdog only if the state is critical
    if (g_currentState == LOCKED || g_currentState == TESTING || g_currentState == ARMED) {
      g_lastKeepAliveTime = millis();

      // Log if we are recovering from missed checks
      if (g_currentKeepAliveStrikes > 0) {
        char logBuf[100];
        snprintf(logBuf, sizeof(logBuf), "Keep-Alive Watchdog Signal received. Resetting %d strikes.", g_currentKeepAliveStrikes);
        logKeyValue("Session", logBuf);
      }

      g_currentKeepAliveStrikes = 0; // Reset strike counter
    }
    xSemaphoreGiveRecursive(stateMutex);
  }
  request->send(200);
}

/**
 * Handler for POST /reboot
 * Only allows reboot if the session is COMPLETED.
 */
void handleReboot(AsyncWebServerRequest *request) {
  if (xSemaphoreTakeRecursive(stateMutex, (TickType_t)pdMS_TO_TICKS(1000)) == pdTRUE) {
    
    // You cannot software-reboot out of an active lock.
    if (g_currentState != COMPLETED && g_currentState != READY) {
      xSemaphoreGiveRecursive(stateMutex);
      sendJsonError(request, 403, "Reboot denied. Device is active/locked. Use physical disconnect to abort.");
      return;
    }
    
    // If we are here, state is COMPLETED or READY. 
    // Proceed with safe reboot to clear memory.
    logKeyValue("WebAPI", "User requested safe system reboot.");
    
    // Send success response immediately
    request->send(200, "application/json", "{\"status\":\"rebooting\", \"message\":\"Rebooting to clear memory session...\"}");
    
    xSemaphoreGiveRecursive(stateMutex);
    
    // Delay slightly to ensure the HTTP response flushes to the client
    delay(500);
    ESP.restart();
    
  } else {
    sendJsonError(request, 503, "System Busy");
  }
}

// =================================================================================
// SECTION: SESSION CONTROL HANDLERS (Arm, Abort, Test)
// =================================================================================

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

  // ALLOCATE ON HEAP via RAII
  std::unique_ptr<JsonDocument> doc(new JsonDocument());

  DeserializationError error = deserializeJson(*doc, (const char *)data, len);
  if (error) {
    char logBuf[100];
    snprintf(logBuf, sizeof(logBuf), "Failed to parse /arm JSON: %s", error.c_str());
    logKeyValue("WebAPI", logBuf);
    sendJsonError(request, 400, "Invalid JSON body.");
    return;
  }

  // --- 1. PENALTY RESOLUTION ---
  // Penalty is no longer passed in the API. It is hardcoded from Provisioning settings.
  unsigned long resolvedPenalty = g_deterrentConfig.rewardPenalty;

  // --- 2. DURATION RESOLUTION ---
  // SessionConfig interface
  const char *durTypeStr = (*doc)["durationType"] | "fixed";
  DurationType durType = DUR_FIXED;

  if (String(durTypeStr) == "random")
    durType = DUR_RANDOM;
  else if (String(durTypeStr) == "time-range" || String(durTypeStr) == "short" || String(durTypeStr) == "medium" ||
           String(durTypeStr) == "long")
    durType = DUR_RANGE;

  unsigned long finalDuration = 0;
  unsigned long inputMin = (*doc)["durationMin"] | 0;
  unsigned long inputMax = (*doc)["durationMax"] | 0;

  // Ensure randomness seed is updated per request based on arrival time
  randomSeed(micros());

  if (durType == DUR_FIXED) {
    // Use the 'duration' field
    finalDuration = (*doc)["duration"] | 0;
    if (finalDuration == 0) {
      sendJsonError(request, 400, "Missing required field: duration (for fixed type).");
      return;
    }
    // Sync min/max for persistence consistency
    inputMin = finalDuration;
    inputMax = finalDuration;
  } else {
    // Random or Range
    // Validate inputs
    if (inputMax < inputMin) {
      // Swap or clamp
      unsigned long temp = inputMax;
      inputMax = inputMin;
      inputMin = temp;
    }

    if (inputMin == 0)
      inputMin = g_sessionLimits.minLockDuration;
    if (inputMax == 0)
      inputMax = inputMin + 60; // Fallback

    // Calculate Duration       // Arduino random(min, max) is exclusive of max, so we add 1
    finalDuration = random(inputMin, inputMax + 1);
  }

  bool newHideTimer = (*doc)["hideTimer"] | false;

  // Parse Strategy
  String stratStr = (*doc)["triggerStrategy"] | "autoCountdown";
  TriggerStrategy requestedStrat = STRAT_AUTO_COUNTDOWN;
  if (stratStr == "buttonTrigger") {
    requestedStrat = STRAT_BUTTON_TRIGGER;
  }

  unsigned long tempDelays[4] = {0};

  // Parse channels from nested 'channelDelays' object (Note: TS interface uses 'channelDelays')
  if ((*doc)["channelDelays"].is<JsonObject>()) {
    JsonObject delaysObj = (*doc)["channelDelays"];
    if (delaysObj["ch1"].is<unsigned long>())
      tempDelays[0] = delaysObj["ch1"];
    if (delaysObj["ch2"].is<unsigned long>())
      tempDelays[1] = delaysObj["ch2"];
    if (delaysObj["ch3"].is<unsigned long>())
      tempDelays[2] = delaysObj["ch3"];
    if (delaysObj["ch4"].is<unsigned long>())
      tempDelays[3] = delaysObj["ch4"];
  }

  // Filter delays by enabled mask (Hardware check)
  for (int i = 0; i < 4; i++) {
    if (!((g_enabledChannelsMask >> i) & 1)) {
      tempDelays[i] = 0;
    }
  }

  // Prepare response buffer
  String responseJson;

  // We lock briefly ONLY to update the state machine.
  if (xSemaphoreTakeRecursive(stateMutex, (TickType_t)pdMS_TO_TICKS(1000)) == pdTRUE) {

    logKeyValue("WebAPI", "/arm");
    char logBuf[100];
    snprintf(logBuf, sizeof(logBuf), "Resolved Duration: %lu s (Type: %s)", finalDuration, durTypeStr);
    logKeyValue("WebAPI", logBuf);

    int result = startSession(finalDuration, resolvedPenalty, requestedStrat, tempDelays, newHideTimer);

    // If successful, update the Metadata fields in Active Config so /status echoes them back
    if (result == 200) {
      g_activeSessionConfig.durationType = durType;
      g_activeSessionConfig.durationMin = inputMin;
      g_activeSessionConfig.durationMax = inputMax;
    }

    xSemaphoreGiveRecursive(stateMutex); // RELEASE LOCK

    // Handle Result
    if (result == 200) {
      std::unique_ptr<JsonDocument> responseDoc(new JsonDocument());
      (*responseDoc)["status"] = "armed";
      (*responseDoc)["triggerStrategy"] = (requestedStrat == STRAT_BUTTON_TRIGGER) ? "buttonTrigger" : "autoCountdown";
      serializeJson(*responseDoc, responseJson);
      request->send(200, "application/json", responseJson);
    } else if (result == 409) {
      sendJsonError(request, 409, "Device is not ready.");
    } else {
      // 400
      sendJsonError(request, 400, "Invalid configuration values (Duration/Penalty out of limits).");
    }

  } else {
    sendJsonError(request, 503, "System Busy");
  }
}

/**
 * Handler for POST /start-test
 */
void handleStartTest(AsyncWebServerRequest *request) {
  String responseJson;

  if (xSemaphoreTakeRecursive(stateMutex, (TickType_t)pdMS_TO_TICKS(1000)) == pdTRUE) {

    logKeyValue("WebAPI", "/start-test");

    int result = startTestSession();

    xSemaphoreGiveRecursive(stateMutex);

    // Handle Result
    if (result == 200) {
      std::unique_ptr<JsonDocument> doc(new JsonDocument());
      (*doc)["status"] = "testing";
      (*doc)["testSecondsRemaining"] = g_systemDefaults.testModeDuration; // Use System Default
      serializeJson(*doc, responseJson);
      request->send(200, "application/json", responseJson);
    } else {
      sendJsonError(request, 409, "Device must be in READY state to run test.");
    }

  } else {
    sendJsonError(request, 503, "System Busy");
  }
}

/**
 * Handler for POST /abort
 */
void handleAbort(AsyncWebServerRequest *request) {
  String responseJson;
  if (xSemaphoreTakeRecursive(stateMutex, (TickType_t)pdMS_TO_TICKS(1000)) == pdTRUE) {
    if (g_currentState != LOCKED && g_currentState != ARMED && g_currentState != TESTING) {
      xSemaphoreGiveRecursive(stateMutex);
      sendJsonError(request, 409, "Device is not in an abortable state.");
      return;
    }

    logKeyValue("WebAPI", "/abort");

    abortSession("API Request");

    std::unique_ptr<JsonDocument> doc(new JsonDocument());
    (*doc)["status"] = (g_currentState == ABORTED) ? "aborted" : (g_currentState == COMPLETED ? "completed" : "ready");
    serializeJson(*doc, responseJson);

    xSemaphoreGiveRecursive(stateMutex);
  } else {
    sendJsonError(request, 503, "System Busy");
    return;
  }
  request->send(200, "application/json", responseJson);
}

// =================================================================================
// SECTION: INFORMATION & STATUS HANDLERS
// =================================================================================

/**
 * Handler for GET /status
 */
void handleStatus(AsyncWebServerRequest *request) {
  // 1. Create the Snapshot (Mapped to SessionStatus interface)
  struct StateSnapshot {
    // Session
    DeviceState state;

    // Timers
    uint32_t lockDuration;
    uint32_t lockRemain;
    uint32_t penaltyRemain;
    uint32_t testRemain;
    uint32_t triggerTimeout;

    // Config Echo
    TriggerStrategy strategy;
    bool hideTimer;
    DurationType durationType;
    uint32_t durationMin;
    uint32_t durationMax;
    uint32_t configDelays[4];

    // Live Data
    uint32_t liveDelays[4];

    // Stats
    uint32_t streaks;
    uint32_t aborted;
    uint32_t completed;
    uint32_t totalLocked;
    uint32_t payback;

    // Hardware
    bool isPressed;
    uint32_t pressDuration;
    int rssi;
    uint32_t freeHeap;
    uint32_t uptime;
    float internalTemp;
  } snapshot;

  // Quick Lock & Copy
  if (xSemaphoreTakeRecursive(stateMutex, (TickType_t)pdMS_TO_TICKS(200)) == pdTRUE) {
    snapshot.state = g_currentState;

    // Timers - Source: g_sessionTimers
    snapshot.lockDuration = g_sessionTimers.lockDuration;
    snapshot.lockRemain = g_sessionTimers.lockRemaining;
    snapshot.penaltyRemain = g_sessionTimers.penaltyRemaining;
    snapshot.testRemain = g_sessionTimers.testRemaining;
    snapshot.triggerTimeout = g_sessionTimers.triggerTimeout;

    // Config Echo - Source: g_activeSessionConfig
    snapshot.strategy = g_activeSessionConfig.triggerStrategy;
    snapshot.hideTimer = g_activeSessionConfig.hideTimer;
    snapshot.durationType = g_activeSessionConfig.durationType;
    snapshot.durationMin = g_activeSessionConfig.durationMin;
    snapshot.durationMax = g_activeSessionConfig.durationMax;
    for (int i = 0; i < 4; i++)
      snapshot.configDelays[i] = g_activeSessionConfig.channelDelays[i];

    // Live Data - Source: g_sessionTimers
    for (int i = 0; i < 4; i++)
      snapshot.liveDelays[i] = g_sessionTimers.channelDelays[i];

    // Stats - Source: g_sessionStats
    snapshot.streaks = g_sessionStats.streaks;
    snapshot.aborted = g_sessionStats.aborted;
    snapshot.completed = g_sessionStats.completed;
    snapshot.totalLocked = g_sessionStats.totalLockedTime;
    snapshot.payback = g_sessionStats.paybackAccumulated;

    // --- HARDWARE STATUS COLLECTION ---
    bool pcbPressed = (digitalRead(PCB_BUTTON_PIN) == LOW);
    bool extPressed = false;
#ifdef EXT_BUTTON_PIN
    extPressed = (digitalRead(EXT_BUTTON_PIN) == HIGH);
#endif
    snapshot.isPressed = pcbPressed || extPressed;
    snapshot.pressDuration = snapshot.isPressed ? (millis() - g_buttonPressStartTime) : 0;

    // 2. Connectivity & System
    snapshot.rssi = WiFi.RSSI();
    snapshot.freeHeap = ESP.getFreeHeap();
    snapshot.uptime = millis() / 1000;
    snapshot.internalTemp = temperatureRead();

    xSemaphoreGiveRecursive(stateMutex);
  } else {
    request->send(503, "text/plain", "System Busy");
    return;
  }

  // 2. Stream directly to TCP buffer
  AsyncResponseStream *response = request->beginResponseStream("application/json");

  // Use Stack memory.
  JsonDocument doc;

  // Status & Duration
  doc["status"] = stateToString(snapshot.state);
  doc["lockDuration"] = snapshot.lockDuration;

  // --- timers (SessionTimers) ---
  JsonObject timers = doc["timers"].to<JsonObject>();
  timers["lockRemaining"] = snapshot.lockRemain;
  timers["rewardRemaining"] = snapshot.penaltyRemain;
  timers["testRemaining"] = snapshot.testRemain;
  if (snapshot.state == ARMED && snapshot.strategy == STRAT_BUTTON_TRIGGER) {
    timers["triggerTimeout"] = snapshot.triggerTimeout;
  }

  // --- config (SessionConfig) ---
  JsonObject config = doc["config"].to<JsonObject>();
  config["triggerStrategy"] = (snapshot.strategy == STRAT_BUTTON_TRIGGER) ? "buttonTrigger" : "autoCountdown";
  config["hideTimer"] = snapshot.hideTimer;

  // Map DurationType enum to TS string
  switch (snapshot.durationType) {
  case DUR_RANDOM:
    config["durationType"] = "random";
    break;
  case DUR_RANGE:
    config["durationType"] = "time-range";
    break;
  default:
    config["durationType"] = "fixed";
    break;
  }
  if (snapshot.durationMin > 0)
    config["durationMin"] = snapshot.durationMin;
  if (snapshot.durationMax > 0)
    config["durationMax"] = snapshot.durationMax;

  // Note: We intentionally do NOT populate 'duration' here because for Random/Range,
  // the client knows it was random. The "actual" resolved duration is found in lockDuration.

  // Reconstruct Configured Channel Delays
  JsonObject cfgCh = config["channelDelays"].to<JsonObject>();
  cfgCh["ch1"] = snapshot.configDelays[0];
  cfgCh["ch2"] = snapshot.configDelays[1];
  cfgCh["ch3"] = snapshot.configDelays[2];
  cfgCh["ch4"] = snapshot.configDelays[3];

  // --- channelDelaysRemaining (Live) ---
  JsonObject liveCh = doc["channelDelaysRemaining"].to<JsonObject>();
  liveCh["ch1"] = snapshot.liveDelays[0];
  liveCh["ch2"] = snapshot.liveDelays[1];
  liveCh["ch3"] = snapshot.liveDelays[2];
  liveCh["ch4"] = snapshot.liveDelays[3];

  // --- stats ---
  JsonObject stats = doc["stats"].to<JsonObject>();
  stats["streaks"] = snapshot.streaks;
  stats["aborted"] = snapshot.aborted;
  stats["completed"] = snapshot.completed;
  stats["totalTimeLocked"] = snapshot.totalLocked;
  stats["pendingPayback"] = snapshot.payback;

  // --- hardware ---
  JsonObject hw = doc["hardware"].to<JsonObject>();
  hw["buttonPressed"] = snapshot.isPressed;
  hw["currentPressDurationMs"] = snapshot.pressDuration;
  hw["rssi"] = snapshot.rssi;
  hw["freeHeap"] = snapshot.freeHeap;
  hw["uptime"] = snapshot.uptime;

  // Handle temperature NaN or invalid reads gracefully
  if (isnan(snapshot.internalTemp)) {
    hw["internalTempC"] = "N/A";
  } else {
    hw["internalTempC"] = snapshot.internalTemp;
  }

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
    char uniqueHostname[30];
    snprintf(uniqueHostname, sizeof(uniqueHostname), "lobster-lock-%02X%02X%02X", mac[3], mac[4], mac[5]);

    std::unique_ptr<JsonDocument> doc(new JsonDocument());
    (*doc)["id"] = uniqueHostname;
    (*doc)["name"] = DEVICE_NAME;
    (*doc)["version"] = DEVICE_VERSION;

#ifdef DEBUG_MODE
    (*doc)["buildType"] = "debug";
#else
    (*doc)["buildType"] = "release";
#endif

    (*doc)["address"] = WiFi.localIP().toString();
    (*doc)["mac"] = WiFi.macAddress();
    (*doc)["port"] = 80;

    // System Defaults & Limits
    (*doc)["longPressMs"] = g_systemDefaults.longPressDuration * 1000;
    (*doc)["minLockDuration"] = g_sessionLimits.minLockDuration;
    (*doc)["maxLockDuration"] = g_sessionLimits.maxLockDuration;
    (*doc)["testModeDuration"] = g_systemDefaults.testModeDuration;

    // Channels Configuration
    JsonObject channels = (*doc)["channels"].to<JsonObject>();
    channels["ch1"] = (bool)((g_enabledChannelsMask >> 0) & 1);
    channels["ch2"] = (bool)((g_enabledChannelsMask >> 1) & 1);
    channels["ch3"] = (bool)((g_enabledChannelsMask >> 2) & 1);
    channels["ch4"] = (bool)((g_enabledChannelsMask >> 3) & 1);

    // Deterrent Configuration
    JsonObject deterrents = (*doc)["deterrents"].to<JsonObject>();
    deterrents["enableStreaks"] = g_deterrentConfig.enableStreaks;
    deterrents["enableRewardCode"] = g_deterrentConfig.enableRewardCode;
    deterrents["rewardPenaltyDuration"] = g_deterrentConfig.rewardPenalty;

    deterrents["enablePaybackTime"] = g_deterrentConfig.enablePaybackTime;
    deterrents["paybackDuration"] = g_deterrentConfig.paybackTime;
    deterrents["minPaybackDuration"] = g_sessionLimits.minPaybackTime;
    deterrents["maxPaybackDuration"] = g_sessionLimits.maxPaybackTime;

    // Add features array
    JsonArray features = (*doc)["features"].to<JsonArray>();
    features.add("footPedal");
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
      lineBuffer[MAX_LOG_ENTRY_LENGTH - 1] = '\0'; // safety
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
 * Handler for GET /reward
 */
void handleReward(AsyncWebServerRequest *request) {
  // To be safe, we just take the lock for the whole operation here since it's
  // read-only and fast enough.
  if (xSemaphoreTakeRecursive(stateMutex, (TickType_t)pdMS_TO_TICKS(1000)) == pdTRUE) {
    if (g_currentState == LOCKED || g_currentState == ABORTED || g_currentState == ARMED) {
      xSemaphoreGiveRecursive(stateMutex);
      sendJsonError(request, 403, "Reward is not yet available.");
    } else {
      std::unique_ptr<JsonDocument> doc(new JsonDocument());
      JsonArray arr = (*doc).to<JsonArray>();
      for (int i = 0; i < REWARD_HISTORY_SIZE; i++) {
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

// =================================================================================
// SECTION: CONFIGURATION & FACTORY RESET
// =================================================================================

/**
 * Handler for POST /update-wifi (body)
 */
void handleUpdateWifi(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
  // Handle JSON body
  if (index + len != total)
    return;

  std::unique_ptr<JsonDocument> doc(new JsonDocument());

  DeserializationError error = deserializeJson(*doc, (const char *)data, len);
  if (error) {
    sendJsonError(request, 400, "Invalid JSON body.");
    return;
  }

  // Validate JSON body
  if (!(*doc)["ssid"].is<const char *>() || !(*doc)["pass"].is<const char *>()) {
    sendJsonError(request, 400, "Missing required fields: ssid, pass.");
    return;
  }

  const char *ssid = (*doc)["ssid"];
  const char *pass = (*doc)["pass"];

  if (xSemaphoreTakeRecursive(stateMutex, (TickType_t)pdMS_TO_TICKS(1000)) == pdTRUE) {
    // State is already checked in the on() handler, but as a safeguard:
    if (g_currentState != READY) {
      xSemaphoreGiveRecursive(stateMutex);
      sendJsonError(request, 409, "Device must be in READY state to update Wi-Fi.");
      return;
    }

    logKeyValue("WebAPI", "/update-wifi");

    // Save new credentials to NVS using new Storage helper
    saveWiFiCredentials(ssid, pass);

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
    if (g_currentState != READY && g_currentState != COMPLETED) {
      xSemaphoreGiveRecursive(stateMutex);
      sendJsonError(request, 409,
                    "Device is in an active session. Cannot reset while "
                    "locked, in countdown, or in penalty.");
      return;
    }

    logKeyValue("WebAPI", "/factory-reset");

    // Erase Wi-Fi
    wifiPreferences.begin("wifi-creds", false);
    wifiPreferences.clear();
    wifiPreferences.end();
    logKeyValue("Prefs", "Wi-Fi credentials erased.");

    // Erase all session state and counters
    sessionState.begin("session", false);
    sessionState.clear();
    sessionState.end();
    logKeyValue("Prefs", "Session state, statistics and config erased.");

    // Erase all device provisioning settings
    provisioningPrefs.begin("provisioning", false);
    provisioningPrefs.clear();
    provisioningPrefs.end();
    logKeyValue("Prefs", "Provisioning settings erased.");

    // Clear boot loop stats
    bootPrefs.begin("boot", false);
    bootPrefs.clear();
    bootPrefs.end();
    logKeyValue("Prefs", "Boot loop statistics erased.");

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

// =================================================================================
// SECTION: SERVER SETUP
// =================================================================================

/**
 * Sets up all API endpoints for the web server.
 * This function is now just a clean list of routes.
 */
void setupWebServer() {

  // Root endpoint, list the API.
  server.on("/", HTTP_GET, handleRoot);

  // API: Lightweight health check
  server.on("/health", HTTP_GET, handleHealth);

  // API: Keep-alive endpoint
  server.on("/keepalive", HTTP_POST, handleKeepAlive);

  // API: System Reboot (Safe Reset)
  server.on("/reboot", HTTP_POST, handleReboot);

  // API: Arm/Start a session
  server.on("/arm", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL, handleArm);

  // Start a short test session
  server.on("/start-test", HTTP_POST, handleStartTest);

  // Abort an active session
  server.on("/abort", HTTP_POST, handleAbort);

  // Get the reward code history
  server.on("/reward", HTTP_GET, handleReward);

  // Get the main device status (dynamic data, polled frequently).
  server.on("/status", HTTP_GET, handleStatus);

  // Get the main device details (static data, polled once).
  server.on("/details", HTTP_GET, handleDetails);

  // Get the in-memory log buffer
  server.on("/log", HTTP_GET, handleLog);

  // Update Wi-Fi Credentials
  server.on("/update-wifi", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL, handleUpdateWifi);

  // Factory reset
  server.on("/factory-reset", HTTP_POST, handleFactoryReset);

  server.begin();

  logKeyValue("WebAPI", "HTTP server started.");
}