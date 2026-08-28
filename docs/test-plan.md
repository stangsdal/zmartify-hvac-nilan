# Initial test plan

## Host/unit tests

- Modbus CRC-16 and request encoding against captured protocol examples.
- P0 poll cycle reads control, ventilation, temperature and alarm groups.
- Reject wrong slave, function, byte count and CRC.
- Decode signed scale-100 temperatures and preserve unavailable values.
- Reject out-of-range run, mode, ventilation and setpoint commands.
- Confirm input telemetry registers are never emitted as write targets.
- Verify `/api/v1/nilan/raw` against captured CTS602 words and classify absent
  room-temperature/CO2 sensors as unavailable (`null` in the state API).

## Hardware gates

- Development OTA upload to the inactive slot, reboot and `/version` check.
- OTA boot validation and rollback after a deliberately invalid image.
- Read-only poll stability and bounded retry/timeout counters.
- One write at a time with mandatory read-back confirmation.
- Verify `H:201` inlet-speed override independently of global level `H:1003`,
  including release with `0%` and restoration of normal operation.
- Exercise `H:200` exhaust-speed override with mandatory read-back. The current
  target returned 30% for a 70% request, so independent exhaust control is not
  accepted until a supported controller configuration or alternative function
  is identified. Verify release with `0%` and restoration of normal operation.
- Cable removal, malformed commands, reboot and broker loss.
- Physical panel coexistence and CTS602 safety overrides.
- 72-hour read-only run and OTA rollback on the target board.
