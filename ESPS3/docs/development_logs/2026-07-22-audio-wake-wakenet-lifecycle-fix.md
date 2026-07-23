# S3 audio_wake_gateway WakeNet lifecycle fix

## Scope

Only the S3 `audio_wake_gateway` WakeNet/session lifecycle and PCM buffer
guards are changed. No ADC, VAD, LCD, radar, BME, alarm, server, or radio
behavior is changed.

## Crash evidence

The live ESP-SR S3 header defines `esp_wn_handle_from_name()` as returning the
interface table, `create()` as returning `model_iface_data_t *`, `detect()` and
`clean()` as taking the model instance, and `destroy()` as releasing it. The
application's original calls at `reset_session()` and sequence-gap handling
passed the correctly typed instance, so this was not an interface/instance
confusion.

Disassembly of the shipped `lib/esp32s3/libwakenet.a` shows that model creation
allocates six queue pointers at `model + 32`, while `model_clean` loops over
eight. Its seventh pointer is the heap tail canary `0xBAAD5678`; the first
queue field access at `+4` therefore produces `EXCVADDR=0xBAAD567C`. The
resource manager and HTTP admission paths have no WakeNet destroy/release
call.

## Fix

- Keep one interface/model-list owner for the gateway lifetime.
- Serialize model replacement and detection under `s_model_lock`.
- On each new stream, keep the boot-created model handle/ready state and reset
  only application session metadata and the PCM remainder.
- Do not call the defective binary `clean()`; sequence gaps clear only the
  application-owned PCM remainder and resume feeding with the same model.
- Add `model_ready`, `session_generation`, explicit buffer-capacity checks, and
  fail-closed detection/feed preconditions.
- Keep the receiver task alive when model recreation fails; the next controlled
  stream-open can retry through the existing owner path.

PCM accounting remains sample-safe: payload bytes are even, `frame_samples`
must equal `payload_length / sizeof(int16_t)`, and copies multiply by
`sizeof(int16_t)` exactly once.

## Verification

`idf.py -C ESPS3 build`: passed with ESP-IDF 5.5.4. The generated
`sensair_s3_gateway.bin` is `0x17b080` bytes; the smallest app partition has
79% free (`0x584f80` bytes).

No flash, serial monitor, hardware WakeNet, or end-to-end validation is run in
this source/build-only task.

## Sequence-gap recovery follow-up

The previous lifecycle repair left a `sequence_gap -> recreate_wakenet_instance`
branch. That caused each UDP loss to destroy and recreate WakeNet, so continuous
speech rarely reached consecutive inference calls. The follow-up now emits
`WAKENET_GAP_RECOVERY_BEGIN`, clears `buffered_samples` to zero, keeps the model,
and emits `WAKENET_GAP_RECOVERY_END` after the next complete WakeNet feed. New
stream session reset follows the same model-retention rule. The C52 wire frame
remains nominally `transport_frame_samples=160` (a tail frame may be shorter);
S3 reports and reassembles it into the
model-reported `wakenet_feed_samples` (typically 512).

Runtime counters now include `model_create_count`, `model_destroy_count`,
`gap_count`, `gap_recovery_count`, `feed_count`, and `inference_count` in
`WAKENET_RUNTIME_STATS`. The HTTP PCM fallback remains an explicit independent
session restart and is the only ordinary non-boot path that can recreate the
model. No hardware or runtime acceptance was performed.

Follow-up build: `idf.py -C ESPS3 -B /tmp/esps3-wakenet-gap-recovery-build
build` passed with ESP-IDF 5.5.4. The generated application image is
`0x17b270` bytes with 79% of the smallest app partition free. Flashing,
monitoring, and hardware wake validation were not run.
