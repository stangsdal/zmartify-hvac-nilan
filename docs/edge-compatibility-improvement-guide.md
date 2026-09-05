# Nilan Edge Compatibility Improvement Guide

This document defines the next engineering steps for bringing the Nilan
Comfort 302 / CTS602 gateway to the same operational level as the AHC9000
controller and keeping it compatible with `zmartify-edge`.

The current firmware is a commissioning scaffold. Preserve its working
read-back checks, inactive-slot OTA flow, boot validation and rollback while
completing the production surfaces below.

## 1. HTTP API

### Stable device contract

Keep these endpoints stable and documented:

- `GET /health`
- `GET /status`
- `GET /version`
- `GET /identity`
- `GET /onboarding/status`
- `GET /api/v1/nilan/state`
- `GET /api/v1/nilan/raw` for diagnostics only
- `POST /onboarding/configure`
- `POST /api/v1/nilan/ventilation`
- `POST /api/v1/nilan/inlet-speed`
- `POST /api/v1/nilan/exhaust-speed`
- `POST /ota`
- `POST /reboot`

Read endpoints must remain safe during controller loss. Return a successful
HTTP response with explicit stale/offline metadata when the last valid CTS602
snapshot is available; do not replace old values with zeroes or claim that the
controller is healthy merely because Wi-Fi is up.

`/health` should distinguish at least:

- network and HTTP readiness;
- RS485 transport readiness;
- CTS602 detected/online state;
- MQTT connection state;
- last successful poll age and poll error counters;
- OTA verification or rollback state.

`/status` should expose counters and timestamps useful for live diagnosis:
poll successes/errors, read-back failures, command successes/errors, MQTT
publish/receive errors, current firmware identity, free heap, and the age of
the cached state.

### Authentication and transport

- Require the provisioned device-admin bearer token for all mutating
  endpoints after claiming, including OTA, reboot, onboarding changes and
  controller writes.
- Keep development gates, but make production builds fail closed when an
  unauthenticated development route is enabled.
- Do not return MQTT passwords, bearer tokens, private keys or full NVS
  contents from any endpoint or log line.
- Bound request bodies, reject malformed JSON, validate numeric ranges and
  reject duplicate or unsupported fields.
- Return consistent JSON errors with an error code, human-readable detail and
  a request/correlation identifier.
- Make reboot asynchronous and return an acknowledgement before restarting.
- Use a single write mutex so HTTP and MQTT cannot issue concurrent CTS602
  writes.

### State and cache semantics

Every decoded state response should include:

- `source_timestamp` or device uptime timestamp;
- `last_successful_poll_at` and `freshness_age_ms`;
- `online` / `stale` status;
- firmware version and device ID where useful;
- the raw or decoded value only when it was actually read.

After a successful Modbus write and read-back, update the in-memory twin
before returning the HTTP response. Rebuild the aggregate state from the
updated channel/register cache and notify the state publisher immediately.
Do not wait for the next scheduled poll to expose the new value.

## 2. OTA and reboot

### Push OTA (`POST /ota`)

Implement the following guardrails:

- Authenticate the request and require a complete image upload.
- Enforce a maximum image size that fits the inactive app partition.
- Stream the upload to the inactive partition; do not buffer the whole image
  on a task stack or in a large temporary heap allocation.
- Validate image header, project identity, chip target, version policy and
  SHA-256 before selecting the new boot partition.
- Reject same-version or downgrade images unless an explicit recovery policy
  permits them.
- Report upload bytes, image version, SHA-256 and verification state without
  exposing credentials.
- Never erase the known-good partition before the new image boots.

### Boot validation and rollback

- Mark the new image valid only after Wi-Fi, HTTP, RS485 and the basic CTS602
  poll have initialized successfully.
- Keep the image pending while required startup checks are incomplete.
- On a failed boot, allow ESP-IDF rollback to the previous valid image.
- Expose `ota_state`, `running_partition`, `last_update_version`,
  `last_update_result` and rollback reason through `/health`, `/status` or
  `/version`.
- Publish a retained startup/recovery state after every boot so Edge can
  distinguish a normal restart, an OTA apply, and a rollback.

### Pull OTA from Edge

Keep the existing Edge-staged flow and make it production-grade:

1. Device receives or periodically checks an OTA manifest.
2. Manifest contains device ID, target version, image URL, image size,
   SHA-256, compatible hardware/project identity and expiry.
