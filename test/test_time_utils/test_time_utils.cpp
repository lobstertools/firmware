/*
 * File: test/test_time_utils/test_time_utils.cpp
 * Description: Unit tests for the static TimeUtils class.
 * Verifies correct human-readable formatting of seconds into y/m/w/d/h/min/s.
 */

#include <unity.h>
#include "TimeUtils.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

// --- Basic Unit Tests ---

void test_format_zero_seconds(void) {
    char buf[32];
    TimeUtils::formatSeconds(0, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("0s", buf);
}

void test_format_seconds_only(void) {
    char buf[32];
    TimeUtils::formatSeconds(45, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("45s", buf);
}

void test_format_minutes_only(void) {
    char buf[32];
    // 2 minutes = 120s
    TimeUtils::formatSeconds(120, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("2min", buf);
}

void test_format_hours_only(void) {
    char buf[32];
    // 2 hours = 7200s
    TimeUtils::formatSeconds(7200, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("2h", buf);
}

// --- Combination & Skipping Tests ---

void test_format_hours_minutes_seconds(void) {
    char buf[64];
    // 1h (3600) + 10min (600) + 5s = 4205
    TimeUtils::formatSeconds(4205, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("1h 10min 5s", buf);
}

void test_format_skips_zero_middle_units(void) {
    char buf[64];
    // 1h (3600) + 5s = 3605. (Minutes should be skipped)
    TimeUtils::formatSeconds(3605, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("1h 5s", buf);
}

void test_format_skips_trailing_zero_units(void) {
    char buf[64];
    // 1d (86400) + 1h (3600) = 90000. (No min, no sec)
    TimeUtils::formatSeconds(90000, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("1d 1h", buf);
}

// --- Long Duration Tests ---

void test_format_weeks_days(void) {
    char buf[64];
    // 1w (604800) + 2d (172800) = 777600
    TimeUtils::formatSeconds(777600, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("1w 2d", buf);
}

void test_format_full_complexity(void) {
    char buf[128];
    // Constants from TimeUtils.h:
    // Year: 31536000, Month: 2592000, Week: 604800, Day: 86400, Hour: 3600, Min: 60
    
    unsigned long total = 
        31536000 +  // 1y
        2592000 +   // 1m
        604800 +    // 1w
        86400 +     // 1d
        3600 +      // 1h
        60 +        // 1min
        1;          // 1s
    
    // Total: 34822861
    TimeUtils::formatSeconds(total, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("1y 1m 1w 1d 1h 1min 1s", buf);
}

// --- Safety Tests ---

void test_buffer_truncation_safety(void) {
    // "1h 5s" requires 6 chars + null = 7 bytes.
    // We provide 4 bytes. 
    // Logic: 
    // 1. Appends "1h " (3 chars). Buffer: "1h \0". Offset: 3.
    // 2. Skips minutes.
    // 3. Seconds logic: if (offset < size)... 
    //    3 < 4 is true. 
    //    snprintf(buf+3, 4-3=1, "%lus", 5).
    //    snprintf needs size for chars + null. With size 1, it writes only null.
    // Expectation: "1h " or "1h" depending on snprintf behavior, but strictly NO overflow.
    
    char buf[4]; 
    memset(buf, 'X', sizeof(buf)); // Fill with garbage
    
    TimeUtils::formatSeconds(3605, buf, sizeof(buf));
    
    // Ensure null termination is present within bounds
    TEST_ASSERT_EQUAL_INT8('\0', buf[3]); // Last byte must be null or earlier
    
    // Check it didn't crash and contains partial data
    TEST_ASSERT_EQUAL_STRING("1h ", buf); 
}

int main(void) {
    UNITY_BEGIN();
    
    RUN_TEST(test_format_zero_seconds);
    RUN_TEST(test_format_seconds_only);
    RUN_TEST(test_format_minutes_only);
    RUN_TEST(test_format_hours_only);
    
    RUN_TEST(test_format_hours_minutes_seconds);
    RUN_TEST(test_format_skips_zero_middle_units);
    RUN_TEST(test_format_skips_trailing_zero_units);
    
    RUN_TEST(test_format_weeks_days);
    RUN_TEST(test_format_full_complexity);
    
    RUN_TEST(test_buffer_truncation_safety);
    
    return UNITY_END();
}