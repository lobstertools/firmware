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

  // Channel Status (from Provisioning)
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

  snprintf(logBuf, sizeof(logBuf), " %-25s : %u s", "Long Press Time", g_systemDefaults.longPressDuration);
  logMessage(logBuf);

  snprintf(logBuf, sizeof(logBuf), " %-25s : %u ms", "External Button Threshold", g_systemDefaults.extButtonSignalDuration);
  logMessage(logBuf);

  processLogQueue();

  // --- SECTION: SYSTEM DEFAULTS ---
  logMessage(LOG_SEP_MINOR);
  logMessage("[ SYSTEM DEFAULTS ]");
  logMessage(LOG_SEP_MINOR);

  snprintf(logBuf, sizeof(logBuf), " %-25s : %u s", "Test Session Duration", g_systemDefaults.testModeDuration);
  logMessage(logBuf);

  snprintf(logBuf, sizeof(logBuf), " %-25s : %u s", "Failsafe Timeout", g_systemDefaults.failsafeMaxLock);
  logMessage(logBuf);

  snprintf(logBuf, sizeof(logBuf), " %-25s : %u ms / %u", "Keep-Alive", g_systemDefaults.keepAliveInterval,
           g_systemDefaults.keepAliveMaxStrikes);
  logMessage(logBuf);

  snprintf(logBuf, sizeof(logBuf), " %-25s : %u / %u ms", "Boot Loop/Stable", g_systemDefaults.bootLoopThreshold,
           g_systemDefaults.stableBootTime);
  logMessage(logBuf);

  snprintf(logBuf, sizeof(logBuf), " %-25s : %u / %u s", "WiFi Retries/ArmedTO", g_systemDefaults.wifiMaxRetries,
           g_systemDefaults.armedTimeoutSeconds);
  logMessage(logBuf);

  processLogQueue();

  // --- SECTION: SESSION LIMITS ---
  logMessage(LOG_SEP_MINOR);
  logMessage("[ SESSION LIMITS ]");
  logMessage(LOG_SEP_MINOR);

  snprintf(logBuf, sizeof(logBuf), " %-25s : %u / %u s", "Lock Range", g_sessionLimits.minLockDuration, g_sessionLimits.maxLockDuration);
  logMessage(logBuf);

  snprintf(logBuf, sizeof(logBuf), " %-25s : %u / %u s", "Penalty Range", g_sessionLimits.minRewardPenaltyDuration,
           g_sessionLimits.maxRewardPenaltyDuration);
  logMessage(logBuf);

  snprintf(logBuf, sizeof(logBuf), " %-25s : %u / %u s", "Payback Range", g_sessionLimits.minPaybackTime, g_sessionLimits.maxPaybackTime);
  logMessage(logBuf);

  processLogQueue();

  // --- SECTION: DETERRENTS ---
  logMessage(LOG_SEP_MINOR);
  logMessage("[ DETERRENT CONFIG ]");
  logMessage(LOG_SEP_MINOR);

  snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Session Streaks", g_deterrentConfig.enableStreaks ? "Enabled" : "Disabled");
  logMessage(logBuf);
  snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Time Payback", g_deterrentConfig.enablePaybackTime ? "Enabled" : "Disabled");
  logMessage(logBuf);

  if (g_deterrentConfig.enablePaybackTime) {
    formatSeconds(g_deterrentConfig.paybackTime, tBuf1, sizeof(tBuf1));
    snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Payback Duration", tBuf1);
    logMessage(logBuf);
  }
  snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Reward Code", g_deterrentConfig.enableRewardCode ? "Enabled" : "Disabled");
  logMessage(logBuf);

  if (g_deterrentConfig.enableRewardCode) {
    formatSeconds(g_deterrentConfig.rewardPenalty, tBuf1, sizeof(tBuf1));
    snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Reward Penalty", tBuf1);
    logMessage(logBuf);
  }

  processLogQueue();

  // --- SECTION: STATISTICS ---
  logMessage(LOG_SEP_MINOR);
  logMessage("[ STATISTICS ]");
  logMessage(LOG_SEP_MINOR);

  // Read from g_sessionStats
  snprintf(logBuf, sizeof(logBuf), " %-25s : %u", "Streak Count", g_sessionStats.streaks);
  logMessage(logBuf);
  snprintf(logBuf, sizeof(logBuf), " %-25s : %u", "Completed Sessions", g_sessionStats.completed);
  logMessage(logBuf);
  snprintf(logBuf, sizeof(logBuf), " %-25s : %u", "Aborted Sessions", g_sessionStats.aborted);
  logMessage(logBuf);

  formatSeconds(g_sessionStats.paybackAccumulated, tBuf1, sizeof(tBuf1));
  snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Accumulated Debt", tBuf1);
  logMessage(logBuf);

  formatSeconds(g_sessionStats.totalLockedTime, tBuf1, sizeof(tBuf1));
  snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Lifetime Locked", tBuf1);
  logMessage(logBuf);

  processLogQueue();

  // --- SECTION: CURRENT SESSION ---
  logMessage(LOG_SEP_MINOR);
  logMessage("[ CURRENT SESSION ]");
  logMessage(LOG_SEP_MINOR);

  snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Current State", stateToString(g_currentState));
  logMessage(logBuf);
  snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Timer Visibility", g_activeSessionConfig.hideTimer ? "Hidden" : "Visible");
  logMessage(logBuf);

  formatSeconds(g_sessionTimers.lockRemaining, tBuf1, sizeof(tBuf1));
  formatSeconds(g_sessionTimers.lockDuration, tBuf2, sizeof(tBuf2));
  snprintf(logBuf, sizeof(logBuf), " %-25s : %s (Cfg: %s)", "Lock Timer", tBuf1, tBuf2);
  logMessage(logBuf);

  formatSeconds(g_sessionTimers.penaltyRemaining, tBuf1, sizeof(tBuf1));
  formatSeconds(g_sessionTimers.penaltyDuration, tBuf2, sizeof(tBuf2));
  snprintf(logBuf, sizeof(logBuf), " %-25s : %s (Cfg: %s)", "Penalty Timer", tBuf1, tBuf2);
  logMessage(logBuf);

  logMessage(LOG_SEP_MAJOR);

  processLogQueue();
}

