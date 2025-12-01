/*
 * =================================================================
 * Project:   Lobster Lock - Self-Bondage Session Manager
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Description:
 * Core firmware for the Lobster Lock ESP32 device.
 * =================================================================
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

// Health Check Timers
unsigned long g_lastHealthCheck = 0;
unsigned long g_bootStartTime = 0;
bool g_bootMarkedStable = false;

// =================================================================
// --- Helper Functions ---
// =================================================================

/**
 * Consolidated function to log all system statistics and configurations
 * on startup. Keeps setup() clean.
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
  logMessage(LOG_SEP_MAJOR);

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

  snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Status LED", "Enabled");
  logMessage(logBuf);
  snprintf(logBuf, sizeof(logBuf), " %-25s : %s (Pin %d)", "Foot Pedal/Button", "Enabled", ONE_BUTTON_PIN);
  logMessage(logBuf);
  snprintf(logBuf, sizeof(logBuf), " %-25s : %lu ms", "Long Press Time", longPressMs);
  logMessage(logBuf);

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
}

// =================================================================
// --- Core Application Setup & Loop ---
// =================================================================

/**
 * Main Arduino setup function. Runs once on boot.
 */
void setup() {
  Serial.begin(SERIAL_BAUD_RATE);
  delay(1000); // Short delay to allow serial to catch up

  // ----------------------------------------------------------------
  // PHASE 1: CRITICAL HARDWARE & SAFETY
  // ----------------------------------------------------------------

  // 1. Mutex (Must exist before any threaded logic)
  stateMutex = xSemaphoreCreateRecursiveMutex();
  if (stateMutex == NULL) {
    Serial.println("Critical Error: Could not create Mutex.");
    ESP.restart();
  }

  // 2. Hardware Output Safety (Force Pins LOW immediately)
  initializeChannels();

  // 3. Boot Loop Detection
  checkBootLoop();
  g_bootStartTime = millis();

  // ----------------------------------------------------------------
  // PHASE 2: SYSTEM INFRASTRUCTURE
  // ----------------------------------------------------------------

  randomSeed(esp_random());
  initializeFailSafeTimer();

  // Hardware Watchdog
  logMessage("Initializing hardware watchdog...");
  esp_task_wdt_init(DEFAULT_WDT_TIMEOUT, true);
  esp_task_wdt_add(NULL);

  // ----------------------------------------------------------------
  // PHASE 3: DATA & STATE RECOVERY
  // ----------------------------------------------------------------

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

  // ----------------------------------------------------------------
  // PHASE 4: PERIPHERAL CONFIGURATION
  // ----------------------------------------------------------------

  // Setup Button
  unsigned long longPressMs = (unsigned long)g_systemConfig.longPressSeconds * 1000;
  if (longPressMs < 1000)
    longPressMs = 1000;

  button.setPressMs(longPressMs);
  button.attachLongPressStart(handleLongPress);
  button.attachDoubleClick(handleDoublePress);
  button.attachPress(handlePress); // For hardware test start time

  // Initial LED State
  setLedPattern(currentState);

  // ----------------------------------------------------------------
  // PHASE 5: DIAGNOSTICS & LOGGING
  // ----------------------------------------------------------------
  printStartupDiagnostics();

  // ----------------------------------------------------------------
  // PHASE 6: CONNECTIVITY & TASKS
  // ----------------------------------------------------------------

  // Network (Blocking until connected or Provisioning triggered)
  waitForNetwork();

  // Master Timer (1 Second Tick)
  logMessage("Attaching master 1-second ticker.");
  oneSecondMasterTicker.attach(1, []() {
    portENTER_CRITICAL_ISR(&timerMux);
    g_tickCounter++;
    portEXIT_CRITICAL_ISR(&timerMux);
  });

  // Web API
  setupWebServer();

  logMessage("Device is operational.");
}

/**
 * Main Arduino loop function. Runs continuously.
 */
void loop() {
  // 0. Check for WiFi Failure Fallback
  if (g_triggerProvisioning) {
    g_triggerProvisioning = false;
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    logMessage("!!! Connection Failed. Entering BLE Provisioning Mode !!!");
    startBLEProvisioning(); // Blocking + Restart
  }

  esp_task_wdt_reset(); // Feed the watchdog

  // 1. Stable Boot Marking
  if (!g_bootMarkedStable && (millis() - g_bootStartTime > g_systemConfig.stableBootTimeMs)) {
    g_bootMarkedStable = true;
    bootPrefs.begin("boot", false);
    bootPrefs.putInt("crashes", 0);
    bootPrefs.end();
    logMessage("System stable. Boot loop counter reset.");
  }

  // 2. Health Checks (Every 60s)
  if (millis() - g_lastHealthCheck > 60000) {
    checkHeapHealth();
    checkSystemHealth();
    g_lastHealthCheck = millis();
  }

  // 3. Housekeeping (Logs & LEDs)
  processLogQueue();

  if (xSemaphoreTakeRecursive(stateMutex, 0) == pdTRUE) {
    statusLed.Update();
    xSemaphoreGiveRecursive(stateMutex);
  }

  // 4. Input Polling
  button.tick();

  // 5. Master Time Ticking (Accumulated)
  uint32_t pendingTicks = 0;
  portENTER_CRITICAL(&timerMux);
  if (g_tickCounter > 0) {
    pendingTicks = g_tickCounter;
    g_tickCounter = 0;
  }
  portEXIT_CRITICAL(&timerMux);

  if (pendingTicks > 0) {
    // Attempt to take lock to process logic
    if (xSemaphoreTakeRecursive(stateMutex, (TickType_t)pdMS_TO_TICKS(50)) == pdTRUE) {
      while (pendingTicks > 0) {
        handleOneSecondTick();
        pendingTicks--;
      }
      enforceHardwareState();
      xSemaphoreGiveRecursive(stateMutex);
    }
  }
}