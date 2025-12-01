#ifndef UTILS_H
#define UTILS_H

#include "Config.h"

// =================================================================================
// SECTION: FORMATTING & CONVERSIONS
// =================================================================================
uint16_t bytesToUint16(uint8_t *data);
uint32_t bytesToUint32(uint8_t *data);
void formatSeconds(unsigned long totalSeconds, char *buffer, size_t size);
const char *stateToString(SessionState state);

// =================================================================================
// SECTION: GENERATORS & HELPERS
// =================================================================================
void generateUniqueSessionCode(char *codeBuffer, char *checksumBuffer);
void calculateChecksum(const char *code, char *outString);
const char *getNatoWord(char c);

#endif