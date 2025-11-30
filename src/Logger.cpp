#include "Logger.h"
#include "Globals.h"
#include <Arduino.h>

// --- Logging System ---
// Ring buffer for storing logs in memory.
const int LOG_BUFFER_SIZE = 150;
const int MAX_LOG_ENTRY_LENGTH = 150;
char logBuffer[LOG_BUFFER_SIZE][MAX_LOG_ENTRY_LENGTH]; // For WebUI
int logBufferIndex = 0;
bool logBufferFull = false;

// Serial Log Queue (To prevent Serial blocks inside Mutex)
const int SERIAL_QUEUE_SIZE = 50;
char serialLogQueue[SERIAL_QUEUE_SIZE][MAX_LOG_ENTRY_LENGTH];
volatile int serialQueueHead = 0;
volatile int serialQueueTail = 0;

/**
 * Thread-safe logging. NO SERIAL IO IN THIS FUNCTION.
 * Adds a message to the in-memory log buffer and pushes to the Serial Queue.
 */
void logMessage(const char *message) {
  // Used shorter timeout here to prevent loop starvation
  if (xSemaphoreTakeRecursive(stateMutex, (TickType_t)pdMS_TO_TICKS(100)) == pdTRUE) {

    // Update RAM ring buffer (for API)
    // Removed timestamping per request. Just store the message.
    snprintf(logBuffer[logBufferIndex], MAX_LOG_ENTRY_LENGTH, "%s", message);
    logBufferIndex++;
    if (logBufferIndex >= LOG_BUFFER_SIZE) {
      logBufferIndex = 0;
      logBufferFull = true;
    }

    // Push to Serial Queue
    int nextHead = (serialQueueHead + 1) % SERIAL_QUEUE_SIZE;

    if (nextHead != serialQueueTail) {
      // Buffer not full
      snprintf(serialLogQueue[serialQueueHead], MAX_LOG_ENTRY_LENGTH, "%s", message);
      serialQueueHead = nextHead;
    } else {
      // Buffer full
    }

    xSemaphoreGiveRecursive(stateMutex);
  } else {
    // Emergency fallback
  }
}

/**
 * Called in main loop to drain log queue safely to Serial port.
 * Allows printing without blocking critical sections.
 * Drains up to 10 messages per call to prevent lag/dropped logs.
 */
void processLogQueue() {
  // Process up to 10 lines at once so we don't fall behind
  int maxLinesToProcess = 10;

  while (maxLinesToProcess > 0) {
    char msgCopy[MAX_LOG_ENTRY_LENGTH];
    bool hasMessage = false;

    // 1. Quick lock to check/pop a message
    // We use a short timeout; if we can't get the lock, we skip printing this
    // cycle
    if (xSemaphoreTakeRecursive(stateMutex, (TickType_t)pdMS_TO_TICKS(5)) == pdTRUE) {
      if (serialQueueHead != serialQueueTail) {
        // Copy message out
        strncpy(msgCopy, serialLogQueue[serialQueueTail], MAX_LOG_ENTRY_LENGTH);
        msgCopy[MAX_LOG_ENTRY_LENGTH - 1] = '\0'; // safety null

        // Advance tail
        serialQueueTail = (serialQueueTail + 1) % SERIAL_QUEUE_SIZE;
        hasMessage = true;
      }
      // RELEASE LOCK IMMEDIATELY
      xSemaphoreGiveRecursive(stateMutex);
    } else {
      // If we couldn't get the lock, stop trying for this cycle
      break;
    }

    // 2. Print OUTSIDE the lock (Serial is slow!)
    if (hasMessage) {
      Serial.println(msgCopy);
      maxLinesToProcess--;
    } else {
      // Queue is empty, we are done
      break;
    }
  }
}