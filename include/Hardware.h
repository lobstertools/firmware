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
// SECTION: SYSTEM HEALTH & SAFETY
// =================================================================================
void checkSystemHealth();
void checkHeapHealth();
void checkBootLoop();
void updateWatchdogTimeout(uint32_t seconds);

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