# ESP Backend API Integration Notes

## Findings

- `ESP-server/docs/api.md` already defines the needed backend routes: gateway-state, `sensor.bme690`, `csi.motion`, logs, smart-home pending/ack, wake prompt cache, and voice turn.
- No backend change is required so far. `ESP-server/public` remains out of scope.
- Existing ESPS3 modules already cover most boundaries:
  - `server_client`: Server-facing HTTP.
  - `sensor_aggregator`: dashboard snapshot and ingest mapping.
  - `command_router`: C5 local command queue.
  - `voice_proxy`: `/local/v1/voice/turn` proxy.
  - `wake_prompt_cache_gateway`: Server prompt config/cache to local PCM.
  - `csi_placeholder_gateway`: CSI summary boundary.
- Existing snapshot code still adds mock appliances. This conflicts with the current requirement to avoid fake real smart-home state and should be removed from firmware snapshots.
- Existing S3 command routing uses old `/api/commands/*`; smart-home P1 needs `/api/smart-home/v1/commands/pending` and `/ack` handled by S3 directly. With no real smart-home hardware attached, ACK must be `failed`.
- Existing S3 non-voice paths skip during `voice_busy`; the current requirement asks voice turn not to block heartbeat/snapshot/log. Periodic snapshot/log work should run in a separate task and not depend on voice completion.
