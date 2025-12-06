/*
 * =================================================================================
 * Project:   Lobster Lock - Self-Bondage Session Manager
 * File:      Hardware.cpp
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Description:
 * Low-level hardware abstraction layer. Manages GPIO control for channels,
 * LED patterns, fail-safe ISRs (Death Grip), system health monitoring
 * (Stack/Heap/Temp), and hardware initialization.
 * =================================================================================
 */
#include "esp_timer.h"
#include <esp_task_wdt.h>

#include "Globals.h"
#include "Hardware.h"
#include "Logger.h"
#include "Session.h"
#include "Utils.h"

// Health Check Timers
unsigned long g_lastHealthCheck = 0;
unsigned long g_bootStartTime = 0;
bool g_bootMarkedStable = false;

// --- Fail-Safe Timer Handle ---
esp_timer_handle_t failsafeTimer = NULL;

// =================================================================================
// SECTION: INTERRUPT SERVICE ROUTINES (ISRs)
// =================================================================================

/**
 * Hardware Timer Callback for "Death Grip".
 * This runs in ISR context when the absolute maximum safety limit is hit.
 * Forces a reboot to ensure pins go low.
 */
void IRAM_ATTR failsafe_timer_callback(void *arg) {

  logKeyValue("System", "!!CRITICAL!! Death Grip Timer Callback. Unlocking all channels.");
  sendChannelOffAll();

  processLogQueue();

  // While technically not 100% ISR-safe (can panic), a panic reset
  // is exactly what we want for a "Failsafe" condition.
  esp_restart();
}

// =================================================================================
// SECTION: INITIALIZATION
// =================================================================================

/**
 * Hardware Watchdog.
 */
void initializeWatchdog() {
  logKeyValue("System", "Initializing Hardware Watchdog...");
  esp_task_wdt_init(DEFAULT_WDT_TIMEOUT, true);
  esp_task_wdt_add(NULL);
}

/**
 * Initializes all channel pins as outputs.
 * Sets them to LOW immediately to prevent floating states on boot.
 * This function does NOT check logical 'enabled' status, it is purely hardware
 * init.
 */
void initializeChannels() {
  logKeyValue("System", "Initializing Channels, Setting all 4 to Output LOW (Safe State)");

  // Iterate through all physical pins defined in hardware config
  for (int i = 0; i < MAX_CHANNELS; i++) {
    pinMode(HARDWARE_PINS[i], OUTPUT);
    digitalWrite(HARDWARE_PINS[i], LOW); // Ensure circuit is open
  }
}

// =================================================================================
// SECTION: CORE HARDWARE LOGIC
// =================================================================================

/**
 * Continuous safety enforcement & Anomaly detection in hardware state
 * * 1. LOCKED / TESTING = Channels ON (if enabled).
 * 2. ALL OTHER STATES = Channels OFF.
 * * Returns true if a correction was applied.
 */
