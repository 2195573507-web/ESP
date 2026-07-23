# 审计发现

- 活跃目标：`/Users/zhiqin/ESP 部分开发/ESPS3`。
- 硬件冷启动、串口、WiFi/BLE/BME/HTTP 运行验收不能由本机源码/构建证明，报告需单独标注。
- 历史记忆提示 ESPS3 曾有启动内存与 PSRAM、radar diagnostics 字符串生命周期和 stack 风险修复；当前源码必须作为事实来源。

## 2026-07-22 S3 ingress 任务现场证据

- `ESPS3/components/Middlewares/local_http_server/local_http_server.c` 的 `read_json_body()` 已使用 `MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT`；`enqueue_body_buffer_with_admission()` 再分配同样 capability 的 `s3_runtime_ingress_t`，并把原始 body 拷贝进其 4KB 内嵌数组。
- sensor handler 在 `validate_local_body()` 中解析一次 JSON，入队前 `local_id_from_json_body()` 再解析一次；worker 的 `fill_unified_from_raw_body()` 与 `protocol_adapter_parse_local_envelope()` 还会继续解析。该路径是当前最明确的重复 parser/节点生命周期热点。
- 目前 event bus 已按 CRITICAL/REALTIME/STATE/BACKGROUND 分层，STATE 槽位可 coalesce，BACKGROUND 才允许丢弃；仍需确认 drop 日志是否包含 producer、age、consumer latency。
- `ESPS3/components/radar_ld2450/include/radar_config.h` 当前 `RADAR_CONFIG_UART_REQUESTED=1` 且 `RADAR_CONFIG_UART_INTERNAL_RESERVE_BYTES=12288U`，需要以 active config 和生命周期代码核实是否真的安装 S3_LOCAL LD2450。
# 2026-07-23 startup-chain findings

- `wifi_connect_to_ap()` previously waited forever; it now uses the existing `WIFI_CONNECT_TIMEOUT_MS` and leaves the reconnect task active.
- `c5_should_run()` now permits BME sampling offline; `server_comm_http` still rejects non-ready Gateway uploads.
- Radar BLE receive/parse/track is transport-local; only upload waits for Gateway.
- MIC ADC/VAD startup is local; PCM stream activation still checks Gateway/Wi-Fi.
- C5 local wake remains the existing S3 WakeNet compatibility boundary; no model was added in this scope.
- BME/system runtime workers are now started before `NETWORK_START`; the
  post-startup deferred task remains only as an allocation-failure retry.
- Integrated final C51/C52 builds passed in `/tmp/codex-final-c51-build` and
  `/tmp/codex-final-c52-real-build`; validation remains build-only.
