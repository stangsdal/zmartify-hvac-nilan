# Design specification: ESP32 controller for Nilan Comfort 302 / CTS602

Status: commissioning-validated draft
Date: 2026-08-26  
Target repository: `stangsdal/zmartify-hvac-nilan`

## 1. Purpose and scope

This document specifies an ESP32-based gateway that controls and monitors a Nilan Comfort 302 ventilation unit through its CTS602 Modbus RTU interface and exposes the unit as a first-class HVAC product in `zmartify-edge`.

The controller runs side-by-side with `zmartify-hvac-ahc9000`, sharing proven ESP-IDF, transport, MQTT, onboarding, OTA and safety code while keeping the Nilan protocol adapter isolated. It sends user-level commands and reads status; it does not replace Nilan's internal frost, airflow, heater, fire or sensor protection.

In scope: ESP32-S3 firmware architecture, RS485/CTS602 integration, telemetry, guarded writes, Edge onboarding/MQTT/OTA, shared-code boundary and UI integration. Out of scope: direct switching of fans/heaters/compressors, automatic installer-setting changes and unvalidated support for every CTS602 variant.

## 2. Evidence and assumptions

The supplied CTS602 protocol document states:

- RS485 Modbus is connected to CN7 pins 2, 3 and 6: A, B and ground. Pins 4 and 5 are the separate user-panel bus and must not be used.
- CTS602 is a Modbus slave; default node address is 30 and the selectable range is 1-247.
- Transport is fixed at 19,200 baud, 8 data bits, even parity and 1 stop bit (8E1).
- Supported functions are 03 Read Holding Registers, 04 Read Input Registers and 16 Preset Multiple Registers. Registers are 16-bit.
- Input addresses in the document are protocol offsets; input 100 is global 30101 and holding 100 is global 40101.

The Comfort 252/302 installation manual identifies the machine as heat-recovery ventilation with supply/extract fans, bypass damper, counter-flow heat exchanger, filters and optional pre-/after-heating. It warns that prolonged stoppage can cause condensation problems. The newer CTS602 Light HMI is used as UX inspiration: a comfort-first main screen, clear alarm handling, a separate data view and protected installer/service access.

Assumption requiring bench validation: exact electrical pinout, direction-control polarity, isolation and termination of the selected ESP32-S3-RS485-CAN board. No guessed pin may be enabled in a release build.

## 3. Architecture options

### Recommended: shared gateway platform plus `nilan_cts602` adapter

Create this as a separate ESP-IDF product repository based on the AHC9000 platform boundary. Reuse generic components and contracts; implement only the Nilan adapter, register map, capability profile and semantic mapping here.

```text
zmartify-edge app/API -> MQTT v2 + Homie v5 -> ESP32 gateway platform
                                        net/storage/OTA/API/MQTT/health
                                                     -> hvac_core
                                                     -> nilan_cts602
                                                     -> comm_rs485 (19200 8E1)
                                                     -> Nilan CTS602
```

### Alternative A: common firmware library

Move generic components into a versioned shared repository used by both products. Cleanest long term, but adds release coordination before the first Nilan field test.

### Alternative B: product fork

Fork AHC9000 and replace its protocol module. Fastest prototype, but security/MQTT/OTA fixes can diverge. Use only as a short-lived prototype branch.

## 4. Repository structure

```text
zmartify-hvac-nilan/
  main/
  components/
    board/ comm_rs485/ hvac_core/ protocol/
    net/ storage/ ota/ security/ health/ api/ hvac_mqtt/
    nilan_cts602/
  docs/
    design-specification-esp32-cts602.md
    design-specification-zmartify-edge-nilan-ui.md
    cts602-register-map.md  hardware-bring-up.md  test-plan.md
```

The reuse boundary must be interface-based. `hvac_core` must not know Modbus addresses or Nilan register names.

## Shared code strategy for future ESP32 controllers

The long-term target should be a small, versioned ESP32 gateway platform with thin product adapters. Sharing should be based on stable interfaces and tests, not on copying complete product repositories.

### What belongs in the shared platform

These components should normally be identical across Nilan, AHC9000 and future integrations:

- board lifecycle and verified board profiles
- UART/RS485 and CAN/TWAI transport primitives
- network provisioning, Wi-Fi, time and mDNS
- NVS configuration, schema migration and factory defaults
- local authenticated API and health model
- MQTT TLS, Homie v5 discovery, MQTT v2 envelopes and command outcomes
- Edge onboarding, device identity, capability advertisement and staged OTA
- generic HVAC command/state interfaces, freshness, fault and audit semantics
- logging, watchdog, rate limiting and test utilities

