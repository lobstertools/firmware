/*
 * =================================================================================
 * Project:   Lobster Lock - Self-Bondage Session Manager
 * File:      Storage.h / Storage.cpp
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Description:
 * NVS (Non-Volatile Storage) abstraction. Handles saving and loading session
 * state, WiFi credentials, and statistics to ESP32 Preferences to survive
 * power cycles and reboots.
 * =================================================================================
 */
#include <esp_task_wdt.h>

#include "Globals.h"
#include "Logger.h"
#include "Session.h"
#include "Storage.h"
#include "Utils.h"

// =================================================================================
// SECTION: SESSION STATE (Namespace: "session")
// =================================================================================

bool loadState() {
  sessionState.begin("session", true);
  unsigned long magic = sessionState.getULong("magic", 0);

  if (magic != MAGIC_VALUE) {
    sessionState.end();
    return false;
  }

  logKeyValue("Prefs", "Valid session data found in NVS.");

  // 1. CORE STATE
  g_currentState = (DeviceState)sessionState.getUChar("state", (uint8_t)READY);

  // 2. SESSION TIMERS
  g_sessionTimers.lockDuration = sessionState.getULong("lockDuration", 0);
  g_sessionTimers.penaltyDuration = sessionState.getULong("penaltyDuration", 0);
  g_sessionTimers.lockRemaining = sessionState.getULong("lockRemain", 0);
  g_sessionTimers.penaltyRemaining = sessionState.getULong("penaltyRemain", 0);

  // 3. STATISTICS
  g_sessionStats.streaks = sessionState.getUInt("streak", 0);
  g_sessionStats.completed = sessionState.getUInt("completed", 0);
  g_sessionStats.aborted = sessionState.getUInt("aborted", 0);
  g_sessionStats.paybackAccumulated = sessionState.getUInt("paybackAccum", 0);
  g_sessionStats.totalLockedTime = sessionState.getUInt("totalLocked", 0);

  // 4. ARRAYS
  sessionState.getBytes("rewards", rewardHistory, sizeof(rewardHistory));

  sessionState.end();
  return true;
}

void saveState(bool force) {
  if (!force)
    return;

  char logBuf[100];
  snprintf(logBuf, sizeof(logBuf), "Saving state to NVS: %s", stateToString(g_currentState));
  logKeyValue("Prefs", logBuf);

  esp_task_wdt_reset();

  sessionState.begin("session", false);

  // 1. CORE STATE
  sessionState.putUChar("state", (uint8_t)g_currentState);

  // 2. SESSION TIMERS
  sessionState.putULong("lockDuration", g_sessionTimers.lockDuration);
  sessionState.putULong("penaltyDuration", g_sessionTimers.penaltyDuration);
  sessionState.putULong("lockRemain", g_sessionTimers.lockRemaining);
  sessionState.putULong("penaltyRemain", g_sessionTimers.penaltyRemaining);

  // 3. STATISTICS
  sessionState.putUInt("streak", g_sessionStats.streaks);
  sessionState.putUInt("completed", g_sessionStats.completed);
  sessionState.putUInt("aborted", g_sessionStats.aborted);
  sessionState.putUInt("paybackAccum", g_sessionStats.paybackAccumulated);
  sessionState.putUInt("totalLocked", g_sessionStats.totalLockedTime);

  // 4. ARRAYS
  sessionState.putBytes("rewards", rewardHistory, sizeof(rewardHistory));

  sessionState.putULong("magic", MAGIC_VALUE);

  sessionState.end();
  esp_task_wdt_reset();
}

// =================================================================================
// SECTION: WIFI CREDENTIALS (Namespace: "wifi-creds")
// =================================================================================

void loadWiFiCredentials() {
  wifiPreferences.begin("wifi-creds", true);
  String ssid = wifiPreferences.getString("ssid", "");
  String pass = wifiPreferences.getString("pass", "");
  wifiPreferences.end();

  memset(g_wifiSSID, 0, sizeof(g_wifiSSID));
  memset(g_wifiPass, 0, sizeof(g_wifiPass));

  if (ssid.length() > 0) {
    strncpy(g_wifiSSID, ssid.c_str(), sizeof(g_wifiSSID) - 1);
    strncpy(g_wifiPass, pass.c_str(), sizeof(g_wifiPass) - 1);
    g_wifiCredentialsExist = true;
    logKeyValue("Prefs", "Loaded Wi-Fi Credentials.");
  } else {
    g_wifiCredentialsExist = false;
    logKeyValue("Prefs", "No Wi-Fi Credentials found.");
  }
}

void saveWiFiCredentials(const char *ssid, const char *pass) {
  wifiPreferences.begin("wifi-creds", false);
  wifiPreferences.putString("ssid", ssid);
  wifiPreferences.putString("pass", pass);
  wifiPreferences.end();

  strncpy(g_wifiSSID, ssid, sizeof(g_wifiSSID) - 1);
  strncpy(g_wifiPass, pass, sizeof(g_wifiPass) - 1);
  g_wifiCredentialsExist = true;
  logKeyValue("Prefs", "Saved new Wi-Fi Credentials.");
}

// =================================================================================
// SECTION: PROVISIONING / HARDWARE CONFIG (Namespace: "provisioning")
// =================================================================================

void loadProvisioningConfig() {
  provisioningPrefs.begin("provisioning", true);

  // 1. Hardware
  g_enabledChannelsMask = provisioningPrefs.getUChar("chMask", 0x0F);

  // 2. Feature Config (Deterrents) - MOVED HERE
  g_deterrentConfig.enableStreaks = provisioningPrefs.getBool("enableStreaks", true);
  g_deterrentConfig.enablePaybackTime = provisioningPrefs.getBool("enablePayback", true);
  g_deterrentConfig.enableRewardCode = provisioningPrefs.getBool("enableCode", true);
  g_deterrentConfig.paybackTime = provisioningPrefs.getUInt("paybackSeconds", 900);
  g_deterrentConfig.rewardPenalty = provisioningPrefs.getUInt("rwdPenaltySec", 2700);

  provisioningPrefs.end();

  char logBuf[64];
  snprintf(logBuf, sizeof(logBuf), "Loaded Provisioning. Ch Mask: 0x%02X", g_enabledChannelsMask);
  logKeyValue("Prefs", logBuf);
}

void saveProvisioningConfig() {
  provisioningPrefs.begin("provisioning", false);

  // 1. Hardware
  provisioningPrefs.putUChar("chMask", g_enabledChannelsMask);

  // 2. Feature Config
  provisioningPrefs.putBool("enableStreaks", g_deterrentConfig.enableStreaks);
  provisioningPrefs.putBool("enablePayback", g_deterrentConfig.enablePaybackTime);
  provisioningPrefs.putBool("enableCode", g_deterrentConfig.enableRewardCode);
  provisioningPrefs.putUInt("paybackSeconds", g_deterrentConfig.paybackTime);
  provisioningPrefs.putUInt("rwdPenaltySec", g_deterrentConfig.rewardPenalty);

  provisioningPrefs.end();
  logKeyValue("Prefs", "Saved Provisioning Config.");
}