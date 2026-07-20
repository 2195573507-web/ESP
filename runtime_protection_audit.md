# C5 Runtime Protection Audit

Date: 2026-07-20

## Scope

This change applies symmetrically to the ESP32-C5 terminal firmware in
`ESPC51` and `ESPC52`. It does not alter PCM format, C5-to-S3 endpoints,
voice protocol, server behavior, or voice-task stack sizes.

## Enabled FreeRTOS Protection

Both active `sdkconfig` files and both `sdkconfig.defaults` files enable:

```text
CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY=y
CONFIG_FREERTOS_WATCHPOINT_END_OF_STACK=y
```

In ESP-IDF 5.5.4, `CONFIG_FREERTOS_CHECK_STACKOVERFLOW` is a Kconfig choice;
the selected `CANARY` value is Method 2 stack-overflow checking. The stack-end
watchpoint detects writes into the final 32 bytes of the active task stack
without waiting for a context switch. It consumes the final hardware watchpoint
and reduces usable stack headroom, so device measurements must be collected
again with this configuration enabled.

## Stack Monitoring

`app_stack_monitor_log()` uses `uxTaskGetStackHighWaterMark(NULL)` in the
executing task and reports the remaining high-water mark in bytes. The relevant
runtime contexts now have identifiable checkpoints:

| Requested component | Actual task/context | Checkpoints |
| --- | --- | --- |
| `voice_chain` | `voice_chain` FreeRTOS task | task entry and every terminal voice event |
| `wake_prompt_stream` | synchronous call within `voice_chain`, not a separate task | before the wake prompt and before/after prompt playback |
| `mic_adc_test` | `mic_adc_test` FreeRTOS task | task entry, state initialization, and existing periodic monitor |
| `server_voice_client` | `server_voice_rx` FreeRTOS task | response task entry and after each response process, plus existing response-phase checkpoints |

Configured task stacks are `voice_chain=4096` bytes,
`mic_adc_test=12288` bytes, and `server_voice_rx=8192` bytes. A low-water
warning is emitted below the existing 1024-byte monitor threshold. No stack
size was changed by this work.

## Heap Integrity

`heap_caps_check_integrity_all(true)` is executed only at voice lifecycle
boundaries, avoiding the Mic sampling loop and per-PCM-chunk path:

1. `wake_before`: immediately before local wake prompt handling.
2. `play_before`: immediately before the speaker stream is opened.
3. `play_after`: after stream finish/abort and prompt PCM buffer release.

Each result has a searchable runtime-protection log. A failed pre-wake check
aborts the current voice round; a failed playback check returns an error rather
than continuing with an allocator known to be inconsistent.

## Buffer-Boundary Coverage

Existing guards remain part of the protection envelope:

- Wake-prompt download rejects a spool size over 96 KiB, validates content
  metadata, and limits each playback read to a 2048-byte buffer.
- Server response processing rejects a response chunk larger than its fixed
  chunk buffer before copying; the one-byte carry buffer accounts for an odd
  PCM byte boundary.
- Voice upload growth is capped at 320 KiB before reallocation and copy.
- Mic ADC reads into a sized raw buffer before parsing samples.

The integrity scan detects allocator consistency failures. It does not provide
AddressSanitizer-style per-allocation redzones because heap poisoning remains
disabled; immediate out-of-bounds detection still depends on the listed bounds
checks, the stack watchpoint, and device execution.

## Verification

Static verification completed:

- Both C5 active configurations contain the Canary and Watchpoint settings.
- Both default configurations preserve those settings for a fresh configure.
- C51 completed an isolated `ninja ... all` build and produced `00_Learn.bin`;
  its generated configuration contains both corresponding definitions.
- C52 completed an isolated ESP-IDF build and produced `00_Learn.bin`; its
  generated configuration contains both corresponding definitions.
- `git diff --check -- ESPC51 ESPC52 runtime_protection_audit.md` passed.

Build/configuration proof does not prove runtime stack margin, heap integrity,
or buffer safety on hardware.

## Required Device Acceptance

Flash each C5 independently and exercise a full local-wake flow: wake prompt,
recording, C5-to-S3 voice request, response PCM playback, cancellation, and
reconnect. Capture all `RUNTIME_PROTECTION`/heap-integrity and stack logs. The
acceptance criteria are no Watchpoint or Canary panic, no low-water warning
below the chosen deployment threshold, no failed integrity result, and no
length/size rejection for valid PCM.

Use malformed and maximum-size prompt/response payloads only in an isolated
test environment to verify that the existing size guards reject them cleanly.
