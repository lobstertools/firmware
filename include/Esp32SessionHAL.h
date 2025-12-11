/*
 * =================================================================================
 * Project:   Lobster Lock - Self-Bondage Session Manager
 * File:      include/Esp32SessionHAL.h
 * Description: Header for the ESP32 implementation of ISessionHAL.
 * Encapsulates Hardware, Buttons, Safety Timers, Logging, and LED.
 * =================================================================================
 */
#pragma once

#include "Globals.h"
#include "SessionContext.h"
#include "Types.h"
#include <Arduino.h>
#include <OneButton.h>
#include <jled.h>

class Esp32SessionHAL : public ISessionHAL {
private:
  Esp32SessionHAL();

  // --- Synchronization
  SemaphoreHandle_t _stateMutex;

  // --- State Members ---
  volatile bool _triggerActionPending;
  volatile bool _abortActionPending;
  volatile bool _shortPressPending;
  DeviceState _cachedState;

  // --- Log State (RAM + Serial) ---
  char _logBuffer[LOG_BUFFER_SIZE][MAX_LOG_LENGTH]; // For WebAPI
  int _logBufferIndex;
  char _serialQueue[SERIAL_QUEUE_SIZE][MAX_LOG_LENGTH]; // For Serial
  int _queueHead;
  int _queueTail;

  // --- Peripherals ---
  OneButton _pcbButton;
  OneButton _extButton;
  JLed _statusLed;

  // --- Channel ---
  uint8_t _enabledChannelsMask;

  // --- Health Tracking ---
  unsigned long _lastHealthCheck;
  unsigned long _bootStartTime;
  bool _bootMarkedStable;

  // --- Helpers ---
  void updateLedPattern(DeviceState state);
  void checkSystemHealth();
  void checkBootLoop();
  void markBootStability();
  void processLogQueue();

  // --- Static Interrupt Handlers ---
  static void handleDoublePressISR();
  static void handleLongPressISR();
  static void handleShortPressISR();

public:
  static Esp32SessionHAL &getInstance();

  void initialize();
  void tick();

  // --- Thread Safety (Mutex Wrapper) ---
  // Returns true if lock acquired, false if timeout/busy
  bool lockState(uint32_t timeoutMs = 100);
  void unlockState();

  // --- Logging API ---
  void log(const char *message) override;
  void logKeyValue(const char *key, const char *value);
  void printStartupDiagnostics();

  // -- Accessor for WebServer
  const char *getLogLine(int index) const {
    if (index >= 0 && index < LOG_BUFFER_SIZE)
      return _logBuffer[index];
    return "";
  }
  int getLogBufferIndex() const { return _logBufferIndex; }

  // --- Used by BLE provisioning
  JLed getStatusLed() const { return _statusLed; }

  // --- Channel modifiers
  void setChannelMask(uint8_t mask);
  uint8_t getChannelMask() const;
  bool isChannelEnabled(int channelIndex) const;

  // --- ISessionHAL Implementation ---
  void setHardwareSafetyMask(uint8_t mask) override;
  bool checkTriggerAction() override;
  bool checkAbortAction() override;
  bool checkShortPressAction() override;
  bool isSafetyInterlockEngaged() override;

  bool isNetworkProvisioningRequested() override;
  void enterNetworkProvisioning() override;

  void setWatchdogTimeout(uint32_t seconds) override;
  void armFailsafeTimer(uint32_t seconds) override;
  void disarmFailsafeTimer() override;

  void saveState(const DeviceState &state, const SessionTimers &timers, const SessionStats &stats) override;
  unsigned long getMillis() override;
  uint32_t getRandom(uint32_t min, uint32_t max) override;

  // Internal Setters for ISRs
  void setTriggerPending() { _triggerActionPending = true; }
  void setAbortPending() { _abortActionPending = true; }
};
