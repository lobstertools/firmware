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
    // 1. Duration Type Mapping
    std::string typeStr = json["durationType"] | "fixed";
    if (typeStr == "random") outConfig.durationType = DUR_RANDOM;
    else if (typeStr == "short") outConfig.durationType = DUR_RANGE_SHORT;
    else if (typeStr == "medium") outConfig.durationType = DUR_RANGE_MEDIUM;
    else if (typeStr == "long") outConfig.durationType = DUR_RANGE_LONG;
    else if (typeStr == "fixed") outConfig.durationType = DUR_FIXED;
    else {
        errorMsg = "Invalid durationType.";
        return false;
    }

    // 2. Numeric Validation
    outConfig.fixedDuration = json["duration"] | 0;
    outConfig.minDuration   = json["minDuration"] | 0; 
    outConfig.maxDuration   = json["maxDuration"] | 0;

    // Sanity Check for Random
    if (outConfig.durationType == DUR_RANDOM && outConfig.minDuration > outConfig.maxDuration) {
        errorMsg = "minDuration cannot be greater than maxDuration.";
        return false;
    }

    // 3. Strategy Mapping
    std::string stratStr = json["triggerStrategy"] | "autoCountdown";
    if (stratStr == "buttonTrigger") outConfig.triggerStrategy = STRAT_BUTTON_TRIGGER;
    else outConfig.triggerStrategy = STRAT_AUTO_COUNTDOWN;
    
    outConfig.hideTimer = json["hideTimer"] | false;

    // 4. Channel Mask Logic
    // Reset delays
    for (int i = 0; i < MAX_CHANNELS; i++) outConfig.channelDelays[i] = 0;
    
    if (json["channelDelays"].is<JsonObject>()) {
        JsonObject dObj = json["channelDelays"];
        
        // Use type-safe conversion to avoid narrowing warnings
        uint32_t raw[4] = { 
            dObj["ch1"].as<uint32_t>(), 
            dObj["ch2"].as<uint32_t>(), 
            dObj["ch3"].as<uint32_t>(), 
            dObj["ch4"].as<uint32_t>() 
        };

        for (int i=0; i<4; i++) {
            // Check if user is trying to set delay for a disabled channel
            if (raw[i] > 0 && !((allowedChannelMask >> i) & 1)) {
                errorMsg = "Cannot set delay for disabled/missing channel: " + std::to_string(i+1);
                return false;
            }
            // Only apply if enabled
            if ((allowedChannelMask >> i) & 1) {
                outConfig.channelDelays[i] = raw[i];
            }
        }
    }
    
    return true;
}
