#ifndef SESSION_H
#define SESSION_H

#include "Config.h"

// Core Session Logic
int startSession(unsigned long duration, unsigned long penalty, TriggerStrategy strategy, unsigned long *delays, bool hide);
void stopTestMode();
void abortSession(const char *source);
void completeSession();

// Hardware Testing
int startTestMode();

// Session utilities
void triggerLock(const char *source);
void enterLockedState(const char *source);
void resetToReady(bool generateNewCode);

// Timer logic
void startTimersForState(SessionState state);
void handleOneSecondTick();

// Session Watch dog & Reboot Recovery
bool checkSessionKeepAliveWatchdog();
void handleRebootState();

// Button callbacks
void handlePress();
void handleLongPress();
void handleDoublePress();

#endif