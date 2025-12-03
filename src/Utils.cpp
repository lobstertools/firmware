/*
 * =================================================================================
 * Project:   Lobster Lock - Self-Bondage Session Manager
 * File:      Utils.h / Utils.cpp
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Description:
 * General utility functions. Includes string formatting, data conversion
 * (Bytes to Int), unique reward code generation, checksum calculations,
 * and state-to-string helpers.
 * =================================================================================
 */
#include "Utils.h"
#include "Globals.h"
#include <Arduino.h>

/**
 * Helper to convert a 2-byte LE array to uint16_t
 */
uint16_t bytesToUint16(uint8_t *data) { return (uint16_t)data[0] | ((uint16_t)data[1] << 8); }

/**
 * Helper to convert a 4-byte LE array to uint32_t
 */
uint32_t bytesToUint32(uint8_t *data) {
  return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

/**
 * Formats seconds into "H h, M min, S s"
 */
void formatSeconds(unsigned long totalSeconds, char *buffer, size_t size) {
  unsigned long hours = totalSeconds / 3600;
  unsigned long minutes = (totalSeconds % 3600) / 60;
  unsigned long seconds = totalSeconds % 60;
  snprintf(buffer, size, "%lu h, %lu min, %lu s", hours, minutes, seconds);
}

/**
 * Converts a SessionState enum to its string representation.
 */
const char *stateToString(SessionState s) {
  switch (s) {
  case READY:
    return "ready";
  case ARMED:
    return "armed";
  case LOCKED:
    return "locked";
  case ABORTED:
    return "aborted";
  case COMPLETED:
    return "completed";
  case TESTING:
    return "testing";
  case VALIDATING:
    return "validating";
  default:
    return "unknown";
  }
}

/**
 * NATO Phonetic Alphabet Lookup
 */
const char *getNatoWord(char c) {
  switch (c) {
  case 'A':
    return "Alpha";
  case 'B':
    return "Bravo";
  case 'C':
    return "Charlie";
  case 'D':
    return "Delta";
  case 'E':
    return "Echo";
  case 'F':
    return "Foxtrot";
  case 'G':
    return "Golf";
  case 'H':
    return "Hotel";
  case 'I':
    return "India";
  case 'J':
    return "Juliett";
  case 'K':
    return "Kilo";
  case 'L':
    return "Lima";
  case 'M':
    return "Mike";
  case 'N':
    return "November";
  case 'O':
    return "Oscar";
  case 'P':
    return "Papa";
  case 'Q':
    return "Quebec";
  case 'R':
    return "Romeo";
  case 'S':
    return "Sierra";
  case 'T':
    return "Tango";
  case 'U':
    return "Uniform";
  case 'V':
    return "Victor";
  case 'W':
    return "Whiskey";
  case 'X':
    return "X-ray";
  case 'Y':
    return "Yankee";
  case 'Z':
    return "Zulu";
  default:
    return "";
  }
}

/**
 * Calculates the Alpha-Numeric Checksum (NATO-00)
 * Output Format: "Alpha-92"
 */
void calculateChecksum(const char *code, char *outString) {
  int weightedSum = 0;
  int rollingVal = 0;
  int len = strlen(code);

  for (int i = 0; i < len; i++) {
    char c = code[i];
    int val = 0;
    if (c == 'U')
      val = 1;
    else if (c == 'D')
      val = 2;
    else if (c == 'L')
      val = 3;
    else if (c == 'R')
      val = 4;

    // Alpha-Tag Logic (Weighted Sum)
    weightedSum += val * (i + 1);

    // Numeric Logic (Rolling Hash)
    rollingVal = (rollingVal * 3 + val) % 100;
  }

  // Map to A-Z
  int alphaIndex = weightedSum % 26;
  char alphaChar = (char)('A' + alphaIndex);

  // Format string: "NATO-NUM"
  snprintf(outString, REWARD_CHECKSUM_LENGTH, "%s-%02d", getNatoWord(alphaChar), rollingVal);
}

/**
 * Fills buffers with a new random session code AND its checksum.
 * Ensures the checksum does NOT collide with any historical checksums.
 */
void generateUniqueSessionCode(char *codeBuffer, char *checksumBuffer) {
  const char chars[] = "UDLR";
  bool collision = true;

  while (collision) {
    // 1. Generate Candidate Code
    for (int i = 0; i < REWARD_CODE_LENGTH; ++i) {
      codeBuffer[i] = chars[esp_random() % 4];
    }
    codeBuffer[REWARD_CODE_LENGTH] = '\0';

    // 2. Calculate Candidate Checksum
    calculateChecksum(codeBuffer, checksumBuffer);

    // 3. Check for collisions against existing history
    // Note: rewardHistory[0] is the slot we are currently filling, so
    // check 1..Size-1
    collision = false;
    for (int i = 1; i < REWARD_HISTORY_SIZE; i++) {
      // We compare the Checksum Strings (stored in the timestamp field)
      // If history entry is empty, skip it
      if (strlen(rewardHistory[i].checksum) > 0) {
        if (strncmp(checksumBuffer, rewardHistory[i].checksum, REWARD_CHECKSUM_LENGTH) == 0) {
          collision = true; // Duplicate found!
          break;
        }
      }
    }
  }
}