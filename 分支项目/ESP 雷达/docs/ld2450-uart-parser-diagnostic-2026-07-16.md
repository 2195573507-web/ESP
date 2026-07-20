# HLK-LD2450 UART parser diagnostic report

Date: 2026-07-16

Scope: `ESPS3/components/radar_ld2450` only. No ESP-server file, existing CSI
file, parser rule, presence state machine, or business protocol was changed.

## Observed diagnostic

The supplied S3 local-source summary was:

```text
uart_online=1
parse_errors=1989
sequence_rejects=0
identity_mismatches=0
```

`uart_online=1` means `radar_service` has received at least one non-empty
return from `uart_read_bytes()`. It is not evidence that a valid LD2450 frame
was received. The `radar_diag` value `parse_errors` is not a UART hardware
error count. It is calculated as:

```text
parser.invalid_tail_frames + parser.discarded_bytes
```

It therefore combines a failed `55 CC` tail check with every byte discarded
while searching for `AA FF 03 00`. It does not include `partial_timeouts`,
UART driver failures, or RX FIFO/ring overflows.

## Actual UART receive format

No raw UART byte capture was provided with the supplied four-line summary, and
the previous source only logged byte counts and cumulative parser counters.
The actual on-wire format cannot be inferred honestly from `uart_online=1` and
`parse_errors=1989` alone.

Diagnostic logging was added only in
`ESPS3/components/radar_ld2450/radar_service.c`. At most once per second after
a non-empty `uart_read_bytes()` result it now emits:

```text
RX raw bytes=<read_len> published=<n> valid_frames=<total>
       invalid_tail=<total> (+<this-read>) discarded=<total> (+<this-read>)
<ESP_LOG_BUFFER_HEXDUMP output for exactly the returned bytes>
```

The hex dump is the actual data returned to the S3 UART task, not a synthetic
parser frame. A read may contain a partial frame or more than one frame, so the
dump must be joined with adjacent dumps before concluding that a 30-byte frame
is absent. The `+` values identify whether that sampled read caused a tail
failure or resynchronization byte discard.

## Parser expectation

The parser is a streaming, fixed-length parser. It retains a partial frame
across reads, searches one byte at a time for this exact sequence, and only
publishes after the full 30-byte frame validates:

```text
AA FF 03 00 | target_1 (8) | target_2 (8) | target_3 (8) | 55 CC
```

Expected length is `4 + 3 * 8 + 2 = 30` bytes. Any non-zero target slot has
four little-endian 16-bit fields: X, Y, speed, and resolution. A zeroed slot
is accepted as an unused target. When the first four bytes do not match, the
parser discards only one byte and retries; when byte 28/29 is not `55 CC`, it
increments `invalid_tail_frames`, discards one byte, and retries.

## LD2450 protocol match

Static source check: PASS.

The repository's LD2450 V1.03 protocol material specifies `256000, 8N1`,
little-endian order, `AA FF 03 00`, three 8-byte target slots, `55 CC`, and a
30-byte reporting frame. The S3 parser uses the same header, tail, field
layout, fixed length, and streaming resynchronization strategy. Its unit test
also decodes the documented sample target `0E 03 B1 86 10 00 40 01` as
X=-782 mm, Y=1713 mm, speed=-16 cm/s, resolution=320 mm.

This verifies code-to-protocol compatibility, not the format of the bytes now
arriving on the physical S3 RX pin.

## Configuration and frame checks

| Check | Result | Evidence |
| --- | --- | --- |
| UART controller | Configured | `UART1` |
| GPIO direction | Configured | MCU TX `GPIO18` to radar RX; MCU RX `GPIO17` from radar TX |
| UART format | PASS (static) | `256000, 8N1`, no parity, no flow control |
| Baud rate | PASS (static), runtime unproven | Code matches the documented LD2450 default `256000`; module may have been reconfigured persistently |
| Header | PASS (static), runtime unproven | Parser requires `AA FF 03 00`; new raw dump will prove or disprove it |
| Tail | PASS (static), runtime failure suspected | Parser requires `55 CC`; `parse_errors` includes `invalid_tail_frames`, but the aggregate does not reveal its portion |
| Length | PASS (static), runtime unproven | Parser waits for exactly 30 accumulated bytes before tail validation |
| Driver receive path | PARTIAL | `uart_online=1` proves non-empty reads; it does not prove baud, pin direction, electrical level, ground, or framing correctness |

## Likely failure class

The parser implementation matches the documented LD2450 reporting protocol,
so the aggregate failure is more likely upstream of business logic. The raw
diagnostic will separate these cases without changing parser behavior:

| Raw dump pattern | Interpretation |
| --- | --- |
| Repeating 30-byte blocks with `AA FF 03 00` and `55 CC` | UART and protocol are correct; investigate whether the sampled counter predates the valid stream |
| Readable/repeating bytes but no `AA FF 03 00` | Wrong UART source, module in a non-report/config mode, wrong radar model, or persisted non-default baud rate |
| Header appears but byte 28/29 is not `55 CC` | Frame corruption, protocol/model mismatch, or wrong frame length assumption; inspect consecutive dumps before changing parser |
| Random high-entropy or unstable bytes | Baud mismatch, GPIO routing/cross-over issue, ground/level/power integrity issue, or an electrically noisy input |
| Empty reads only | Wiring/power/module reporting issue; this is not the current symptom because `uart_online=1` |

## Required runtime evidence

Build and flash this diagnostic-only S3 change, then collect at least five
consecutive `radar_ld2450` `RX raw` records and their hex dump lines. Record
the boot-time UART configuration line as well. No runtime conclusion should be
made from the aggregate `radar_diag` line alone.

No device was flashed or hardware-tested during this diagnosis.
