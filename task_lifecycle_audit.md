# ESP C5 FreeRTOS Task Lifecycle Audit

Date: 2026-07-20

## Scope and Method

This audit covers the live `ESPC51` and `ESPC52` firmware source trees. It
excludes `archive/`, `分支项目/`, and generated build directories. The scan
includes `TaskHandle_t`, `vTaskDelete`, `xTaskCreate`,
`xTaskCreateStatic`, `xTaskCreateWithCaps`, and
`xTaskCreatePinnedToCore`.

No `xTaskCreatePinnedToCore` call exists in either C5 target. `local_wake_word`
and `wake_prompt_stream` do not create FreeRTOS tasks: they execute in their
caller task, normally `mic_adc_test` or `voice_chain`.

## Task Inventory

`C51 / C52` location pairs below point to the live source at audit time.

| Task | Creation location | Handle | Creator context | Deletion location / context | Self-delete | Repeat-delete assessment |
| --- | --- | --- | --- | --- | --- | --- |
| `app_startup` | `main/main.c:61 / :61`, `xTaskCreate` | `s_app_startup_task` | `app_main` | `main/main.c:41 / :41`, task itself after `app_orchestrator_start()` returns | Yes | Low; no external delete path |
| `voice_chain` | `voice_domain/voice_chain.c:1077 / :1079`, `xTaskCreate` | `s_voice.task` | `voice_chain_start` | `voice_chain_cleanup_start_failure`, startup caller | No | Fixed: critical-section take-and-clear makes the handle single-owner before delete |
| `mic_adc_test` | `mic/mic_adc_test.c:1950 / :1902`, `xTaskCreateStatic` | `s_mic_adc_task_handle` | `mic_adc_test_start`, invoked from voice startup | startup rollback and `mic_adc_test_stop_and_deinit_for_reconnect` | No; it confirms STOPPED then suspends for owner deletion | Fixed: shutdown ownership serializes callers; expected-handle take-and-clear permits one delete |
| `speaker_iis_writer` | `speaker/speaker_player.c:843 / :846`, `xTaskCreateStatic` | `s_pcm_stream_ctx.writer_task` | `audio_player_init`, reached by wake prompt/server PCM playback | None after this change | No | Fixed for static TCB reuse: invalid-context path logs and suspends without delete or handle clear |
| `server_voice_rx` | `server_voice/server_voice_client.c:597 / :599`, `xTaskCreateStatic` | `s_voice.response_task` | `server_voice_client_init` | None | No | Low; permanent notification loop and no deletion API |
| `wifi_reconnect` | `wifi/wifi_manager.c:230 / :230`, `xTaskCreate` | `s_wifi_reconnect_task_handle` | `wifi_manager_init` | None | No | Low; permanent reconnect loop |
| `gateway_link` | `server_comm/gateway_link.c:642 / :642`, `xTaskCreate` | `s_link.task` | `gateway_link_start` | None | No | Low in current one-start model; no delete path |
| `c5_event_dispatcher` | `runtime/c5_backpressure_controller.c:426 / :426`, `xTaskCreate` | `s_dispatcher_task` | `c5_scheduler_start` | None | No | Low; permanent dispatcher loop |
| `bme_worker` | `runtime/c5_runtime_workers.c:196`, calls at `:231 / :231`, `xTaskCreate` | `s_bme_worker_task` | `c5_runtime_workers_start` helper | None | No | Low; permanent queue worker |
| `system_worker` | `runtime/c5_runtime_workers.c:196`, calls at `:234 / :234`, `xTaskCreate` | `s_system_worker_task` | `c5_runtime_workers_start` helper | None | No | Low; permanent queue worker |
| `radar_ble_rx` | `radar_ble/radar_ble_runtime.c:67 / :67`, `xTaskCreateWithCaps` | `s_task` | `radar_ble_runtime_start` | None | No | Low; permanent receive loop |
| `radar_report` | `sensor_domain/radar/radar_state_client.c:189 / :189`, `xTaskCreate` | `s_task` | `radar_state_client_start` | None | No | Low; permanent report loop |
| `radar_worker` | `radar_domain/radar_worker.c:417 / :417`, `xTaskCreateWithCaps` | `s_worker_task` | `radar_domain_start` | None | No | Medium, outside requested voice scope: upload creation can fail after worker creation without rollback |
| `radar_upload` | `radar_domain/radar_worker.c:419 / :419`, `xTaskCreateWithCaps` | `s_upload_task` | `radar_domain_start` | None | No | Medium, paired with `radar_worker` partial-create failure |