// =================================================================
// --- Helper Functions: Initialization Phases ---
// =================================================================

/**
 * Recovers state from NVS.
 * Now splits hardware/provisioning loading from dynamic session loading.
 */
void recoverDeviceState() {
  // 1. Load Provisioning Settings (Hardware Mask + Deterrent Config)
  // This ensures g_deterrentConfig and g_enabledChannelsMask are set before logic runs.
  loadProvisioningConfig();

  // 2. Load Dynamic Session State (Timers, Stats, Active Config)
  bool validStateLoaded = loadState();

  // 3. Handle Logic (e.g. power loss during lock)
  if (validStateLoaded) {
    handleRebootState();
  } else {
    resetToReady(true);
    g_currentState = VALIDATING;
  }
}

void setupPeripherals() {
  // Setup Buttons
  unsigned long longPressMs = (unsigned long)g_systemDefaults.longPressDuration * 1000;
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

  extButton.attachDoubleClick(handleDoublePress);
  extButton.attachDuringLongPress(handleLongPress);
  extButton.attachPress(handlePress);
  logKeyValue("System", "External Button Configured.");
#endif
}

/**
 * Set the current state to READY
 */
void moveToReady() {
  g_currentState = READY;
  logKeyValue("System", "Device is operational.");
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
  recoverDeviceState();
  setTimersForCurrentState(); 

  // ------------------------------
  // Diagnostic Dump
  // ------------------------------
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

#ifdef EXT_BUTTON_PIN
    // Tick the External button
    extButton.tick();

    if (g_currentState == VALIDATING) {
      if (digitalRead(EXT_BUTTON_PIN) == LOW) {
        // It is connected/safe. Start timing if we haven't already.
        if (extButtonSignalStartTime == 0) {
          extButtonSignalStartTime = millis();
        }

        // If it has been safe for sufficient time, proceed
        if (millis() - extButtonSignalStartTime > g_systemDefaults.extButtonSignalDuration * 1000) {
          logKeyValue("System", "Safety Pedal connection verified.");
          extButtonSignalStartTime = 0; // Reset for next time
          moveToReady();
        }
      } else {
        // If it flickers HIGH (disconnected) reset the timer
        extButtonSignalStartTime = 0;
      }
    }
#else
    // in DEBUG, transition to READY immediately
    if (g_currentState == VALIDATING) {
      moveToReady();
    }
#endif

    xSemaphoreGiveRecursive(stateMutex);
  }

  // 4. Core Session Logic (Time-based)
  processSessionLogic();
}