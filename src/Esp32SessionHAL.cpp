/*
 * =================================================================================
 * Project:   Lobster Lock - Self-Bondage Session Manager
 * File:      src/Esp32SessionHAL.cpp
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Description:
 * Low-level hardware abstraction layer. Manages GPIO control for channels,
 * LED patterns, fail-safe ISRs (Death Grip), system health monitoring.
 * =================================================================================
 */
#include "Esp32SessionHAL.h"
#include "esp_timer.h"
#include <esp_task_wdt.h>

#include "Config.h"
#include "Globals.h"
#include "Network.h"
#include "SettingsManager.h"

#define BYTE_TO_BINARY_PATTERN "%c%c%c%c%c%c%c%c"
#define BYTE_TO_BINARY(byte)                                                                                                               \
  ((byte) & 0x80 ? '1' : '0'), ((byte) & 0x40 ? '1' : '0'), ((byte) & 0x20 ? '1' : '0'), ((byte) & 0x10 ? '1' : '0'),                      \
      ((byte) & 0x08 ? '1' : '0'), ((byte) & 0x04 ? '1' : '0'), ((byte) & 0x02 ? '1' : '0'), ((byte) & 0x01 ? '1' : '0')

// =================================================================================
// SECTION: STATIC GLOBALS (Required for ISRs/C-APIs)
// =================================================================================

// Failsafe Timer Handle (Must be static/global for esp_timer callback)
static esp_timer_handle_t s_failsafeTimer = NULL;

/**
 * Hardware Timer Callback for "Death Grip".
 * This runs in ISR context when the absolute maximum safety limit is hit.
 * Forces a reboot to ensure pins go low.
 */
static void IRAM_ATTR failsafe_timer_callback(void *arg) {
  for (int i = 0; i < MAX_CHANNELS; i++) {
    digitalWrite(HARDWARE_PINS[i], LOW);
  }
  // Panic reset is desired for a Failsafe condition.
  esp_restart();
}

// =================================================================================
// SECTION: CLASS IMPLEMENTATION
// =================================================================================

Esp32SessionHAL::Esp32SessionHAL()
    : _triggerActionPending(false), _abortActionPending(false), _shortPressPending(false), _pcbPressed(false), _extPressed(false),
      _pressStartTime(0), _cachedState((DeviceState)-1), _logBufferIndex(0), _queueHead(0), _queueTail(0), _statusLed(JLed(STATUS_LED_PIN)),
      _lastHealthCheck(0), _bootStartTime(0), _bootMarkedStable(false), _enabledChannelsMask(0x0F),
      
      // Safety Logic Init
      _safetyStableStart(0), 
      _safetyLostStart(0), 
      _isSafetyValid(false), 
      _lastSafetyRaw(false),
      // LED Control Init
      _isLedEnabled(true)
{
  // OneButton Setup (Pin, ActiveLow, Pullup)
  _pcbButton = OneButton(PCB_BUTTON_PIN, true, true);

#ifdef EXT_BUTTON_PIN
  if (EXT_BUTTON_PIN != -1) {
    _extButton = OneButton(EXT_BUTTON_PIN, false, true);
  }
#endif

  // Clear log buffer
  for (int i = 0; i < LOG_BUFFER_SIZE; i++)
    _logBuffer[i][0] = '\0';
}

Esp32SessionHAL &Esp32SessionHAL::getInstance() {
  static Esp32SessionHAL instance;
  return instance;
}

// --- Initialization ---

