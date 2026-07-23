# 2026-07-23 - C52 WiFi and Voice Recovery

**Status:** Complete for scoped source and build validation. Flash and hardware
validation are excluded.

## Objective

Repair the C52 lifecycle in which a WiFi initialization or connection failure
enters `OFFLINE_MODE` permanently and leaves the server voice backend without a
recovery path. Local LCD, Radar BLE, BME690, MIC ADC, and VAD remain outside
this task's modification scope.

## Required Recovery Flow

```text
WiFi unavailable -> retry -> NETWORK_READY -> VOICE_NETWORK_AVAILABLE
                 -> server voice reconnect -> PCM streaming resume
```

## Initial Source Findings

- Local runtime is intentionally initialized before `NETWORK_START`; that
  ordering is appropriate for offline-capable local functions.
- Earlier startup work documented a possible fatal WiFi initialization error
  path and a Gateway-to-Voice notification that only handled loss of READY.
- Before this repair, `wifi_manager_init()` used fatal `ESP_ERROR_CHECK` calls
  through `esp_wifi_init()`.  An `ESP_ERR_NO_MEM` there prevented creation of
  the WiFi event group and reconnect task, so the later offline state could
  not recover.
- Before this repair, startup also started runtime workers before `NETWORK_START` despite a later
  Phase-2 mechanism intended to defer them until the startup task releases its
  Internal stack.  The early BME worker has an Internal static stack and can
  reduce the contiguous Internal memory available to WiFi initialization.
- The `ESP_ERR_NO_MEM` investigation must distinguish internal/DMA free and
  largest-contiguous blocks from aggregate heap totals, and account for early
  task allocation and allocation churn.

## Work Ledger

| Area | Owner | Status | Notes |
| --- | --- | --- | --- |
| WiFi lifecycle and memory evidence | Agent 1 | Complete | `wifi_manager` and gateway recovery |
| Voice network recovery | Agent 2 | Complete | queued voice transition and server-turn admission |
| Startup/global-state audit | Agent 3 | Complete | local/network/offline/ready transitions |
| Integration, build, and this record | Lead | Complete | Source/build evidence only; no runtime acceptance |

## Confirmed Design Direction

- Keep `LOCAL_RUNTIME_READY -> NETWORK_START -> OFFLINE_MODE` as a bounded
  initial-result path, not a terminal mode.
- Retain WiFi driver/reconnect retries after a recoverable initialization
  failure; do not make the background retry contingent on the first driver
  initialization succeeding.
- Treat `LINK_READY` (health and child registration complete), not merely
  WiFi `GOT_IP`, as the network-ready signal for server voice.
- On the `LINK_READY` edge, notify Voice on its own task/queue so the gateway
  reconnect task never performs voice/client allocation or PCM work inline.

## Implemented Changes

- `ESPC52/components/Middlewares/wifi/wifi_manager.[ch]`
  - Replaced fatal WiFi initialization checks with propagated recoverable
    errors.  The event group is created before driver initialization, so the
    reconnect task can retry `esp_wifi_init()` after `ESP_ERR_NO_MEM`.
  - Added driver-init retry state, idempotent platform setup, and capability
    heap telemetry before/after driver initialization failure and success.
  - Added `WIFI_STATE_CHANGE`, `WIFI_RETRY_START`, `WIFI_CONNECTED`,
    `WIFI_LOST`, and `WIFI_READY_FOR_VOICE` diagnostics.
- `ESPC52/components/Middlewares/server_comm/gateway_link.[ch]`
  - Added a decoupled Voice availability callback.  It is invoked outside the
    gateway lock only at `LINK_READY` enter/leave edges, so server_comm does
    not depend on voice_domain.
  - `LINK_READY` emits `NETWORK_READY`; leaving it emits `OFFLINE_MODE` and
    preserves the established in-flight voice abort behavior.
