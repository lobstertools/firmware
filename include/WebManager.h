/*
 * =================================================================================
 * File:      include/WebManager.h
 * Description:
 * Async HTTP Server Controller.
 * - Singleton Architecture.
 * - Handles all REST endpoints for Session and Device control.
 * - Uses Dependency Injection for Engine and HAL.
 * =================================================================================
 */
#pragma once
#include "Session.h"
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

class WebManager {
public:
  static WebManager &getInstance();

  // Initialization (Call in setup)
  void begin(SessionEngine *engine);

private:
  WebManager();

  // --- Dependencies ---
  AsyncWebServer _server;
  SessionEngine *_engine;

  // --- Helper Functions ---
  void sendJsonError(AsyncWebServerRequest *request, int code, const std::string &message);
  void registerEndpoints();
  void log(const char *key, const char *value);

  // --- Route Handlers ---

  // System & Health
  void handleRoot(AsyncWebServerRequest *request);
  void handleHealth(AsyncWebServerRequest *request);
  void handleKeepAlive(AsyncWebServerRequest *request);
  void handleReboot(AsyncWebServerRequest *request);
  void handleFactoryReset(AsyncWebServerRequest *request);

  // Session Control
  void handleArm(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total);
  void handleStartTest(AsyncWebServerRequest *request);
  void handleAbort(AsyncWebServerRequest *request);

  // Information
  void handleStatus(AsyncWebServerRequest *request);
  void handleDetails(AsyncWebServerRequest *request);
  void handleLog(AsyncWebServerRequest *request);
  void handleReward(AsyncWebServerRequest *request);

  // Configuration
  void handleUpdateWifi(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total);
};
