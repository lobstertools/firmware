/*
 * =================================================================
 * Project:   Lobster Lock - Self-Bondage Session Manager
 * File:      main.cpp
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Description:
 * Core firmware for the Lobster Lock ESP32 device. This code
 * manages all session state (e.g., READY, ARMED, LOCKED),
 * provides a JSON API for the web UI, and persists the
 * session to NVS (Preferences) to survive power loss.
 * =================================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <Ticker.h>
#include <esp_task_wdt.h>

// --- Module Includes ---
#include "Config.h"
#include "Globals.h"
#include "Logger.h"
#include "Utils.h"
#include "Hardware.h"
#include "Storage.h"
#include "Network.h"
#include "Session.h"
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
// --- Core Application Setup & Loop ---
// =================================================================

/**
 * Main Arduino setup function. Runs once on boot.
 */
void setup() {

  Serial.begin(SERIAL_BAUD_RATE);
  delay(3000);

  // Initialize the state Mutex before anything else
  stateMutex = xSemaphoreCreateRecursiveMutex();
  if (stateMutex == NULL) {
    Serial.println("Critical Error: Could not create Mutex.");
    ESP.restart();
  }

  // Safety First: Initialize all hardware pins to a safe state (LOW)
  // immediately, before any logic or config loading occurs.
  initializeChannels();

  // --- Boot Loop Detection ---
  checkBootLoop();
  g_bootStartTime = millis();

  // --- Basic hardware init ---
  randomSeed(esp_random());

  // --- Init Failsafe Timer ---
  initializeFailSafeTimer();

  char logBuf[150]; // Increased size slightly for complex lines
  char tBuf1[50];   // Helper buffer 1
  char tBuf2[50];   // Helper buffer 2

  // --- STARTUP BANNER ---
  logMessage(LOG_SEP_MAJOR);
  snprintf(logBuf, sizeof(logBuf), " %s", DEVICE_NAME);
  logMessage(logBuf);
  snprintf(logBuf, sizeof(logBuf), " Version: %s", DEVICE_VERSION);
  logMessage(logBuf);
  logMessage(LOG_SEP_MAJOR);

  logMessage("Initializing hardware watchdog...");
  esp_task_wdt_init(DEFAULT_WDT_TIMEOUT, true);
  esp_task_wdt_add(NULL);

  // =================================================================
  // 1. DATA LOADING
  // =================================================================

  // --- Load Hardware Enable Mask --- 
  provisioningPrefs.begin("provisioning", true);
  g_enabledChannelsMask = provisioningPrefs.getUChar("chMask", 0x0F);
  provisioningPrefs.end();

  // --- Load Session State & Deterrent Configs --- 
  bool validStateLoaded = loadState();

  // --- Determine logical state (Handle power-loss during lock, etc) --- 
  if (validStateLoaded) {
    handleRebootState();
  } else {
    resetToReady(true);
  }

  // =================================================================
  // 2. PERIPHERAL SETUP
  // =================================================================

  // --- Initialize Button
  unsigned long longPressMs = (unsigned long)g_systemConfig.longPressSeconds * 1000;
  if (longPressMs < 1000) {
    longPressMs = 1000;
  }
  button.setPressMs(longPressMs);
  button.attachLongPressStart(handleLongPress);
  button.attachDoubleClick(handleDoublePress);

  // --- Track Press Start for Hardware Test
  button.attachPress(handlePress);

  // --- Set initial LED pattern based on the loaded/determined state
  setLedPattern(currentState);

  // =================================================================
  // 3. LOGGING SECTIONS
  // =================================================================

  // --- SECTION: HARDWARE & FEATURES ---
  logMessage(LOG_SEP_MINOR);
  logMessage("[ HARDWARE & FEATURES ]");
  logMessage(LOG_SEP_MINOR);

  // Channels
  for (int i = 0; i < MAX_CHANNELS; i++) {
    bool isEnabled = (g_enabledChannelsMask >> i) & 1;
    snprintf(logBuf, sizeof(logBuf), " %-25s : %s (GPIO %d)",
             ("Channel " + String(i + 1)).c_str(),
             isEnabled ? "ENABLED" : "DISABLED", HARDWARE_PINS[i]);
    logMessage(logBuf);
  }

  // Other Hardware
  snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Status LED", "Enabled");
  logMessage(logBuf);

  snprintf(logBuf, sizeof(logBuf), " %-25s : %s (Pin %d)", "Foot Pedal/Button",
           "Enabled", ONE_BUTTON_PIN);
  logMessage(logBuf);

  snprintf(logBuf, sizeof(logBuf), " %-25s : %lu ms", "Long Press Time",
           longPressMs);
  logMessage(logBuf);

  // --- SECTION: DETERRENTS ---
  logMessage(LOG_SEP_MINOR);
  logMessage("[ DETERRENT CONFIG ]");
  logMessage(LOG_SEP_MINOR);

  snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Session Streaks",
           enableStreaks ? "Enabled" : "Disabled");
  logMessage(logBuf);

  snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Time Payback",
           enablePaybackTime ? "Enabled" : "Disabled");
  logMessage(logBuf);

  // Only show details if enabled
  if (enablePaybackTime) {
    formatSeconds(paybackTimeSeconds, tBuf1, sizeof(tBuf1));
    snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Payback Duration", tBuf1);
    logMessage(logBuf);
  }

  snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Reward Code",
           enableRewardCode ? "Enabled" : "Disabled");
  logMessage(logBuf);

  // --- SECTION: STATISTICS ---
  logMessage(LOG_SEP_MINOR);
  logMessage("[ STATISTICS ]");
  logMessage(LOG_SEP_MINOR);

  snprintf(logBuf, sizeof(logBuf), " %-25s : %u", "Streak Count",
           sessionStreakCount);
  logMessage(logBuf);

  snprintf(logBuf, sizeof(logBuf), " %-25s : %u", "Completed Sessions",
           completedSessions);
  logMessage(logBuf);

  snprintf(logBuf, sizeof(logBuf), " %-25s : %u", "Aborted Sessions",
           abortedSessions);
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

  snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Current State",
           stateToString(currentState));
  logMessage(logBuf);

  snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Timer Visibility",
           hideTimer ? "Hidden" : "Visible");
  logMessage(logBuf);

  // Lock Timer (Remaining vs Config)
  formatSeconds(lockSecondsRemaining, tBuf1, sizeof(tBuf1));
  formatSeconds(lockSecondsConfig, tBuf2, sizeof(tBuf2));
  snprintf(logBuf, sizeof(logBuf), " %-25s : %s (Cfg: %s)", "Lock Timer", tBuf1,
           tBuf2);
  logMessage(logBuf);

  // Penalty Timer (Remaining vs Config)
  formatSeconds(penaltySecondsRemaining, tBuf1, sizeof(tBuf1));
  formatSeconds(penaltySecondsConfig, tBuf2, sizeof(tBuf2));
  snprintf(logBuf, sizeof(logBuf), " %-25s : %s (Cfg: %s)", "Penalty Timer",
           tBuf1, tBuf2);
  logMessage(logBuf);

  logMessage(LOG_SEP_MAJOR); // End of logging blocks

  // =================================================================
  // 4. NETWORK STARTUP
  // =================================================================

  // --- WiFi or BLE Provisioning ---
  waitForNetwork();

  // =================================================================
  // 5. MASTER TIMER
  // =================================================================
  logMessage("Attaching master 1-second ticker.");

  // ISR-Safe Ticker Callback
  oneSecondMasterTicker.attach(1, []() {
    portENTER_CRITICAL_ISR(&timerMux);
    g_tickCounter++;
    portEXIT_CRITICAL_ISR(&timerMux);
  });

  // =================================================================
  // 6. API endpoints
  // =================================================================

  // --- HTTP Server ---
  setupWebServer();

  logMessage("Device is operational.");   
}