bool enforceHardwareState() {
  bool correctionApplied = false;

  // ------------------------------------------------------------
  // 0. STATUS LED & STATE CHANGE DETECTION
  // ------------------------------------------------------------
  // Track state history to update LEDs immediately upon transition,
  // even if the physical pin mask (relays) does not change.
  static DeviceState lastEnforcedState = (DeviceState)-1; // Initialize to invalid to force first update

  if (g_currentState != lastEnforcedState) {
    setLedPattern();
    lastEnforcedState = g_currentState;
  }

  // ------------------------------------------------------------
  // 1. SAFETY: Detect Memory Corruption
  // ------------------------------------------------------------
  if (g_currentState > TESTING) {
    static unsigned long lastPanicLog = 0;
    if (millis() - lastPanicLog > 1000) {
      logKeyValue("System", "CRITICAL: Invalid State Enum! Failsafe Triggered.");
      lastPanicLog = millis();
    }

    sendChannelOffAll();
    delay(3000);
    ESP.restart();

    return true;
  }

  // ------------------------------------------------------------
  // 2. CALCULATE TARGET MASK
  // ------------------------------------------------------------
  uint8_t targetMask = 0;

  for (int i = 0; i < MAX_CHANNELS; i++) {
    bool isEnabled = (g_enabledChannelsMask >> i) & 1;
    bool shouldBeHigh = false;

    if (isEnabled) {
      switch (g_currentState) {
      case LOCKED:
      case TESTING:
        shouldBeHigh = true;
        break;

      case ARMED:
        // In countdown, pin is high only if delay has elapsed.
        // Check Active Config for strategy and Session Timers for delays.
        if (g_activeSessionConfig.triggerStrategy == STRAT_AUTO_COUNTDOWN && g_sessionTimers.channelDelays[i] == 0) {
          shouldBeHigh = true;
        }
        break;

      case READY:
      case ABORTED:
      case COMPLETED:
      default:
        shouldBeHigh = false;
        break;
      }
    }

    if (shouldBeHigh) {
      targetMask |= (1 << i);
    }
  }

  // ------------------------------------------------------------
  // 3. DETECT LOGIC CHANGES (For Logging Purposes)
  // ------------------------------------------------------------
  static uint8_t lastTargetMask = 0;
  bool logicHasChanged = (targetMask != lastTargetMask);

  // ------------------------------------------------------------
  // 4. ENFORCE HARDWARE
  // ------------------------------------------------------------
  for (int i = 0; i < MAX_CHANNELS; i++) {
    int pin = HARDWARE_PINS[i];
    int expectedLevel = (targetMask >> i) & 1 ? HIGH : LOW;
    int currentLevel = digitalRead(pin);

    // If physics don't match logic...
    if (currentLevel != expectedLevel) {

      // set hardware state
      digitalWrite(pin, expectedLevel);
      correctionApplied = true;

      if (logicHasChanged) {
        char logBuf[64];
        snprintf(logBuf, sizeof(logBuf), "State Update: Ch%d set to %s", i + 1, expectedLevel ? "HIGH" : "LOW");
        logKeyValue("System", logBuf);
      } else {
        // CRITICAL ANOMALY (Glitch or Interference)
        char logBuf[100];
        snprintf(logBuf, sizeof(logBuf), "CRITICAL ANOMALY: Ch%d drifted to %s! Corrected.", i + 1, currentLevel ? "HIGH" : "LOW");
        logKeyValue("System", logBuf);
      }
    }
  }

  lastTargetMask = targetMask;
  return correctionApplied;
}

// =================================================================================
// SECTION: CHANNEL CONTROL (LOW LEVEL)
// =================================================================================

/**
 * Turns a specific Channel channel ON (closes circuit).
 */
void sendChannelOn(int channelIndex, bool silent) {
  if (channelIndex < 0 || channelIndex >= MAX_CHANNELS)
    return;

  // Hardware Safety Check: Is this channel enabled in config?
  if (!((g_enabledChannelsMask >> channelIndex) & 1)) {
    return; // Silently fail if channel is disabled in config
  }

  if (!silent) {
    char logBuf[50];
    snprintf(logBuf, sizeof(logBuf), "Channel %d: ON ", channelIndex + 1);
    logKeyValue("System", logBuf);
  }

  digitalWrite(HARDWARE_PINS[channelIndex], HIGH);
}

/**
 * Turns a specific Channel channel OFF (opens circuit).
 */
void sendChannelOff(int channelIndex, bool silent) {
  if (channelIndex < 0 || channelIndex >= MAX_CHANNELS)
    return;
  // We always allow turning off, even if disabled, for safety.

  if (!silent) {
    char logBuf[50];
    snprintf(logBuf, sizeof(logBuf), "Channel %d: OFF", channelIndex + 1);
    logKeyValue("System", logBuf);
  }

  digitalWrite(HARDWARE_PINS[channelIndex], LOW);
}

/**
 * Turns all Channel channels ON.
 */
void sendChannelOnAll() {
  logKeyValue("System", "All Enabled Channels ON");
  for (int i = 0; i < MAX_CHANNELS; i++) {
    sendChannelOn(i, true);
  }
}

/**
 * Turns all Channel channels OFF.
 */
void sendChannelOffAll() {
  logKeyValue("System", "All Enabled Channels OFF");
  for (int i = 0; i < MAX_CHANNELS; i++) {
    sendChannelOff(i, true);
  }
}

// =================================================================================
// SECTION: FAILSAFE TIMER CONTROL
// =================================================================================

