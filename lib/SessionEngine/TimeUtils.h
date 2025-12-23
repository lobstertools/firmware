/*
 * =================================================================================
 * Project:   Lobster Lock - Self-Bondage Session Manager
 * File:      lib/SessionEngine/TimeUtils.h
 *
 * Description:
 * Static utility class for time manipulation and string formatting.
 * Provides functionality to convert raw seconds into human-readable duration 
 * strings (e.g., "2w 3d 5h 10s") supporting units from Years down to Seconds.
 * Designed for clear logging and debugging output.
 * =================================================================================
 */
#pragma once
#include <stddef.h>
#include <stdio.h>

class TimeUtils {
public:
    /**
     * Formats seconds into a human-readable string (e.g., "1y 2m 3w 4d 5h 6m 7s").
     * Units with 0 values are omitted unless the total time is 0s.
     * @param totalSeconds The duration in seconds.
     * @param buffer       The destination buffer.
     * @param size         The size of the buffer.
     */
    static void formatSeconds(unsigned long totalSeconds, char *buffer, size_t size) {
        if (totalSeconds == 0) {
            snprintf(buffer, size, "0s");
            return;
        }

        // Time Constants
        const unsigned long SECS_MIN   = 60;
        const unsigned long SECS_HOUR  = 3600;
        const unsigned long SECS_DAY   = 86400;
        const unsigned long SECS_WEEK  = 604800;  // 7 days
        const unsigned long SECS_MONTH = 2592000; // 30 days (Approximation)
        const unsigned long SECS_YEAR  = 31536000; // 365 days

        unsigned long rem = totalSeconds;

        unsigned long y = rem / SECS_YEAR;
        rem %= SECS_YEAR;

        unsigned long mo = rem / SECS_MONTH;
        rem %= SECS_MONTH;

        unsigned long w = rem / SECS_WEEK;
        rem %= SECS_WEEK;

        unsigned long d = rem / SECS_DAY;
        rem %= SECS_DAY;

        unsigned long h = rem / SECS_HOUR;
        rem %= SECS_HOUR;

        unsigned long m = rem / SECS_MIN;
        unsigned long s = rem % SECS_MIN;

        // Build string
        buffer[0] = '\0';
        size_t offset = 0;

        auto append = [&](unsigned long val, const char* suffix) {
            if (val > 0 && offset < size) {
                offset += snprintf(buffer + offset, size - offset, "%lu%s ", val, suffix);
            }
        };

        append(y, "y");
        append(mo, "m");
        append(w, "w");
        append(d, "d");
        append(h, "h");
        append(m, "min"); // using 'min' to distinguish from 'm' (month)

        // Show seconds if it's the only unit, or if it exists
        if (s > 0 || offset == 0) {
            if (offset < size) {
                offset += snprintf(buffer + offset, size - offset, "%lus", s);
            }
        } else {
            // Trim trailing space
            if (offset > 0 && buffer[offset - 1] == ' ') {
                buffer[offset - 1] = '\0';
            }
        }
    }
};