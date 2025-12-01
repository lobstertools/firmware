#ifndef STORAGE_H
#define STORAGE_H

#include <Arduino.h>

// =================================================================================
// SECTION: NVS STATE PERSISTENCE
// =================================================================================
bool loadState();
void saveState(bool force = false);

#endif