3. Device authenticates the download with its device-admin credential or a
   short-lived signed download token.
4. Device downloads to the inactive partition with bounded retries and
   resumable range support where practical.
5. Device verifies the complete image before switching boot partitions.
6. Device reboots, validates the new image, and reports the result to Edge.

The MQTT trigger must remain compatible with:

`homie/5/<device-id>/gateway/ota-check/set`

The command must be idempotent. Repeated triggers for the same manifest must
not start parallel downloads or repeatedly reboot the device.

## 3. MQTT v2 compatibility

Use the v2 topic style and JSON contracts already consumed by `zmartify-edge`:

- commands: `zmartify/v2/devices/<device-id>/commands/...`;
- retained state: `zmartify/v2/devices/<device-id>/state/reported`;
- command outcomes: `zmartify/v2/devices/<device-id>/events/.../outcome`;
- OTA trigger compatibility topic documented above.

### Reported state

Publish a retained, valid JSON snapshot after connection, after every
successful state refresh, after a confirmed write, and after a meaningful
online/offline transition. The envelope must include:

- `schema_version: "2.0"`;
- `device_id`;
- `firmware_version`;
- `source_timestamp`;
- `online` and `mqtt_connected`;
- `last_error` when applicable;
- Nilan ventilation level, temperatures, alarms and availability metadata;
- poll freshness and controller health metadata.

Do not publish an empty early snapshot that overwrites a valid retained state.
Use adequate static or heap storage for the complete payload and publish from
an asynchronous task rather than a timing-sensitive Modbus or MQTT callback.

### Commands and outcomes

Every mutating MQTT command must:

- validate `schema_version`, `command_id`, `command_type`, target and numeric
  range;
- serialize through the same write mutex as HTTP;
- perform the Modbus write and mandatory read-back;
- update the twin cache before publishing the outcome;
- publish exactly one outcome for the command ID, with `accepted`, `confirmed`
  or `rejected`/`failed` semantics;
- include requested value, confirmed read-back value, source timestamp,
  detail and a bounded error code.

`confirmed` means the controller register read-back succeeded. It must not
wait for the next background poll. If an effective controller value is
asynchronous or controlled by another mode, report that separately as
`confirmation_pending` while preserving the confirmed register value.

The device must deduplicate repeated command IDs and retain enough recent
outcome history to answer a retry without executing the write twice.

## 4. Reliability and observability

- Use bounded Modbus transaction timeouts and exponential/backoff retries.
- Keep general polling, fast freshness/change polling and immediate writes in
  separate scheduling lanes.
- Reset watchdogs during long OTA uploads and multi-register snapshots.
- Avoid large automatic variables on FreeRTOS task stacks.
- Rate-limit repeated error logs and never log credentials or full command
  payloads containing secrets.
- Include device ID, command ID and firmware version in diagnostic logs.
- Preserve a small ring buffer of recent write traces: operation, register,
  requested value, echoed/read-back value, duration and result.
- Treat controller-offline, stale-data and empty-data as separate states in
  both HTTP and MQTT.

## 5. Edge integration acceptance tests

Before production exposure, verify the complete path against a local or
staging `zmartify-edge` instance:

1. Claim the device and confirm its public device ID is stable across reboot.
2. Confirm Edge receives retained `state/reported` JSON with schema version
   `2.0` and does not reject the Nilan-specific state block.
3. Send a ventilation command through HTTP and MQTT with the same command ID.
4. Confirm Modbus write, register read-back, cache update, MQTT outcome and
   Edge persistence all agree on the value.
5. Confirm the active WebSocket receives the updated state immediately after
   the outcome; do not depend on a periodic REST refresh.
6. Disconnect RS485 and verify stale/offline state without fabricated values.
7. Disconnect MQTT and verify local HTTP remains diagnosable and reconnect
   republishes the latest retained state.
8. Stage OTA, verify manifest/hash/device compatibility, reboot, validate the
   new image, and confirm Edge receives the post-boot result.
9. Exercise failed image verification and rollback, then verify the previous
   image remains operational and the rollback reason is visible.
10. Run a 72-hour soak test with poll, MQTT reconnect, write and heap counters
    recorded.

The implementation is complete only when the device, MQTT contract, Edge
ingest, persisted twin and mobile WebSocket all show the same confirmed state.