### What stays product-specific

Each integration owns its protocol and equipment knowledge:

- wire protocol and register/frame definitions
- device discovery and capability detection
- raw-to-typed decoding and typed-to-command encoding
- equipment-specific operating states and alarm catalogue
- product-specific safety constraints that are stricter than the common baseline
- mapping from the equipment model to the generic HVAC model

### Recommended repository evolution

Use a staged extraction rather than introducing a shared repository before the Nilan adapter is understood:

1. **Now:** keep `zmartify-hvac-nilan` separate and align its component interfaces with AHC9000. Add contract tests that run against both products.
2. **After two stable products:** extract only proven, protocol-neutral components into a versioned `zmartify-esp32-common` repository or ESP-IDF component registry package.
3. **Then:** keep each product repository small: product manifest, board profile, adapter, register/protocol map, product tests and release configuration.
4. **Later:** if the number of products grows substantially, move to a platform monorepo with product build targets. Do not use a monorepo merely to hide unclear ownership boundaries.

### Versioning and compatibility

- Version the shared platform independently using semantic versioning.
- Pin each product to an exact common-platform version; do not build field firmware from an unpinned branch.
- Treat changes to MQTT payloads, onboarding identity, OTA behavior, storage schema and public API as compatibility-sensitive.
- Require a compatibility matrix in CI: every supported product must compile, pass unit tests and validate its contract against the selected platform version.
- Allow product repositories to stay one platform release behind temporarily, but record the exception and its upgrade deadline.
- Use an explicit migration layer when a shared interface changes; never silently reinterpret an existing device state.

### Ownership and release gates

The common platform owns lifecycle/security defects. The product adapter owner owns protocol correctness and equipment behavior. A shared release is allowed only when:

- all product adapters compile against it;
- common unit, contract and static checks pass;
- onboarding, MQTT, local API and OTA smoke tests pass;
- at least one hardware regression test exists for every changed transport or board component;
- migration notes exist for NVS/configuration and Edge contracts;
- no product-specific code has leaked into the common layer.

### Avoiding a lowest-common-denominator platform

The common interfaces should expose capability discovery and optional features rather than forcing every controller to implement every HVAC concept. A Nilan unit should not be made to look like AHC9000 zones, and a future heat pump should not inherit ventilation assumptions. The generic model should therefore be capability-driven, with typed optional fields and explicit `unavailable`/`unsupported` states.

## 5. Nilan adapter

### Transport

The adapter uses a `comm_rs485` transaction interface and owns no UART lifecycle:

- 19,200 baud, 8E1, Modbus RTU CRC-16
- configurable slave address, default 30
- single master transaction at a time
- bounded timeout/retry and correct inter-frame timing
- counters for requests, responses, timeouts, CRC/framing errors, exceptions and retries

Failure marks data stale/degraded; it must not crash or reset the firmware.

### Poll groups

Use bounded priority polling, not a scan of the whole register space.

| Priority | Group | Function | Initial registers | Purpose |
|---|---|---:|---|---|
| P0 | Control/status | 04 | 1000-1003 | Run, mode, state, time in state |
| P0 | Ventilation | 04 | 1100-1104 | Requested/actual level and filter days |
| P0 | Temperature | 04 | 1200-1206 | Room/inlet temperature, efficiency, capacity |
| P0 | Alarm | 04 | 400-409 | Alarm state, codes, timestamps |
| P1 | Sensors | 04 | 200-222 | T0-T17, humidity, CO2 when installed |
| P1 | Display | 04 | 3000-3010 | Bypass, heater, defrost and display values |
| P2 | Configuration view | 03 | 4000-4050 | Selected user settings and week program |

The Comfort 302-supported subset and contiguous ranges must be confirmed by read-only discovery. Unsupported registers are `unavailable`, never zero.

Default cadence: P0 every 2 s; temperatures/air quality every 5 s; display/derived status every 5 s; configuration on demand and max once per 30 s; full diagnostics only by explicit service action.

### Typed model

Retain raw 16-bit values for diagnostics and expose typed values with register source and timestamp. Scale-100 values are signed hundredths of the engineering unit; status values are enums/bit masks; display registers contain two ASCII characters.

MVP model: run state, operation mode, control state, ventilation level, actual inlet/exhaust levels, room temperature and source, setpoint, T3/T4/T7/T8, humidity, CO2, bypass, defrost, fan percentages, filter days, alarm state/codes/severity and freshness.

