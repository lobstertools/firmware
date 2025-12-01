#include <ArduinoJson.h>
#include <WiFi.h>

#include "Globals.h"
#include "Hardware.h"
#include "Logger.h"
#include "Session.h"
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

  // ALLOCATE ON HEAP via RAII (Prefer PSRAM if available via ArduinoJson
  // internals)
  std::unique_ptr<JsonDocument> doc(new JsonDocument());

  DeserializationError error = deserializeJson(*doc, (const char *)data, len);
  if (error) {
    char logBuf[100];
    snprintf(logBuf, sizeof(logBuf), "Failed to parse /arm JSON: %s", error.c_str());
    logMessage(logBuf);
    sendJsonError(request, 400, "Invalid JSON body.");
    return;
  }

  if (!(*doc)["lockDurationSeconds"].is<JsonInteger>()) {
    sendJsonError(request, 400, "Missing required field: lockDurationSeconds.");
    return;
  }

  // Penalty is required ONLY if Reward Code is enabled
  if (enableRewardCode && !(*doc)["penaltyDurationSeconds"].is<JsonInteger>()) {
    sendJsonError(request, 400,
                  "Missing required field: penaltyDurationSeconds (Reward Code "
                  "is enabled).");
    return;
  }

  // Read session-specific data from the request
  unsigned long durationSeconds = (*doc)["lockDurationSeconds"];
  // Default to 0 if missing (allowed only if Reward Code is disabled)
  unsigned long penaltySeconds = (*doc)["penaltyDurationSeconds"] | 0;
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
    if (delaysObj["ch1"].is<unsigned long>())
      tempDelays[0] = delaysObj["ch1"].as<unsigned long>();
    if (delaysObj["ch2"].is<unsigned long>())
      tempDelays[1] = delaysObj["ch2"].as<unsigned long>();
    if (delaysObj["ch3"].is<unsigned long>())
      tempDelays[2] = delaysObj["ch3"].as<unsigned long>();
    if (delaysObj["ch4"].is<unsigned long>())
      tempDelays[3] = delaysObj["ch4"].as<unsigned long>();
  }

  // Filter delays by enabled mask
  for (int i = 0; i < 4; i++) {
    if (!((g_enabledChannelsMask >> i) & 1)) {
      tempDelays[i] = 0;
    }
  }

  // Prepare response buffer (stack allocated String)
  String responseJson;

  // We lock briefly ONLY to update the state machine.
  if (xSemaphoreTakeRecursive(stateMutex, (TickType_t)pdMS_TO_TICKS(1000)) == pdTRUE) {

    logMessage(LOG_SEP_MAJOR);
    logMessage("API REQUEST: /arm");

    int result = startSession(durationSeconds, penaltySeconds, requestedStrat, tempDelays, newHideTimer);

    logMessage(LOG_SEP_MINOR);           // End Interaction Visual
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
      sendJsonError(request, 400, "Invalid configuration values.");
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

    logMessage(LOG_SEP_MAJOR);
    logMessage("API REQUEST: /start-test");

    int result = startTestMode();

    logMessage(LOG_SEP_MINOR); // End Interaction Visual
    xSemaphoreGiveRecursive(stateMutex);

    // Handle Result
    if (result == 200) {
      std::unique_ptr<JsonDocument> doc(new JsonDocument());
      (*doc)["status"] = "testing";
      (*doc)["testSecondsRemaining"] = g_systemConfig.testModeDurationSeconds;
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
    if (currentState != LOCKED && currentState != ARMED && currentState != TESTING) {
      xSemaphoreGiveRecursive(stateMutex);
      sendJsonError(request, 409, "Device is not in an abortable state.");
      return;
    }

    logMessage(LOG_SEP_MAJOR);
    logMessage("API REQUEST: /abort");

    abortSession("API Request");

    logMessage(LOG_SEP_MINOR); // End Interaction Visual

    std::unique_ptr<JsonDocument> doc(new JsonDocument());
    (*doc)["status"] = (currentState == ABORTED) ? "aborted" : (currentState == COMPLETED ? "completed" : "ready");
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
  // 1. Create the Snapshot
  struct StateSnapshot {

    // Session
    SessionState state;
    TriggerStrategy strategy;
    unsigned long lockRemain;
    unsigned long penaltyRemain;
    unsigned long testRemain;
    unsigned long delays[4];
    bool hideTimer;

    // Stats
    uint32_t streaks;
    uint32_t aborted;
    uint32_t completed;
    uint32_t totalLocked;
    uint32_t payback;

    // Hardware
    bool isPressed;
    uint32_t pressDuration;
    uint32_t longPressThreshold;
    int rssi;
    uint32_t freeHeap;
    uint32_t uptime;
    float internalTemp;
  } snapshot;

  // Quick Lock & Copy
  if (xSemaphoreTakeRecursive(stateMutex, (TickType_t)pdMS_TO_TICKS(200)) == pdTRUE) {
    snapshot.state = currentState;
    snapshot.strategy = currentStrategy;
    snapshot.lockRemain = lockSecondsRemaining;
    snapshot.penaltyRemain = penaltySecondsRemaining;
    snapshot.testRemain = testSecondsRemaining;
    for (int i = 0; i < 4; i++)
      snapshot.delays[i] = channelDelaysRemaining[i];
    snapshot.hideTimer = hideTimer;
    snapshot.streaks = sessionStreakCount;
    snapshot.aborted = abortedSessions;
    snapshot.completed = completedSessions;
    snapshot.totalLocked = totalLockedSessionSeconds;
    snapshot.payback = paybackAccumulated;

    // --- HARDWARE STATUS COLLECTION ---
    // 1. Button Logic: "Instant Truth" check
    // Active LOW: 0 = Pressed, 1 = Released
    snapshot.isPressed = (digitalRead(ONE_BUTTON_PIN) == 0);

    if (snapshot.isPressed) {
      snapshot.pressDuration = millis() - g_buttonPressStartTime;
    } else {
      snapshot.pressDuration = 0;
    }

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

  // Channel info
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

  // --- Hardware
  JsonObject hw = doc["hardwareStatus"].to<JsonObject>();
  hw["buttonPressed"] = snapshot.isPressed;
  hw["currentPressDurationMs"] = snapshot.pressDuration;
  hw["rssi"] = snapshot.rssi;
  hw["freeHeap"] = snapshot.freeHeap;
  hw["uptimeSeconds"] = snapshot.uptime;

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
    char uniqueHostname[20];
    snprintf(uniqueHostname, sizeof(uniqueHostname), "lobster-lock-%02X%02X%02X", mac[3], mac[4], mac[5]);

    std::unique_ptr<JsonDocument> doc(new JsonDocument());
    (*doc)["name"] = DEVICE_NAME;
    (*doc)["id"] = uniqueHostname;
    (*doc)["version"] = DEVICE_VERSION;

    (*doc)["longPressMs"] = g_systemConfig.longPressSeconds * 1000;

    // System Limits
    (*doc)["minLockSeconds"] = g_systemConfig.minLockSeconds;
    (*doc)["maxLockSeconds"] = g_systemConfig.maxLockSeconds;
    (*doc)["minPenaltySeconds"] = g_systemConfig.minPenaltySeconds;
    (*doc)["maxPenaltySeconds"] = g_systemConfig.maxPenaltySeconds;
    (*doc)["testModeDurationSeconds"] = g_systemConfig.testModeDurationSeconds;

    (*doc)["address"] = WiFi.localIP().toString();
    (*doc)["mac"] = WiFi.macAddress();
    (*doc)["port"] = 80;

    // Channels Configuration
    JsonObject channels = (*doc)["channels"].to<JsonObject>();
    channels["ch1"] = (bool)((g_enabledChannelsMask >> 0) & 1);
    channels["ch2"] = (bool)((g_enabledChannelsMask >> 1) & 1);
    channels["ch3"] = (bool)((g_enabledChannelsMask >> 2) & 1);
    channels["ch4"] = (bool)((g_enabledChannelsMask >> 3) & 1);

    // Deterrent Configuration
    JsonObject deterrents = (*doc)["deterrents"].to<JsonObject>();
    deterrents["enableStreaks"] = enableStreaks;
    deterrents["enablePaybackTime"] = enablePaybackTime;
    deterrents["enableRewardCode"] = enableRewardCode;
    deterrents["paybackDurationSeconds"] = paybackTimeSeconds;
    deterrents["minPaybackTimeSeconds"] = g_systemConfig.minPaybackTimeSeconds;
    deterrents["maxPaybackTimeSeconds"] = g_systemConfig.maxPaybackTimeSeconds;

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
    if (currentState == LOCKED || currentState == ABORTED || currentState == ARMED) {
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
    char logBuf[100];
    snprintf(logBuf, sizeof(logBuf), "Failed to parse /update-wifi JSON: %s", error.c_str());
    logMessage(logBuf);
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
      logMessage("API: /factory-reset failed. Device is currently in an active "
                 "session.");
      sendJsonError(request, 409,
                    "Device is in an active session. Cannot reset while "
                    "locked, in countdown, or in penalty.");
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

  // API: Arm/Start a session
  server.on("/arm", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL, handleArm);

  // Start a short test mode
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

  logMessage("HTTP server started.");
}