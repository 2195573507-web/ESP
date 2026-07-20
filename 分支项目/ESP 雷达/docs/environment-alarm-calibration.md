# Environment Alarm Calibration Guide

## Status

All values below are initial defaults copied from ESP1. They are not calibrated
for any current room and must not be presented as hardware acceptance.

## Rule Defaults

| Rule | Activate | Recover | Debounce / samples | Window / gate |
| --- | --- | --- | --- | --- |
| High temperature | `>= 35 C` | `<= 33 C` | 60 s / 3; 120 s / 3 | temperature valid |
| Low temperature | `<= 10 C` | `>= 12 C` | 60 s / 3; 120 s / 3 | temperature valid |
| Fast temperature change | delta `>= 3 C` | delta `<= 1 C` | immediate / 2; 120 s / 1 | 5 min temperature history |
| High humidity | `>= 75 %` | `<= 70 %` | 60 s / 3; 120 s / 3 | humidity valid |
| Low humidity | `<= 25 %` | `>= 30 %` | 60 s / 3; 120 s / 3 | humidity valid |
| Fast humidity change | delta `>= 15 %` | delta `<= 8 %` | immediate / 2; 120 s / 1 | 5 min humidity history |
| AQ warning | score `<= 49` or `poor` | score `>= 55`, not poor/bad | 60 s / 3; 120 s / 3 | READY, score and level valid |
| AQ critical | score `<= 29` or `bad` | score `>= 35`, not bad | 30 s / 2; 120 s / 3 | READY, score and level valid |
| AQ deterioration | historical max minus score `>= 20`, score `< 70`, ratio `<= .85` | score improves `>= 10` from activation or `>= 70` | 60 s / 1; 10 min / 1 | READY, 10 min AQ history and gas ratio |
| Pollution spike | gas ratio `<= .70`; upgrades at `<= .55` | gas ratio `>= .80` | immediate / 2; 120 s / 3 | READY, gas ratio valid |
| Environment unstable | stability `< .55` | stability `>= .70` | immediate / 3; 120 s / 3 | READY, stability score valid |
| Sensor degraded | sensor state `DEGRADED` | sensor state `READY` | immediate / 3; immediate / 3 | real sensor state only |
| Critical environment | AQ critical + temp `>= 40 C` or `<= 5 C`; or pollution active + humidity `>= 85 %` or `<= 15 %` | all recorded participant conditions clear | 60 s / 1; 300 s / 1 | READY plus corresponding fields/rule states |

The maximum valid inter-sample gap for debounce continuity is 45 seconds.
Large gaps reset pending/recovery accumulation rather than producing a false
transition.

## State Interpretation

- `WARMUP`: temperature and humidity can run when valid. AQ, gas, trend,
  stability, pollution, and composite rules are gated.
- `READY`: dependent rules run only with valid input fields. The stability rule
  requires a valid real stability score.
- `DEGRADED`: the sensor-degraded rule can activate. DEGRADED AQ/gas fields do
  not act as READY data.
- `UNKNOWN`: only independent valid temperature/humidity rules can run.

Warmup completion is not inferred from elapsed time. It is the C5-provided
`air_quality.sensor_state=READY` state.

## Proposed Field Collection

Collect separate data for C51 and C52 because they may sit in different rooms.
For each unit record the full V3 envelope fields: device id, room id, boot id,
remote sequence, S3 receipt time, temperature, humidity, pressure, gas
resistance, AQ score/level, gas ratio, stability score, sensor state, and
whether time is synchronized.

1. Leave the device in a normal room for at least 30 minutes after a stable
   READY transition, then capture at least 24 hours of normal operation.
2. Repeat the 24-hour baseline for each distinct room. Do not pool rooms before
   comparing their distributions.
3. Capture controlled, safe changes separately: ordinary ventilation, humidity
   change, normal cooking-distance pollution exposure, and recovery. Do not
   expose hardware or people to hazardous conditions merely to force alarms.
4. Record no passwords, gateway token, complete request body, or personally
   identifying room data in exported calibration logs.

## Analysis And Adjustment Criteria

- Plot temperature, humidity, AQ score, gas ratio, and stability distributions
  per device/room and identify the normal 5th/50th/95th percentiles.
- Compare the highest normal short-window temperature/humidity deltas against
  the fast-change thresholds; increase debounce before loosening a threshold
  when short transient noise is the cause.
- Check AQ deterioration and pollution candidates against ventilation/cooking
  notes. A candidate with no plausible physical event is a false positive.
- Verify a DEGRADED transition produces only the sensor-degraded path and that
  READY recovery produces the matching recovery event after its configured
  samples.
- Verify each active/recovered pair retains order through temporary network
  loss and does not get merged across device or rule.
- Adjust only after enough per-room data exists. Record the prior value, new
  value, reason, data period, and false-positive/false-negative evidence.

## Suggested Acceptance Matrix

| Check | C51 | C52 | Evidence Required |
| --- | --- | --- | --- |
| Warmup to READY | pending | pending | state, stability, timestamped samples |
| Normal room baseline | pending | pending | 24-hour distribution summary |
| Temperature/humidity hysteresis | pending | pending | ordered active/recovered events |
| AQ/pollution false-positive review | pending | pending | sample trace and room activity note |
| DEGRADED and READY recovery | pending | pending | real V3 state transition |
| C5 reboot / sequence reset | pending | pending | boot id, remote/local sequence logs |
| Offline retry and recovery | pending | pending | queue/retry/HTTP status logs |
| Server persistence/idempotency | pending | pending | authenticated test-server record, not production |

Useful bounded logs are `ENV_ALARM_INIT`, `ENV_ALARM_SAMPLE`,
`ENV_ALARM_EVENT`, `ENV_ALARM_REPORT`, and `ENV_ALARM_STATS`. The logs expose
device, rule, state, queue action, sequence, and retry fields without
credentials or full sensitive payloads.
