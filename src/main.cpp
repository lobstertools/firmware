/*
 * =================================================================================
 * Project:   Lobster Lock - Self-Bondage Session Manager
 * File:      main.cpp
 * Description: Application entry point.
 * Refactor:  Integrated SessionPresets into SettingsManager loading.
 * =================================================================================
 */

#include <Arduino.h>
#include <Ticker.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

// --- Module Includes ---
#include "Config.h"
#include "Esp32SessionHAL.h"
#include "Globals.h"
#include "Network.h"
#include "SettingsManager.h"
#include "WebManager.h"

// --- Session Engine Includes ---
#include "Session.h"
#include "StandardRules.h"

// --- Main ticker ---
Ticker oneSecondMasterTicker;
volatile uint32_t tickCounter = 0;
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;

// --- Dependencies ---
Esp32SessionHAL &hal = Esp32SessionHAL::getInstance();
NetworkManager &network = NetworkManager::getInstance();
WebManager &web = WebManager::getInstance();

// --- Session Engine
SessionEngine *sessionEngine = nullptr;
StandardRules rules;

/**
 * Prints high-level firmware identity and build information.
 * Place this in main.cpp and call it during setup().
 */
void printFirmwareDiagnostics() {
    // Access the HAL singleton for logging
    Esp32SessionHAL& hal = Esp32SessionHAL::getInstance();
    char logBuf[128];

    hal.log("==========================================================================");
    hal.log("                       FIRMWARE IDENTITY                                  ");
    hal.log("==========================================================================");

    // -------------------------------------------------------------------------
    // SECTION: IDENTITY
    // -------------------------------------------------------------------------
    hal.log("[ VERSION INFO ]");

    // Device Name
    snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Device Name", String(DEVICE_NAME).c_str());
    hal.log(logBuf);

    // Firmware Version
    snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Firmware Version", String(DEVICE_VERSION).c_str());
    hal.log(logBuf);

    // -------------------------------------------------------------------------
    // SECTION: BUILD METADATA
    // -------------------------------------------------------------------------
    hal.log("");
    hal.log("[ BUILD DETAILS ]");

    // Compilation Date
    snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Build Date", __DATE__);
    hal.log(logBuf);

    // Compilation Time
    snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Build Time", __TIME__);
    hal.log(logBuf);

    // C++ Standard
    snprintf(logBuf, sizeof(logBuf), " %-25s : %ld", "C++ Standard", __cplusplus);
    hal.log(logBuf);

    hal.log("==========================================================================");
}

// =================================================================
// --- Core Application Setup & Loop ---
// =================================================================

void setup() {
  Serial.begin(SERIAL_BAUD_RATE);
  delay(3000);

  // 1. Initialize Hardware
  hal.initialize();
  printFirmwareDiagnostics();

  hal.tick();

  // 2. Load Deterrents & Session Presets
  DeterrentConfig loadedDeterrents = {};
  SessionPresets sessionPresets = {};
  uint8_t loadedChannelMask = 0x0F;
  SettingsManager::loadProvisioningConfig(loadedDeterrents, sessionPresets, loadedChannelMask);

  // 3. Initialize Engine
  sessionEngine = new SessionEngine(hal, rules, g_systemDefaults, sessionPresets, loadedDeterrents);

  // 4. Load & Apply Saved Session State (Resume)
  DeviceState savedState = READY;
  SessionTimers savedTimers;
  SessionStats savedStats;

  // Ensure structs are clean
  memset(&savedTimers, 0, sizeof(savedTimers));
  memset(&savedStats, 0, sizeof(savedStats));

  bool hasState = SettingsManager::loadSessionState(savedState, savedTimers, savedStats);

  if (hasState) {
    hal.logKeyValue("System", "Restoring state to Session Engine...");
    sessionEngine->loadState(savedState);
    sessionEngine->loadTimers(savedTimers);
    sessionEngine->loadStats(savedStats);
    sessionEngine->handleReboot();
  } else {
    hal.logKeyValue("System", "No previous state. Starting fresh.");
  }

  // 5. Network
  network.connectOrRequestProvisioning();

  // 6. Diagnostics
  hal.printStartupDiagnostics();
  hal.tick();
  
  sessionEngine->printStartupDiagnostics();
  hal.tick();
  
  network.printStartupDiagnostics();
  hal.tick();

  hal.log("==========================================================================");

  // 7. Start Master Timer
  hal.logKeyValue("Session", "Attaching master 1-second ticker.");
  oneSecondMasterTicker.attach(1, []() {
    portENTER_CRITICAL_ISR(&timerMux);
    tickCounter++;
    portEXIT_CRITICAL_ISR(&timerMux);
  });

  // 8. Start Web API
  web.begin(sessionEngine);
}

void loop() {
  // 1. System Housekeeping
  esp_task_wdt_reset();

  // 2. Hardware Tick (Inputs, LEDs, Health, Logging)
  hal.tick();

  // 3. Session Engine Tick (Time, Rules, Safety, Network Checks)
  uint32_t pendingTicks = 0;
  portENTER_CRITICAL(&timerMux);
  if (tickCounter > 0) {
    pendingTicks = tickCounter;
    tickCounter = 0;
  }
  portEXIT_CRITICAL(&timerMux);

  if (pendingTicks > 0) {
    if (hal.lockState()) {
      while (pendingTicks > 0 && sessionEngine != nullptr) {
        sessionEngine->tick();
        pendingTicks--;
      }

      hal.unlockState();
    }
  }
}