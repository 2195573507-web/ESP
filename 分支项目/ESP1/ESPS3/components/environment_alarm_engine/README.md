# Environment Alarm Engine

`environment_alarm_engine` is an ESPS3-only, explicit-call component for
deterministic BME690 environment alarms. It is not connected to the existing
sensor, protocol, network, server, or task chain.

The component owns only fixed-capacity rule state, rolling histories, and a
128-entry event queue. All time-based decisions use `esp_timer_get_time()`;
tests inject a monotonic clock through `alarm_engine_options_t`.

The component is intentionally unintegrated. A future caller must provide a
validated, normalized sample and a strictly increasing per-device
`ingest_seq`; integration must not be added by implicit hooks or startup side
effects.
