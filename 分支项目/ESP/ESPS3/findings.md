# ESPS3 CSI 上传调查记录

- 顶层仓库已有大量未提交改动；本任务必须只触碰 `ESPS3/` 内相关文件并保留现状。
- 历史记录提示 `network_worker` 已是 server readiness 和 latest-only CSI coalescing 的主要边界，但必须以当前源码为准。
- 最近 ESPS3 压力优化已涉及 CSI 日志节流、worker yield 和上传软成功；本次不能破坏这些稳定性措施。
- 用户明确要求 server payload 只含 `gateway_id`、`type`、`schema_version`、`timestamp_ms`、`links` 和 `fused_state` 三字段。
- `sensor_aggregator_handle_csi_fact()` 只负责更新本地 latest fact、调用 `protocol_adapter_build_csi_event_v2_json()`，再提交 `NETWORK_WORKER_SERVER_JSON_CSI_EVENT`；本地状态与上云构造边界清晰。
- `csi_fusion_feature_t` / `csi_fusion_link_state_t` 仍保存 motion/confidence/quality/RSSI/energy/variance/CV；`csi_fusion_fact_t` 只保存融合状态、融合分数、置信度和 links，适合作为精简上云源。
- 当前 `network_worker_submit_server_json()` 已把 CSI 写入 latest slot 并用 generation 丢弃过期 work item，但发送 work item 仍进入普通 `s_work_queue`；需要进一步核对锁、失败 dirty 标记和重入队行为。
- 当前 server JSON builder 输出旧 canonical 形态：`schema_version`、`trace_id`、`tick_id`、匿名 `links`、字符串 `fused_state`、顶层 `confidence`、`timestamp_ms`。它缺少目标中的 `type`、`gateway_id` 和 `fused_state.motion_score`，并含本次禁止的额外调试标识。
- 旧 builder 把内部 `S3_TO_C51/S3_TO_C52` 映射为 `link_0/link_1`，注释说明这是当前 ESP-server 严格校验要求；必须只读核对 live backend 后再决定兼容策略。
- live ESP-server 的 `csiMotionService` 对旧 7 个顶层键、`schema_version="v2"`、字符串 `fused_state` 和连续 `link_N` 做 exact validation；目标新 JSON 会被当前 Server 以 HTTP 400 拒绝，因此 Server 后续必须做接收兼容。
- 最小实现只需修改 `protocol_adapter.c` 和 `network_worker.c`：前者按 fused fact 构造白名单 JSON，后者由现有 upload worker 直接消费单槽 latest，不触碰融合、local HTTP 或 transport 层。
- 失败语义采用单槽 dirty：HTTP 失败不 requeue；临时发送副本释放，新融合 generation 继续覆盖 latest，成功只清除同 generation 的 pending 状态。
- `server_client_post_csi_event_json()` 原本复用通用最多 5 次的退避重试；CSI 必须改为单次 `perform_json_once()`，其他 Server API 保留原重试策略。
- CSI 等待全局 HTTP slot 超时时属于本地资源竞争；必须与真实网络失败区分，否则连续 generation 会误关 `server_ready` 并暂停普通上云。
- Server probe 共享同一 HTTP slot；probe slot busy 也必须保持当前 ready 状态，不能累计失败计数。

## 2026-07-10 `/kernel/csi_event` 兼容复查

