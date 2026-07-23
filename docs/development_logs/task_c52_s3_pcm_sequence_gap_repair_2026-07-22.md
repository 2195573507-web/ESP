# C52 to S3 PCM Pre-roll Sequence Gap Repair

## Scope

- C52 `c5_audio_transport` pre-roll queue, sender task, UDP transport, and
  sequence allocation.
- Existing S3 `audio_wake_gateway` receive/gap diagnostics are retained.
- WakeNet model selection and lifecycle are unchanged.
- C52 VAD behavior, thresholds, and capture boundaries are unchanged.

## Observed Failure

The hardware log sequence was fixed and repeatable:

```text
S3_PCM_RX_GAP expected=8 received=51 missing=43
PCM_PRE_ROLL_BEGIN frame_count=50 first_sequence=1
PCM_PRE_ROLL_END frames_sent=50 last_sequence=50
PCM_LIVE_BEGIN first_sequence=51
```

The opening control frame is sequence 0. Pre-roll PCM therefore occupies
sequences 1 through 50, and live PCM begins at 51. S3 receiving 1 through 7
then 51 means the pre-roll datagrams 8 through 50 did not arrive at S3.

## Audit Result

`mic_adc_wake_stream_send_pre_roll()` queues one pre-roll marker followed by
all pre-roll PCM slots and only then queues the live marker. The C52 transport
has 64 slots, while the observed start, marker, 50 PCM frames, and live marker
need 53 slots.

The current sender is the sole owner of `s_sequence`: it materializes a frame
at FIFO head and increments the sequence only after a full `sendto()`. Queue
overflow and local send failures have explicit failure paths. There is no
source path that selectively drops queued sequences 8 through 50 while still
sending 51.

However, `sendto()` success means only that the C52 network stack accepted the
datagram. `PCM_PRE_ROLL_END frames_sent` is consequently a local send count,
not remote delivery acknowledgement. The pre-roll frames are historical audio
and are emitted in a burst; the C52 sender has higher priority than Mic. The
fixed loss boundary is therefore consistent with the Wi-Fi/IP path being
overrun by the unpaced UDP pre-roll burst after local acceptance.

## Repair

The C52 sender yields one FreeRTOS tick after each successfully submitted
pre-roll PCM datagram. This keeps the existing FIFO ordering and UDP protocol,
but gives Wi-Fi/IP workers a scheduling window between the historical frames.
Live PCM is not paced and retains its normal capture cadence.

Per-frame C52 traces now use:

```text
PCM_TX_FRAME stage=queue_push ... sequence=pending frame_type=pre_roll|live
PCM_TX_FRAME stage=queue_pop ... sequence=<assigned> frame_type=pre_roll|live
PCM_TX_FRAME stage=socket_send ... sequence=<assigned> frame_type=pre_roll|live result=ok|failed errno=<n>
```

These records distinguish a producer admission failure, FIFO ownership/order,
and local socket acceptance. The S3 `S3_PCM_RX_GAP` log remains the remote
delivery authority.

## Validation Boundary

Build validation checks that the C52 change and unmodified S3 receiver compile.
It does not flash either board, collect monitor output, prove Wi-Fi delivery,
or establish WakeNet detection acceptance. Hardware validation must confirm
one stream with contiguous C52 `socket_send` sequences, matching S3 reception,
and no `S3_PCM_RX_GAP` before claiming the wake issue is closed.
