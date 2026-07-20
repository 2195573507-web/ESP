# Wake Prompt Source Audit

Date: 2026-07-20

Scope: only `local_wake_word` and `wake_prompt_stream` (`wake_prompt_cache`).
This is a source/static audit; it does not prove a flashed device, SPIFFS
runtime, HTTP peer, or I2S-driver behavior.

The ESPC51 and ESPC52 copies of all four scoped source/header files are
byte-identical (`diff -u` produced no output). Line references below use
ESPC52 and apply equally to ESPC51.

## Reproduction Path

`local_wake_word_on_local_wake_detected()` clears the recording window, sets
`s_ack_active`, and logs `WAKE_ACK_PLAYBACK_START` at
`ESPC52/components/Middlewares/wake/local_wake_word.c:309-317`.

It then calls `wake_prompt_cache_play()` through `local_wake_word_play_ack()`
at `local_wake_word.c:96-100`. The stream routine accepts the call only while
`s_ack_active` is set (`wake_prompt_cache.c:255-260`), downloads the PCM to
`/wake_prompt/wake_prompt.pcm`, closes HTTP, and only then opens the I2S
stream (`wake_prompt_cache.c:364-412`). The ACK flag is cleared after the
function returns, regardless of the result (`local_wake_word.c:317-322`).

If SPIFFS/HTTP/PCM playback fails, `local_wake_word_play_ack()` releases the
audio session and attempts the static short beep (`local_wake_word.c:102-131`).
Consequently, a prompt failure does not itself abort the wake round when the
fallback succeeds.

## Findings

### P1 - The prompt stream is not serialized, but owns one fixed SPIFFS path

Affected code:

- `local_wake_word.c:309-322` sets `s_ack_active = true` but does not reject
  a call already in progress.
- `wake_prompt_cache.c:364-370` unconditionally removes and opens the single
  path `/wake_prompt/wake_prompt.pcm`.
- `wake_prompt_cache.c:407-419` reopens and removes that same path.

Trigger: two execution contexts call `local_wake_word_on_local_wake_detected()`
before the first call returns. The second call is permitted because the active
flag is an admission check, not an exclusive lock. Both operations can write,
read, and delete the same file, while both can enter speaker streaming.

Impact: a corrupted/truncated prompt, failed read, premature deletion, or
conflicting I2S session cleanup. The code's normal single `voice_chain` call
site may serialize this today, but neither scoped module enforces that
precondition.

Remediation: add one non-blocking prompt/ACK ownership guard spanning the
whole `wake_prompt_cache_play()` transaction, or reject an ACK entry when
`s_ack_active` is already true. The guard must cover file remove/open/read/
delete and the speaker stream, then release through one cleanup path.

### P2 - SPIFFS temporary-file deletion errors are discarded

Affected code: `wake_prompt_cache.c:364` and `:417-419` call `remove()` and
discard its return value.

Trigger: a filesystem error, a path still in use, or an unexpected concurrent
caller makes either deletion fail.

Impact: the mount remains live and the failed operation still returns the
previous result. A stale PCM file can occupy the one-file SPIFFS budget
(`max_files = 1` at `wake_prompt_cache.c:86-91`) and the final cleanup failure
is invisible to diagnostics. The next run does attempt a pre-write deletion,
but that does not make a persistent deletion failure safe.

Remediation: check and log each `remove()` failure. After a successful prompt
write/play, propagate final-delete failure or record a dedicated cleanup error
counter so storage exhaustion is observable. Do not remove the pre-write
cleanup; it protects recovery after interrupted playback.

## Task Creation And Deletion

No task is created or deleted in the scoped modules:

- no `xTaskCreate*`, `vTaskDelete`, task handle, queue, or task function
  exists in either `local_wake_word.c` or `wake_prompt_cache.c`;
- despite its name, `wake_prompt_cache_start_async()` only calls
  `wake_prompt_spool_mount()` synchronously (`wake_prompt_cache.c:239-246`);
- I2S work is delegated synchronously through the `audio_player_*` interface;
  the lifecycle of its implementation is outside this audit scope.

Therefore there is no prompt-task create/delete leak in these two modules.

## SPIFFS Handle Lifecycle

Good paths:

- SPIFFS registers once and the mounted flag is set only after a successful
  registration (`wake_prompt_cache.c:80-103`).
- Writer `FILE *` is opened at `:365`, closed on every post-open path at
  `:384-389`, then nulled.
- Reader `FILE *` is opened at `:407`, closed on every post-open path at
  `:414-416`.
- The HTTP stream is closed after every successful `begin()` call, including
  header, validation, open, write, and playback failures (`:390-391`). A
  `begin()` failure returns before ownership of a stream is established
  (`:295-305`).

Intentional retained resource: the SPIFFS VFS mount is process-lifetime state;
there is no `esp_vfs_spiffs_unregister()` or module deinit API. This is not a
per-wake leak, but the code provides no shutdown/reinitialization lifecycle.

## Audio Stream Buffer And I2S Cleanup

`wake_prompt_play_spool_file()` validates the file arguments, allocates one
2048-byte PCM buffer in PSRAM, and frees it on all paths after allocation:

- allocation failure returns before acquiring an audio stream
  (`wake_prompt_cache.c:158-163`);
- stream-open failure frees the buffer before returning (`:168-173`);
- a successful stream open always reaches either `audio_player_stream_finish()`
  or `audio_player_stream_abort()` (`:175-205`), including `fread` and
  `audio_player_write_pcm_chunk()` errors;
- the normal/common exit frees the buffer at `:209-211`.

`free()` appears once in `local_wake_word.c:217`, paired with
`esp_srmodel_get_wake_words()`. No heap buffer is allocated by the scoped code
per wake. The fallback beep is a static array, so it requires no cleanup.

Residual interface boundary: if `audio_player_stream_finish()` itself returns
an error, this module returns that error but does not subsequently call
`audio_player_stream_abort()` (`wake_prompt_cache.c:202-205`). Whether that
leaves an I2S session active depends on the out-of-scope audio-player contract;
this is not a proven defect in the two audited files, but it is the specific
contract point to verify during device testing.

## Return-Path Cleanup Matrix

| Stage | Failure return | Resources already held | Cleanup in scope |
| --- | --- | --- | --- |
| Wi-Fi / ACK admission | `:255-260` | none | immediate return is safe |
| SPIFFS mount | `:262-265` | none from this call | immediate return is safe |
| HTTP begin | `:295-305` | request-active flag | flag reset before return |
| Header / response validation / spool open or write | `:316-381` | HTTP stream, possibly writer | writer close, HTTP close, request flag reset, then file cleanup if created |
| PCM validation / reader open | `:394-412` | temporary file, possibly reader | reader close if opened, file-delete attempted |
| PCM read/write / I2S failure | `:175-211` | PSRAM buffer and opened stream | finish on success, abort on prior error, then buffer free; caller closes reader and deletes file |
| Prompt error returned to ACK handler | `local_wake_word.c:102-131` | no scoped prompt resources | release prior audio session, then fallback beep; its result is returned |

## Audit Verdict

No prompt task lifecycle leak, SPIFFS `FILE *` leak, HTTP stream leak after a
successful open, or PSRAM audio-buffer leak was found in the scoped source.
The two actionable code-level issues are missing transaction serialization
(P1) and ignored temporary-file deletion errors (P2). No production source was
modified for this audit.
