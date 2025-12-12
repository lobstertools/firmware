/*
 * =================================================================================
 * File:      include/SettingsManager.h
 * Description:
 * Central controller for Device Configuration and Storage.
 * - Manages all NVS (Preferences) interactions.
 * - Validates inputs against safety limits.
 * =================================================================================
 */
#pragma once
#include "Types.h"
#include <stdint.h>

class SettingsManager {
public:
  // --- WiFi Management ---
  static void setWifiSSID(const char *ssid);
  static void setWifiPassword(const char *pass);
  static void getWifiSSID(char *buf, size_t maxLen);
  static void getWifiPassword(char *buf, size_t maxLen);

  // --- Features and Provisioing ---
  static void setRewardCodeEnabled(bool enabled);
  static void setStreaksEnabled(bool enabled);
  static void setPaybackEnabled(bool enabled);

  // --- Session Configuration ---
  // Sets global safety floor/ceiling
  static void setSessionLimits(uint32_t minDuration, uint32_t maxDuration);

  // Sets ranges for DUR_RANGE_SHORT, DUR_RANGE_MEDIUM, DUR_RANGE_LONG
  static void setDurationPreset(DurationType type, uint32_t min, uint32_t max);

  // --- Deterrent Configuration ---
  static void setPaybackStrategy(DeterrentStrategy strategy);
  static void setPaybackRange(uint32_t min, uint32_t max);

  static void setRewardStrategy(DeterrentStrategy strategy);
  static void setRewardRange(uint32_t min, uint32_t max);

  // Loads all provisioning features into the updated structs
  static void loadProvisioningConfig(DeterrentConfig &config, SessionPresets &presets, uint8_t &channelMask);

  // --- Numeric Settings (Validated) ---
  // Sets the fixed/base duration for these deterrents
  static uint32_t setPaybackDuration(uint32_t seconds);
  static uint32_t setRewardPenaltyDuration(uint32_t seconds);

  // --- Hardware Configuration ---
  static void setChannelEnabled(int channelIndex, bool enabled);

  // --- Dynamic Session State (Save/Load) ---
  static void saveSessionState(const DeviceState &state, const SessionTimers &timers, const SessionStats &stats);
  static bool loadSessionState(DeviceState &state, SessionTimers &timers, SessionStats &stats);

  // --- Boot & Crash Diagnostics ---
  static int getCrashCount();
  static void incrementCrashCount();
  static void clearCrashCount();

  // --- Factory Reset ---
  static void wipeAll();

private:
  // Internal helper to perform clamping and logging
  static uint32_t validateAndSave(const char *key, uint32_t value, uint32_t min, uint32_t max, const char *label);
  static void log(const char *key, const char *value);
};