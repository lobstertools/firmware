/*
 * =================================================================================
 * Project:   Lobster Lock - Self-Bondage Session Manager
 * File:      main.cpp
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Description:
 * Application entry point. Handles system initialization,
 * orchestrates the main execution loop, manages critical health checks,
 * and ties together the Network, Hardware, and Session modules.
 * =================================================================================
 */

#include <Arduino.h>
#include <Ticker.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

// --- Module Includes ---
#include "Config.h"
#include "Globals.h"
#include "Hardware.h"
#include "Logger.h"
#include "Network.h"
#include "Session.h"
#include "Storage.h"
#include "Utils.h"
#include "WebAPI.h"

// --- Globals Local to Main ---
Ticker oneSecondMasterTicker;

// Use a counter to track missed ticks (ISR Safe)
volatile uint32_t g_tickCounter = 0;

// =================================================================
// --- Helper Functions: Diagnostics ---
// =================================================================

/**
 * Consolidated function to log all system statistics and configurations
 * on startup.
 */
void printStartupDiagnostics() {
  char logBuf[150];
  char tBuf1[50];
  char tBuf2[50];
  unsigned long longPressMs = (unsigned long)g_systemConfig.longPressSeconds * 1000;

  // --- STARTUP BANNER ---
  logMessage(LOG_SEP_MAJOR);
  snprintf(logBuf, sizeof(logBuf), " %s", DEVICE_NAME);
  logMessage(logBuf);
  snprintf(logBuf, sizeof(logBuf), " Version: %s", DEVICE_VERSION);
  logMessage(logBuf);

  processLogQueue();

  // --- SECTION: HARDWARE & FEATURES ---
  logMessage(LOG_SEP_MINOR);
  logMessage("[ HARDWARE & FEATURES ]");
  logMessage(LOG_SEP_MINOR);

  for (int i = 0; i < MAX_CHANNELS; i++) {
    bool isEnabled = (g_enabledChannelsMask >> i) & 1;
    snprintf(logBuf, sizeof(logBuf), " %-25s : %s (GPIO %d)", ("Channel " + String(i + 1)).c_str(), isEnabled ? "ENABLED" : "DISABLED",
             HARDWARE_PINS[i]);
    logMessage(logBuf);
  }

  snprintf(logBuf, sizeof(logBuf), " %-25s : %s (Pin %d)", "Status LED", "Enabled", STATUS_LED_PIN);
  logMessage(logBuf);

  snprintf(logBuf, sizeof(logBuf), " %-25s : %s (Pin %d)", "PCB Button", "Enabled", PCB_BUTTON_PIN);
  logMessage(logBuf);

#ifdef EXT_BUTTON_PIN
  snprintf(logBuf, sizeof(logBuf), " %-25s : %s (Pin %d) [NC/FailSafe]", "External Button", "Enabled", EXT_BUTTON_PIN);
#else
  snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "External Button", "Disabled (Debug)");