void Esp32SessionHAL::initialize() {

  // 1. Acquire the Mutex
  _stateMutex = xSemaphoreCreateRecursiveMutex();
  if (_stateMutex == NULL) {
    Serial.println("Critical Error: Could not create Mutex.");
    ESP.restart();
  }

  // 2. Random Number Generator
  randomSeed(esp_random());

  // 3. Logging Init
  logKeyValue("System", "Initializing Hardware...");

  // 4. Channels
  logKeyValue("System", "Initializing Channels...");
  for (int i = 0; i < MAX_CHANNELS; i++) {
    pinMode(HARDWARE_PINS[i], OUTPUT);
    digitalWrite(HARDWARE_PINS[i], LOW);
  }

  // 5. Button Attachments
  // We use detailed handlers to track the "Pressed" state for telemetry
  // while preserving the original Click/Double/Long logic.

  // -- PCB Button --
  _pcbButton.attachPress(handlePcbPressStart);        // State: DOWN
  _pcbButton.attachClick(handlePcbClick);             // State: UP + Action: Short
  _pcbButton.attachDoubleClick(handlePcbDoubleClick); // State: UP + Action: Trigger
  _pcbButton.setPressMs(g_systemDefaults.longPressDuration * 1000);
  _pcbButton.attachLongPressStart(handlePcbLongStart); // Action: Abort
  _pcbButton.attachLongPressStop(handlePcbLongStop);   // State: UP

  // -- EXT Button --
#ifdef EXT_BUTTON_PIN
  if (EXT_BUTTON_PIN != -1) {
    _extButton.attachPress(handleExtPressStart);
    _extButton.attachClick(handleExtClick);
    _extButton.attachDoubleClick(handleExtDoubleClick);
    _extButton.setPressMs(g_systemDefaults.longPressDuration * 1000);
    _extButton.attachLongPressStart(handleExtLongStart);
    _extButton.attachLongPressStop(handleExtLongStop);
  }
#endif

  // 6. Watchdogs & Failsafe
  logKeyValue("System", "Initializing Hardware Watchdog...");
  esp_task_wdt_init(DEFAULT_WDT_TIMEOUT, true);
  esp_task_wdt_add(NULL);

  logKeyValue("System", "Initializing Death Grip Timer...");
  const esp_timer_create_args_t failsafe_timer_args = {.callback = &failsafe_timer_callback, .name = "failsafe_wdt"};
  esp_timer_create(&failsafe_timer_args, &s_failsafeTimer);

  // 7. Boot Checks
  checkBootLoop();
  
  // 8. Force Initial LED State
  // Initialize to READY pattern (Breathe) so device is not dark on boot
  _statusLed.Breathe(4000).Forever();
}

// --- Main Tick ---

void Esp32SessionHAL::tick() {

  if (!lockState()) {
    return;
  }
  
  // 1. Process Safety Logic (Before peripherals to ensure graceful aborts)
  updateSafetyLogic();

  // 2. Process Serial Logs (Internal Queue)
  processLogQueue();

  // 3. Tick Peripherals
  _pcbButton.tick();
#ifdef EXT_BUTTON_PIN
  _extButton.tick();
#endif

  _statusLed.Update();

  // 4. Periodic Health Checks (Every 60s)
  if (millis() - _lastHealthCheck > 60000) {
    checkSystemHealth();
    _lastHealthCheck = millis();
  }

  // 5. Maintenance
  markBootStability();

  unlockState();
}

// =================================================================================
// SECTION: SAFETY LOGIC (Debounce / Grace Period)
// =================================================================================

void Esp32SessionHAL::updateSafetyLogic() {
    // 1. Identify Mode
    #ifdef EXT_BUTTON_PIN
        bool isDevMode = (EXT_BUTTON_PIN == -1);
    #else
        bool isDevMode = true;
    #endif

    // 2. Dev Mode Bypass
    // If no hardware switch is defined, we validate immediately.
    // This prevents the "Stabilizing..." delay on boot which conflicts 
    // with restored Critical States (LOCKED/ABORTED) causing false alarms.
    if (isDevMode) {
        _isSafetyValid = true;
        _lastSafetyRaw = true;
        return; 
    }

    // 3. Hardware Logic (Active Switch)
    // NC Switch: LOW = Connected/Safe, HIGH = Disconnected/Unsafe
    // We only reach here if EXT_BUTTON_PIN is defined and valid.
    
    #ifdef EXT_BUTTON_PIN 
    bool isConnectedRaw = (digitalRead(EXT_BUTTON_PIN) == LOW);
    unsigned long now = millis();

    if (isConnectedRaw) {
        // --- CASE: PHYSICALLY CONNECTED ---
        _safetyLostStart = 0; // Clear loss timer

        if (!_lastSafetyRaw) {
            // Rising Edge: Just plugged in
            _safetyStableStart = now;
            logKeyValue("Safety", "Signal Detected. Stabilizing...");
        }

        // On-Delay: Wait for signal to be stable before granting permission
        if (!_isSafetyValid) {
            unsigned long stableTime = g_systemDefaults.extButtonSignalDuration * 1000;
            if (now - _safetyStableStart >= stableTime) {
                _isSafetyValid = true;
                logKeyValue("Safety", "Interlock Verified. Hardware Permitted.");
            }
        }
    } 
    else {
        // --- CASE: PHYSICALLY DISCONNECTED ---
        // (Could be a cable cut OR a button press)

        if (_isSafetyValid) {
            // It WAS valid, now it's gone. Start Grace Period Timer.
            if (_safetyLostStart == 0) {
                _safetyLostStart = now;
            }

            // Off-Delay: Wait for LongPress duration + Buffer (e.g., 500ms)
            // This ensures OneButton has time to detect the LongPress event
            // before we declare the safety invalid.
            unsigned long gracePeriod = (g_systemDefaults.longPressDuration * 1000) + 500;

            if (now - _safetyLostStart > gracePeriod) {
                // Time's up. It wasn't a button press. It's a disconnect.
                _isSafetyValid = false;
                _safetyStableStart = 0;
                logKeyValue("Safety", "Interlock Signal Lost (Timeout).");
            }
            // Else: We are in the grace period. Keep _isSafetyValid = TRUE.
        } 
        else {
            // It wasn't valid to begin with. Reset.
            _safetyStableStart = 0;
        }
    }

    _lastSafetyRaw = isConnectedRaw;
    #endif
}

