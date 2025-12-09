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

#include <Arduino.h>

// --- Hardware & Global Configuration ---
#define SERIAL_BAUD_RATE 115200

#define DEFAULT_WDT_TIMEOUT 20 // Relaxed for READY state
#define CRITICAL_WDT_TIMEOUT 5 // Tight for LOCKED state
#define MAX_SAFE_TEMP_C 85.0   // Safety Threshold (85°C)

// --- System Identification & Validation ---
#define MAGIC_VALUE 0x3CADD200

// --- Pin Definitions ---
#define PCB_BUTTON_PIN 0 // Standard ESP32 Boot Button

#ifdef DEBUG_MODE
// Development (Diymore Debug)
#define STATUS_LED_PIN 23
// EXT_BUTTON_PIN is purposefully undefined in Debug mode
#else
// Production (Diymore Release)
#define STATUS_LED_PIN 21
#define EXT_BUTTON_PIN 15 // External NC Switch
#endif

// --- Channel-Specific Configuration ---
#define MAX_CHANNELS 4
static const int HARDWARE_PINS[MAX_CHANNELS] = {16, 17, 26, 27};

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
void setLedPattern();

// =================================================================================
// SECTION: INPUT CALLBACKS
// =================================================================================
void handlePress();
void handleLongPress();
void handleDoublePress();

#endif