/**
 * Hardware Timer for "Death Grip".
 */
void initializeFailSafeTimer() {
  logKeyValue("System", "Initializing Death Grip Timer...");
  const esp_timer_create_args_t failsafe_timer_args = {.callback = &failsafe_timer_callback, .name = "failsafe_wdt"};
  esp_timer_create(&failsafe_timer_args, &failsafeTimer);
}

/**
 * Arms the independent hardware timer.
 */
void armFailsafeTimer() {
  if (failsafeTimer == NULL) {
    logKeyValue("System", "!!CRITICAL!! armFailsafeTimer called before initializeFailSafeTimer.");
    return;
  }

  // Start one-shot timer. Converted from seconds to microseconds.
  uint64_t timeout_us = (uint64_t)g_systemDefaults.failsafeMaxLock * 1000000ULL;
  esp_timer_start_once(failsafeTimer, timeout_us);

  char timeStr[64];
  formatSeconds(g_systemDefaults.failsafeMaxLock, timeStr, sizeof(timeStr));

  char logBuf[100];
  snprintf(logBuf, sizeof(logBuf), "Death Grip Timer ARMED: %s", timeStr);
  logKeyValue("System", logBuf);
}

/**
 * Disarms the independent hardware timer.
 */
void disarmFailsafeTimer() {
  if (failsafeTimer == NULL) {
    logKeyValue("System", "!!CRITICAL!! disarmFailsafeTimer called before initializeFailSafeTimer.");
    return;
  }

  esp_timer_stop(failsafeTimer);
  logKeyValue("System", "Death Grip Timer DISARMED.");
}

// =================================================================================
// SECTION: INPUT HANDLING (BUTTONS)
// =================================================================================

/**
 * Immediate callback when button goes active (DOWN).
 * Used for hardware state.
 */
void handlePress() { g_buttonPressStartTime = millis(); }

/**
 * DOUBLE PRESS: Used to TRIGGER the session when ARMED.
 */
void handleDoublePress() {
  if (xSemaphoreTakeRecursive(stateMutex, (TickType_t)pdMS_TO_TICKS(100)) == pdTRUE) {

    logKeyValue("System", "Double-Click detected.");

    triggerLock("Button Double-Click");

    xSemaphoreGiveRecursive(stateMutex);
  }
}

/**
 * LONG PRESS:  Universal Cancel/Abort.
 * 1. If ARMED: Cancels arming (Returns to READY, no penalty).
 * 2. If LOCKED: Triggers ABORT (Emergency Stop / Penalty).
 */
void handleLongPress() {
  if (xSemaphoreTakeRecursive(stateMutex, (TickType_t)pdMS_TO_TICKS(100)) == pdTRUE) {

    logKeyValue("System", "Long-Press detected.");

    abortSession("Button Long-Press");

    xSemaphoreGiveRecursive(stateMutex);
  }
}

// =================================================================================
// SECTION: SYSTEM HEALTH & SAFETY
// =================================================================================

/**
 * Check for rapid crashes and enter safe mode if detected.
 */
void checkBootLoop() {

  bootPrefs.begin("boot", false);
  int crashes = bootPrefs.getInt("crashes", 0);

  // Use system default threshold
  if (crashes >= g_systemDefaults.bootLoopThreshold) {
    Serial.println("CRITICAL: Boot Loop Detected! Entering Safe Mode.");

    // Safe Mode: Delay startup, disarm everything.
    // This gives the power rail time to stabilize or user time to factory reset.
    delay(5000);
    sendChannelOffAll();

    pinMode(STATUS_LED_PIN, OUTPUT);
    digitalWrite(STATUS_LED_PIN, HIGH);

    delay(30000); // 30 Second penalty box before attempting start
  }

  bootPrefs.putInt("crashes", crashes + 1);
  bootPrefs.end();

  g_bootStartTime = millis();
}

void markBootStability() {
  // Use system default for stable time
  if (!g_bootMarkedStable && (millis() - g_bootStartTime > g_systemDefaults.stableBootTime)) {
    g_bootMarkedStable = true;
    bootPrefs.begin("boot", false);
    bootPrefs.putInt("crashes", 0);
    bootPrefs.end();
    logKeyValue("System", "System stable. Boot loop counter reset.");
  }
}

