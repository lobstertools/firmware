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

### SessionOutcome

**Values:** `SUCCESS` | `ABORTED` | `UNKNOWN`

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
  durationFixed: number;           // seconds
  durationMin: number;             // seconds
  durationMax: number;             // seconds
  triggerStrategy: TriggerStrategy;
  channelDelays: [number, number, number, number];  // seconds for each channel
  hideTimer: boolean;              // Hide remaining time
  disableLED: boolean;             // Disable status LED
}
```

### SessionTimers

Current timing information.

```typescript
{
  lockDuration: number;            // seconds - Total lock duration
  potentialDebtServed: number;     // seconds - Potential debt reduction if completed now
  penaltyDuration: number;         // seconds - Total penalty duration
  lockRemaining: number;           // seconds - Lock time remaining
  penaltyRemaining: number;        // seconds - Penalty time remaining
  testRemaining: number;           // seconds - Test mode time remaining
  triggerTimeout: number;          // seconds - Time until trigger timeout
  channelDelays: [number, number, number, number];  // seconds
}
```

### SessionStats

Session statistics.

```typescript
{
  streaks: number;                 // Current streak count
  completed: number;               // Completed sessions
  aborted: number;                 // Aborted sessions
  paybackAccumulated: number;      // seconds - Accumulated payback time
  totalLockedTime: number;         // seconds - Total time locked
}
```

### Telemetry

Real-time hardware telemetry.

```typescript
{
  buttonPressed: boolean;
  currentPressDurationMs: number;  // milliseconds - Current button press duration
  rssi: number;                    // dBm - WiFi signal strength
  freeHeap: number;                // bytes - Free memory
  uptime: number;                  // seconds - Uptime
  internalTempC: number | "N/A";   // °C - Internal temperature
}
```

### SessionPresets

Duration range presets.

```typescript
{
  shortMin: number;                // seconds
  shortMax: number;                // seconds
  mediumMin: number;               // seconds
  mediumMax: number;               // seconds
  longMin: number;                 // seconds
  longMax: number;                 // seconds
  minSessionDuration: number;      // seconds - Absolute minimum
  maxSessionDuration: number;      // seconds - Absolute maximum
}
```

### DeterrentConfig

Deterrent and penalty configuration.

```typescript
{
  enableStreaks: boolean;
  enableRewardCode: boolean;
  rewardPenaltyStrategy: DeterrentStrategy;
  rewardPenaltyMin: number;        // seconds
  rewardPenaltyMax: number;        // seconds
  rewardPenalty: number;           // seconds
  enablePaybackTime: boolean;
  paybackTimeStrategy: DeterrentStrategy;
  paybackTimeMin: number;          // seconds
  paybackTimeMax: number;          // seconds
  paybackTime: number;             // seconds
  enableTimeModification: boolean; // Allow time add/remove during session
  timeModificationStep: number;    // seconds - Amount to add/remove per request
}
```

### SystemDefaults

System-level default values.

```typescript
{
  longPressDuration: number;       // seconds
  extButtonSignalDuration: number; // seconds
  testModeDuration: number;        // seconds
  keepAliveInterval: number;       // seconds
  keepAliveMaxStrikes: number;     // count
  bootLoopThreshold: number;       // count
  stableBootTime: number;          // seconds
  wifiMaxRetries: number;          // count
  armedTimeout: number;            // seconds
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
  "durationFixed": 300,
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
- All duration values are in seconds
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

#### POST /time/add

Adds time to the current session. Only available when `enableTimeModification` is true in deterrent config.

**Response:**
```json
{
  "status": "ok"
}
```

**Error Responses:**
- `400` - Modification rejected (feature disabled or limits reached)
- `503` - System busy

**Notes:**
- Amount added is determined by `timeModificationStep` in deterrent config
- Subject to `maxSessionDuration` limit
- Only available during `LOCKED` state

---

#### POST /time/remove

Removes time from the current session. Only available when `enableTimeModification` is true in deterrent config.

**Response:**
```json
{
  "status": "ok"
}
```

**Error Responses:**
- `400` - Modification rejected (feature disabled or limits reached)
- `503` - System busy

**Notes:**
- Amount removed is determined by `timeModificationStep` in deterrent config
- Subject to `minSessionDuration` limit
- Only available during `LOCKED` state

---

### Status & Information

#### GET /status

Returns complete session status including configuration, timers, stats, and telemetry.

**Response:** `application/json`

```json
{
  "state": "LOCKED",
  "outcome": "SUCCESS",
  "verified": true,
  "config": {
    "durationType": "DUR_FIXED",
    "durationFixed": 300,
    "durationMin": 0,
    "durationMax": 0,
    "triggerStrategy": "STRAT_AUTO_COUNTDOWN",
    "hideTimer": false,
    "disableLED": false,
    "channelDelays": [0, 0, 0, 0]
  },
  "timers": {
    "lockDuration": 300,
    "potentialDebtServed": 150,
    "penaltyDuration": 0,
    "lockRemaining": 250,
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
    "totalLockedTime": 1500
  },
  "telemetry": {
    "buttonPressed": false,
    "currentPressDurationMs": 0,
    "rssi": -45,
    "freeHeap": 123456,
    "uptime": 3600,
    "internalTempC": 45.2
  }
}
```

**Error Responses:**
- `503` - System busy

**Field Details:**
- All time values are in **seconds** except `currentPressDurationMs` which is in **milliseconds**
- `verified`: Indicates if hardware is functioning correctly
- `outcome`: Only meaningful when state is `COMPLETED` or `ABORTED`
- `potentialDebtServed`: Shows how much payback time would be reduced if session completes now

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
    "shortMin": 60,
    "shortMax": 300,
    "mediumMin": 300,
    "mediumMax": 900,
    "longMin": 900,
    "longMax": 3600,
    "minSessionDuration": 30,
    "maxSessionDuration": 7200
  },
  "deterrentConfig": {
    "enableStreaks": true,
    "enableRewardCode": true,
    "rewardPenaltyStrategy": "DETERRENT_RANDOM",
    "rewardPenaltyMin": 60,
    "rewardPenaltyMax": 300,
    "rewardPenalty": 120,
    "enablePaybackTime": false,
    "paybackTimeStrategy": "DETERRENT_FIXED",
    "paybackTimeMin": 0,
    "paybackTimeMax": 0,
    "paybackTime": 0,
    "enableTimeModification": true,
    "timeModificationStep": 60
  },
  "defaults": {
    "longPressDuration": 3,
    "extButtonSignalDuration": 0.5,
    "testModeDuration": 10,
    "keepAliveInterval": 30,
    "wifiMaxRetries": 3,
    "armedTimeout": 60
  }
}
```

**Error Responses:**
- `503` - System busy

**Field Details:**
- All time values are in **seconds**
- `id`: Generated from device MAC address
- `cppStandard`: Numeric value representing C++ standard version used
- `enableTimeModification`: When true, allows `/time/add` and `/time/remove` endpoints
- `timeModificationStep`: Amount of time (in seconds) added or removed per request

---

#### GET /log

Returns the device's internal log buffer as plain text.

**Response:** `text/plain`

Each log entry is on a separate line. Returns up to 150 log entries.

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

**Notes:**
- Returns up to 10 reward codes
- Only completed sessions generate reward codes (when enabled)
- Reward codes are hidden during `ARMED`, `LOCKED`, `TESTING`, and when penalty time is active

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

**Notes:**
- Changes are saved to persistent storage but require a reboot to take effect
- SSID and password validation is performed before saving

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

**Transitions:**
- → `LOCKED` when trigger condition is met
- → `ABORTED` on abort or timeout

### LOCKED
Session is active, device is locked.

**Allowed Operations:**
- Abort (`/abort`)
- Keep alive (`/keepalive`)
- Add time (`/time/add`) - if enabled
- Remove time (`/time/remove`) - if enabled

**Transitions:**
- → `COMPLETED` when session duration expires (successful completion)
- → `ABORTED` on abort or keepalive timeout

### TESTING
Test mode is active.

**Allowed Operations:**
- Abort (`/abort`)

**Transitions:**
- → `READY` when test completes or is aborted

### ABORTED
Session was aborted before completion.

**Allowed Operations:**
- Get status (`/status`)
- Get details (`/details`)

**Transitions:**
- → `READY` after penalty duration expires (if applicable)

### COMPLETED
Session completed successfully.

**Allowed Operations:**
- Reboot (`/reboot`)
- Get status (`/status`)
- Get details (`/details`)
- Get reward (`/reward`)

**Transitions:**
- → `READY` after penalty duration expires (if applicable)

---

## Usage Examples

### Starting a Fixed Duration Session

```bash
# 1. Arm the device with a 5-minute fixed session
curl -X POST http://192.168.1.100/arm \
  -H "Content-Type: application/json" \
  -d '{
    "durationType": "DUR_FIXED",
    "durationFixed": 300,
    "durationMin": 0,
    "durationMax": 0,
    "triggerStrategy": "STRAT_AUTO_COUNTDOWN",
    "channelDelays": [0, 0, 0, 0],
    "hideTimer": false,
    "disableLED": false
  }'

