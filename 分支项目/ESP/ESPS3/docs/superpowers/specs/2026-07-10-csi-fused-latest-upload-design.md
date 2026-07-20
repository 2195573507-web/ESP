# ESPS3 CSI 融合状态 latest-only 上传设计

## 范围

只修改 ESPS3。C5 上报协议、S3 CSI 融合算法、`/local/v1/csi/result` 接收、完整 per-link 特征缓存和本地调试日志保持不变。

## Server Payload

S3 只从 `csi_fusion_fact_t` 构造以下 JSON：

```json
{
  "type": "csi_event",
  "schema_version": 2,
  "gateway_id": "sensair_s3_gateway_01",
  "links": ["S3_TO_C51", "S3_TO_C52"],
  "fused_state": {
    "state": "IDLE",
    "motion_score": 0.38,
    "confidence": 0.72
  },
  "timestamp_ms": 123456789
}
```

不上传 `energy`、`variance`、`cv`、`rssi`、`quality`、`local_hint`、`trace_id`、`tick_id` 或单 link `CSI_RESULT_V2`。

## 数据流

1. `csi_placeholder_gateway` 继续接收并缓存完整 link 特征，输出现有 `CSI_RESULT_V2`、CSI RX、fusion telemetry 和 latest diagnostics。
2. `csi_fusion` 继续按现有算法生成 fused fact，不修改阈值、权重或状态机。
3. `protocol_adapter` 只把 fused fact 投影为字段白名单 JSON。
4. `network_worker` 接管 JSON 所有权并覆盖唯一 latest 槽位；CSI 不进入普通 upload queue。
5. 现有 upload worker 在 Server gate 打开且 cadence 到期时复制当前 latest 快照，锁外执行 HTTP。

## 失败与并发

- latest 槽位和 generation 受现有 CSI mutex 保护。
- HTTP 发送中的快照是临时副本；新融合状态可以同时覆盖 latest 槽位。
- 成功只确认同 generation；发送期间出现的新 generation 仍保持待发送。
- HTTP 失败不 requeue，不新增 retry item，只保留单槽 latest 状态供后续正常 cadence 或更新覆盖。
- CSI HTTP transport 只尝试一次，不对同一 JSON 执行通用 2/5/10/30 秒退避重试。
- HTTP slot 被语音等本地请求占用时，只丢当前 CSI generation，不把本地资源竞争计为 Server 故障。
- 同一 slot 竞争也不会让 Server probe 错误关闭上云 gate。
- 普通 ingest、snapshot、command 队列语义不变。

## 兼容与验证

- 本地 C5/S3 与调试接口不变，前端代码不改。
- 当前 ESP-server `/kernel/csi_event` 只接受旧 canonical v2，部署新 payload 前需要 Server 增加新 shape 的接收/归一化；本任务不修改 Server。
- 验证包括字段白名单扫描、ESPS3 范围 `git diff --check` 和 ESP-IDF 5.5.4 全量编译。
