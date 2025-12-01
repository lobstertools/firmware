/*
 * =================================================================================
 * Project:   Lobster Lock - Self-Bondage Session Manager
 * File:      Logger.h / Logger.cpp
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Description:
 * Thread-safe logging system. Manages a ring buffer for storing logs in memory,
 * allowing them to be retrieved via the Web API or printed to Serial.
 * =================================================================================
 */
#ifndef LOGGER_H
#define LOGGER_H

// =================================================================================
// SECTION: LOGGING CONSTANTS & MACROS
// =================================================================================
#define LOG_SEP_MAJOR "=========================================================================="
#define LOG_SEP_MINOR "--------------------------------------------------------------------------"
#define LOG_PREFIX_STATE ">>> STATE CHANGE: "

// =================================================================================
// SECTION: CORE LOGGING FUNCTIONS
// =================================================================================
void logMessage(const char *message);
void processLogQueue();

// =================================================================================
// SECTION: BUFFER EXTERNS (FOR API)
// =================================================================================
extern const int LOG_BUFFER_SIZE;
extern const int MAX_LOG_ENTRY_LENGTH;
extern char logBuffer[][150];
extern int logBufferIndex;
extern bool logBufferFull;

#endif