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

  // --- Feature Toggles ---
  static void setRewardCodeEnabled(bool enabled);
  static void setStreaksEnabled(bool enabled);
  static void setPaybackEnabled(bool enabled);

  // Loads all provisioning features
  static void loadProvisioningConfig(DeterrentConfig &config, SessionPresets &presets, uint8_t &channelMask);

  // --- Numeric Settings (Validated) ---
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
  // Clears ALL namespaces (WiFi, Provisioning, Session, Boot)
  static void wipeAll();

private:
  // Internal helper to perform clamping and logging
  static uint32_t validateAndSave(const char *key, uint32_t value, uint32_t min, uint32_t max, const char *label);
  static void log(const char *key, const char *value);
};
