#ifndef HARDWARE_H
#define HARDWARE_H

#include "Config.h"
#include <Arduino.h>

// Channel Control
void initializeChannels();
bool enforceHardwareState();
void sendChannelOn(int channelIndex, bool silent = false);
void sendChannelOff(int channelIndex, bool silent = false);
void sendChannelOnAll();
void sendChannelOffAll();

// Visuals
void setLedPattern(SessionState state);

// Failsafe / Death Grip
void initializeFailSafeTimer();
void armFailsafeTimer();
void disarmFailsafeTimer();

// Health
void checkSystemHealth();
void checkHeapHealth();
void updateWatchdogTimeout(uint32_t seconds);

#endif