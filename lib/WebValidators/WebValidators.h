#pragma once
#include <ArduinoJson.h>
#include <string.h>
#include "Session.h"

class WebValidators {
public:
    // Validates WiFi credentials (length checks, empty checks)
    // Returns true if valid, false otherwise. Writes explanation to errorMsg.
    static bool validateWifiCredentials(const char* ssid, const char* pass, std::string& errorMsg);

    // Parses JSON and validates against a hardware mask.
    // Returns true if valid. Populates outConfig.
    static bool parseSessionConfig(const JsonVariant& json, uint8_t allowedChannelMask, SessionConfig& outConfig, std::string& errorMsg);
};