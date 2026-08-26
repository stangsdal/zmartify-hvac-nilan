# CTS602 hardware bring-up

The firmware now reuses the AHC9000 board profile for this identical hardware
platform. The profile currently contains TX=17, RX=18 and DE/RE=21, but the
AHC9000 hardware notes still mark schematic and polarity verification as open.
Treat these as inherited commissioning values, not as independently verified
Nilan release evidence.

1. Verify the board schematic: UART pins, DE/RE polarity, isolation and
   termination.
2. Connect only CTS602 CN7 A, B and GND. Never connect CN7 12 V to ESP32.
3. Configure 19200 baud, 8 data bits, even parity, 1 stop bit and slave 30.
4. Start with function 04 reads only. Capture raw frames and verify CRC.
5. Enable writes only after commissioning explicitly changes the write gate.

The current binary is a safe boot scaffold: it reports the missing board
profile and performs no bus I/O.
