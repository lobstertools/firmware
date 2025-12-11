/*
 * =================================================================================
 * Project:   Lobster Lock - Self-Bondage Session Manager
 * File:      lib/SessionEngine/LogicUtils.h
 *
 * Description:
 * Pure logic utilities for checksums and formatting.
 * Extracted here to allow usage in both Firmware (Arduino) and Native Tests (PC)
 * without dependency conflicts.
 * =================================================================================
 */
#pragma once
#include <stdio.h>
#include <string.h>
#include <stdint.h>

class LogicUtils {
public:
    /**
     * NATO Phonetic Alphabet Lookup.
     * Used for generating human-readable checksum strings.
     */
    static const char* getNatoWord(char c) {
        switch (c) {
        case 'A': return "Alpha";
        case 'B': return "Bravo";
        case 'C': return "Charlie";
        case 'D': return "Delta";
        case 'E': return "Echo";
        case 'F': return "Foxtrot";
        case 'G': return "Golf";
        case 'H': return "Hotel";
        case 'I': return "India";
        case 'J': return "Juliett";
        case 'K': return "Kilo";
        case 'L': return "Lima";
        case 'M': return "Mike";
        case 'N': return "November";
        case 'O': return "Oscar";
        case 'P': return "Papa";
        case 'Q': return "Quebec";
        case 'R': return "Romeo";
        case 'S': return "Sierra";
        case 'T': return "Tango";
        case 'U': return "Uniform";
        case 'V': return "Victor";
        case 'W': return "Whiskey";
        case 'X': return "X-ray";
        case 'Y': return "Yankee";
        case 'Z': return "Zulu";
        default: return "";
        }
    }

    /**
     * Calculates the Alpha-Numeric Checksum (NATO-00).
     * Output Format: "Alpha-92"
     * * @param code The source session code (e.g., "UDLR...")
     * @param outString Buffer to write the checksum to (Must be >= 16 chars)
     */
    static void calculateChecksum(const char *code, char *outString) {
        int weightedSum = 0;
        int rollingVal = 0;
        int len = strlen(code);

        for (int i = 0; i < len; i++) {
            char c = code[i];
            int val = 0;
            
            // Map directional characters to values
            if (c == 'U') val = 1;
            else if (c == 'D') val = 2;
            else if (c == 'L') val = 3;
            else if (c == 'R') val = 4;

            // Alpha-Tag Logic (Weighted Sum)
            weightedSum += val * (i + 1);

            // Numeric Logic (Rolling Hash)
            rollingVal = (rollingVal * 3 + val) % 100;
        }

        // Map Weighted Sum to A-Z
        int alphaIndex = weightedSum % 26;
        char alphaChar = (char)('A' + alphaIndex);

        // Format string: "NATO-NUM"
        // Ensure buffer safety in caller
        snprintf(outString, 16, "%s-%02d", getNatoWord(alphaChar), rollingVal);
    }
};

