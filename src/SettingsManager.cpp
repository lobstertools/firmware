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
static const uint32_t ABS_MIN_PAYBACK = 1 * 60;
static const uint32_t ABS_MAX_PAYBACK = 720 * 60;
static const uint32_t ABS_MIN_PENALTY = 1 * 60;
static const uint32_t ABS_MAX_PENALTY = 360 * 60;

// Helper for logging via HAL
void SettingsManager::log(const char *key, const char *val) { Esp32SessionHAL::getInstance().logKeyValue(key, val); }

// =================================================================================
// SECTION: FACTORY RESET
// =================================================================================

void SettingsManager::wipeAll() {
  log("Settings", "Performing Full Factory Wipe...");

  wifiPrefs.begin("wifi-creds", false);
  wifiPrefs.clear();
  wifiPrefs.end();
  provPrefs.begin("provisioning", false);
  provPrefs.clear();
  provPrefs.end();
  sessionPrefs.begin("session", false);
  sessionPrefs.clear();
  sessionPrefs.end();
  bootPrefs.begin("boot", false);
  bootPrefs.clear();
  bootPrefs.end();

  log("Settings", "Factory Wipe Complete.");
}

// =================================================================================
// SECTION: WIFI
// =================================================================================

void SettingsManager::setWifiSSID(const char *ssid) {
  wifiPrefs.begin("wifi-creds", false);
  wifiPrefs.putString("ssid", ssid);
  wifiPrefs.end();
  log("Settings", "SSID Updated");
}

void SettingsManager::setWifiPassword(const char *pass) {
  wifiPrefs.begin("wifi-creds", false);
  wifiPrefs.putString("pass", pass);
  wifiPrefs.end();
  log("Settings", "WiFi Password Updated");
}

void SettingsManager::getWifiSSID(char *buf, size_t maxLen) {
  wifiPrefs.begin("wifi-creds", true);
  String s = wifiPrefs.getString("ssid", "");
  wifiPrefs.end();
  if (maxLen > 0) {
    strncpy(buf, s.c_str(), maxLen);
    buf[maxLen - 1] = '\0';
  }
}

