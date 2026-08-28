# CTS602 hardware bring-up

The firmware now reuses the AHC9000 board profile for this identical hardware
platform. The profile currently contains TX=17, RX=18 and DE/RE=21, but the
AHC9000 hardware notes still mark schematic and polarity verification as open.
Treat these as inherited commissioning values, not as independently verified
Nilan release evidence.

The board uses an ESP32-S3R8: 16 MB external SPI flash and 8 MB octal PSRAM.
The ESP-IDF defaults select the full 16 MB flash address space and enable
octal PSRAM with automatic PSRAM type detection. The flash size and PSRAM
configuration must remain aligned in both the Nilan and shared AHC9000
projects.

1. Verify the board schematic: UART pins, DE/RE polarity, isolation and
   termination.
2. Connect only CTS602 CN7 A, B and GND. Never connect CN7 12 V to ESP32.
3. Configure 19200 baud, 8 data bits, even parity, 1 stop bit and slave 30.
4. Start with function 04 reads only. Capture raw frames and verify CRC.
5. Enable writes only after commissioning explicitly changes the write gate.

Before disconnecting USB, the development OTA path can be smoke-tested over
Wi-Fi with `POST /ota` followed by `POST /reboot`. Confirm `/version` and
`/health` after reboot. This route is unauthenticated and must be disabled
with `CONFIG_NILAN_ENABLE_DEV_OTA=n` before production exposure.

The current binary is a safe boot scaffold: it reports the missing board
profile and performs no bus I/O.
