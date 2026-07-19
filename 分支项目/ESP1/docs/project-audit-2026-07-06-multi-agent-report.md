# ESP-111 Multi-Agent Read-Only Audit Report - 2026-07-06

## 0. 审计边界

本次审计目标: 至少 5 个 agent 并发审计 `/Users/zhiqin/ESP-111`，不修改业务代码，只写审计报告。

执行方式:

- 并发启动 6 个只读审计 agent:
  - Agent A: 固件层 / C5-S3-Server 边界
  - Agent B: ESP-server API / 数据 / 鉴权
  - Agent C: CSI 链路 / 三层状态边界
  - Agent D: public 前端 / tools/csi-debug-web
  - Agent E: 构建 / 配置 / 供应链 / 仓库卫生
  - Agent F: 文档 / 架构一致性 / 历史风险回归
- 主线程只读核对关键证据并运行非硬件验证命令。
- 本次唯一有意写入的文件是本报告。

未做事项:

- 未修改固件、后端、前端、数据库或配置代码。
- 未烧录硬件，未执行 ESP-IDF firmware build，未做真实设备端到端运行验证。
- 未修复发现的问题。

## 1. 总结结论

当前主架构仍基本保持三层模型:

```text
ESPC51 / ESPC52 C5 terminals
  -> ESPS3 local gateway
  -> ESP-server truth / persistence / dashboard APIs
```

正向进展:

- C5 active 链路未发现直接访问 ESP-server `/api/*` 的公网 Server URL；`server_comm_build_url()` 仍拒绝 `http://` / `https://` 绝对 endpoint。
- ESPS3 `local_http_server` 只注册 `/local/v1/*` 本地 routes，没有把 Server `/api/*` 暴露给 C5。
- 后端 device / dashboard / command poll / command ack / smart-home state / event write 等部分路由已经接入 `requireGatewayAuth()` 和 gateway-device binding 框架。
- public dashboard 主读路径已迁移到 `/api/dashboard/v1/*`，旧报告里 “public 仍直接读 `/sensor/latest` / `/asr/latest` / `/llm/latest`” 的结论已过期。
- CSI 主链路基本符合 canonical fact 模型: C5 输出轻量 feature，S3 做 fusion/state，ESP-server 的 `csiMotionService` 拒绝 payload 内 raw/subcarrier/legacy occupancy 字段。

最高优先级风险:

1. `POST /api/csi/behavior` 是未鉴权 legacy/agent-state CSI 写入口，可把 raw/subcarrier 类字段放进 `features` 或 event payload 落库，绕过 canonical CSI 拒绝逻辑。
2. Gateway auth 默认 fail-open: 未配置 `GATEWAY_AUTH_TOKEN(S)` 时，声明 `X-Gateway-Id` 即可通过 `gatewayOnly` 写入面；同时 S3 默认 auth token 为空。
3. 固件源码和构建产物暴露明文网络凭据、固定 SoftAP PSK、明文 HTTP Server URL；S3 -> Server 默认不是强认证 + TLS。
4. voice、prompt config、LLM structured command、命令创建、smart-home control、日志清理等管理/控制面仍缺 operator/admin auth。
5. `ESP-server/db/database.db` 在嵌套 repo 中被 git 跟踪且当前含业务数据，容易泄露真实记录并持续污染工作区。

## 2. 关键发现

### P0-1. `/api/csi/behavior` 可绕过 canonical CSI raw/subcarrier 拒绝逻辑

证据:

- `ESP-server/src/routes/agentStateRoutes.js:353` 直接注册 `router.post("/api/csi/behavior", ...)`，未挂 `requireGatewayAuth()` 或 admin middleware。
- `ESP-server/src/agent/stateStore.js:851-860` 将 `input.features` 原样 `jsonText(input?.features || {})` 写入 `csi_behavior_events.features_json`。
- `ESP-server/src/routes/agentStateRoutes.js:357-365` 又把完整 `req.body` 写入 event log payload。
- 对比: canonical CSI 入口 `ESP-server/src/services/csiMotionService.js:83-91` 只拒绝 `payload` 内 `raw_csi`、`subcarrier_data`、`selected_subcarriers`、`iq`、`phase`。

