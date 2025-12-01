/*
 * =================================================================================
 * Project:   Lobster Lock - Self-Bondage Session Manager
 * File:      Hardware.h / Hardware.cpp
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Description:
 * Low-level hardware abstraction layer. Manages GPIO control for channels,
 * LED patterns, fail-safe ISRs (Death Grip), system health monitoring
 * (Stack/Heap/Temp), and hardware initialization.
 * =================================================================================
 */
#ifndef HARDWARE_H
#define HARDWARE_H

#include "Config.h"
#include <Arduino.h>

// =================================================================================
// SECTION: CORE HARDWARE LOGIC
// =================================================================================
bool enforceHardwareState();

// =================================================================================
// SECTION: CHANNEL CONTROL (LOW LEVEL)
// =================================================================================
void initializeChannels();
void sendChannelOn(int channelIndex, bool silent = false);
void sendChannelOff(int channelIndex, bool silent = false);
void sendChannelOnAll();
void sendChannelOffAll();

// =================================================================================
// SECTION: FAILSAFE TIMER (DEATH GRIP)
// =================================================================================
void initializeFailSafeTimer();
void armFailsafeTimer();
void disarmFailsafeTimer();

// =================================================================================
// SECTION: WATCHDOG
// =================================================================================
void initializeWatchdog();
void updateWatchdogTimeout(uint32_t seconds);

// =================================================================================
// SECTION: SYSTEM HEALTH & SAFETY
// =================================================================================
void performPeriodicHealthChecks();
void checkBootLoop();
void markBootStability();

// =================================================================================
// SECTION: FEEDBACK (LEDS)
// =================================================================================
void setLedPattern(SessionState state);

// =================================================================================
// SECTION: INPUT CALLBACKS
// =================================================================================
void handlePress();
void handleLongPress();
void handleDoublePress();

#endif