# 2. Monitor status
curl http://192.168.1.100/status

# 3. Send keep-alive during session (every 30 seconds)
curl -X POST http://192.168.1.100/keepalive
```

### Starting a Random Duration Session

```bash
# Arm with random duration between 5 and 15 minutes
curl -X POST http://192.168.1.100/arm \
  -H "Content-Type: application/json" \
  -d '{
    "durationType": "DUR_RANDOM",
    "durationFixed": 0,
    "durationMin": 300,
    "durationMax": 900,
    "triggerStrategy": "STRAT_AUTO_COUNTDOWN",
    "channelDelays": [0, 0, 0, 0],
    "hideTimer": true,
    "disableLED": false
  }'
```

### Using Preset Ranges

```bash
# Use medium preset range
curl -X POST http://192.168.1.100/arm \
  -H "Content-Type: application/json" \
  -d '{
    "durationType": "DUR_RANGE_MEDIUM",
    "durationFixed": 0,
    "durationMin": 0,
    "durationMax": 0,
    "triggerStrategy": "STRAT_AUTO_COUNTDOWN",
    "channelDelays": [0, 0, 0, 0],
    "hideTimer": false,
    "disableLED": false
  }'
```

### Modifying Session Time

```bash
# Add time during an active session (if enabled)
curl -X POST http://192.168.1.100/time/add