风险:

- 任意调用方可把 `raw_csi`、`subcarrier_data`、`selected_subcarriers` 放入 `/api/csi/behavior` 的 `features` 或 body 其他位置落库。
- 这会破坏 “ESP-server 只保存 canonical CSI facts，不保存 raw/subcarrier 数据” 的边界。

建议:

- 立即下线或改名该 route 为 legacy/admin-only。
- 至少补 gateway/admin auth，并对整个 body 做递归 forbidden-key 拒绝。
- 将 `features_json` 白名单化，禁止进入 dashboard、LLM、memory 或 canonical CSI fact 语义。

### P0-2. Gateway auth 默认 fail-open

证据:

- `ESP-server/src/services/gatewayAuthService.js:93-100` 中，`configuredGatewayTokens()` 为空时返回 `ok: true, auth_required: false, source: "declared"`。
- `ESP-server/src/services/gatewayAuthService.js:152-173` 的 `requireGatewayAuth()` 会接受该结果并继续 `next()`。
- `ESPS3/components/Middlewares/gateway_config/gateway_config.h:77-79` 默认 `GATEWAY_CONFIG_AUTH_TOKEN ""`。
- `ESPS3/components/Middlewares/server_client/server_client.c:132-138` 只有 token 非空才发送 `X-Gateway-Token`。
- `ESP-server/.env.example:1-42` 未列 `GATEWAY_AUTH_TOKEN(S)` 或 `ADMIN_TOKEN` 作为必填安全变量。

风险:

- 默认部署或漏配环境变量时，任何能访问 ESP-server 的客户端只要声明 gateway id，就能通过 gateway 写入面。
- 影响 `/api/device/v1/ingest`、dashboard snapshot、command pending/ack、smart-home pending/ack、event write 等路由。

建议:

- 生产默认 fail-closed: 未配置 gateway token 时返回 503。
- 使用 per-gateway token hash 或 mTLS/HMAC，绑定 gateway id，而不是共享静态 token。
- `.env.example` 标注安全变量为生产必填；测试用显式 `ALLOW_INSECURE_GATEWAY_AUTH=1` 一类开关。

### P0-3. 明文凭据和公网 HTTP Server URL 已进入源码 / 构建产物

证据:

- `ESPS3/components/Middlewares/gateway_config/gateway_wifi_credentials.h:26-28` 含真实 STA SSID/密码条目。
- `shared_components/esp111_protocol_common/include/esp111_protocol_common.h:37-39` 固定 SoftAP SSID、PSK 和 gateway IP。
- `ESPS3/components/Middlewares/gateway_config/gateway_config.h:73-79` 固定明文 HTTP Server URL，auth token 默认空。
- Agent E 只读检查发现 `ESPS3/build/sensair_s3_gateway.bin` 可通过 `strings` 读出这些网络标识/URL/凭据类字符串。

风险:

- 源码或固件镜像泄露后即可获取本地网关接入凭据和后端入口。
- 明文 HTTP 上云链路易被中间人拦截或篡改。

建议:

- 立即轮换已暴露 WiFi / SoftAP 凭据。
- 生产凭据通过 NVS / secure storage / provisioning 注入，不进入 git 和默认固件镜像。
- 生产 Server URL 使用 HTTPS 并做证书校验；auth token 必须非空。

## 3. 高风险发现

### P1-1. Voice / prompt config 未接入 gateway/admin auth

证据:

- `ESP-server/server.js:101` 在全局 JSON parser 前挂载 voice router。
- `ESP-server/src/routes/voiceRoutes.js:596-600` 直接注册 `/api/voice/turn`、`/api/voice/prompt/config`、`/api/voice/prompt`、`/api/voice/prompt-cache`。
- `ESP-server/src/voice/http.js:44-60` voice device id 来自 header/query，缺失时 fallback 到 `req.ip` 或 `unknown`。
- `ESP-server/src/voice/turnConfig.js:5-24` 默认 body 上限 4 MiB、单轮 45 秒、并发 1。

风险:

- 未授权调用可触发 ASR/LLM/TTS 资源消耗、伪造 voice turn 活跃状态。
- `PUT /api/voice/prompt/config` 可改 wake prompt/TTS 配置并写本地配置文件。