- 当前 `protocol_adapter_build_csi_event_v2_json()` 输出 6 键 body：`type`、数字 `schema_version=2`、`gateway_id`、内部物理 `links`、对象型 `fused_state`、`timestamp_ms`。
- 当前 `network_worker.c` 的 final-payload validator 与上述 6 键形状绑定，因此会在发送前认可一个 Server 实际拒绝的 body。
- live Server `validateCanonicalCsiEventV2()` 要求顶层键严格且恰好为：`schema_version`、`trace_id`、`tick_id`、`fused_state`、`confidence`、`links`、`timestamp_ms`。
- Server 要求 `schema_version` 为字符串 `"v2"`，`fused_state` 为 `IDLE/MOTION/HOLD` 字符串，`links` 为从 `link_0` 起连续的 canonical id 数组；任何额外键（包括 body 内 `gateway_id`）都会触发 `INVALID_CANONICAL_CSI_EVENT`。
- `/kernel/csi_event` route 从 gateway auth/binding 取得可信 `gateway_id`，S3 的 `server_client` 已通过 `X-Gateway-Id`/token headers 携带身份，因此从 body 移除 `gateway_id` 不会丢失网关身份。
- smoke regression 的成功样例与 validator 完全一致；加入 `device_id` 等额外键的用例明确预期 `INVALID_CANONICAL_CSI_EVENT`。
- 最小兼容方向是恢复严格 7 键 canonical body，并让 `network_worker` 对最终 JSON 的键、类型、状态、连续 `link_N` 映射执行同构校验与字段摘要日志。
- `csi_fusion_fact_t` 已完整保留 `schema_version=2`（内部数值）、`trace_id`、`tick_id`、`links`、`fused_state`、`motion_score`、`confidence`、`timestamp_ms`；兼容修复只需改变上云序列化投影，不需修改 fusion 或 C5。
- Server canonical schema 不接收独立 `motion_score`；它把顶层 `confidence` 同时作为持久化 `motion_score`。因此最终 body 应使用 `fact->confidence`，而本地融合 `fact->motion_score` 继续保留在内部 telemetry/debug 面。
- `server_client.perform_json_once()` 已在所有 JSON POST 上设置 `X-Gateway-Id`、`X-Device-Id` 和 gateway token；可信 gateway 身份可在不污染 strict canonical body 的前提下保留。
- 当前 `read_csi_metadata()` 同时兼容字符串和对象型 `fused_state`，恢复 canonical 字符串后 latest-only cadence/state tracking 不受影响。
- 当前 final-payload validator 必须从 6 键业务形状同步改为 7 键 canonical 形状，否则 builder 修正后会被 S3 自己拒绝。
- 顶层历史提交 `0e15c2d` 中的旧 builder 正好生成 Server 当前要求的 7 键并将 `S3_TO_C51/S3_TO_C52` 映射为 `link_0/link_1`，可作为低风险恢复依据。
- Server 严格 CanonicalEvent v2 合同来自提交 `90ea7f4`，后续 validator 形状未发生变化；当前故障是 ESPS3 序列化回归，而不是 Server schema 漂移。
- 已实现的 link 投影按 active-link ordinal 输出连续 `link_0...link_n`，同时通过 `CSI_SERVER_LINK_MAP internal_link=... server_link=...` 保留物理到 canonical 的可观测映射；这样仅 C52 活跃时也会生成 Server 可接受的单元素 `["link_0"]`。
- 最终 JSON 现在只序列化 7 个 Server canonical 字段；`gateway_id` 不进 body，继续由 `server_client` 的认证 header 携带。
- `network_worker` final validator 已同步检查 exact keys、`"v2"`、canonical trace、整数 tick/timestamp、状态、置信度和连续 links，并输出节流的 `CSI_SERVER_FINAL_SCHEMA` 摘要。
- 独立审查确认 strict schema、single-active-link 映射、C 格式化和 header 身份处理均无阻断问题；仅发现并修复旧 final-links 日志未节流的问题。
- 完整 ESP-IDF 5.5.4 构建通过，固件大小 `0x1077b0`，最小应用分区剩余 `0x5f8850`（85%）。
- 修改前后 C51/C52/ESP-server 顶层 diff SHA-256 均为 `49a589394a1c279de6d20ce7ace9f2745d44e406a0360a65c29c334a6937db9b`，嵌套 ESP-server diff SHA-256 均为 `93fc94c7644d3fc5ba87323603e46650b7e18c508b49cc7fc90eb39f1f3acfc6`；禁止目录未被本任务改动。