bool Esp32SessionHAL::isSafetyInterlockValid() {
    return _isSafetyValid;
}

// =================================================================================
// SECTION: CHANNEL CONFIGURATION
// =================================================================================

void Esp32SessionHAL::setChannelMask(uint8_t mask) { _enabledChannelsMask = mask; }

uint8_t Esp32SessionHAL::getChannelMask() const { return _enabledChannelsMask; }

bool Esp32SessionHAL::isChannelEnabled(int channelIndex) const {
  if (channelIndex < 0 || channelIndex >= MAX_CHANNELS)
    return false;
  return (_enabledChannelsMask >> channelIndex) & 1;
}

bool Esp32SessionHAL::isButtonPressed() const { return _pcbPressed || _extPressed; }

uint32_t Esp32SessionHAL::getCurrentPressDurationMs() const {
  if (!isButtonPressed() || _pressStartTime == 0) {
    return 0;
  }
  return millis() - _pressStartTime;
}

// =================================================================================
// SECTION: HARDWARE CONTROL
// =================================================================================

void Esp32SessionHAL::setLedEnabled(bool enabled) {
    if (_isLedEnabled == enabled) return;
    _isLedEnabled = enabled;

    if (enabled) {
        // If enabling, force a pattern refresh for the current cached state
        DeviceState current = _cachedState;
        _cachedState = (DeviceState)-1; // Invalidate cache to force updateLedPattern logic to run
        updateLedPattern(current);
    } else {
        // If disabling, turn off immediately
        _statusLed.Off().Forever();
    }
}

// =================================================================================
// SECTION: SYNCHRONIZATION
// =================================================================================

bool Esp32SessionHAL::lockState(uint32_t timeoutMs) {
  if (_stateMutex == NULL)
    return false;
  return (xSemaphoreTakeRecursive(_stateMutex, (TickType_t)pdMS_TO_TICKS(timeoutMs)) == pdTRUE);
}

void Esp32SessionHAL::unlockState() {
  if (_stateMutex != NULL) {
    xSemaphoreGiveRecursive(_stateMutex);
  }
}

// =================================================================================
// SECTION: LOGGING SYSTEM
// =================================================================================

void Esp32SessionHAL::log(const char *message) {
  // 1. Write to RAM (WebAPI Buffer)
  strncpy(_logBuffer[_logBufferIndex], message, MAX_LOG_LENGTH);
  _logBuffer[_logBufferIndex][MAX_LOG_LENGTH - 1] = '\0';

  _logBufferIndex++;
  if (_logBufferIndex >= LOG_BUFFER_SIZE) {
    _logBufferIndex = 0;
  }

  // 2. Write to Serial Queue
  int nextHead = (_queueHead + 1) % SERIAL_QUEUE_SIZE;
  if (nextHead != _queueTail) {
    strncpy(_serialQueue[_queueHead], message, MAX_LOG_LENGTH);
    _serialQueue[_queueHead][MAX_LOG_LENGTH - 1] = '\0';
    _queueHead = nextHead;
  }
  // Else: Queue full, drop message to prevent blocking
}

