/*
 * =================================================================================
 * Project:   Lobster Lock - Self-Bondage Session Manager
 * File:      WebAPI.h / WebAPI.cpp
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Description:
 * Async HTTP Server implementation. Defines RESTful JSON endpoints for
 * controlling the device (Arm/Abort), retrieving status/logs, and
 * configuring settings via a web client.
 * =================================================================================
 */
#ifndef WEBAPI_H
#define WEBAPI_H

// =================================================================================
// SECTION: SERVER SETUP
// =================================================================================
void setupWebServer();

#endif