#endif
  logMessage(logBuf);

  snprintf(logBuf, sizeof(logBuf), " %-25s : %lu ms", "Long Press Time", longPressMs);
  logMessage(logBuf);

  processLogQueue();

  // --- SECTION: SYSTEM CONFIG ---
  logMessage(LOG_SEP_MINOR);
  logMessage("[ SYSTEM CONFIG ]");
  logMessage(LOG_SEP_MINOR);

  snprintf(logBuf, sizeof(logBuf), " %-25s : %u / %u s", "Lock Range", g_systemConfig.minLockSeconds, g_systemConfig.maxLockSeconds);
  logMessage(logBuf);

  snprintf(logBuf, sizeof(logBuf), " %-25s : %u / %u s", "Penalty Range", g_systemConfig.minPenaltySeconds,
           g_systemConfig.maxPenaltySeconds);
  logMessage(logBuf);

  snprintf(logBuf, sizeof(logBuf), " %-25s : %u / %u s", "Payback Range", g_systemConfig.minPaybackTimeSeconds,
           g_systemConfig.maxPaybackTimeSeconds);
  logMessage(logBuf);

  snprintf(logBuf, sizeof(logBuf), " %-25s : %u s", "Test Session Duration", g_systemConfig.testModeDurationSeconds);
  logMessage(logBuf);

  snprintf(logBuf, sizeof(logBuf), " %-25s : %u s", "Failsafe Timeout", g_systemConfig.failsafeMaxLockSeconds);
  logMessage(logBuf);

  snprintf(logBuf, sizeof(logBuf), " %-25s : %u ms / %u", "Keep-Alive", g_systemConfig.keepAliveIntervalMs,
           g_systemConfig.keepAliveMaxStrikes);
  logMessage(logBuf);

  snprintf(logBuf, sizeof(logBuf), " %-25s : %u / %u ms", "Boot Loop/Stable", g_systemConfig.bootLoopThreshold,
           g_systemConfig.stableBootTimeMs);
  logMessage(logBuf);

  snprintf(logBuf, sizeof(logBuf), " %-25s : %u / %u s", "WiFi Retries/ArmedTO", g_systemConfig.wifiMaxRetries,
           g_systemConfig.armedTimeoutSeconds);
  logMessage(logBuf);

  processLogQueue();

  // --- SECTION: DETERRENTS ---
  logMessage(LOG_SEP_MINOR);
  logMessage("[ DETERRENT CONFIG ]");
  logMessage(LOG_SEP_MINOR);

  snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Session Streaks", enableStreaks ? "Enabled" : "Disabled");
  logMessage(logBuf);
  snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Time Payback", enablePaybackTime ? "Enabled" : "Disabled");
  logMessage(logBuf);

  if (enablePaybackTime) {
    formatSeconds(paybackTimeSeconds, tBuf1, sizeof(tBuf1));
    snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Payback Duration", tBuf1);
    logMessage(logBuf);
  }
  snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Reward Code", enableRewardCode ? "Enabled" : "Disabled");
  logMessage(logBuf);

  processLogQueue();

  // --- SECTION: STATISTICS ---
  logMessage(LOG_SEP_MINOR);
  logMessage("[ STATISTICS ]");
  logMessage(LOG_SEP_MINOR);

  snprintf(logBuf, sizeof(logBuf), " %-25s : %u", "Streak Count", sessionStreakCount);
  logMessage(logBuf);
  snprintf(logBuf, sizeof(logBuf), " %-25s : %u", "Completed Sessions", completedSessions);
  logMessage(logBuf);
  snprintf(logBuf, sizeof(logBuf), " %-25s : %u", "Aborted Sessions", abortedSessions);
  logMessage(logBuf);

  formatSeconds(paybackAccumulated, tBuf1, sizeof(tBuf1));
  snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Accumulated Debt", tBuf1);
  logMessage(logBuf);

  formatSeconds(totalLockedSessionSeconds, tBuf1, sizeof(tBuf1));
  snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Lifetime Locked", tBuf1);
  logMessage(logBuf);

  processLogQueue();

  // --- SECTION: CURRENT SESSION ---
  logMessage(LOG_SEP_MINOR);
  logMessage("[ CURRENT SESSION ]");
  logMessage(LOG_SEP_MINOR);

  snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Current State", stateToString(currentState));
  logMessage(logBuf);
  snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Timer Visibility", hideTimer ? "Hidden" : "Visible");
  logMessage(logBuf);

  formatSeconds(lockSecondsRemaining, tBuf1, sizeof(tBuf1));
  formatSeconds(lockSecondsConfig, tBuf2, sizeof(tBuf2));
  snprintf(logBuf, sizeof(logBuf), " %-25s : %s (Cfg: %s)", "Lock Timer", tBuf1, tBuf2);
  logMessage(logBuf);

  formatSeconds(penaltySecondsRemaining, tBuf1, sizeof(tBuf1));
  formatSeconds(penaltySecondsConfig, tBuf2, sizeof(tBuf2));
  snprintf(logBuf, sizeof(logBuf), " %-25s : %s (Cfg: %s)", "Penalty Timer", tBuf1, tBuf2);
  logMessage(logBuf);

  logMessage(LOG_SEP_MAJOR);

  processLogQueue();
}

// =================================================================
// --- Helper Functions: Initialization Phases ---
// =================================================================

void recoverSessionState() {
  // Load Preferences
  provisioningPrefs.begin("provisioning", true);
  g_enabledChannelsMask = provisioningPrefs.getUChar("chMask", 0x0F);
  provisioningPrefs.end();

  // Load Session State
  bool validStateLoaded = loadState();

  // Handle Logic (e.g. power loss during lock)
  if (validStateLoaded) {
    handleRebootState();
  } else {
    resetToReady(true);
  }
}

