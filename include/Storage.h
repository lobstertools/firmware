#ifndef STORAGE_H
#define STORAGE_H

#include <Arduino.h>

bool loadState();
void saveState(bool force = false);
void checkBootLoop();

#endif