/**
 * Monitor heap memory and emergency stop if fragmentation is high.
 */
void checkHeapHealth() {
  size_t freeMem = ESP.getFreeHeap();
  size_t largestBlock = ESP.getMaxAllocHeap();

  // If fragmentation is high (plenty of free mem but no big blocks)
  if (largestBlock < 8192 && freeMem > 20000) {
    logKeyValue("System", "WARNING: Heap fragmentation detected!");
  }

  // Critical low memory
  if (freeMem < 10000) {
    logKeyValue("System", "CRITICAL: Low Heap! Executing failsafe.");
    sendChannelOffAll();
  }
}

/**
 * Monitor stack usage and internal temperature
 * and emergency stop if overflow is imminent or overheating.
 */
void checkSystemHealth() {

  // 1. Stack Health Check
  UBaseType_t loopStack = uxTaskGetStackHighWaterMark(NULL);
  TaskHandle_t asyncHandle = xTaskGetHandle("async_tcp");
  UBaseType_t asyncStack = 0;
  if (asyncHandle != NULL) {
    asyncStack = uxTaskGetStackHighWaterMark(asyncHandle);
  }

  if (loopStack < 512 || (asyncHandle != NULL && asyncStack < 512)) {
    logKeyValue("System", "CRITICAL: Stack near overflow! Emergency Stop.");
    sendChannelOffAll();
    // Force a restart as memory corruption is likely
    ESP.restart();
  }

  // 2. Thermal Protection
  float currentTemp = temperatureRead(); // The die temperature in Celsius.

  // Validate reading (isnan check) and compare against threshold
  if (!isnan(currentTemp) && currentTemp > MAX_SAFE_TEMP_C) {

    // Log the critical event
    char logBuf[100];
    snprintf(logBuf, sizeof(logBuf), "CRITICAL: Overheating detected (%.1f C)! Emergency Stop.", currentTemp);
    logKeyValue("System", logBuf);

    sendChannelOffAll();
  }
}

/**
 * Executes non-blocking health checks every 60 seconds.
 */
void performPeriodicHealthChecks() {
  if (millis() - g_lastHealthCheck > 60000) {
    checkHeapHealth();
    checkSystemHealth();
    g_lastHealthCheck = millis();
  }
}

/**
 * Dynamically update the Task Watchdog Timeout period.
 */
void updateWatchdogTimeout(uint32_t seconds) {
  esp_task_wdt_init(seconds, true);
  char logBuf[50];
  snprintf(logBuf, sizeof(logBuf), "Hardware Watchdog Timeout set to %u s", seconds);
  logKeyValue("System", logBuf);
}

// =================================================================================
// SECTION: FEEDBACK (LEDS)
// =================================================================================

/**
 * Sets the JLed pattern based on the current session state.
 */
void setLedPattern() {
  char logBuf[50];
  snprintf(logBuf, sizeof(logBuf), "Setting LED pattern for: %s", stateToString(g_currentState));
  logKeyValue("System", logBuf);

  switch (g_currentState) {
  case VALIDATING:
    // Triple Flash Heartbeat - "System Initializing / Busy"
    statusLed.Blink(100, 100).Repeat(3).DelayAfter(1000).Forever();
    break;
  case READY:
    // Slow Breath - Waiting for session configuration
    statusLed.Breathe(4000).Forever();
    break;
  case ARMED:
    // Fast Blink (2Hz) - Waiting for trigger or counting down
    statusLed.Blink(250, 250).Forever();
    break;
  case LOCKED:
    // Solid On - Solid as a lock
    statusLed.On().Forever();
    break;
  case ABORTED:
    // Standard Blink (1Hz) - Steady "Penalty Wait" pacing
    statusLed.Blink(500, 500).Forever();
    break;
  case COMPLETED:
    // Slow Double-Blink notification - Ready for more?
    statusLed.Blink(200, 200).Repeat(2).DelayAfter(3000).Forever();
    break;
  case TESTING:
    // Medium Pulse - I'm alive and all is well
    statusLed.FadeOn(750).FadeOff(750).Forever();
    break;
  default:
    // Default to off
    statusLed.Off().Forever();
    break;
  }
}