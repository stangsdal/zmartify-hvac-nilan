# Initial test plan

## Host/unit tests

- Modbus CRC-16 and request encoding against captured protocol examples.
- P0 poll cycle reads control, ventilation, temperature and alarm groups.
- Reject wrong slave, function, byte count and CRC.
- Decode signed scale-100 temperatures and preserve unavailable values.
- Reject out-of-range run, mode, ventilation and setpoint commands.
- Confirm input telemetry registers are never emitted as write targets.

## Hardware gates

- Read-only poll stability and bounded retry/timeout counters.
- One write at a time with mandatory read-back confirmation.
- Cable removal, malformed commands, reboot and broker loss.
- Physical panel coexistence and CTS602 safety overrides.
- 72-hour read-only run and OTA rollback on the target board.