# Remove time during an active session (if enabled)
curl -X POST http://192.168.1.100/time/remove

# Check updated remaining time
curl http://192.168.1.100/status | jq '.timers.lockRemaining'
```

### Getting Device Information

```bash
# Get detailed device info
curl http://192.168.1.100/details

# Check health
curl http://192.168.1.100/health

# View logs
curl http://192.168.1.100/log

# Get reward codes (only when not in active session)
curl http://192.168.1.100/reward
```

### Emergency Abort

```bash
curl -X POST http://192.168.1.100/abort
```

### Testing Hardware

```bash
# Start a 10-second test of all channels
curl -X POST http://192.168.1.100/start-test

# Monitor test progress
curl http://192.168.1.100/status
```

---

## Notes

- **Time Units**: All duration values are in **seconds**, except `currentPressDurationMs` in telemetry which is in **milliseconds**
- **Thread Safety**: The device uses a mutex lock system for thread safety - operations may return `503` if another operation is in progress
- **Keep-Alive**: Must be sent at regular intervals (default: every 30 seconds) during active sessions to prevent automatic abort
- **Reward Codes**: Only accessible when no session is active and no penalty time remaining
- **WiFi Changes**: Require a reboot to take effect
- **Channel Delays**: Allow staggered activation of outputs, useful for timed releases
- **Hidden Timer**: When enabled, prevents the frontend from displaying remaining time
- **Hardware Verification**: The `verified` field in status indicates if all enabled channels are functioning correctly
- **Time Modification**: The `/time/add` and `/time/remove` endpoints allow dynamic session adjustment when enabled, subject to min/max duration limits
- **Potential Debt Served**: Tracks how much accumulated payback time would be reduced if the current session is completed successfully