> [!IMPORTANT]
> **Unreleased Beta**
> This software is currently in an unreleased beta state. Features are subject to change and stability is not guaranteed.

# Lobster Lock - Firmware

This is the PlatformIO-based firmware for the Lobster Lock ESP32 device.

This firmware is responsible for:
* Managing the complete session state machine (ready, countdown, locked, aborted, etc.).
* Providing a JSON API over Wi-Fi for the main application.
* Persisting all session state and counters to NVS (flash) to survive reboots.
* Handling two-stage setup:
    1.  **BLE Provisioning:** On first boot, it creates a BLE service to receive Wi-Fi credentials and initial settings.
    2.  **mDNS Discovery:** Once provisioned, it connects to Wi-Fi and announces itself on the network as `lobster-lock.local`.
* Controlling hardware: MOSFET channels, status LED, and an optional abort pedal.

## Technical Overview

* **Platform:** [PlatformIO](https://platformio.org/)
* **Framework:** Arduino
* **Target:** ESP32
* **Key Libraries:**
    * `ESPAsyncWebServer`: For the non-blocking web server and API.
    * `ArduinoJson`: For all JSON serialization/deserialization.
    * `Preferences`: For saving state to NVS.
    * `BLEDevice`: For the initial provisioning service.
    * `OneButton`: For debouncing and handling the abort pedal.
    * `JLed`: For creating non-blocking LED patterns.

## Building and Uploading

This project is managed with PlatformIO. Ensure you have the [PlatformIO Core CLI](https://platformio.org/install/cli) or the [VSCode Extension](https://platformio.org/platformio-ide) installed.

### Environments

The `platformio.ini` file defines two primary environments:

* `esp32_diymore_debug`: A development build that includes the `DEBUG_MODE` flag.
* `esp32_diymore_prod`: A production build for release.

### Common Commands

**Build:**
```bash
# Build the debug environment
pio run -e esp32_diymore_debug

# Build the production environment
pio run -e esp32_diymore_prod
````

**Upload:**

```bash
# Build and upload the debug environment
pio run -t upload -e esp32_diymore_debug
```

**Monitor:**

```bash
# Connect to the device's serial monitor
pio run -t monitor -e esp32_diymore_debug
```

**Upload & Monitor:**

```bash
# A convenient shortcut to upload and then immediately open the monitor
pio run -t upload -t monitor -e esp32_diymore_debug
```