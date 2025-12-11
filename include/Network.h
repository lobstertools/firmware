/*
 * =================================================================================
 * File:      include/Network.h
 * Description: Public interface for Network Management.
 * Refactor: Converted to Singleton Class 'NetworkManager'.
 * =================================================================================
 */
#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <freertos/timers.h>

class NetworkManager {
public:
  // Singleton Accessor
  static NetworkManager &getInstance();

  // --- Public API ---

  /**
   * Attempts to connect to WiFi using stored credentials.
   * If it fails after retries, it sets an internal flag requesting provisioning.
   * It DOES NOT block or change state itself.
   */
  void connectOrRequestProvisioning();

  /**
   * Checks if the network layer has failed and is requesting user intervention.
   * Used by the HAL/SessionEngine to decide when to pause operations.
   */
  bool isProvisioningNeeded() const { return _triggerProvisioning; }

  /**
   * Enters the blocking BLE Provisioning loop.
   * This function does not return until the device is rebooted.
   * It enforces hardware safety (pins LOW) internally.
   */
  void startBLEProvisioningBlocking();

  void printStartupDiagnostics();

private:
  NetworkManager(); // Private Constructor

  void log(const char *key, const char *value);

  // --- Internal State ---
  char _wifiSSID[33];
  char _wifiPass[65];
  bool _wifiCredentialsExist;

  volatile bool _triggerProvisioning;
  volatile int _wifiRetries;
  TimerHandle_t _wifiReconnectTimer;

  // --- Helpers ---
  void connectToWiFi();
  void startMDNS();

  // --- Static Callbacks (Trampolines) ---
  static void onWiFiEvent(WiFiEvent_t event);
  static void onWifiTimer(TimerHandle_t t);

  // --- Member Event Handlers ---
  void handleWiFiEvent(WiFiEvent_t event);
  void handleWifiTimer();
};