建议:

- `/api/voice/turn` 和 prompt cache 走 gateway auth + device binding。
- prompt config GET/PUT 走 admin auth。
- 加速率限制、IP/网关并发限制、请求体和上游响应总字节上限。

### P1-2. 控制 / 命令创建入口缺 operator/admin auth

证据:

- `ESP-server/src/routes/commandRoutes.js:162-175` 的 `POST /api/commands` 未鉴权。
- `ESP-server/src/routes/commandRoutes.js:243-257` 的 natural-language command 未鉴权。
- `ESP-server/src/routes/smartHomeRoutes.js:66-81` 的 `POST /api/smart-home/v1/control` 未鉴权。
- `ESP-server/src/routes/agentStateRoutes.js:403-430` 的 `POST /api/lcd/display` 会 enqueue `display.show_text`，未鉴权。
- `ESP-server/src/routes/structuredLlmRoutes.js:29-116` 的 `/api/llm/structured` 可解析 LLM 输出并 enqueue commands，未鉴权。

风险:

- 外部调用者可排队设备命令、消耗 LLM 配额、影响 LCD/智能家居控制面。

建议:

- 区分 gateway auth 与 operator/admin auth。
- 命令创建、LLM structured、smart-home control、LCD display 全部接入 RBAC / admin token。
- 加 idempotency key、速率限制、审计字段和目标设备授权检查。

### P1-3. 日志 cleanup/delete 未挂 admin auth

证据:

- `ESP-server/src/routes/eventRoutes.js:150-186` 注册 `POST /api/logs/v1/cleanup` 和 `DELETE /api/logs/v1/events`，无 admin middleware。
- `ESP-server/src/routes/userDataRoutes.js:32-48` 则有 `requireUserDataAdmin`，说明项目已有 admin-token 模式可复用。
- `docs/api-boundary-v1.md` 曾将日志 cleanup/delete 标为 Admin only，但 live source 未落实。

风险:

- 未授权请求可清理或删除 event logs，削弱审计证据链。

建议:

- 抽出通用 admin middleware，保护 cleanup/delete/config 类接口。
- 删除审计记录写入不可被同接口删除的独立表或外部日志。

### P1-4. 智能家居 command claim/ack 绑定仍弱

证据:

- `ESP-server/src/services/smartHomeService.js:341-367` 中，未绑定 gateway 的 queued command 可被当前 gateway claim。
- `ESP-server/src/services/smartHomeService.js:370-440` 的 ACK 校验可选 `target_id`，最终 UPDATE 未限制当前状态和 gateway 条件。
- `ESP-server/src/routes/smartHomeRoutes.js:113-118` 只把 `req.gatewayAuth.gateway_id` 注入 body。

风险:

- 命令创建时若未绑定 gateway，任意 gateway 可 claim。
- ACK 状态转移不够原子，执行结果可被抢占或覆盖。

建议:

- 命令创建时必须绑定目标 gateway/device，或由 Server 根据设备绑定推导。
- ACK SQL `WHERE` 同时限制 `command_id`、`gateway_id`、`target_id`、`status='dispatched'`。
- 增加 lease / attempt token。

### P1-5. `createIfMissing` 会把声明的 device 绑定到 gateway

证据:

- `ESP-server/src/services/gatewayAuthService.js:216-269` 支持 `createIfMissing / allowNewBinding`。
- `ESP-server/src/routes/deviceRoutes.js:126-130` 对 `/api/device/v1/ingest` 允许新 binding。
- `ESP-server/src/routes/deviceRoutes.js:211-213`、`ESP-server/src/routes/dashboardRoutes.js:89-91` 会根据 snapshot 中 device ids 绑定 gateway-device。

风险:

- 在共享 token 或 token 泄露时，攻击者可用声明式 body/header 抢占设备身份。
- 与 “Server truth / S3 owns device mapping” 边界冲突。

建议:

- 遥测写入只接受已存在绑定。
- 新 binding 走单独配对流程、一次性凭据或管理员授权。

### P1-6. 后端 SQLite 数据库被跟踪并含业务数据

证据:

