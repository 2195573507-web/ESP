# ESPS3 CSI 融合上传优化计划

## 目标

仅修改 ESPS3，把发往 ESP-server 的逐 link CSI 完整结果改为 latest-only 的融合 `csi_event`，保持 C5 协议、融合算法、local_http、本地调试数据和前端兼容。

## 阶段

1. [complete] 核对实时源码、现有未提交改动和上传链路
2. [complete] 比较可行实现并确认最小边界设计
3. [complete] 实现融合 payload 与 latest-only 独立槽位语义
4. [complete] 静态检查、定向测试和 ESP-IDF 编译
5. [complete] 核对范围并输出交付结果

## 约束

- 不修改 ESPC51、ESPC52、ESP-server、前端或 tools。
- 不修改 CSI 融合算法和 C5 通信协议。
- 保留 S3 内部完整 link 特征、`CSI_RESULT_V2`、`CSI_DEBUG` 和本地调试接口。
- HTTP 失败不得积累旧 CSI；待发送状态始终只保留最新融合快照。
- 不覆盖或回退工作区已有改动。

## 错误记录

| 错误 | 次数 | 处理 |
|---|---:|---|
| 新建 `/goal` 时发现任务已有活动目标 | 1 | 读取并沿用现有目标 |
| 在 `ESPS3/` cwd 下重复使用 `ESPS3/` 路径，行号查询失败 | 1 | 改用 `components/...` 相对路径重跑 |
| planning 检查脚本未识别中文编号阶段，显示 `5/0` | 1 | 人工核对 5 个阶段均为 complete，并以 progress 记录为准 |

---

# `/kernel/csi_event` CanonicalEvent v2 兼容修复

## 目标

仅修改 ESPS3，使 `network_worker` 最终 POST body 严格符合当前 ESP-server 的 Canonical CSI event v2 验收合同，并保留网关身份、融合状态、links 映射和时间戳语义。

## 阶段

1. [complete] 核对 live ESPS3 最终 body、ESP-server validator、smoke 测试与历史实现
2. [complete] 确认 `gateway_id`/版本表达与严格 body 合同的处理方式
3. [complete] 修改 ESPS3 payload builder 和最终 schema 字段日志
4. [complete] 运行定向静态检查与 ESP-IDF 完整构建
5. [complete] 核对最终 diff，确认 C51/C52/ESP-server 未被本任务修改

## 约束

- 不修改 ESPC51、ESPC52、ESP-server、前端或 tools。
- 不回退当前工作树已有修改。
- 以 live `csiMotionService.js` 与 smoke regression 为验收真源。
- 最终 POST body 不携带 Server 明确禁止的额外键。

## 错误记录

| 错误 | 次数 | 处理 |
|---|---:|---|
| 用户要求保留 `gateway_id`、`schema_version=2`，但 live Server body 严格禁止 `gateway_id` 且要求字符串 `v2` | 1 | 用户已确认：gateway 身份保留在 header，内部数值版本保留为 2，最终 body 序列化为 `"v2"` |
| planning 完成性脚本不识别中文编号 `[complete]` 格式，显示 `10/0` | 1 | 人工核对两组共 10 个阶段均为 complete，以 task plan 和 progress 实际状态为准 |