### Guarded writes

MVP writes:

- holding 1001: run 0/1
- holding 1002: mode 0-3; service is not a user mode
- holding 1003: ventilation 0-4
- holding 1004: temperature setpoint, scale 100
- holding 1100: air-exchange mode 0-2 if capability is verified
- holding 400: explicit alarm code or 255 for all alarms

For each write: validate in Edge and firmware; serialize with polling; use function 16; re-read affected state; publish an asynchronous terminal outcome. Alarm reset confirmation means the alarm was re-read, not that the fault was repaired. Installer/service registers are never writable from the normal user path.

Commissioning validation has confirmed a temporary inlet-speed override through
holding register `H:201` (`Output.InletSpeed`), expressed as 0-100% and released
with 0%. The implementation requires a holding-register read-back. This remains
a development-only operation. The matching exhaust-side output register `H:200`
is implemented behind the same gate, but the target Comfort 302 returned 30%
when 70% was requested. The write was therefore correctly reported as failed
and the override was released again. H:200 is not a confirmed independent
exhaust-control capability on this installation.

### Asymmetric airflow and cooker-hood profile

The API must be designed now for asymmetric airflow. The CTS602 specification
documents the requested ventilation step at holding register 1003, actual
inlet/exhaust levels at input registers 1101 and 1102, and output values at
holding registers H:200/H:201. On the target Comfort 302, H:201 has now been
validated as a temporary independent inlet override with read-back. H:200 has
been exercised, but did not read back the requested value (70% requested, 30%
returned), so a complete paired asymmetric profile is not supported yet.

Expose a capability-driven command at the semantic API boundary:

```json
{
  "command_type": "hvac.set_airflow_profile",
  "target_ref": "nilan",
  "parameters": {
    "profile": "cooker_hood",
    "inlet_level": 4,
    "exhaust_level": 2,
    "duration_s": 1800,
    "restore_previous": true
  }
}
```

Use `inlet_level` for indblæsning/tilluft and `exhaust_level` for udsugning/fraluft. The command must be accepted only when the capability profile says the selected implementation is supported. A normal user-facing “Boost ventilation” action may use a named profile; a home-automation system should also be able to request explicit levels, duration and restoration behavior.

Implement the capability in three possible tiers:

1. **Direct asymmetric command:** use the validated output-register mechanism when both sides have been confirmed on the target Comfort 302 firmware. Inlet override is `confirmed`; exhaust override is currently `unsupported` on this installation pending a documented controller configuration or alternative supported function.
2. **Named Nilan user function:** activate the documented cooker-hood/user-function path through the supported registers, while reporting the configured resulting inlet/exhaust levels and actual read-back. This is the safe fallback if CTS602 owns the exact fan values.
3. **Unsupported:** report `unsupported` and do not emulate asymmetry by repeatedly writing the global ventilation step.

Do not write input registers 1101/1102; they are telemetry. Keep H:200/H:201
behind the commissioning write gate and mandatory read-back; they must not be
exposed as generic public register writes. If the exhaust-side behavior cannot
be verified, the API must report asymmetric control as partial/unsupported
rather than emulate it by repeatedly writing the global ventilation step.

The reported state should include:

```json
{
  "airflow": {
    "requested_inlet_level": 4,
    "requested_exhaust_level": 2,
    "actual_inlet_level": 4,
    "actual_exhaust_level": 2,
    "active_profile": "cooker_hood",
    "profile_remaining_s": 1742,
    "control_source": "home_automation",
    "asymmetric_control": "confirmed"
  }
}
```

Use explicit states `available`, `confirmed`, `emulated`, `unsupported`, `pending` and `stale`; never present a requested bias as active until the read-back or named-function state confirms it. The command must be rate-limited, bounded by safe levels, cancellable, and automatically restored on expiry when requested.

Commissioning must verify: (a) what CTS602 does when the physical cooker-hood input/user function is active, (b) whether the Modbus master can activate it, (c) actual inlet/exhaust response, (d) interaction with humidity, defrost and fire/frost protection, and (e) coexistence with the physical panel. The result belongs in the Nilan capability profile and the automated hardware test suite.

## 6. Shared firmware and Nilan-specific modules

Reuse the AHC9000 interfaces/components for `board`, `comm_rs485`, `hvac_core`, `protocol`, `net`, `storage`, `security`, `health`, `api`, `ota` and `hvac_mqtt`. Do not reuse AHC9000 packet logic or zone/channel assumptions.

Nilan-specific modules:

- `nilan_cts602_transport_profile`
- `nilan_cts602_register_map`
- `nilan_cts602_decoder` / `encoder`
- `nilan_cts602_adapter`
- `nilan_capabilities`

## 7. Edge contract

```json
{
  "device_id": "zmartify-hvac-nilan-aabbccddeeff",
  "product_type": "hvac",
  "product_model": "nilan-comfort-302-cts602",
  "integration_mode": "nilan_cts602_modbus",
  "capabilities": ["hvac.ventilation", "hvac.temperature", "hvac.alarms", "hvac.air_quality"]
}
```

The current adapter publishes read-only state at `homie/5/<device-id>/nilan/state`
and `zmartify/v2/devices/<device-id>/state/hvac`. It now also publishes basic
Homie v5 `$description` and `$state` lifecycle topics. It does not yet consume
command topics. It now supports bounded Nilan MQTT v2 ventilation/output
commands and read-back outcomes behind a compile-time commissioning flag. The
production adapter must
advertise capability schema 2.0, omit optional sensors until verified and use
MQTT v2 envelopes with schema version, command ID, command type, target,
parameters and timestamps.

Recommended future commands: `hvac.set_run_state`, `hvac.set_operation_mode`,
`hvac.set_ventilation_level`, `hvac.set_temperature_setpoint` and
`hvac.reset_alarm`. The current firmware increment enables only local,
development-gated commissioning writes for global ventilation and the two
output-register experiments; it does not consume production MQTT command
topics.

The reported state should contain `controller_online`, `freshness_age_ms`, run/mode/state, requested and actual ventilation, temperatures, air quality, bypass/defrost, filter and alarms. Edge should represent this as one HVAC device, not artificial zones/channels. If legacy API compatibility requires a zone, use one stable virtual zone named `Ventilation` only at the boundary.

## 8. Safety and failure behavior

- Release firmware starts read-only; the current development configuration
  explicitly enables only the bounded local commissioning writes.
- CTS602 remains authority for frost, airflow, heater, fire and sensor protection.
- Wi-Fi/MQTT loss does not stop local polling or the last valid local operation.
- On timeout, mark `controller_online=false` after a bounded threshold and retain last-known values with freshness age.
- Reject writes while unavailable, busy or expired; rate-limit and coalesce slider updates.
- The current `/api/v1/nilan/state` endpoint is a read-only commissioning
  endpoint. It must not be treated as the production authenticated API.
- Authenticate the local API in normal operation and disable commissioning/debug
  endpoints by default.
- Never log passwords, tokens, claim data or raw credential payloads.

## 9. Commissioning

1. Validate ESP32 board pins, RS485 electrical levels, isolation and termination without Nilan connected.
2. Connect only CN7 A, B and GND; never connect CN7 12 V to the ESP32.
3. Verify address 30, 8E1 transport and read-only identity/status.
4. Capture baseline status, temperatures, fan levels, display and alarms.
5. Validate one write at a time: global fan level and temporary inlet override.
   H:200 exhaust override is currently a documented failed read-back test;
   setpoint, run and alarm reset remain uncommissioned.
6. Test coexistence with the physical CTS602 panel and document the supported policy.
7. Validate MQTT state/outcomes, Edge twin freshness, onboarding, authenticated
   local operations, OTA and rollback. Edge-staged pull OTA is implemented;
   end-to-end staging still requires a staged Edge artifact.
8. Test ESP32 reboot, Wi-Fi/broker loss, Modbus cable removal and malformed commands.

## 10. Acceptance criteria

- No unverified board pin is enabled in a release build.
- 72-hour read-only run without crash, memory leak or uncontrolled bus traffic.
- Valid polls complete within timeout under normal wiring; all failures are counted.
- Every MVP write has a deterministic outcome and read-back confirmation.
- No user action writes service/installer registers.
- Edge online/offline/freshness matches device state.
- OTA rollback is verified on real hardware.
- Optional sensors never appear as zero when unavailable.

## 11. Sources

- `2016-02-16_CTS602_Modbus_protokol.pdf`: connection, transport, register layout, commands and alarms.
- `2016-08-18_M21_Comfort_252-302-Top_DK.pdf`: Comfort 302 characteristics and operating constraints.
- `S24_Comfort_GB HMI interface.pdf`: current CTS602 Light HMI and alarm concepts.
- `stangsdal/zmartify-hvac-ahc9000`: ESP-IDF, MQTT, OTA and safety baseline.
- `stangsdal/zmartify-edge`: onboarding, authorization, MQTT v2 and mobile API.
