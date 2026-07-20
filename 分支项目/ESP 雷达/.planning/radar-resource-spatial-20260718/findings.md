# Findings

- C5 主链路在 `components/Middlewares/radar_domain/radar_worker.c`，当前每个有效帧进入有界上传队列并重试，未满足 latest-only。
- `components/Middlewares/sensor_domain/radar/radar_state_client.c` 已有独立 latest 草案，但未由主 CMake 接入，不能并行保留两条上传路径。
- C51/C52 的 `radar_domain` 与 `sensor_domain/radar` 源码已可保持 parity，身份由 `radar_ble_binding_config.h` / board config 区分。
- S3 `radar_gateway_ingest.c` 已有远端 source slot 和独立 `radar_spatial_state_t`，`radar_local_adapter.c` 已有 S3_LOCAL 独立实例；`radar_log_manager.c` 仍只保留一个 snapshot/rate/timer。
- S3 `radar_target_tracker.c` 已接收 `radar_zone_map_t`，但远端 slot 初始化传入 `NULL`，需要把 per-source registry/install 配置接入而不改 tracker 算法。
- 既有 S3 日志标签为 `RADAR_STATE`、`RADAR_TRACK`、`RADAR_TRACK_COMPAT`、`RADAR_TRACK_COMPAT_METERS`、`RADAR_TRACKER`、`RADAR_LOG_DROP`，Mac parser 依赖这些标签。
