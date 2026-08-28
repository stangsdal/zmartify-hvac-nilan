# Zmartify HVAC Nilan

ESP-IDF firmware foundation for a Nilan Comfort 302 / CTS602 gateway.

## Current implementation

- ESP32-S3R8 ESP-IDF project with 16 MB flash and 8 MB octal PSRAM.
- CTS602 Modbus RTU CRC-16 and request/response framing.
- Protocol offsets and guarded MVP write targets.
- Typed ventilation and temperature state decoder.
- Reuses the AHC9000 checkout's `board` and `comm_rs485` components through the
  configurable `NILAN_AHC9000_PLATFORM_DIR` CMake path.
- Starts the shared RS485 transport with CTS602 settings (`19200 8E1`, default
  slave 30) and a polling adapter. Physical writes are limited to the
  development-gated commissioning endpoints described below.
- Host tests for protocol and validation logic.
- When MQTT connects, the device publishes basic Homie v5 `$description` and
  `$state` lifecycle topics in addition to the state payload.

## Development endpoints

The local HTTP API is read-only except for explicitly development-gated
commissioning endpoints:

- `GET /health` - liveness and CTS602 controller health.
- `GET /status` - poll counters, freshness, heap and transport status.
- `GET /version` - firmware, ESP-IDF, build and ELF identity metadata.
- `GET /api/v1/nilan/state` - decoded CTS602 state and poll counters.
- `GET /api/v1/nilan/raw` - development-only raw read-only CTS602 register words.
- `GET /identity` and `GET /claim-token` - Edge discovery and claim bootstrap.
- `GET /onboarding/status` and `POST /onboarding/configure` - onboarding state
  and credential provisioning; configuration currently takes effect after a
  reboot.
- `POST /api/v1/nilan/ventilation` - development-only global ventilation step write with read-back.
- `POST /api/v1/nilan/inlet-speed` - development-only temporary inlet output override (0-100%) with read-back.
- `POST /api/v1/nilan/exhaust-speed` - development-only temporary exhaust output override (0-100%) with read-back.
- `POST /ota` - development-only firmware upload to the inactive OTA slot.
- `POST /reboot` - development-only delayed reboot after an OTA upload.

Onboarding provisions the Edge URL, MQTT credentials and device-admin token in
NVS. After claiming, write, OTA and reboot endpoints require
`Authorization: Bearer <device-admin-token>`. MQTT v2 command subscriptions are
available for ventilation, inlet speed and exhaust speed when
`CONFIG_NILAN_ENABLE_MQTT_COMMANDS=y`; each command publishes a read-back-based
outcome. Edge-staged pull OTA is implemented: after onboarding, the device
polls Edge periodically and can also be triggered through
`homie/5/<device-id>/gateway/ota-check/set`. Downloads require the provisioned
device-admin bearer token and are SHA-256 verified before the OTA partition is
selected.

The read endpoints do not expose MQTT credentials. OTA and reboot are enabled
only for local development through `CONFIG_NILAN_ENABLE_DEV_OTA`; they must be
disabled before production exposure because this commissioning route is not
authenticated or transport-protected.
Raw diagnostics are similarly gated by `CONFIG_NILAN_ENABLE_DEV_DIAGNOSTICS`.
Sensor-absent room-temperature and CO₂ values are returned as `null` while the
raw register words remain available through the diagnostic endpoint.
Development writes are gated by `CONFIG_NILAN_ENABLE_DEV_WRITES`; inlet speed
uses CTS602 holding register `H:201` and exhaust speed uses `H:200`. A `0%`
request releases either override. On the current Comfort 302 installation,
H:201 has confirmed read-back, while an H:200 request for 70% returned 30% and
was reported as failed; independent exhaust control must therefore not be
assumed.

### Development OTA

```sh
curl -sS --data-binary @build/zmartify_hvac_nilan.bin \\
  http://<device-ip>/ota
curl -sS -X POST http://<device-ip>/reboot
curl -sS http://<device-ip>/version
curl -sS http://<device-ip>/health
```

The OTA upload writes the inactive app partition and switches the boot target.
On the next boot, the application marks the image valid after startup
initialization; a failed boot remains pending verification and is eligible for
ESP-IDF rollback.

The current firmware is intentionally a commissioning scaffold. Network
provisioning, authenticated API, MQTT, OTA and production Edge onboarding remain
staged commissioning work; the shared platform is available as an external
component source.

## Build and test

```sh
source /Users/peter/.espressif/v6.0.1/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
sh tests/run_host_tests.sh
```

Do not flash to a Nilan installation before completing
[`docs/hardware-bring-up.md`](docs/hardware-bring-up.md).
