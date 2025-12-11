/*
 * File: test/test_web_validation/test_web_validation.cpp
 * Description: Unit tests for WebValidators input validation.
 * Verifies reasonable lengths for WiFi credentials and valid SessionConfig parsing.
 */
#include <unity.h>
#include <string.h>
#include <ArduinoJson.h>
#include "WebValidators.h"
#include "Types.h"

// ============================================================================
// WIFI VALIDATION TESTS
// ============================================================================

void test_wifi_valid_credentials(void) {
    std::string err;
    bool res = WebValidators::validateWifiCredentials("MyNetwork", "MyPassword123", err);
    TEST_ASSERT_TRUE_MESSAGE(res, "Should accept valid credentials");
    TEST_ASSERT_EQUAL_STRING("", err.c_str());
}

void test_wifi_ssid_empty(void) {
    std::string err;
    bool res = WebValidators::validateWifiCredentials("", "pass", err);
    TEST_ASSERT_FALSE_MESSAGE(res, "Should reject empty SSID");
    TEST_ASSERT_EQUAL_STRING("SSID cannot be empty.", err.c_str());
}

void test_wifi_ssid_too_long(void) {
    std::string err;
    // 33 chars
    const char* longSsid = "123456789012345678901234567890123"; 
    bool res = WebValidators::validateWifiCredentials(longSsid, "pass", err);
    
    TEST_ASSERT_FALSE_MESSAGE(res, "Should reject SSID > 32 chars");
    TEST_ASSERT_EQUAL_STRING("SSID too long (max 32 chars).", err.c_str());
}

void test_wifi_pass_too_long(void) {
    std::string err;
    // 65 chars
    const char* longPass = "12345678901234567890123456789012345678901234567890123456789012345";
    bool res = WebValidators::validateWifiCredentials("Network", longPass, err);
    
    TEST_ASSERT_FALSE_MESSAGE(res, "Should reject Password > 64 chars");
    TEST_ASSERT_EQUAL_STRING("Password too long (max 64 chars).", err.c_str());
}

void test_wifi_pass_empty_allowed(void) {
    std::string err;
    // Empty password is valid for Open networks
    bool res = WebValidators::validateWifiCredentials("OpenNetwork", "", err);
    TEST_ASSERT_TRUE(res);
}

// ============================================================================
// SESSION CONFIG PARSING TESTS
// ============================================================================

void test_parse_valid_fixed_config(void) {
    JsonDocument doc;
    doc["durationType"] = "fixed";
    doc["duration"] = 600;
    doc["triggerStrategy"] = "buttonTrigger";

    SessionConfig cfg;
    std::string err;
    // Assume all channels enabled (mask 0x0F = 1111)
    bool res = WebValidators::parseSessionConfig(doc, 0x0F, cfg, err);

    TEST_ASSERT_TRUE(res);
    TEST_ASSERT_EQUAL(DUR_FIXED, cfg.durationType);
    TEST_ASSERT_EQUAL_UINT32(600, cfg.fixedDuration);
    TEST_ASSERT_EQUAL(STRAT_BUTTON_TRIGGER, cfg.triggerStrategy);
}

void test_parse_invalid_duration_type(void) {
    JsonDocument doc;
    doc["durationType"] = "infinite"; // Invalid

    SessionConfig cfg;
    std::string err;
    bool res = WebValidators::parseSessionConfig(doc, 0x0F, cfg, err);

    TEST_ASSERT_FALSE(res);
    TEST_ASSERT_EQUAL_STRING("Invalid durationType.", err.c_str());
}

void test_parse_channel_mask_enforcement(void) {
    JsonDocument doc;
    JsonObject delays = doc["channelDelays"].to<JsonObject>();
    delays["ch1"] = 10;
    delays["ch2"] = 10; // Requesting delay on Ch2

    SessionConfig cfg;
    std::string err;
    // Hardware mask 0x01 (Only Ch1 enabled, Ch2 disabled)
    uint8_t hardwareMask = 0x01; 

    bool res = WebValidators::parseSessionConfig(doc, hardwareMask, cfg, err);

    TEST_ASSERT_FALSE_MESSAGE(res, "Should reject delay for disabled channel");
    TEST_ASSERT_EQUAL_STRING("Cannot set delay for disabled/missing channel: 2", err.c_str());
}

void test_parse_random_range_sanity(void) {
    JsonDocument doc;
    doc["durationType"] = "random";
    doc["minDuration"] = 500;
    doc["maxDuration"] = 100; // Invalid: Min > Max

    SessionConfig cfg;
    std::string err;
    bool res = WebValidators::parseSessionConfig(doc, 0x0F, cfg, err);

    TEST_ASSERT_FALSE(res);
    TEST_ASSERT_EQUAL_STRING("minDuration cannot be greater than maxDuration.", err.c_str());
}

// ============================================================================
// MAIN
// ============================================================================

void setUp(void) {}
void tearDown(void) {}

int main(void) {
    UNITY_BEGIN();
    
    // WiFi Validation
    RUN_TEST(test_wifi_valid_credentials);
    RUN_TEST(test_wifi_ssid_empty);
    RUN_TEST(test_wifi_ssid_too_long);
    RUN_TEST(test_wifi_pass_too_long);
    RUN_TEST(test_wifi_pass_empty_allowed);

    // Session Config Validation
    RUN_TEST(test_parse_valid_fixed_config);
    RUN_TEST(test_parse_invalid_duration_type);
    RUN_TEST(test_parse_channel_mask_enforcement);
    RUN_TEST(test_parse_random_range_sanity);

    return UNITY_END();
}