- `ESPC52/components/Middlewares/voice_domain/voice_chain.[ch]` and
  `ESPC52/components/Middlewares/server_voice/server_voice_client.[ch]`
  - Voice registers the gateway callback, queues network transitions to its
    own task, and treats network availability as a prerequisite for new turns
    and PCM upload.
  - Network loss moves an idle listener out of the upload-capable listening
    state and aborts an active lease.  Network recovery performs server-turn
    admission, then restores listening and the VAD-to-S3 PCM pipeline.
  - Added `VOICE_NETWORK_STATE`, `VOICE_RECONNECT_START`,
    `VOICE_RECONNECT_DONE`, and `VOICE_PCM_PIPELINE_STATE` diagnostics.
- `ESPC52/components/Middlewares/app_orchestrator/app_orchestrator.c`
  - Keeps local sensor/runtime and MIC/VAD startup before networking, but no
    longer starts Phase-2 runtime workers before WiFi.  This avoids the early
    Internal BME worker stack allocation; deferred workers remain owned by the
    post-startup continuation path.
  - Initial `NETWORK_READY` now means gateway health/register is ready, not
    merely WiFi GOT_IP.  The initial offline result is explicitly recoverable.

## Resulting State Flow

```text
BOOT -> LCD_READY -> LOCAL_SENSOR_READY -> LOCAL_RUNTIME_READY -> NETWORK_START
     -> OFFLINE_MODE (retry pending) -- WiFi/Gateway recovery --> NETWORK_READY
     -> VOICE_NETWORK_AVAILABLE -> VOICE_RECONNECT_START/DONE
     -> VOICE_PCM_PIPELINE_STATE=READY -> VOICE_WAKE_LISTENING
```

On loss, `LINK_READY -> non-ready` emits the unavailable callback, blocks new
PCM turns, and aborts an in-flight turn.  Existing C5 VAD ownership and the
S3 WakeNet/server boundary are unchanged.

## Root Cause

`esp_wifi_init()` could return `ESP_ERR_NO_MEM` while the old
`wifi_manager_init()` still used `ESP_ERROR_CHECK`.  The resulting fatal path
prevented the WiFi event group and reconnect task from being created; the
orchestrator then recorded a one-time offline result with no recovery route.
The condition must be evaluated from Internal and DMA free/largest-contiguous
blocks, not aggregate heap.  The premature Internal BME worker allocation
also contradicted the existing deferred-worker design and could increase the
risk of WiFi driver allocation failure.

The prior voice design consumed only link loss.  A later `LINK_READY` did not
re-arm Voice, allowing a UI-visible listening state without server upload
admission.

## Build Validation

| Command | Result |
| --- | --- |
| `idf.py -C ESPC52 -B /tmp/c52-wifi-voice-recovery-build build` | Passed |
| `ninja -C /tmp/c52-wifi-voice-recovery-build` | Passed; `00_Learn.bin` `0x240550`, 55% app partition free |
| Targeted `git diff --check` | Passed |

No flash, serial monitor, WiFi association, heap telemetry capture, gateway
health/register exchange, VAD capture, PCM delivery, S3 WakeNet, server voice,
or playback validation was performed.  C51 was not modified because this task
is scoped to C52; it retains its prior worker timing.

## Hardware Follow-up

1. Boot without the S3 SoftAP and confirm recurring `WIFI_RETRY_START` plus
   `WIFI_MEM` Internal/DMA largest-block telemetry without a reboot.
2. Bring the SoftAP online and verify `WIFI_CONNECTED`, `NETWORK_READY`,
   `VOICE_NETWORK_AVAILABLE`, `VOICE_RECONNECT_START/DONE`, and
   `VOICE_PCM_PIPELINE_STATE state=READY` in order.
3. Disconnect during an active voice turn; confirm `WIFI_LOST`,
   `OFFLINE_MODE`, `VOICE_PCM_PIPELINE_STATE state=BLOCKED_NETWORK`, then
   reconnection and a new VAD/wake turn that uploads and plays a response.

## Validation Boundary

The final validation is an isolated ESP-IDF C52 build. It does not demonstrate
WiFi association, reconnect timing, gateway reachability, PCM delivery, S3
WakeNet, server response, or audio playback on a device.
