#ifndef LOGGER_H
#define LOGGER_H

void logMessage(const char *message);
void processLogQueue();

// Accessors for WebAPI
extern const int LOG_BUFFER_SIZE;
extern const int MAX_LOG_ENTRY_LENGTH;
extern char logBuffer[][150];
extern int logBufferIndex;
extern bool logBufferFull;

#endif