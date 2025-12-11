/*
 * =================================================================================
 * File:      src/SettingsManager.cpp
 * Description: Implementation of configuration validation and saving.
 * =================================================================================
 */
#include "SettingsManager.h"
#include "Esp32SessionHAL.h" // For logging
#include "Globals.h"
#include <Arduino.h>
#include <Preferences.h>

// --- Preferences Namespaces ---
static Preferences wifiPrefs;
static Preferences provPrefs;
static Preferences sessionPrefs;
static Preferences bootPrefs;

// --- Safety Limits ---
static const uint32_t ABS_MIN_PAYBACK = 300;
static const uint32_t ABS_MAX_PAYBACK = 3600;
static const uint32_t ABS_MIN_PENALTY = 300;
static const uint32_t ABS_MAX_PENALTY = 14400;

// Helper for logging via HAL
void SettingsManager::log(const char *key, const char *val) { Esp32SessionHAL::getInstance().logKeyValue(key, val); }

// =================================================================================
// SECTION: FACTORY RESET
// =================================================================================

void SettingsManager::wipeAll() {
  log("Settings", "Performing Full Factory Wipe...");

  // 1. WiFi Credentials
  wifiPrefs.begin("wifi-creds", false);
  wifiPrefs.clear();
  wifiPrefs.end();
  log("Settings", " - WiFi Cleared");

  // 2. Provisioning & Features
  provPrefs.begin("provisioning", false);
  provPrefs.clear();
  provPrefs.end();
  log("Settings", " - Provisioning Cleared");

  // 3. Session State & Stats
  sessionPrefs.begin("session", false);
  sessionPrefs.clear();
  sessionPrefs.end();
  log("Settings", " - Session State Cleared");

  // 4. Boot Diagnostics
  bootPrefs.begin("boot", false);
  bootPrefs.clear();
  bootPrefs.end();
  log("Settings", " - Boot Stats Cleared");

  log("Settings", "Factory Wipe Complete.");
}

// =================================================================================
// SECTION: WIFI
// =================================================================================

void SettingsManager::setWifiSSID(const char *ssid) {
  wifiPrefs.begin("wifi-creds", false);
  wifiPrefs.putString("ssid", ssid);
  wifiPrefs.end();

  char logBuf[64];
  snprintf(logBuf, sizeof(logBuf), "SSID Updated: %s", ssid);
  log("Settings", logBuf);
}

void SettingsManager::setWifiPassword(const char *pass) {
  wifiPrefs.begin("wifi-creds", false);
  wifiPrefs.putString("pass", pass);
  wifiPrefs.end();

  log("Settings", "WiFi Password Updated");
}

void SettingsManager::getWifiSSID(char *buf, size_t maxLen) {
  wifiPrefs.begin("wifi-creds", true); // Read-only
  String s = wifiPrefs.getString("ssid", "");
  wifiPrefs.end();

  if (maxLen > 0) {
    strncpy(buf, s.c_str(), maxLen);
    buf[maxLen - 1] = '\0';
  }
}

void SettingsManager::getWifiPassword(char *buf, size_t maxLen) {
  wifiPrefs.begin("wifi-creds", true); // Read-only
  String p = wifiPrefs.getString("pass", "");
  wifiPrefs.end();

  if (maxLen > 0) {
    strncpy(buf, p.c_str(), maxLen);
    buf[maxLen - 1] = '\0';
  }
}

// =================================================================================
// SECTION: FEATURES & PROVISIONING
// =================================================================================

void SettingsManager::setRewardCodeEnabled(bool enabled) {
  provPrefs.begin("provisioning", false);
  provPrefs.putBool("enableCode", enabled);
  provPrefs.end();
  log("Settings", enabled ? "Reward Code: ENABLED" : "Reward Code: DISABLED");
}

void SettingsManager::setStreaksEnabled(bool enabled) {
  provPrefs.begin("provisioning", false);
  provPrefs.putBool("enableStreaks", enabled);
  provPrefs.end();
  log("Settings", enabled ? "Streaks: ENABLED" : "Streaks: DISABLED");
}

void SettingsManager::setPaybackEnabled(bool enabled) {
  provPrefs.begin("provisioning", false);
  provPrefs.putBool("enablePayback", enabled);
  provPrefs.end();
  log("Settings", enabled ? "Payback: ENABLED" : "Payback: DISABLED");
}

