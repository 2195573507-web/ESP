# ESPS3 Radar UART Timeout and Recovery Design

## Scope

Change only the ESPS3 `radar_ld2450` UART receive path. Preserve the LD2450 protocol, 30-byte frame layout, GPIO configuration, `radar_domain`, occupancy, and tracker behavior.

## Behavior

- A normal UART read timeout keeps parser partial data and records a keep diagnostic.
- A partial buffer is force-reset only after at least 2000 ms without parser-buffer change, or during explicit UART recovery.
- Recovery is triggered by persistent driver errors, UART overflow/buffer-full events, or prolonged absence of all RX bytes. A short no-valid-frame interval is not sufficient.
- Recovery records parser partial length, discard count, and last RX timestamp, then stops reads, flushes the hardware FIFO, rebuilds the UART, and explicitly resets the parser.
- Raw UART logging is behind `RADAR_UART_RAW_DEBUG` and is disabled by default.

## Diagnostics and verification

Startup emits UART configuration. Periodic RX diagnostics include bytes, partial length, valid frames, discarded bytes, and resync count. Host tests cover timeout retention, aged partial reset, and recovery gating. Validation is host tests, `idf.py -C ESPS3 build`, and `git diff --check -- ESPS3`.