void Esp32SessionHAL::logKeyValue(const char *key, const char *value) {
  char tempBuf[MAX_LOG_LENGTH];
  snprintf(tempBuf, MAX_LOG_LENGTH, " %-8s : %s", key, value);
  log(tempBuf);
}

void Esp32SessionHAL::processLogQueue() {
  // Process a batch of logs to keep Serial active without blocking too long
  int maxLines = 10;
  while (_queueHead != _queueTail && maxLines > 0) {
    Serial.println(_serialQueue[_queueTail]);
    _queueTail = (_queueTail + 1) % SERIAL_QUEUE_SIZE;
    maxLines--;
  }
}

void Esp32SessionHAL::printStartupDiagnostics() {
  char logBuf[128];

  log("==========================================================================");
  log("                            DEVICE DIAGNOSTICS                           ");
  log("==========================================================================");

  // -------------------------------------------------------------------------
  // SECTION: SYSTEM HEALTH
  // -------------------------------------------------------------------------
  log("[ SYSTEM HEALTH ]");

  // Heap Memory
  uint32_t freeHeap = ESP.getFreeHeap();
  snprintf(logBuf, sizeof(logBuf), " %-25s : %u bytes", "Free Heap", freeHeap);
  log(logBuf);

  // Temperature (Built-in sensor)
  float temp = temperatureRead();
  snprintf(logBuf, sizeof(logBuf), " %-25s : %.1f C", "CPU Temp", temp);
  log(logBuf);

  // Crash Counters
  int crashes = SettingsManager::getCrashCount();
  snprintf(logBuf, sizeof(logBuf), " %-25s : %d", "Recorded Crashes", crashes);
  log(logBuf);

  // -------------------------------------------------------------------------
  // SECTION: GPIO CONFIGURATION
  // -------------------------------------------------------------------------
  log(""); // Spacer
  log("[ GPIO & PERIPHERALS ]");

  // Buttons
  snprintf(logBuf, sizeof(logBuf), " %-25s : GPIO %d", "PCB Button", PCB_BUTTON_PIN);
  log(logBuf);

#ifdef EXT_BUTTON_PIN
  if (EXT_BUTTON_PIN != -1) {
    snprintf(logBuf, sizeof(logBuf), " %-25s : GPIO %d (Active Low)", "Ext. Safety Switch", EXT_BUTTON_PIN);
  } else {
    snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Ext. Safety Switch", "DISABLED");
  }
#else
  snprintf(logBuf, sizeof(logBuf), " %-25s : %s", "Ext. Safety Switch", "NOT DEFINED");
#endif
  log(logBuf);

  // Status LED
  snprintf(logBuf, sizeof(logBuf), " %-25s : GPIO %d", "Status LED", STATUS_LED_PIN);
  log(logBuf);

  // -------------------------------------------------------------------------
  // SECTION: CHANNEL OUTPUTS
  // -------------------------------------------------------------------------
  log(""); // Spacer
  log("[ CHANNEL STATUS ]");

  // Global Mask
  snprintf(logBuf, sizeof(logBuf), " %-25s : 0x%02X (Binary: " BYTE_TO_BINARY_PATTERN ")", "Hardware Mask", _enabledChannelsMask,
           BYTE_TO_BINARY(_enabledChannelsMask));
  log(logBuf);

  // Individual Pins
  for (int i = 0; i < MAX_CHANNELS; i++) {
    // Read actual physical state of the pin
    int state = digitalRead(HARDWARE_PINS[i]);
    bool enabledInMask = (_enabledChannelsMask >> i) & 1;

    snprintf(logBuf, sizeof(logBuf), " %-25s : GPIO %d | State: %s | Mask: %s", (String("Channel ") + String(i + 1)).c_str(),
             HARDWARE_PINS[i], state == HIGH ? "HIGH (ON)" : "LOW (OFF)", enabledInMask ? "ENABLED" : "MASKED");
    log(logBuf);
  }
}

// =================================================================================
// SECTION: ISessionHAL IMPLEMENTATION
// =================================================================================

void Esp32SessionHAL::setHardwareSafetyMask(uint8_t mask) {
  for (int i = 0; i < MAX_CHANNELS; i++) {
    // Logic: mask bit 1 = HIGH, 0 = LOW
    int level = (mask >> i) & 1 ? HIGH : LOW;
    digitalWrite(HARDWARE_PINS[i], level);
  }
}

// --- Input Events ---