- 在 `ESP-server` 嵌套 repo 内，`git ls-files -s db/database.db` 显示数据库已被跟踪。
- 当前 `git -C ESP-server status --short` 显示 `M db/database.db`。
- 只读 SQL 统计显示当前库内有 `sensor_records=4097`、`asr_records=1`、`llm_records=1`、`gateway_auth=1`、`gateway_device_bindings=1`。
- `ESP-server/src/db/sqlite.js:5-8` 默认使用 `ESP_SERVER_DB_PATH`，未设置时落到 repo 内 `db/database.db`。

风险:

- 真实传感器、ASR/LLM、网关绑定和事件记录可能随提交泄露。
- 本地运行持续污染 git 状态。

建议:

- 从 git 中移除 `db/database.db`，改用 migrations / seed。
- `.gitignore` 增加 `db/*.db`、`db/*.sqlite*`。
- 本地测试和 smoke 强制使用临时 `ESP_SERVER_DB_PATH`。

### P1-7. S3 本地短 `id` 在严格校验前 cast 为 `uint8_t`

证据:

- `ESPS3/components/Middlewares/local_http_server/local_http_server.c:263-281` 中 `/local/v1/commands/pending?id=` 使用 `atoi()` 后 cast 到 `uint8_t`。
- `ESPS3/components/Middlewares/protocol_adapter/protocol_adapter.c:462-473` 对 JSON `id` 也先 `(uint8_t)local_id_item->valueint` 后再映射。

风险:

- SoftAP 内调用者可用 `257` 这类越界数字绕成 `1`，冒充 C51/C52。

建议:

- cast 前要求原始值是整数且等于允许集合 `{1,2}`。
- 拒绝负数、小数、溢出、尾随字符。
- 后续引入每设备密钥或本地配对凭据。

### P1-8. Command ACK 本地成功与 Server 成功语义混淆

证据:

- `ESPS3/components/Middlewares/command_router/command_router.c:385-389` 在 Server ACK 上传前先把本地命令置为 `COMMAND_STATE_ACKED`。
- `ESPS3/components/Middlewares/command_router/command_router.c:457-485` voice busy 或 Server ACK 失败后仍最终返回 `ESP_OK`。
- `ESPC51/components/Middlewares/command_domain/system_command/system_server_client.c:671-687` C5 将本地 ACK POST 结果作为到 S3 的 HTTP 结果处理。

风险:

- C5 applied、S3 accepted、Server persisted 三种语义混在一起。
- Server ACK 丢失时，设备侧可能已显示成功，Server truth 未完成。

建议:

- 拆分 `local_applied`、`s3_accepted`、`server_acked`。
- Server 2xx 后再置最终 ACK；失败进入 durable retry / outbox。

## 4. 中风险发现

### P2-1. `csiMotionService` 只检查 payload 内 forbidden keys，raw_json 保存整包

证据:

- `ESP-server/src/services/csiMotionService.js:83-91` 只检查 `body.payload`。
- `ESP-server/src/services/csiMotionService.js:177-184` 将 `raw_json: body` 写入 fact。
- `ESP-server/src/db/csiMotion.js:47-64` 将 `record.raw_json` JSON stringify 后写入 `csi_motion_events.raw_json`。

风险:

- 持有 gateway auth 的直连请求可把 forbidden keys 放到 envelope 顶层并被 `raw_json` 持久化。

建议:

- 对整个 envelope 做递归 forbidden-key 扫描。
- 或只保存白名单化 canonical raw_json。

### P2-2. CSI calibration duration / min samples 配置没有实际 gate

证据:

- `ESPC51/components/Middlewares/sensor_domain/csi_phase_a/csi_feature.c:397-400` 默认 `calibration_duration_ms=7000`、`min_calibration_samples=24`。
- `ESPC51/components/Middlewares/sensor_domain/csi_phase_a/csi_feature.c:470-480` 计算 `elapsed` 后 `(void)elapsed`，退出只看 `calibration_variance_converged()`。

风险:

- 日志/配置暗示 7 秒 + 最少 24 样本，实际可能提前收敛，影响 baseline 与后续 S3 state 质量。

建议:

- 真正强制 duration/min samples gate，或删除/改名误导性配置。