void setupPeripherals() {
  // Setup Buttons
  unsigned long longPressMs = (unsigned long)g_systemConfig.longPressSeconds * 1000;
  if (longPressMs < 1000)
    longPressMs = 1000;

  // 1. PCB Button (Always Active)
  pcbButton.setPressMs(longPressMs);
  pcbButton.attachLongPressStart(handleLongPress);
  pcbButton.attachDoubleClick(handleDoublePress);
  pcbButton.attachPress(handlePress); // For hardware test start time
  logKeyValue("System", "PCB Button Configured.");

  // 2. External Button (Only in Release Mode)
#ifdef EXT_BUTTON_PIN
  extButton.setPressMs(longPressMs);
  extButton.setLongPressIntervalMs(longPressMs);

  extButton.attachLongPressStart(handleLongPress);
  extButton.attachDoubleClick(handleDoublePress);
  extButton.attachDuringLongPress(handleLongPress);
  extButton.attachPress(handlePress);
  logKeyValue("System", "External Button Configured.");
#endif

  // Initial LED State
  setLedPattern(currentState);
}

// =================================================================
// --- Helper Functions: Loop Logic ---
// =================================================================

/**
 * Handles the accumulated 1-second ticks from the timer interrupt.
 * Validates hardware state and processes session logic.
 */
void processSessionLogic() {
  uint32_t pendingTicks = 0;

  // Enter critical section to read/reset the volatile counter
  portENTER_CRITICAL(&timerMux);
  if (g_tickCounter > 0) {
    pendingTicks = g_tickCounter;
    g_tickCounter = 0;
  }
  portEXIT_CRITICAL(&timerMux);

  // If time has passed, process the logic
  if (pendingTicks > 0) {
    // Attempt to take lock to process logic
    if (xSemaphoreTakeRecursive(stateMutex, (TickType_t)pdMS_TO_TICKS(50)) == pdTRUE) {
      while (pendingTicks > 0) {
        handleOneSecondTick();
        pendingTicks--;
      }
      // Physics check: Ensure hardware matches software state
      enforceHardwareState();

      xSemaphoreGiveRecursive(stateMutex);
    }
  }
}

// =================================================================
// --- Core Application Setup & Loop ---
// =================================================================

/**
 * Main Arduino setup function. Runs once on boot.
 */
void setup() {
  Serial.begin(SERIAL_BAUD_RATE);
  delay(3000); // Short delay to allow serial to catch up

  // ------------------------------
  // Hardware & Safety
  // ------------------------------
  stateMutex = xSemaphoreCreateRecursiveMutex();
  if (stateMutex == NULL) {
    Serial.println("Critical Error: Could not create Mutex.");
    ESP.restart();
  }

  randomSeed(esp_random());
  initializeChannels();
  initializeFailSafeTimer();
  initializeWatchdog();
  checkBootLoop();

  // ------------------------------
  // Data & State Recovery
  // ------------------------------
  recoverSessionState();
  printStartupDiagnostics();

  // ------------------------------
  // Networking & BLE provisioning
  // ------------------------------
  connectWiFiOrProvision();

  // ------------------------------
  // Master Timer (1 Second Tick)
  // ------------------------------
  logKeyValue("Session", "Attaching master 1-second ticker.");
  oneSecondMasterTicker.attach(1, []() {
    portENTER_CRITICAL_ISR(&timerMux);
    g_tickCounter++;
    portEXIT_CRITICAL_ISR(&timerMux);
  });

  // ------------------------------
  // Input and APIs
  // ------------------------------
  setupWebServer();
  setupPeripherals();

  logKeyValue("System", "Device is operational.");
}

/**
 * Main Arduino loop function. Runs continuously.
 */
void loop() {

  // 1. High Priority: Network Fallback & Watchdog
  handleNetworkFallback();
  esp_task_wdt_reset();

  // 2. System Maintenance
  markBootStability();
  performPeriodicHealthChecks();
  processLogQueue();

  // 3. User Feedback & Input
  if (xSemaphoreTakeRecursive(stateMutex, 0) == pdTRUE) {
    statusLed.Update();

    // Always tick the PCB button
    pcbButton.tick();

// Tick the External button
#ifdef EXT_BUTTON_PIN
    extButton.tick();
#endif

    xSemaphoreGiveRecursive(stateMutex);
  }

  // 4. Core Session Logic (Time-based)
  processSessionLogic();
}