void SettingsManager::getWifiPassword(char *buf, size_t maxLen) {
  wifiPrefs.begin("wifi-creds", true);
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

// --- Session Configuration ---

void SettingsManager::setSessionLimits(uint32_t minDuration, uint32_t maxDuration) {
  provPrefs.begin("provisioning", false);
  provPrefs.putUInt("minSessionDur", minDuration);
  provPrefs.putUInt("maxSessionDur", maxDuration);
  provPrefs.end();

  char logBuf[64];
  snprintf(logBuf, sizeof(logBuf), "Global Limits: %u - %u s", minDuration, maxDuration);
  log("Settings", logBuf);
}

void SettingsManager::setDurationPreset(DurationType type, uint32_t min, uint32_t max) {
  provPrefs.begin("provisioning", false);
  const char *keyMin = "";
  const char *keyMax = "";
  const char *label = "";

  switch (type) {
  case DUR_RANGE_SHORT:
    keyMin = "shMin";
    keyMax = "shMax";
    label = "Short Preset";
    break;
  case DUR_RANGE_MEDIUM:
    keyMin = "mdMin";
    keyMax = "mdMax";
    label = "Medium Preset";
    break;
  case DUR_RANGE_LONG:
    keyMin = "lgMin";
    keyMax = "lgMax";
    label = "Long Preset";
    break;
  default:
    provPrefs.end();
    return;
  }

  provPrefs.putUInt(keyMin, min);
  provPrefs.putUInt(keyMax, max);
  provPrefs.end();

  char logBuf[64];
  snprintf(logBuf, sizeof(logBuf), "%s: %u - %u s", label, min, max);
  log("Settings", logBuf);
}

// --- Deterrent Configuration ---

void SettingsManager::setPaybackStrategy(DeterrentStrategy strategy) {
  provPrefs.begin("provisioning", false);
  provPrefs.putUChar("payStrat", (uint8_t)strategy);
  provPrefs.end();
  log("Settings", strategy == DETERRENT_RANDOM ? "Payback: RANDOM" : "Payback: FIXED");
}

void SettingsManager::setPaybackRange(uint32_t min, uint32_t max) {
  provPrefs.begin("provisioning", false);
  provPrefs.putUInt("payMin", min);
  provPrefs.putUInt("payMax", max);
  provPrefs.end();

  char logBuf[64];
  snprintf(logBuf, sizeof(logBuf), "Payback Range: %u - %u s", min, max);
  log("Settings", logBuf);
}

void SettingsManager::setRewardStrategy(DeterrentStrategy strategy) {
  provPrefs.begin("provisioning", false);
  provPrefs.putUChar("rwdStrat", (uint8_t)strategy);
  provPrefs.end();
  log("Settings", strategy == DETERRENT_RANDOM ? "Reward Pen: RANDOM" : "Reward Pen: FIXED");
}

void SettingsManager::setRewardRange(uint32_t min, uint32_t max) {
  provPrefs.begin("provisioning", false);
  provPrefs.putUInt("penMin", min);
  provPrefs.putUInt("penMax", max);
  provPrefs.end();

  char logBuf[64];
  snprintf(logBuf, sizeof(logBuf), "Reward Pen Range: %u - %u s", min, max);
  log("Settings", logBuf);
}

// =================================================================================
// SECTION: LOADER
// =================================================================================

void SettingsManager::loadProvisioningConfig(DeterrentConfig &config, SessionPresets &presets, uint8_t &channelMask) {
  provPrefs.begin("provisioning", true);

  // 1. Hardware Mask
  channelMask = provPrefs.getUChar("chMask", 0x0F);

  // 2. Deterrent Config - Flags
  config.enableStreaks = provPrefs.getBool("enableStreaks", config.enableStreaks);
  config.enableRewardCode = provPrefs.getBool("enableCode", config.enableRewardCode);
  config.enablePaybackTime = provPrefs.getBool("enablePayback", config.enablePaybackTime);

  // 3. Deterrent Config - Strategies & Values
  config.paybackTimeStrategy = (DeterrentStrategy)provPrefs.getUChar("payStrat", (uint8_t)DETERRENT_FIXED);
  config.paybackTime = provPrefs.getUInt("paybackSeconds", config.paybackTime);
  config.paybackTimeMin = provPrefs.getUInt("payMin", 300);
  config.paybackTimeMax = provPrefs.getUInt("payMax", 900);

  config.rewardPenaltyStrategy = (DeterrentStrategy)provPrefs.getUChar("rwdStrat", (uint8_t)DETERRENT_FIXED);
  config.rewardPenalty = provPrefs.getUInt("rwdPenaltySec", config.rewardPenalty);
  config.rewardPenaltyMin = provPrefs.getUInt("penMin", 300);
  config.rewardPenaltyMax = provPrefs.getUInt("penMax", 1800);

  // 4. Session Presets - Generators
  // Default values are provided if NVS is empty
  presets.shortMin = provPrefs.getUInt("shMin", 300);  // 5m
  presets.shortMax = provPrefs.getUInt("shMax", 1800); // 30m

  presets.mediumMin = provPrefs.getUInt("mdMin", 1800); // 30m
  presets.mediumMax = provPrefs.getUInt("mdMax", 7200); // 2h

  presets.longMin = provPrefs.getUInt("lgMin", 7200);  // 2h
  presets.longMax = provPrefs.getUInt("lgMax", 21600); // 6h

  // 5. Session Presets - Global Safety Limits
  presets.maxSessionDuration = provPrefs.getUInt("maxSessionDur", presets.maxSessionDuration);
  presets.minSessionDuration = provPrefs.getUInt("minSessionkDur", presets.minSessionDuration);

  // Sanity check to prevent logic errors in global limits
  if (presets.maxSessionDuration < presets.minSessionDuration) {
    presets.maxSessionDuration = presets.minSessionDuration;
  }

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