# LD2450 UART Raw HEX Debug Patch

## Modified file

- `ESPS3/components/radar_ld2450/radar_service.c`

## Insert location

`radar_rx_task()` now calls `log_raw_rx_hex()` after a successful
`ld2450_uart_read()` and before `parser_feed()`. The log therefore shows the
exact byte sequence received by the S3 UART before any parser resynchronization
or frame validation occurs.

## Log behavior

- `RADAR_ENABLE_RAW_HEX_LOG` defaults to `0`; enable it only for UART capture.
- Each enabled log starts with the actual received length, a millisecond
  timestamp, and the number of dumped bytes. Following lines contain lowercase
  hexadecimal bytes separated by spaces.
- One event prints at most 256 bytes. A larger read reports `(truncated)` while
  preserving the actual receive length.
- Logs are limited to one raw dump per second to avoid saturating the console.

Example:

```text
I (...) radar_ld2450: radar_raw_rx len=64 timestamp_ms=123456 dump_len=64
I (...) radar_ld2450: f4 f3 f2 f1 5c 00 0a 00 ...
I (...) radar_ld2450: f8 f7 f6 f5
```

## Enable HEX logging

Set the local compile-time macro at the top of
`ESPS3/components/radar_ld2450/radar_service.c` to `1`:

```c
#define RADAR_ENABLE_RAW_HEX_LOG 1
```

Alternatively, define `RADAR_ENABLE_RAW_HEX_LOG=1` in the build flags. The
parser, UART settings, target logic, tracker, zones, and spatial state are not
changed by this patch.