void SettingsManager::loadProvisioningConfig(DeterrentConfig &config, SessionPresets &presets, uint8_t &channelMask) {
  provPrefs.begin("provisioning", true);

  // 1. Hardware Mask
  channelMask = provPrefs.getUChar("chMask", 0x0F);

  // 2. Deterrent Config
  config.enableStreaks = provPrefs.getBool("enableStreaks", config.enableStreaks);
  config.enableRewardCode = provPrefs.getBool("enableCode", config.enableRewardCode);
  config.enablePaybackTime = provPrefs.getBool("enablePayback", config.enablePaybackTime);
  config.paybackTime = provPrefs.getUInt("paybackSeconds", config.paybackTime);
  config.rewardPenalty = provPrefs.getUInt("rwdPenaltySec", config.rewardPenalty);

  // 3. Session Presets (Time Ranges)
  presets.shortMin = provPrefs.getUInt("shortMin", presets.shortMin);
  presets.shortMax = provPrefs.getUInt("shortMax", presets.shortMax);
  presets.mediumMin = provPrefs.getUInt("medMin", presets.mediumMin);
  presets.mediumMax = provPrefs.getUInt("medMax", presets.mediumMax);
  presets.longMin = provPrefs.getUInt("longMin", presets.longMin);
  presets.longMax = provPrefs.getUInt("longMax", presets.longMax);

  // 4. Deterrent Ranges
  presets.penaltyMin = provPrefs.getUInt("penMin", presets.penaltyMin);
  presets.penaltyMax = provPrefs.getUInt("penMax", presets.penaltyMax);
  presets.paybackMin = provPrefs.getUInt("payMin", presets.paybackMin);
  presets.paybackMax = provPrefs.getUInt("payMax", presets.paybackMax);

  // 5. Safety Ceilings & Floors
  presets.limitLockMax = provPrefs.getUInt("limLockMax", presets.limitLockMax);
  presets.limitPenaltyMax = provPrefs.getUInt("limPenMax", presets.limitPenaltyMax);
  presets.limitPaybackMax = provPrefs.getUInt("limPayMax", presets.limitPaybackMax);

  presets.minLockDuration = provPrefs.getUInt("minLockDur", presets.minLockDuration);
  presets.minRewardPenaltyDuration = provPrefs.getUInt("minPenDur", presets.minRewardPenaltyDuration);
  presets.minPaybackTime = provPrefs.getUInt("minPayDur", presets.minPaybackTime);

  provPrefs.end();
}

// =================================================================================
// SECTION: VALIDATED NUMERICS
// =================================================================================

uint32_t SettingsManager::validateAndSave(const char *key, uint32_t value, uint32_t min, uint32_t max, const char *label) {
  uint32_t finalValue = value;
  const char *note = "";

  if (finalValue < min) {
    finalValue = min;
    note = " (Clamped Min)";
  } else if (finalValue > max) {
    finalValue = max;
    note = " (Clamped Max)";
  }

  provPrefs.begin("provisioning", false);
  provPrefs.putUInt(key, finalValue);
  provPrefs.end();

  char logBuf[128];
  if (value != finalValue) {
    snprintf(logBuf, sizeof(logBuf), "%s: %u s%s (Req: %u)", label, finalValue, note, value);
  } else {
    snprintf(logBuf, sizeof(logBuf), "%s: %u s", label, finalValue);
  }
  log("Settings", logBuf);

  return finalValue;
}

uint32_t SettingsManager::setPaybackDuration(uint32_t seconds) {
  return validateAndSave("paybackSeconds", seconds, ABS_MIN_PAYBACK, ABS_MAX_PAYBACK, "Payback Time");
}

uint32_t SettingsManager::setRewardPenaltyDuration(uint32_t seconds) {
  return validateAndSave("rwdPenaltySec", seconds, ABS_MIN_PENALTY, ABS_MAX_PENALTY, "Reward Penalty");
}

// =================================================================================
// SECTION: HARDWARE CONFIG
// =================================================================================

void SettingsManager::setChannelEnabled(int channelIndex, bool enabled) {
  if (channelIndex < 0 || channelIndex >= 4)
    return;

  provPrefs.begin("provisioning", false);
  uint8_t currentMask = provPrefs.getUChar("chMask", 0x0F);

  if (enabled)
    currentMask |= (1 << channelIndex);
  else
    currentMask &= ~(1 << channelIndex);

  provPrefs.putUChar("chMask", currentMask);
  provPrefs.end();

  Esp32SessionHAL::getInstance().setChannelMask(currentMask);

  char logBuf[64];
  snprintf(logBuf, sizeof(logBuf), "Ch%d Config: %s (Mask: 0x%02X)", channelIndex + 1, enabled ? "ENABLED" : "DISABLED", currentMask);
  log("Settings", logBuf);
}

// =================================================================================
// SECTION: DYNAMIC SESSION STATE
// =================================================================================

void SettingsManager::saveSessionState(const DeviceState &state, const SessionTimers &timers, const SessionStats &stats) {
  sessionPrefs.begin("session", false);
  sessionPrefs.putUChar("state", (uint8_t)state);
  sessionPrefs.putBytes("timers", &timers, sizeof(SessionTimers));
  sessionPrefs.putBytes("stats", &stats, sizeof(SessionStats));
  sessionPrefs.end();
}

bool SettingsManager::loadSessionState(DeviceState &state, SessionTimers &timers, SessionStats &stats) {
  sessionPrefs.begin("session", true);
  if (!sessionPrefs.isKey("state")) {
    sessionPrefs.end();
    return false;
  }
  state = (DeviceState)sessionPrefs.getUChar("state", (uint8_t)READY);
  sessionPrefs.getBytes("timers", &timers, sizeof(SessionTimers));
  sessionPrefs.getBytes("stats", &stats, sizeof(SessionStats));
  sessionPrefs.end();
  return true;
}

// =================================================================================
// SECTION: BOOT DIAGNOSTICS
// =================================================================================

int SettingsManager::getCrashCount() {
  bootPrefs.begin("boot", true);
  int c = bootPrefs.getInt("crashes", 0);
  bootPrefs.end();
  return c;
}

void SettingsManager::incrementCrashCount() {
  bootPrefs.begin("boot", false);
  int c = bootPrefs.getInt("crashes", 0);
  bootPrefs.putInt("crashes", c + 1);
  bootPrefs.end();
}

void SettingsManager::clearCrashCount() {
  bootPrefs.begin("boot", false);
  bootPrefs.putInt("crashes", 0);
  bootPrefs.end();
}