/**
 * Main Arduino loop function. Runs continuously.
 * Handles periodic tasks (NTP retry, LED updates, Button polling).
 */
void loop() {
  // 0. Check for WiFi Failure Fallback
  if (g_triggerProvisioning) {
    g_triggerProvisioning = false; // Clear flag

    // Shut down WiFi to save power/conflicts
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);

    logMessage("!!! Connection Failed. Entering BLE Provisioning Mode !!!");

    // Enter blocking provisioning mode
    // Note: This function ends with ESP.restart(), so we never return here.
    startBLEProvisioning();
  }

  esp_task_wdt_reset(); // Feed the watchdog

  // Check if we have been running long enough to be considered "Stable"
  // Uses g_systemConfig.stableBootTimeMs
  if (!g_bootMarkedStable &&
      (millis() - g_bootStartTime > g_systemConfig.stableBootTimeMs)) {
    g_bootMarkedStable = true;
    bootPrefs.begin("boot", false);
    bootPrefs.putInt("crashes", 0);
    bootPrefs.end();
    logMessage("System stable. Boot loop counter reset.");
  }

  // Interval-based Health Checks (Every 60s)
  if (millis() - g_lastHealthCheck > 60000) {
    checkHeapHealth();
    checkSystemHealth();
    g_lastHealthCheck = millis();
  }

  // Drain the log queue (Non-blocking I/O)
  processLogQueue();

  // --- 2. JLed ---
  // JLed is not thread safe.
  if (xSemaphoreTakeRecursive(stateMutex, 0) == pdTRUE) {
    statusLed.Update(); // Update JLed state machine
    xSemaphoreGiveRecursive(stateMutex);
  }

  // --- 3. Button ---
  button.tick(); // Poll the button

  // --- 4. One Second Tick (Accumulated) ---
  // ISR Safe Read of g_tickCounter
  uint32_t pendingTicks = 0;
  portENTER_CRITICAL(&timerMux);
  if (g_tickCounter > 0) {
    pendingTicks = g_tickCounter;
    g_tickCounter = 0;
  }
  portEXIT_CRITICAL(&timerMux);

  if (pendingTicks > 0) {
    // Only process ticks if we can get the lock.
    if (xSemaphoreTakeRecursive(stateMutex, (TickType_t)pdMS_TO_TICKS(50)) ==
        pdTRUE) {
      while (pendingTicks > 0) {
        handleOneSecondTick();
        pendingTicks--;
      }
      
      // Run immediately after processing logic ticks to ensure synchronization.
      // Checks for anomalies and ensures ARMED/Safety logic is respected.
      enforceHardwareState();
      
      xSemaphoreGiveRecursive(stateMutex);
    } else {
      // Mutex contention detected.
      // For this application, dropping a second under extreme load is better
      // than deadlock.
    }
  }
}