### P2-3. C51 / C52 CSI 调度语义不一致，且与 S3 100ms tick 容易混淆

证据:

- C51 `csi_service_process_tick()` / `csi_service_report_tick()` 由 backpressure scheduler 调用，见 `ESPC51/.../csi_service.c:181-210`。
- C52 自建 `csi_service_task()`，见 `ESPC52/.../csi_service.c:188-210`。
- C51/C52 `CSI_REPORT_INTERVAL_MS` 均为 `1000U`，S3 `GATEWAY_CONFIG_CSI_TRIGGER_INTERVAL_MS` 是 `100U`。

风险:

- 调试时容易把 S3 trigger/fusion 100ms 误读成 C5 feature 上报频率。
- 两块 C5 行为模型不同，排查困难。

建议:

- 统一调度模型，或在日志/docs 明确: C5 feature report=1Hz，S3 fusion/trigger=100ms。

### P2-4. public dashboard mock fallback 仍可能显示“看起来在线”

证据:

- `ESP-server/public/app.js:43-89` 定义 mock sensor/ASR/LLM/logs。
- `ESP-server/public/app.js:510-524` API 失败时 fallback mock。
- `ESP-server/public/app.js:554-585` history、alert logs、system logs 仍返回 mock。

风险:

- 接口失败或无真实数据时，页面可能显示正常指标、近期时间戳或在线感。

建议:

- 生产模式禁用 mock fallback。
- 无数据显示 `unknown/offline/unavailable`。
- mock 只在显式 demo mode 下启用，并在主状态区强提示。

### P2-5. S3 页面仍整页 mock

证据:

- `ESP-server/public/pages/s3.js` 定义并渲染 `s3MockData`。

风险:

- 即使有模拟数据标识，视觉上仍容易被当成真实 gateway/device/smart-home 状态。

建议:

- 接入 `/api/dashboard/v1/overview`。
- 无真实 snapshot 时展示不可用，而不是在线设备/已完成命令。

### P2-6. debug web 本地控制端点无 token / Origin 校验

证据:

- `tools/csi-debug-web/server.js:25` 绑定 `127.0.0.1`。
- `tools/csi-debug-web/server.js:789-860` 暴露 `/api/serial/connect`、`/api/serial/disconnect`、`/api/csi/sample`、`/api/csi/mock`、`DELETE /api/csi/history`，无 token 或 Origin 检查。

风险:

- 虽然只监听 loopback，本机浏览器上下文仍可能触发断开串口、写 mock、清空历史等操作。

建议:

- 启动时生成随机 token，所有非 GET 请求校验 token 与 Origin。
- 继续保持 loopback 绑定。

### P2-7. debug web 仍使用 legacy occupancy / local derived state 模型

证据:

- `tools/csi-debug-web/server.js:149-169` 将 `state/occupancy/presence_state` 互转为 occupied/vacant。
- `tools/csi-debug-web/src/csiTelemetryEngine.js:567-573` 本地 state timeline 阈值为 `0.62/0.42/0.24`，不同于 S3 fusion 参数。

风险:

- 工具虽 local-only，但会误导用户以为旧 occupancy/CV/amplitude 模型仍是 server truth。

建议:

- 默认展示 canonical `IDLE/MOTION/HOLD + frame_energy/variance/rssi/motion_score`。
- legacy 字段放入显式 toggle，并标注“不写 Server / 非 canonical”。

### P2-8. `ESP-server` 当前 npm audit 报 1 个 high

证据:

- `npm audit --registry=https://registry.npmjs.org --omit=dev --json` 在 `ESP-server` 报告 `undici <=6.26.0` high。
- `ESP-server/package-lock.json` 中 `node-gyp` 依赖 `undici ^6.25.0`。

风险:

- 已知 HTTP header / WebSocket DoS / keep-alive 相关 advisory 面。

建议:

- 受控升级 lockfile，或运行 `npm audit fix` 后回归 smoke。
- CI 固定跑 `npm audit --registry=https://registry.npmjs.org --omit=dev`。

### P2-9. 生产固件安全配置未启用

证据:

- `ESPC51/sdkconfig`、`ESPC52/sdkconfig`、`ESPS3/sdkconfig` 均显示 `# CONFIG_SECURE_BOOT is not set`、`# CONFIG_NVS_ENCRYPTION is not set`、`# CONFIG_FLASH_ENCRYPTION_ENABLED is not set`。

风险:

- 设备或镜像被拿到后，更容易提取凭据、复制身份或篡改固件。

建议:

- 新增 production sdkconfig defaults，启用 secure boot、flash encryption、NVS encryption、rollback。
- 开发配置和生产配置分离。

### P2-10. wake prompt gateway 绕过统一 `server_client`

证据:

- `ESPS3/components/Middlewares/wake_prompt_cache_gateway/wake_prompt_cache_gateway.c:38-39` 自定义 `/api/voice/prompt/config` 和 `/api/voice/prompt?...`。
- `ESPS3/components/Middlewares/wake_prompt_cache_gateway/wake_prompt_cache_gateway.c:285-301` 自行初始化 HTTP client，只设置 `X-Gateway-Id`。

风险:

- 出现第二条 Server-facing HTTP 路径，token、TLS、offline policy、错误审计与 `server_client` 分叉。

建议:

- 将 wake prompt config/PCM 下载封装进 `server_client`，统一 header、token、TLS 和错误记录。

### P2-11. event reporter alarm 参数未决定 alarm route

证据:

- `ESPS3/components/Middlewares/gateway_event_reporter/gateway_event_reporter.c:51-55` 的 `post_json(bool alarm, ...)` 无论 `alarm` 值都调用 `server_client_post_system_log_json()`。

风险:

- 报警事件可能进入 system log 而非 alarms API，统计和告警 UI 失真。

建议:

- `alarm=true` 时调用 alarm API；增加测试覆盖。

### P2-12. C5 room_id 可覆盖 S3 映射

证据:

- `ESPS3/components/Middlewares/protocol_adapter/protocol_adapter.c:561-568` 先按 local id 设置 room，再允许 C5 body `rid` 覆盖。

风险:

- C5/NVS 可影响 Server-facing room metadata，削弱 S3/Server 对设备归属的控制。

建议:

- Server-facing room_id 只来自 S3 allowlist 或 Server device metadata。
- C5 上报 room 仅作为 diagnostic 字段。

## 5. 文档和仓库一致性问题

### P3-1. `ESP-server/docs/api.md` 仍描述旧 CSI occupancy 模型

证据:

- 文档仍描述 `occupancy.state=unknown/vacant/occupied`、`sample_count` 等。
- live `csiMotionService` 明确拒绝 `occupancy`、`cv`、`sample_count`，要求 `state=IDLE/MOTION/HOLD`、`frame_energy`、`variance`、`motion_score`。

建议:

- 以 canonical CSI facts 更新 docs: `state/link_id/frame_energy/variance/rssi/motion_score/timestamp`。
- 明确 raw/subcarrier/legacy occupancy 被拒绝。
- 补 `/api/dashboard/v1/csi/history` 响应文档。

### P3-2. `docs/api-boundary-v1.md` 部分结论是历史快照

证据:

- 该文档仍说 public 依赖 legacy latest routes。
- live `ESP-server/public/app.js` 主读路径已是 `/api/dashboard/v1/sensors/latest`、`/api/dashboard/v1/asr/latest`、`/api/dashboard/v1/llm/latest`。
- 文档仍把 gateway auth / command ACK ownership 作为未修 critical/high，但 live source 已部分缓解，剩余缺口是 fail-open、voice/admin 未覆盖、atomic SQL 条件不够强。

建议:

- 增加 “当前状态 / 已缓解 / 剩余缺口” 表。
- 标注 2026-06-17 段落为历史审计快照。

### P3-3. CSI 默认态文档与 live source 冲突

证据:

- 旧文档说 CSI 默认关闭、不触发、不上传。
- live `ESPC51/components/app_config/app_main_config.h:35-42` 和 `ESPC52/components/app_config/app_main_config.h:35-42` 都是 `MAIN_ENABLE_CSI_SERVICE 1`、`CSI_REPORT_INTERVAL_MS 1000U`。
- `ESPC52/sdkconfig:1499` 又显示 `# CONFIG_ESP_WIFI_CSI_ENABLED is not set`。
- `ESPS3/components/Middlewares/gateway_config/gateway_config.h:36-45` S3 trigger / result ingest 默认 1，interval 100ms。

