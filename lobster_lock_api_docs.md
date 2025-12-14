# Lobster Lock API Documentation

## Overview

The Lobster Lock is a self-bondage session management device with an HTTP API for remote control and monitoring. This documentation describes all available endpoints, data structures, and usage patterns.

**Base URL:** `http://<device-ip>/`  
**Default Port:** `80`

---

## Table of Contents

- [Authentication](#authentication)
- [Data Structures](#data-structures)
- [Endpoints](#endpoints)
  - [System & Health](#system--health)
  - [Session Control](#session-control)
  - [Status & Information](#status--information)
  - [Configuration](#configuration)
- [Error Handling](#error-handling)
- [State Machine](#state-machine)

---

## Authentication

Currently, no authentication is required. Ensure the device is only accessible on trusted networks.

---

## Data Structures

### DeviceState

Represents the current operational state of the device.

**Values:** `READY` | `ARMED` | `LOCKED` | `ABORTED` | `COMPLETED` | `TESTING`

### TriggerStrategy

**Values:**
- `STRAT_AUTO_COUNTDOWN` - Session starts automatically after arming
- `STRAT_BUTTON_TRIGGER` - Session starts when button is pressed

### DurationType

**Values:**
- `DUR_FIXED` - Fixed duration specified by `durationFixed`
- `DUR_RANDOM` - Random duration between `durationMin` and `durationMax`
- `DUR_RANGE_SHORT` - Uses preset short range
- `DUR_RANGE_MEDIUM` - Uses preset medium range
- `DUR_RANGE_LONG` - Uses preset long range

### DeterrentStrategy

**Values:**
- `DETERRENT_FIXED` - Fixed deterrent value
- `DETERRENT_RANDOM` - Random deterrent within specified range

### DeviceFeature

**Values:** `footPedal` | `startCountdown` | `statusLed`

### Identity

Firmware identification and build metadata.

```typescript
{
  name: string;           // e.g., "Lobster Lock"
  version: string;        // e.g., "1.2.0"
  buildType: "beta" | "debug" | "mock" | "local_release" | "release";
  buildDate: string;      // Compilation date
  buildTime: string;      // Compilation time
  cppStandard: number;    // C++ standard version
}
```

### Network

Network configuration and state.

```typescript
{
  ssid: string;
  rssi: number;           // Signal strength in dBm
  mac: string;            // MAC address
  ip: string;             // IP address
  subnetMask: string;
  gateway: string;
  hostname: string;
  port: number;           // Always 80
}
```

### Channels

Hardware channel configuration.

```typescript
{
  ch1: boolean;
  ch2: boolean;
  ch3: boolean;
  ch4: boolean;
}
```

### SessionConfig

Configuration for a session.

```typescript
{
  durationType: DurationType;
  durationFixed: number;           // (Seconds)
  durationMin: number;             // (Seconds)
  durationMax: number;             // (Seconds)
  triggerStrategy: TriggerStrategy;
  channelDelays: [number, number, number, number];  // (Seconds) for each channel
  hideTimer: boolean;              // Hide remaining time
  disableLED: boolean;             // Disable status LED
}
```

### SessionTimers

Current timing information.

```typescript
{
  lockDuration: number;            // Total lock duration
  debtServed: number;              // The amount of debt paid off when completing
  penaltyDuration: number;         // Total penalty duration
  lockRemaining: number;           // Lock time remaining
  penaltyRemaining: number;        // Penalty time remaining
  testRemaining: number;           // Test mode time remaining
  triggerTimeout: number;          // Time until trigger timeout
  channelDelays: [number, number, number, number];
}
```

### SessionStats

Session statistics.

```typescript
{
  streaks: number;                 // Current streak count
  completed: number;               // Completed sessions
  aborted: number;                 // Aborted sessions
  paybackAccumulated: number;      // Accumulated payback time
  totalLockedTime: number;         // Total time locked
}
```

### Telemetry

Real-time hardware telemetry.

```typescript
{
  buttonPressed: boolean;
  currentPressDurationMs: number;
  rssi: number;                    // WiFi signal strength (dBm)
  freeHeap: number;                // Free memory (bytes)
  uptime: number;                  // Uptime in (Seconds)
  internalTempC: number | "N/A";   // Internal temperature
}
```

### SessionPresets

Duration range presets.

```typescript
{
  shortMin: number;                // (Seconds)
  shortMax: number;
  mediumMin: number;
  mediumMax: number;
  longMin: number;
  longMax: number;
  minSessionDuration: number;      // Absolute minimum
  maxSessionDuration: number;      // Absolute maximum
}
```

### DeterrentConfig

Deterrent and penalty configuration.

```typescript
{
  enableStreaks: boolean;
  enableRewardCode: boolean;
  rewardPenaltyStrategy: DeterrentStrategy;
  rewardPenaltyMin: number;        // (Seconds)
  rewardPenaltyMax: number;
  rewardPenalty: number;
  enablePaybackTime: boolean;
  paybackTimeStrategy: DeterrentStrategy;
  paybackTimeMin: number;
  paybackTimeMax: number;
  paybackTime: number;
}
```

### SystemDefaults

System-level default values.

```typescript
{
  longPressDuration: number;       // (Seconds)
  extButtonSignalDuration: number;
  testModeDuration: number;
  keepAliveInterval: number;
  keepAliveMaxStrikes: number;
  bootLoopThreshold: number;
  stableBootTime: number;
  wifiMaxRetries: number;
  armedTimeout: number;
}
```

### Reward

Reward code information.

```typescript
{
  code: string;
  checksum: string;
}
```

---

## Endpoints

### System & Health

#### GET /

Returns a simple HTML page with device information.

**Response:** `text/html`

---

#### GET /health

Health check endpoint to verify device is reachable.

**Response:**
```json
{
  "status": "ok",
  "message": "Device is reachable."
}
```

---

#### POST /keepalive

Resets the watchdog timer during active sessions.

**Response:** `200 OK` (empty body)

**Error Responses:**
- `503` - System busy

---

#### POST /reboot

Reboots the device. Only allowed when device is in `READY` or `COMPLETED` state.

**Response:**
```json
{
  "status": "rebooting"
}
```

**Error Responses:**
- `403` - Reboot denied, device is active
- `503` - System busy

---

#### POST /factory-reset

Performs a factory reset, wiping all settings. Only allowed when device is in `READY` state.

**Response:**
```json
{
  "status": "resetting"
}
```

**Error Responses:**
- `409` - Cannot reset while device is active
- `503` - System busy

---

### Session Control

#### POST /arm

Arms the device with a new session configuration.

**Request Body:** `application/json`

```json
{
  "durationType": "DUR_FIXED",
  "durationFixed": 300000,
  "durationMin": 0,
  "durationMax": 0,
  "triggerStrategy": "STRAT_AUTO_COUNTDOWN",
  "channelDelays": [0, 0, 0, 0],
  "hideTimer": false,
  "disableLED": false
}
```

**Response:**
```json
{
  "status": "armed"
}
```

**Error Responses:**
- `400` - Invalid JSON or configuration
- `503` - System busy

**Notes:**
- Duration values are in (Seconds)
- `channelDelays` must be an array of 4 numbers
- Device must be in `READY` state to arm

---

#### POST /start-test

Starts a test mode session to verify hardware functionality.

**Response:**
```json
{
  "status": "testing"
}
```

**Error Responses:**
- `409` - Cannot start test (device not ready)
- `503` - System busy

---

#### POST /abort

Aborts the current session or test.

**Response:**
```json
{
  "status": "ABORTED"
}
```

**Possible status values:** `ABORTED` | `COMPLETED` | `READY`

**Error Responses:**
- `503` - System busy

---

### Status & Information

#### GET /status

Returns complete session status including configuration, timers, stats, and telemetry.

**Response:** `application/json`

```json
{
  "state": "LOCKED",
  "verified": true,
  "config": {
    "durationType": "DUR_FIXED",
    "durationFixed": 300000,
    "durationMin": 0,
    "durationMax": 0,
    "triggerStrategy": "STRAT_AUTO_COUNTDOWN",
    "hideTimer": false,
    "disableLED": false,
    "channelDelays": [0, 0, 0, 0]
  },
  "timers": {
    "lockDuration": 300000,
    "penaltyDuration": 0,
    "lockRemaining": 250000,
    "penaltyRemaining": 0,
    "testRemaining": 0,
    "triggerTimeout": 0,
    "channelDelays": [0, 0, 0, 0]
  },
  "stats": {
    "streaks": 2,
    "completed": 5,
    "aborted": 1,
    "paybackAccumulated": 0,
    "totalLockedTime": 1500000
  },
  "telemetry": {
    "buttonPressed": false,
    "currentPressDurationMs": 0,
    "rssi": -45,
    "freeHeap": 123456,
    "uptime": 3600000,
    "internalTempC": 45.2
  }
}
```

**Error Responses:**
- `503` - System busy

---

#### GET /details

Returns comprehensive device details including identity, network, features, channels, presets, deterrent configuration, and system defaults.

**Response:** `application/json`

```json
{
  "id": "lobster-lock-A1B2C3",
  "identity": {
    "name": "Lobster Lock",
    "version": "1.2.0",
    "buildType": "release",
    "buildDate": "Dec 14 2025",
    "buildTime": "10:30:00",
    "cppStandard": 201703
  },
  "network": {
    "ssid": "MyNetwork",
    "rssi": -45,
    "mac": "AA:BB:CC:DD:EE:FF",
    "ip": "192.168.1.100",
    "subnetMask": "255.255.255.0",
    "gateway": "192.168.1.1",
    "hostname": "lobster-lock",
    "port": 80
  },
  "features": ["footPedal", "startCountdown", "statusLed"],
  "channels": {
    "ch1": true,
    "ch2": true,
    "ch3": false,
    "ch4": false
  },
  "presets": {
    "shortMin": 60000,
    "shortMax": 300000,
    "mediumMin": 300000,
    "mediumMax": 900000,
    "longMin": 900000,
    "longMax": 3600000,
    "minSessionDuration": 30000,
    "maxSessionDuration": 7200000
  },
  "deterrentConfig": {
    "enableStreaks": true,
    "enableRewardCode": true,
    "rewardPenaltyStrategy": "DETERRENT_RANDOM",
    "rewardPenaltyMin": 60000,
    "rewardPenaltyMax": 300000,
    "rewardPenalty": 120000,
    "enablePaybackTime": false,
    "paybackTimeStrategy": "DETERRENT_FIXED",
    "paybackTimeMin": 0,
    "paybackTimeMax": 0,
    "paybackTime": 0
  },
  "defaults": {
    "longPressDuration": 3000,
    "extButtonSignalDuration": 500,
    "testModeDuration": 10000,
    "keepAliveInterval": 30000,
    "wifiMaxRetries": 3,
    "armedTimeout": 60000
  }
}
```

**Error Responses:**
- `503` - System busy

---

#### GET /log

Returns the device's internal log buffer as plain text.

**Response:** `text/plain`

Each log entry is on a separate line.

---

#### GET /reward

Returns the reward code history. Only available when device is not in an active session or penalty state.

**Response:** `application/json`

```json
[
  {
    "code": "ABC123",
    "checksum": "XYZ789"
  },
  {
    "code": "DEF456",
    "checksum": "UVW012"
  }
]
```

**Error Responses:**
- `403` - Rewards are hidden during active session or penalty
- `503` - System busy

---

### Configuration

#### POST /update-wifi

Updates WiFi credentials. Only allowed when device is in `READY` state. Requires reboot to apply changes.

**Request Body:** `application/json`

```json
{
  "ssid": "NewNetwork",
  "pass": "newpassword123"
}
```

**Response:**
```json
{
  "status": "saved",
  "message": "Reboot to apply."
}
```

**Error Responses:**
- `400` - Invalid JSON or credentials
- `403` - Update denied, device is active
- `503` - System busy

---

## Error Handling

All error responses follow this format:

```json
{
  "status": "error",
  "message": "Description of the error"
}
```

### Common HTTP Status Codes

- `200` - Success
- `400` - Bad Request (invalid JSON or parameters)
- `403` - Forbidden (operation not allowed in current state)
- `409` - Conflict (state conflict)
- `503` - Service Unavailable (system busy, try again)

---

## State Machine

The device operates as a finite state machine with the following states:

### READY
Initial state. Device is idle and ready to accept a new session.

**Allowed Operations:**
- Arm session (`/arm`)
- Start test (`/start-test`)
- Update WiFi (`/update-wifi`)
- Factory reset (`/factory-reset`)
- Reboot (`/reboot`)

### ARMED
Session is configured and waiting for trigger.

**Allowed Operations:**
- Abort (`/abort`)
- Keep alive (`/keepalive`)

### LOCKED
Session is active, device is locked.

**Allowed Operations:**
- Abort (`/abort`)
- Keep alive (`/keepalive`)

### TESTING
Test mode is active.

**Allowed Operations:**
- Abort (`/abort`)

### ABORTED
Session was aborted before completion.

**Transitions to:** `READY` (after penalty duration if applicable)

### COMPLETED
Session completed successfully.

**Allowed Operations:**
- Reboot (`/reboot`)

**Transitions to:** `READY` (automatically or after penalty)

---

## Usage Examples

### Starting a Session

```bash
# 1. Arm the device with a 5-minute fixed session
curl -X POST http://192.168.1.100/arm \
  -H "Content-Type: application/json" \
  -d '{
    "durationType": "DUR_FIXED",
    "durationFixed": 300000,
    "durationMin": 0,
    "durationMax": 0,
    "triggerStrategy": "STRAT_AUTO_COUNTDOWN",
    "channelDelays": [0, 0, 0, 0],
    "hideTimer": false,
    "disableLED": false
  }'

# 2. Monitor status
curl http://192.168.1.100/status

# 3. Send keep-alive during session
curl -X POST http://192.168.1.100/keepalive
```

### Getting Device Information

```bash
# Get detailed device info
curl http://192.168.1.100/details

# Check health
curl http://192.168.1.100/health

# View logs
curl http://192.168.1.100/log
```

### Emergency Abort

```bash
curl -X POST http://192.168.1.100/abort
```

---

## Notes

- All duration values are in seconds, unless specified otherwise
- The device uses a mutex lock system for thread safety - operations may return `503` if another operation is in progress
- Keep-alive must be sent at regular intervals during active sessions to prevent automatic abort
- Reward codes are only accessible when no session is active
- WiFi changes require a reboot to take effect