bool Esp32SessionHAL::checkTriggerAction() {
  if (_triggerActionPending) {
    _triggerActionPending = false;
    return true;
  }
  return false;
}

bool Esp32SessionHAL::checkAbortAction() {
  if (_abortActionPending) {
    _abortActionPending = false;
    return true;
  }
  return false;
}

bool Esp32SessionHAL::checkShortPressAction() {
  if (_shortPressPending) {
    _shortPressPending = false;
    return true;
  }
  return false;
}

// --- Safety Interlock ---

bool Esp32SessionHAL::isSafetyInterlockEngaged() {
#ifdef EXT_BUTTON_PIN
  // Production: Normally Closed (NC) switch.
  // CLOSED (GND/LOW) = Safe/Connected.
  // OPEN (VCC/HIGH) = Unsafe/Disconnected.
  if (EXT_BUTTON_PIN != -1) return (digitalRead(EXT_BUTTON_PIN) == LOW);
  return true;
#else
  return true; // Dev mode always safe
#endif
}

// --- Network ---

bool Esp32SessionHAL::isNetworkProvisioningRequested() { return NetworkManager::getInstance().isProvisioningNeeded(); }

void Esp32SessionHAL::enterNetworkProvisioning() {
  // Force safe state first
  setHardwareSafetyMask(0x00);
  NetworkManager::getInstance().startBLEProvisioningBlocking();
}

// --- Watchdogs ---

void Esp32SessionHAL::setWatchdogTimeout(uint32_t seconds) { esp_task_wdt_init(seconds, true); }

void Esp32SessionHAL::armFailsafeTimer(uint32_t seconds) {
  if (s_failsafeTimer == NULL) {
    logKeyValue("System", "CRITICAL: Failsafe Timer not initialized!");
    return;
  }

  // Verify 0 wasn't passed by mistake (which would fire immediately or fail)
  if (seconds == 0) return;

  uint64_t timeout_us = (uint64_t)seconds * 1000000ULL;
  
  // Stop existing before starting new (standard ESP timer practice)
  esp_timer_stop(s_failsafeTimer);
  esp_timer_start_once(s_failsafeTimer, timeout_us);

  char logBuf[64];
  snprintf(logBuf, sizeof(logBuf), "Death Grip ARMED: %u s", seconds);
  logKeyValue("System", logBuf);
}

void Esp32SessionHAL::disarmFailsafeTimer() {
  if (s_failsafeTimer != NULL) {
    esp_timer_stop(s_failsafeTimer);
    logKeyValue("System", "Death Grip Timer DISARMED.");
  }
}

// --- Storage ---

void Esp32SessionHAL::saveState(const DeviceState &state, const SessionTimers &timers, const SessionStats &stats, const SessionConfig &config) {
  updateLedPattern(state);

  // Delegate to SettingsManager
  SettingsManager::saveSessionState(state, timers, stats, config);
}

// --- Utils ---

unsigned long Esp32SessionHAL::getMillis() { return millis(); }

uint32_t Esp32SessionHAL::getRandom(uint32_t min, uint32_t max) { return random(min, max); }

// =================================================================================
// SECTION: INTERNAL LOGIC & HELPERS
// =================================================================================

void Esp32SessionHAL::updateLedPattern(DeviceState state) {
  bool stateChanged = (state != _cachedState);
  _cachedState = state;

  // 1. If LED is disabled, ensure it remains OFF and exit
  if (!_isLedEnabled) {
      // Force OFF if we just changed state or if we are here due to a toggle
      _statusLed.Off().Forever();
      return;
  }

  // 2. If LED is enabled and state changed, verify and apply pattern
  if (stateChanged) {
    char logBuf[50];
    snprintf(logBuf, sizeof(logBuf), "LED Pattern: State %s", stateToString(state));
    logKeyValue("System", logBuf);

    switch (state) {
    case READY:
        _statusLed.Breathe(4000).Forever();
        break;
    case ARMED:
        _statusLed.Blink(250, 250).Forever();
        break;
    case LOCKED:
        _statusLed.On().Forever();
        break;
    case ABORTED:
        _statusLed.Blink(500, 500).Forever();
        break;
    case COMPLETED:
        _statusLed.Blink(200, 200).Repeat(2).DelayAfter(3000).Forever();
        break;
    case TESTING:
        _statusLed.FadeOn(750).FadeOff(750).Forever();
        break;
    default:
        _statusLed.Off().Forever();
        break;
    }
  }
}

