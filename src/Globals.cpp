/*
 * =================================================================================
 * Project:   Lobster Lock - Self-Bondage Session Manager
 * File:      Globals.cpp
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Description:
 * Definitions of shared global state variables, synchronization primitives (Mutex),
 * hardware object instances (Button, LED), and persistent configuration flags.
 * =================================================================================
 */
#include "Globals.h"
#include "Config.h"

// =================================================================================
// SECTION: HARDWARE & SYSTEM OBJECTS
// =================================================================================

SystemDefaults g_systemDefaults = DEFAULT_SYSTEM_DEFS;