`server_comm_http.c` stores `s_voice_request_task` and
`s_wake_prompt_request_task` as request-owner identity only. It does not create
those tasks and is therefore not an additional lifecycle row.

## Root Cause

The voice failure/reconnect paths had two external `mic_adc_test` deletion
sites. Each could observe the same task handle after STOPPED confirmation and
call `vTaskDelete` before clearing the shared handle. Concurrent callers could
therefore remove the same TCB twice, matching the observed
`uxListRemove()` / `prvCheckTasksWaitingTermination()` / Idle termination-list
corruption failure class.

`voice_chain` startup rollback also deleted its handle without ownership
transfer. The static speaker writer had an exceptional self-delete branch; a
later `xTaskCreateStatic` could reuse its `StaticTask_t` before the Idle task
processed the termination list.

## Implemented Lifecycle Hardening

- `voice_chain`: `voice_chain_take_task_handle()` takes and nulls
  `s_voice.task` under `s_voice_task_lock`; only the caller receiving that
  handle executes `vTaskDelete`.
- `mic_adc_test`: task-handle operations use `s_mic_adc_task_lock`.
  Shutdown has a single owner, snapshots the expected handle, and uses
  take-and-clear before either deletion path. Timeout and deinit-error paths
  release shutdown ownership so a later recovery attempt can proceed.
- `speaker_iis_writer`: the invalid initial-context branch no longer calls
  `vTaskDelete`. It retains its static TCB and suspends permanently, so the
  static storage cannot be reused while awaiting Idle cleanup.
- `server_voice_rx`: remains deliberately persistent; creation is logged and
  there is no task exit or deletion path to add.
- No wake, ADC sampling, playback protocol, voice state transition, or server
  request behavior was changed.

## Lifecycle Logs

The voice task paths now emit these stable records:

- `TASK_CREATE`: after successful `voice_chain`, `mic_adc_test`,
  `speaker_iis_writer`, and `server_voice_rx` creation.
- `TASK_DELETE_BEGIN`: immediately before each owner-authorized external
  deletion of `voice_chain` or `mic_adc_test`.
- `TASK_EXIT`: when `mic_adc_test` has stopped and suspended for external
  reclamation, and when the static speaker writer enters its fail-closed
  invalid-context suspension path.

No task entry function falls through and returns: normal workers loop forever,
the startup task self-deletes, and the two exceptional static-task paths suspend
instead of returning.

## Remaining Findings

The radar pair's partial-create rollback is a separate lifecycle issue: if
`radar_worker` starts and `radar_upload` creation fails, a retry may create an
additional worker. It is reported here but intentionally not changed because it
is outside the requested voice/wake/mic/server/playback scope.

## Verification

- Source scan confirmed no `xTaskCreatePinnedToCore` calls and only three
  remaining voice-range `vTaskDelete` sites per target: the guarded
  `voice_chain` rollback and the two guarded `mic_adc_test` owner deletions.
- `git diff --check` passed.
- Fresh isolated ESPC51 CMake/Ninja build completed and produced
  `00_Learn.elf` and `00_Learn.bin`.
- Fresh isolated ESPC52 CMake/Ninja build completed and produced
  `00_Learn.elf` and `00_Learn.bin` after the final shutdown-serialization
  change.

Build and static scans do not prove runtime task scheduling or hardware audio
behavior. Flash/monitor validation should specifically exercise repeated Mic
start-failure rollback, concurrent stop requests, disconnect during playback,
and repeated wake/server-voice rounds while watching the `TASK_*` records and
Idle-task behavior.
