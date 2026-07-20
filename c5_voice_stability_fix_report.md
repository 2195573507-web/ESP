# C5 Voice Stability Fix Report

Date: 2026-07-20

## Scope

Implemented for both ESPC51 and ESPC52 only. Radar, BLE, BME690, network
protocols, and S3 communication were not changed by this task.

## Modified Files

Mirrored in `ESPC51/` and `ESPC52/`:

- `components/Middlewares/CMakeLists.txt`
- `components/Middlewares/runtime/c5_task_lifecycle.c`
- `components/Middlewares/runtime/c5_task_lifecycle.h`
- `components/Middlewares/runtime/c5_resource_manager.c`
- `components/Middlewares/mic/mic_adc_test.c`
- `components/Middlewares/speaker/speaker_player.c`
- `components/Middlewares/voice_domain/voice_chain.c`
- `components/Middlewares/wake/local_wake_word.c`
- `components/Middlewares/wake/wake_prompt_cache.c`
- `components/app_config/app_stack_monitor.h`

## Fixed Problems

- Added `c5_task_lifecycle`: publish and clear task handles under their owner
  lock before deletion. `c5_task_delete_safe()` prevents stale-handle and
  duplicate-delete paths for ordinary tasks.
- Replaced the Mic task's `xTaskCreateStatic` plus externally managed static
  TCB/PSRAM stack with `xTaskCreateWithCaps` and `vTaskDeleteWithCaps`. Its
  task memory is now reclaimed by FreeRTOS after termination-list processing,
  rather than being cleared or reused while `prvCheckTasksWaitingTermination()`
  may still reference it.
- Serialized same-generation voice release in `c5_resource_manager`; only one
  release owner can abort the server, drain I2S, and resume background work.
  Failed releases become ordered retries rather than concurrent cleanup.
- Enforced the Mic/Speaker phase order: listening/record-ready, Mic pause,
  Mic DMA released, speaker ownership, speaker released, then Mic ready.
  A worker quiesce timeout now explicitly restores its pause gate.
- Made speaker session teardown wait for the persistent IIS writer completion
  acknowledgement before I2S deinit or ring/PSRAM/DMA buffer release.
- Made `WAKE_ACK_PLAYBACK_START` reject reentry and serialize the complete
  SPIFFS spool, playback, and cleanup transaction. Prompt cleanup propagates
  failed file removal and aborts a stream when finish cannot converge.

## Runtime Guards

- Added lifecycle-bound `heap_caps_check_integrity_all(true)` checks before
  Mic and speaker allocation and around cleanup-sensitive wake playback.
- Corrected task high-water conversion to bytes and retained explicit
  `TASK_CREATE`, `TASK_DELETE_BEGIN`, and `TASK_EXIT` trace points.

## Verification

- `git diff --check -- ESPC51 ESPC52` passed.
- The listed C51/C52 voice files are byte-identical.
- `ESPC51`: `idf.py -C ESPC51 -B /Users/zhiqin/ESP 部分开发/build-c5-voice-stability-c51 build` produced `00_Learn.elf` and `00_Learn.bin`.
- `ESPC52`: `idf.py -C ESPC52 -B /Users/zhiqin/ESP 部分开发/build-c5-voice-stability-c52 build` produced `00_Learn.elf` and `00_Learn.bin`.
- The resulting images are `0x1ae400` bytes (C51) and `0x1ae3f0` bytes (C52);
  both fit the `0x500000` application partition. The 16-byte difference is
  outside the mirrored voice source set.

No device was flashed or monitored. The builds prove source integration and
firmware generation; on-device wake, prompt playback, and long-run FreeRTOS
termination-list stability still require hardware runtime evidence.