建议:

- 先决定正式默认态，再同步 docs、project summary、macros、sdkconfig。
- 特别标明 C52 当前 “宏开、Kconfig 关” 的混合态。

### P3-4. 顶层 repo 与 `ESP-server` 嵌套 repo 边界不清

证据:

- 当前存在 `./.git` 与 `./ESP-server/.git` 两个 git root。
- 顶层 repo 和嵌套 repo 都可能管理 `ESP-server` 相关文件；没有 `.gitmodules`。

风险:

- 发布时容易漏提交、重复提交、或在错误 repo 判断状态。

建议:

- 明确 submodule / subtree / 完全拆仓策略。
- 若保留嵌套 repo，顶层停止跟踪 `ESP-server/**` 并写清发布流程。

### P3-5. 供应链和构建复现细节

证据:

- `tools/csi-debug-web/package-lock.json` 使用 `registry.npmmirror.com`，`ESP-server/package-lock.json` 使用 npmjs registry。
- `sqlite3`、`@serialport/bindings-cpp` 有 native install script。
- C51/C52 `idf_component.yml` 写 `idf >=4.1.0`，但 lockfile 实际解析 ESP-IDF 5.5.4；ESPS3 已要求 `>=5.5.0`。
- `ESP-server/docs/deploy-branches.md` 部署示例使用 `npm install`。

建议:

- 统一 registry 策略。
- CI / production 使用 `npm ci --omit=dev`。
- C51/C52 manifest 对齐到实际支持下限。

## 6. 验证记录

通过:

- `find . -maxdepth 3 -type d -name .git -print`
  - 发现 `./.git` 与 `./ESP-server/.git`。
- `ESP_SERVER_DB_PATH=<temp> VOICE_TURN_MOCK=1 GATEWAY_AUTH_TOKEN=<temp> USER_DATA_DELETE_TOKEN=<temp> npm test` in `ESP-server`
  - smoke regression passed。
- `npm test` in `tools/csi-debug-web`
  - 15 tests passed。
- `rg --files ... | xargs -n 1 node --check`
  - ESP-server 与 csi-debug-web JS syntax check passed。
- `npm audit --registry=https://registry.npmjs.org --omit=dev --json` in `tools/csi-debug-web`
  - 0 vulnerabilities。

失败 / 风险:

- `npm audit --registry=https://registry.npmjs.org --omit=dev --json` in `ESP-server`
  - 1 high vulnerability: transitive `undici <=6.26.0`。

未覆盖:

- 未跑 ESP-IDF firmware build。
- 未连接真实 C5/S3 硬件。
- 未跑浏览器 Playwright UI 回归。
- 未运行真实 ASR/LLM/TTS 上游。

## 7. 建议修复顺序

1. 先封堵 Server truth 写入:
   - `/api/csi/behavior` 下线或强鉴权 + forbidden-key 递归扫描。
   - Gateway auth fail-closed，生产强制 token。
   - voice/prompt/log cleanup/command creation/smart-home control/LLM structured 加 admin/operator auth。

2. 再处理凭据和数据泄露:
   - 轮换已暴露 WiFi / SoftAP credentials。
   - S3 Server URL / token 改安全配置注入。
   - 移除 git 跟踪的 `ESP-server/db/database.db`。
   - 清理含密构建产物。

3. 然后收紧 command / device identity:
   - Device binding 改为预注册/配对。
   - command/smart-home ACK 增加 gateway/device/status 原子条件。
   - S3 local id cast 前严格校验。

4. 最后同步文档与工具:
   - 更新 CSI canonical docs。
   - 标注历史审计快照。
   - debug web legacy occupancy 改显式 legacy mode。
   - public mock fallback 改 production unavailable 状态。

## 8. 本次只读约束确认

本报告之外，本次审计未有意修改业务代码、固件源码、后端源码、前端源码、配置文件或数据库。仓库在审计前已经存在大量未提交改动和未跟踪过程文件；这些应视为本次审计的背景状态，而不是本次报告生成造成的代码变更。
