#include "WebValidators.h"

bool WebValidators::validateWifiCredentials(const char* ssid, const char* pass, std::string& errorMsg) {
    if (!ssid || strlen(ssid) == 0) {
        errorMsg = "SSID cannot be empty.";
        return false;
    }
    if (strlen(ssid) > 32) {
        errorMsg = "SSID too long (max 32 chars).";
        return false;
    }
    // Password can be empty for Open networks, but max length applies
    if (pass && strlen(pass) > 64) {
        errorMsg = "Password too long (max 64 chars).";
        return false;
    }
    return true;
}

bool WebValidators::parseSessionConfig(const JsonVariant& json, uint8_t allowedChannelMask, SessionConfig& outConfig, std::string& errorMsg) {
    // 1. Duration Type Mapping (Matching TS Enums)
    std::string typeStr = json["durationType"] | "DUR_FIXED";
    
    if (typeStr == "DUR_RANDOM") outConfig.durationType = DUR_RANDOM;
    else if (typeStr == "DUR_RANGE_SHORT") outConfig.durationType = DUR_RANGE_SHORT;
    else if (typeStr == "DUR_RANGE_MEDIUM") outConfig.durationType = DUR_RANGE_MEDIUM;
    else if (typeStr == "DUR_RANGE_LONG") outConfig.durationType = DUR_RANGE_LONG;
    else if (typeStr == "DUR_FIXED") outConfig.durationType = DUR_FIXED;
    else {
        // Fallback for backward compatibility or error
        errorMsg = "Invalid durationType: " + typeStr;
        return false;
    }

    // 2. Numeric Validation (Matching TS Interface)
    outConfig.durationFixed = json["durationFixed"] | 0;
    outConfig.durationMin   = json["durationMin"] | 0; 
    outConfig.durationMax   = json["durationMax"] | 0;

    // Sanity Check for Random
    if (outConfig.durationType == DUR_RANDOM && outConfig.durationMin > outConfig.durationMax) {
        errorMsg = "durationMin cannot be greater than durationMax.";
        return false;
    }

    // 3. Strategy Mapping
    std::string stratStr = json["triggerStrategy"] | "STRAT_AUTO_COUNTDOWN";
    if (stratStr == "STRAT_BUTTON_TRIGGER") outConfig.triggerStrategy = STRAT_BUTTON_TRIGGER;
    else outConfig.triggerStrategy = STRAT_AUTO_COUNTDOWN;
    
    // 4. Booleans
    outConfig.hideTimer = json["hideTimer"] | false;
    outConfig.disableLED = json["disableLED"] | false;

    // 5. Channel Mask Logic (Expecting Array [ch1, ch2, ch3, ch4])
    // Reset delays
    for (int i = 0; i < MAX_CHANNELS; i++) outConfig.channelDelays[i] = 0;
    
    if (json["channelDelays"].is<JsonArray>()) {
        JsonArray delays = json["channelDelays"].as<JsonArray>();
        
        // Ensure we don't overflow if the array is too large
        int count = 0;
        for (JsonVariant v : delays) {
            if (count >= 4) break;
            
            uint32_t delayVal = v.as<uint32_t>();

            // Check if user is trying to set delay for a disabled channel
            if (delayVal > 0 && !((allowedChannelMask >> count) & 1)) {
                errorMsg = "Cannot set delay for disabled/missing channel index: " + std::to_string(count);
                return false;
            }

            // Only apply if enabled
            if ((allowedChannelMask >> count) & 1) {
                outConfig.channelDelays[count] = delayVal;
            }
            count++;
        }
    } else {
        // If missing or wrong type, default to 0s is already set above
    }
    
    return true;
}