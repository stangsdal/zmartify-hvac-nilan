# Zmartify HVAC Nilan

ESP-IDF firmware foundation for a Nilan Comfort 302 / CTS602 gateway.

## Current implementation

- ESP32-S3 ESP-IDF project.
- CTS602 Modbus RTU CRC-16 and request/response framing.
- Protocol offsets and guarded MVP write targets.
- Typed ventilation and temperature state decoder.
- Reuses the AHC9000 checkout's `board` and `comm_rs485` components through the
  configurable `NILAN_AHC9000_PLATFORM_DIR` CMake path.
- Starts the shared RS485 transport with CTS602 settings (`19200 8E1`, default
  slave 30); Nilan polling and writes are still disabled pending the adapter
  transaction task and hardware commissioning.
- Host tests for protocol and validation logic.

The current firmware is intentionally a commissioning scaffold. It does not yet
start Nilan polling, network provisioning, authenticated API, MQTT, OTA or Edge
onboarding yet; the shared platform is now available as an external component
source and will be integrated incrementally.

## Build and test

```sh
source /Users/peter/.espressif/v6.0.1/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
sh tests/run_host_tests.sh
```

Do not flash to a Nilan installation before completing
[`docs/hardware-bring-up.md`](docs/hardware-bring-up.md).
