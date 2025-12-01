#ifndef SESSION_H
#define SESSION_H

#include "Config.h"

// =================================================================================
// SECTION: LIFECYCLE & RECOVERY
// =================================================================================
void handleRebootState();
void resetToReady(bool generateNewCode);

// =================================================================================
// SECTION: SESSION INITIATION
// =================================================================================
int startSession(unsigned long duration, unsigned long penalty, TriggerStrategy strategy, unsigned long *delays, bool hide);
int startTestMode();

// =================================================================================
// SECTION: ACTIVE STATE TRANSITIONS
// =================================================================================
void triggerLock(const char *source);
void enterLockedState(const char *source);
void stopTestMode();

// =================================================================================
// SECTION: SESSION TERMINATION
// =================================================================================
void abortSession(const char *source);
void completeSession();

// =================================================================================
// SECTION: PERIODIC LOGIC & WATCHDOGS
// =================================================================================
void startTimersForState(SessionState state);
void handleOneSecondTick();
bool checkSessionKeepAliveWatchdog();

#endif