void Esp32SessionHAL::checkBootLoop() {
  int crashes = SettingsManager::getCrashCount();

  if (crashes >= g_systemDefaults.bootLoopThreshold) {
    Serial.println("CRITICAL: Boot Loop Detected! Entering Safe Mode.");
    delay(5000);
    // Emergency Pins Low
    for (int i = 0; i < MAX_CHANNELS; i++)
      digitalWrite(HARDWARE_PINS[i], LOW);

    pinMode(STATUS_LED_PIN, OUTPUT);
    digitalWrite(STATUS_LED_PIN, HIGH);

    // Try and erase all settings to reboot into provisioning mode
    SettingsManager::wipeAll();

    for (int i = 0; i < 30; i++) {
      delay(1000);
    }
  }

  SettingsManager::incrementCrashCount();
  _bootStartTime = millis();
}

void Esp32SessionHAL::markBootStability() {
  if (!_bootMarkedStable && (millis() - _bootStartTime > g_systemDefaults.stableBootTime)) {
    _bootMarkedStable = true;
    SettingsManager::clearCrashCount();
    logKeyValue("System", "System stable.");
  }
}

void Esp32SessionHAL::checkSystemHealth() {
  size_t freeMem = ESP.getFreeHeap();
  if (freeMem < 10000) {
    logKeyValue("System", "CRITICAL: Low Heap! Emergency Stop.");
    for (int i = 0; i < MAX_CHANNELS; i++)
      digitalWrite(HARDWARE_PINS[i], LOW);
    ESP.restart();
  }

  float currentTemp = temperatureRead();
  if (!isnan(currentTemp) && currentTemp > MAX_SAFE_TEMP_C) {
    char logBuf[100];
    snprintf(logBuf, sizeof(logBuf), "CRITICAL: Overheating (%.1f C)!", currentTemp);
    logKeyValue("System", logBuf);
    // Force pins low immediately
    for (int i = 0; i < MAX_CHANNELS; i++)
      digitalWrite(HARDWARE_PINS[i], LOW);
  }
}

void Esp32SessionHAL::checkPressState() {
  bool wasPressed = (_pressStartTime != 0);
  bool isPressed = _pcbPressed || _extPressed;

  if (isPressed && !wasPressed) {
    _pressStartTime = millis();
  } else if (!isPressed && wasPressed) {
    _pressStartTime = 0;
  }
}

// =================================================================================
// SECTION: STATIC HANDLERS (OneButton Callbacks)
// =================================================================================

// --- PCB Button Handlers ---

void Esp32SessionHAL::handlePcbPressStart() {
  Esp32SessionHAL &inst = getInstance();
  inst._pcbPressed = true;
  inst.checkPressState();
}

void Esp32SessionHAL::handlePcbClick() {
  Esp32SessionHAL &inst = getInstance();
  inst._pcbPressed = false;
  inst.checkPressState();
  inst.setShortPressPending();
}

void Esp32SessionHAL::handlePcbDoubleClick() {
  Esp32SessionHAL &inst = getInstance();
  inst._pcbPressed = false;
  inst.checkPressState();
  inst.setTriggerPending();
}

void Esp32SessionHAL::handlePcbLongStart() {
  Esp32SessionHAL &inst = getInstance();
  inst.setAbortPending();
}

void Esp32SessionHAL::handlePcbLongStop() {
  Esp32SessionHAL &inst = getInstance();
  inst._pcbPressed = false;
  inst.checkPressState();
}

// --- EXT Button Handlers ---

void Esp32SessionHAL::handleExtPressStart() {
  Esp32SessionHAL &inst = getInstance();
  inst._extPressed = true;
  inst.checkPressState();
}

void Esp32SessionHAL::handleExtClick() {
  Esp32SessionHAL &inst = getInstance();
  inst._extPressed = false;
  inst.checkPressState();
  inst.setShortPressPending();
}

void Esp32SessionHAL::handleExtDoubleClick() {
  Esp32SessionHAL &inst = getInstance();
  inst._extPressed = false;
  inst.checkPressState();
  inst.setTriggerPending();
}

void Esp32SessionHAL::handleExtLongStart() { getInstance().setAbortPending(); }

void Esp32SessionHAL::handleExtLongStop() {
  Esp32SessionHAL &inst = getInstance();
  inst._extPressed = false;
  inst.checkPressState();
}