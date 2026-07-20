# C5 Audio Memory Lifecycle Audit

Date: 2026-07-20

## Scope and method

This is a source-level audit of the current `ESPC51` and `ESPC52` audio paths:

- I2S/PDM TX (`components/BSP/IIS`)
- speaker player and its writer task
- microphone ADC, voice pre-roll, and WakeNet input buffer
- wake prompt HTTP spool and PCM playback
- server-voice upload and response playback buffers
- the `c5_memory` allocator that supplies these modules

Reviewed allocation/free APIs: `heap_caps_malloc`, `heap_caps_free`, `malloc`,
`free`, and the C5 wrappers `c5_mem_alloc`, `c5_mem_calloc`, `c5_mem_realloc`,
and `c5_mem_free`.

`archive/` is excluded because it is not part of the current C5 build. The IIS,
allocator, speaker, mic, and voice-chain implementations match between C51 and
C52. C52 has two additional observability-only changes: heap-integrity checks
around wake-prompt playback and stack-monitor calls in the server-voice response
task. These do not change the allocation/free ownership described below; they
are recorded here so C51/C52 parity is not overstated.

## Memory-domain policy

| Domain | Allocation policy | Reviewed audio users | Result |
|---|---|---|---|
| Internal DMA RAM | `MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT` | speaker `dma_staging`; ESP-IDF I2S and ADC driver DMA | Correct. PCM supplied to I2S is staged in DMA-capable internal RAM. |
| Internal control RAM | `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` | WakeNet realtime input buffer | Correct. This is CPU-only detector input, not DMA. |
| PSRAM | `MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT` | speaker ring/scratch/writer stack, mic pre-roll, wake prompt spool PCM, server voice upload/playback/task stack | Correct for non-DMA bulk storage. |
| Default heap | `malloc`/`free` | ESP-SR wake-word string returned by `esp_srmodel_get_wake_words` | Correct matching `free`; it is a local diagnostic string. |

The mapping is defined by `c5_mem_caps()` in
`ESPC51/components/Middlewares/memory/c5_memory.c:10` and identically in C52.

## Lifecycle findings

### I2S and driver DMA

`iis_init()` creates exactly one PDM TX channel with the configured DMA descriptor
and frame counts (`ESPC51/components/BSP/IIS/iis.c:257-271`). Every post-create
initialization failure deletes the channel and clears `s_tx_chan`
(`:311-351`). Normal release disables the channel when necessary, calls
`i2s_del_channel`, then sets both `s_tx_chan = NULL` and `s_tx_enabled = false`
(`:455-486`).

Result: no source-visible duplicate `i2s_del_channel`, stale channel-handle use,
or application-owned driver DMA buffer. The driver owns and releases its own DMA
allocation through the channel lifecycle.

### Speaker application buffers

`audio_player_init()` allocates the PSRAM ring, PSRAM scratch item, internal-DMA
staging buffer, and PSRAM static writer-task stack
(`ESPC51/components/Middlewares/speaker/speaker_player.c:785-852`). Allocation
rollback frees each successfully created object and immediately clears its stored
pointer (`:884-896`).

The stream writer is the sole I2S write owner (`:585-683`). A stream completes or
aborts before `audio_player_release_session()` invokes `iis_deinit_timed`; only
then does the function free ring/scratch/DMA staging and set each pointer to
`NULL` (`:1155-1205`). This ordering prevents the writer from using application
buffers after they are released.

The writer task and its PSRAM stack intentionally remain persistent across audio
sessions. They are not task-exit leaks: the task is re-used on the next session.
The exceptional invalid-context branch clears `writer_task` before deleting the
task (`:591-604`), but full firmware-stop teardown for this intentionally
long-lived task is not present in the reviewed API surface. This is a lifecycle
design constraint, not a confirmed runtime leak during normal session release.

### Microphone ADC and buffers

Mic storage comprises a PSRAM pre-roll buffer (`heap_caps_malloc`,
`ESPC51/components/Middlewares/mic/mic_adc_test.c:752-779`), a PSRAM static task
stack (`:782-789`, allocated at `:1875-1891`), and an internal-control WakeNet
PCM buffer (`:420-469`). All long-lived owner pointers are cleared by their
release functions; `mic_adc_local_wake_deinit()` additionally clears the complete
owner struct after freeing its samples.

The stop path first requests stop and waits for `MIC_ADC_CONTROL_STOPPED_BIT`.
Only after the worker has stopped ADC use and deinitialized its local WakeNet
state does the requester delete the task, deinitialize the ADC handle/delete the
event group, and release PSRAM buffers/task stack (`:2039-2091`). The current
ordering satisfies the requirement that task-owned buffers are not freed while
the task can still access them.

### Wake prompt stream

Wake prompt download streams into a temporary spool file, closes the HTTP stream,
then reopens the file for playback (`ESPC51/components/Middlewares/wake/wake_prompt_cache.c:288-420`). The PSRAM PCM spool buffer is allocated only while
playing; both the failed-open branch and the common completion branch free it
exactly once (`:151-211`). The local pointer has no later use after either free.

### Server voice buffers

The upload buffer is grown through `c5_mem_realloc` in PSRAM
(`ESPC51/components/Middlewares/server_voice/server_voice_client.c:173-210`). It
is released by the common client cleanup function, which clears
`s_voice.upload_buf` (`:152-164`), and after a successful request handoff, which
also clears the pointer and capacity (`:794-799`). These branches are mutually
exclusive for a given ownership state.

The per-response playback context is PSRAM allocated and freed within the
response-processing task (`:478-513`); its scope ends immediately after free.
The receive task and its PSRAM static stack persist for the C5 application
lifetime, mirroring the persistent speaker writer task.

## Pointer NULL audit

All stored pointers in the reviewed audio ownership structures are cleared after
release:

| Stored pointer | Clear-after-free evidence |
|---|---|
| `s_tx_chan` | `ESPC51/components/BSP/IIS/iis.c:313-349`, `:480-485` |
| speaker ring, scratch, DMA staging | `ESPC51/components/Middlewares/speaker/speaker_player.c:884-896`, `:1182-1192` |
| speaker writer-task handle/stack on create failure | `speaker_player.c:853-856` |
| mic PSRAM pre-roll and task stack | `ESPC51/components/Middlewares/mic/mic_adc_test.c:774-789` |
| mic WakeNet samples | `mic_adc_test.c:460-469` (whole owner struct zeroed) |
| server voice upload buffer | `ESPC51/components/Middlewares/server_voice/server_voice_client.c:152-164`, `:794-799` |
| server voice static stack on create failure | `server_voice_client.c:604-607` |

Temporary locals (`wake_words`, `pcm_buf`, `playback_ctx`, and the wake-prompt
HTTP stream) are not dereferenced or freed again after release; assigning `NULL`
to a local immediately before scope exit would not add a runtime safety property.

## Result

No confirmed source-level use-after-free, double-free, or task-exit-before-buffer
release defect was found in the reviewed current C5 audio path. Existing cleanup
already applies `pointer = NULL` to every persistent released owner pointer, so
no source change is required solely to add redundant NULL assignments.

Static review cannot prove driver, PSRAM, or scheduler behavior on a target.
Device validation should exercise repeated wake prompt playback, server response
abort during I2S write, and repeated mic-to-speaker-to-mic handoff while checking
the existing `C5_MEM`, `IIS_DMA_REQUIREMENT`, `SPEAKER_TX_DEINIT_OK`, and mic
heap logs.
