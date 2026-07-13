# C5 Runtime Memory Stability Design

## Scope

Apply the design identically to `ESPC51` and `ESPC52`, except for existing device
identity and local configuration differences. The current uncommitted C5 streaming
voice implementation is the baseline and its application behavior is preserved.

Out of scope: `ESP-server`, ESPS3 protocol interfaces, voice state-machine
semantics, full-response PCM buffering, heap-size increases, and feature removal.

## Goals

- Prevent internal-RAM fragmentation and allocation peaks during steady WiFi, CSI,
  BME690, voice, and speaker operation.
- Keep DMA and ISR data in internal RAM while moving large non-DMA payload storage
  to PSRAM.
- Make buffer lifetime and release authority explicit across HTTP, speaker, CSI,
  and BME asynchronous paths.
- Eliminate per-turn task creation and large internal allocations from playback.
- Emit comparable internal and PSRAM capacity telemetry at lifecycle milestones.

## Memory Domains

`c5_memory` is the only new allocation gateway for business-layer dynamic storage.

| Domain | Capability | Allowed objects |
| --- | --- | --- |
| `C5_MEM_INTERNAL_DMA` | `MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA` | I2S staging and descriptor-adjacent buffers, microphone DMA, ISR-owned data |
| `C5_MEM_INTERNAL_CONTROL` | `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` | compact locks, queues, high-frequency control state |
| `C5_MEM_PSRAM` | `MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT` | PCM ring slots, HTTP/JSON bodies, voice cache, sensor and CSI history, non-real-time temporary storage |

All non-DMA allocations above 4 KiB use `C5_MEM_PSRAM`. Allocation records include
the owner label and size. Telemetry reports `internal_free`, `internal_largest`,
`psram_free`, and `psram_largest` at startup, WiFi connected, voice start, before
and after PCM playback, CSI start, and BME start.

## Speaker Data Flow

```text
HTTP response worker -> fixed PSRAM PCM slots -> persistent speaker writer
                                                -> fixed internal DMA staging
                                                -> I2S DMA
```

The speaker writer is created once during initialization using static FreeRTOS
allocation. It is the sole I2S caller and the sole consumer/releaser of PSRAM slots.
The producer owns a slot only until it publishes it. The fixed ring has no runtime
allocation, and the internal staging buffer is allocated before I2S begins.

Ring high-water stops the response worker from reading further HTTP bytes until the
writer signals low-water. This wait happens only in the response worker, never in a
WiFi task. A bounded wait failure aborts the current playback through its existing
error path; PCM is never silently dropped or overwritten.

## Turn and Callback Ownership

The response worker is one persistent, statically allocated task notified for each
turn. Every turn receives a monotonically increasing generation. Callback context
contains only a reference to static worker-owned state plus that generation. A
callback verifies that generation before publishing PCM. Turn cleanup first closes
the producer, then drains or aborts the ring, then changes the active generation.

No task receives a pointer to stack storage. Async queue items carry copied metadata
or references to statically owned/PSRAM storage with one documented releaser. CSI
callback data is copied before worker handoff. BME calibration and service context
remain static for their service lifetime.

## Long-Lived Tasks

Speaker writer and server voice response worker use `xTaskCreateStatic` and are
created once during subsystem startup. Existing microphone and voice-chain task
lifetimes are audited and converted only where this preserves their current startup
and state-machine behavior. No playback-phase path creates a task or allocates a
large internal buffer.

## Verification

- Build `ESPC51` and `ESPC52` with zero new warnings.
- Compare paired C5 trees, allowing only documented identity/local-config files.
- Run static checks for raw business-layer allocators, dynamic playback tasks, stack
  address escapes, and callback/task ownership.
- Run board stress scenarios: 30-minute idle S3 connection, 50 voice turns,
  voice+CSI+BME, WiFi reconnect, and 10-second server delay.
- Capture telemetry before/after each scenario. Acceptance requires no
  `i2s_alloc_dma_desc failed`, `ESP_ERR_NO_MEM`, or Load access fault and no
  sustained decline in internal largest block.
