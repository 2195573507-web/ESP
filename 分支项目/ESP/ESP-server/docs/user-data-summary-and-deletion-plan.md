# 服务器端每日/每周总结与数据删除能力计划

## 1. 目标与边界

本计划面向正式上线的数据管理能力，不是开发期 reset-test-data，也不是仅清理 AI 记忆或仅清理 daily/weekly summary。

目标：

- 支持服务器端每日上传总结。
- 支持服务器端每周上传总结。
- 支持用户删除服务器端收集和存储的用户数据与设备运行数据。
- 将每日总结、每周总结、用户画像、环境画像、经验记忆、关系记忆纳入可删除范围。
- 后端 API 预留给未来 Dashboard 直接接入。

本轮边界：

- 本计划当前只涉及 ESP-server 后端规划。
- 本轮不实现代码，不修改 `server.js`、`src/`、`public/`、`scripts/`、`package.json`。
- 本轮不修改真实 `db/database.db`。
- 本轮不执行 `DELETE`、`DROP`、`TRUNCATE`、`VACUUM` 等破坏性 SQL。
- 本轮不引入依赖。
- Dashboard 前端本轮不改；后端 API 需要为未来 Dashboard 数据管理页预留字段。

审计口径：

- 当前真实 `db/database.db` 只读检查到 `asr_records`、`llm_records`、`sensor_records` 三张业务表，另有 SQLite 内部 `sqlite_sequence`。
- 源码启动流程会通过 `server.js` 调用 `src/db/*` 的 ensure 方法创建更多后端表。本计划将“当前真实库已存在表”和“源码管理的运行期表结构”分开说明。
- 未在当前源码或只读 schema 中确认的表、字段会标注为“待代码确认”。

## 2. 当前代码现状

后端入口：

- `server.js` 负责创建数据库连接、挂载路由、初始化表结构。
- 表初始化来自 `src/db/records.js`、`src/db/sensorRecords.js`、`src/db/deviceStatus.js`、`src/db/voiceTurns.js`、`src/db/commands.js`、`src/db/memory.js`、`src/db/agentState.js`。
- 新能力应继续拆到 `src/db/`、`src/services/`、`src/routes/`、`src/jobs/`，不要把业务逻辑堆回 `server.js`。

当前已具备的 memory/job/API 能力：

- 记忆基础 API：`/api/conversation/turns`、`/api/memory/daily`、`/api/memory/profile`、`/api/memory/corrections`。
- Agent 状态基础 API：`/api/environment/profile`、`/api/memory/experience`、`/api/memory/relation`、`/api/reminders/rules`、`/api/reminders/events`、`/api/emergency/events`、`/api/csi/behavior`、`/api/lcd/status`、`/api/lcd/display`。
- Job 入口：`POST /api/jobs/daily-summary/run`、`POST /api/jobs/weekly-profile/run`、`GET /api/jobs/memory`。
- 设备与运行数据 API：`/sensor`、`/sensor/latest`、`/sensor/history`、`/api/device/v1/ingest`、`/api/device/v1/status`、`/api/device/v1/modules/status`、`/api/device/v1/context`、`/api/commands/*`、`/api/voice/turn`、`/api/voice/prompt`。

当前 daily/weekly job 状态：

- `src/jobs/memoryJobs.js` 当前只创建 `memory_job_runs` 任务入口。
- `runDailySummaryJob` 在请求体包含 `summary` 时会把该 summary 写入 `daily_memory`，状态为 `candidate`。
- `runWeeklyProfileJob` 当前只创建 `weekly_profile` job 记录，不生成 weekly summary、profile、environment profile、experience memory 或 relation memory。
- 当前未发现真正的每日聚合逻辑，也未发现真正的每周聚合逻辑。

当前删除能力：

- 当前未发现正式的用户数据删除 API。
- 当前未发现 `GET /api/user-data/summary`、`POST /api/user-data/delete/preview`、`POST /api/user-data/delete`、`GET /api/user-data/deletion-runs`、`GET /api/user-data/export`。
- `public/app.js` 中的 `clear-logs` 仅清理当前页面内存里的临时操作记录，并明确提示服务器记录未变更；它不是服务器数据删除能力。

当前 soft delete 字段：

- 多张 memory/agent 表已有 `status`，例如 `daily_memory`、`long_term_profile`、`memory_corrections`、`memory_job_runs`、`environment_profile`、`experience_memory`、`relation_memory`、`reminder_rules`、`reminder_records`、`emergency_events`。
- `command_queue` 和 `voice_turns` 也有 `status`，但它们的 `status` 当前用于命令/语音生命周期，不等同于删除状态。
- 当前未确认任何业务表已有 `deleted_at` 或 `delete_reason` 字段。
- 当前未确认现有 status 枚举支持 `deleted`。

当前测试隔离：

- `scripts/smoke-regression.js` 使用 `ESP_SERVER_DB_PATH` 指向临时 SQLite DB。
- smoke 测试会创建 legacy schema、启动临时 server、写入临时数据，并在测试结束后清理临时目录。
- 后续数据删除能力测试应继续使用该临时 DB 方式，禁止污染真实 `db/database.db`。

## 3. 数据表分类

### 3.1 可删除：AI 记忆、总结、画像数据

以下表来自 `src/db/memory.js` 和 `src/db/agentState.js` 的源码审计。当前真实 `db/database.db` 是否已经初始化这些表，取决于服务启动和迁移历史；真实库当前只读检查未看到这些表。

| 表 | 用途 | 是否可删除 | 推荐删除模式 | 字段现状与迁移需求 |
| --- | --- | --- | --- | --- |
| `conversation_turns` | 保存对话 turn，包含输入、回复、结构化输出和命令 ID | 可删除 | soft_delete + hard_delete | 当前有 `memory_level`、`created_at`、`updated_at`，无 `status/deleted_at/delete_reason`；建议新增 `status`、`deleted_at`、`delete_reason` |
| `daily_memory` | 保存每日摘要候选；未来也可承载 weekly summary | 可删除 | soft_delete + hard_delete | 当前有 `status`，无 `deleted_at/delete_reason`；当前无 `memory_type`，weekly summary 复用该表需新增 `memory_type` 或另建 `weekly_memory` |
| `long_term_profile` | 保存长期用户画像候选或 active 画像 | 可删除 | soft_delete + hard_delete | 当前有 `status`、`correction_count`、`created_at`、`updated_at`，无 `deleted_at/delete_reason`；需扩展 status 或新增删除字段 |
| `memory_corrections` | 保存用户纠错记录 | 可删除 | soft_delete + hard_delete | 当前有 `status`、`created_at`、`updated_at`，无 `deleted_at/delete_reason`；需扩展删除字段 |
| `memory_job_runs` | 保存 daily/weekly job 运行记录 | 可删除但需谨慎 | soft_delete + hard_delete | 当前有 `status`、`created_at`、`updated_at`、`completed_at`，无 `deleted_at/delete_reason`；作为 job/audit 类记录需单独 scope |
| `environment_profile` | 保存环境画像候选 | 可删除 | soft_delete + hard_delete | 当前有 `status`、`created_at`、`updated_at`，无 `deleted_at/delete_reason`；需扩展删除字段 |
| `experience_memory` | 保存经验记忆候选 | 可删除 | soft_delete + hard_delete | 当前有 `status`、`created_at`、`updated_at`，无 `deleted_at/delete_reason`；需扩展删除字段 |
| `relation_memory` | 保存关系记忆候选 | 可删除 | soft_delete + hard_delete | 当前有 `status`、`created_at`、`updated_at`，无 `deleted_at/delete_reason`；需扩展删除字段 |

说明：

- `daily_memory` 当前没有 `memory_type` 字段；如果要在同表存 weekly summary，必须新增 `memory_type`，例如 `daily_summary`、`weekly_summary`。
- 如果不新增 `memory_type`，建议新增 `weekly_memory` 表；该表当前未在源码中确认，属于待代码实现。
- 画像类数据默认不应 hard delete 前直接消失，建议第一版默认 soft delete，hard delete 作为强确认危险操作。

### 3.2 可删除：设备运行与上传数据

以下表来自真实 DB 只读 schema 与源码表初始化审计。

| 表 | 用途 | 是否可删除 | 推荐删除模式 | 是否影响 daily/weekly summary |
| --- | --- | --- | --- | --- |
| `asr_records` | legacy ASR 文本记录，当前真实 DB 已存在 | 可删除 | hard_delete 或补字段后 soft_delete + hard_delete | 影响对话/语音原始输入统计 |
| `llm_records` | legacy LLM prompt/response 记录，当前真实 DB 已存在 | 可删除 | hard_delete 或补字段后 soft_delete + hard_delete | 影响对话/LLM 回复统计；注意 prompt 中可能包含用户输入 |
| `sensor_records` | 传感器上传数据，当前真实 DB 已存在；源码会补充设备协议与空气状态字段 | 可删除 | hard_delete 或补字段后 soft_delete + hard_delete | daily/weekly 的环境、设备健康、上传趋势主要来源 |
| `voice_turns` | `/api/voice/turn` 诊断日志，保存请求、状态、错误、耗时和字节数 | 可删除 | soft_delete + hard_delete | 影响语音交互次数、失败率、耗时统计 |
| `command_queue` | 命令队列、命令状态、结果与错误记录 | 可删除 | soft_delete + hard_delete | 影响命令执行次数、成功率、失败原因统计 |
| `device_status` | 设备整机最近状态快照 | 可删除 | hard_delete 或补字段后 soft_delete + hard_delete | 影响设备在线、延迟、重启计数统计；删除后可由设备上报恢复 |
| `device_module_status` | 模块最近状态快照 | 可删除 | hard_delete 或补字段后 soft_delete + hard_delete | 影响模块在线、延迟、可用性统计；删除后可由设备上报恢复 |
| `csi_behavior_events` | 预留 CSI 行为事件记录 | 可删除 | soft_delete + hard_delete | 影响行为趋势与 presence/motion 类总结；当前 CSI 仅协议预留 |
| `lcd_status` | 预留 LCD 状态快照 | 可删除 | hard_delete 或补字段后 soft_delete + hard_delete | 影响屏幕状态统计；当前 LCD 底层未接入 |
| `reminder_rules` | 提醒规则记录 | 可删除 | soft_delete + hard_delete | 影响主动提醒配置和规则总结 |
| `reminder_records` | 提醒事件/运行记录 | 可删除 | soft_delete + hard_delete | 影响提醒触发次数、确认/取消统计 |
| `emergency_events` | ESP 上报紧急事件记录 | 可删除但需谨慎 | soft_delete + hard_delete | 影响安全事件统计；建议 preview 中标高 danger |

说明：

- `sensor_records`、`asr_records`、`llm_records` 当前真实库中没有 `status/deleted_at/delete_reason`。
- `device_status`、`device_module_status` 是状态快照，不是结构配置；可以纳入用户数据删除范围，但不能影响表结构和索引。
- `emergency_events` 可能具有审计价值，建议用户明确选择相关 scope 后再删除，不建议在普通 `device_history` 的默认 soft_delete 中悄悄清理。

### 3.3 谨慎处理：审计与 job 记录

| 表 | 当前状态 | 默认策略 | include_audit_logs 策略 |
| --- | --- | --- | --- |
| `memory_job_runs` | 源码已确认，保存 daily/weekly job 入口和结果状态 | `jobs` scope 可删除；`all_user_data` 可包含 | 不属于删除审计本身，可随 `jobs` 删除 |
| `data_deletion_runs` | 已实现，保存 preview/delete 审计 | 默认保留且查询默认过滤已 soft-deleted 审计 | 仅当 `include_audit_logs=true` 且 scope 明确允许时删除旧记录 |
| 其他 job/audit 表 | 当前未确认 | 待代码确认 | 待代码确认 |

`data_deletion_runs` 必须默认保留，因为 hard delete 后用户需要知道曾经发生过什么删除操作。允许删除审计日志会降低可追溯性，必须在 preview 返回中明确提示。

### 3.4 不允许删除：系统结构与能力配置

| 表或对象 | 来源 | 不允许被 `all_user_data` 删除的原因 |
| --- | --- | --- |
| `sqlite_sequence` | SQLite 内部表，真实 DB 已确认 | SQLite 自增序列内部结构，不属于用户数据删除 API 的业务目标 |
| schema/migration 相关结构 | `src/db/migrations.js` 管理列与唯一索引迁移 | 删除会破坏后端启动和兼容迁移 |
| 表结构、索引、触发器、视图 | SQLite schema | 删除数据 API 只能处理行数据，不能清表结构 |
| `device_capabilities` | `src/db/commands.js` | 设备能力定义与命令白名单适配相关，删除会导致命令系统无法判断设备支持能力 |
| 命令白名单定义 | `src/commands/schema.js`，非表 | 系统能力配置，不属于用户数据 |
| 基础配置表 | 当前未确认存在 | 如后续新增配置表，默认不纳入 `all_user_data`，需单独审计 |

如果未来要支持“解绑设备并删除能力注册”，应设计独立 scope，例如 `device_registration`，不能混入 `all_user_data`。

## 4. 每日总结功能设计

接口：

```http
POST /api/jobs/daily-summary/run
```

请求体建议：

```json
{
  "date": "2026-06-09",
  "force": true,
  "dry_run": false
}
```

daily runner 应读取：

- `sensor_records`
- `device_status`
- `device_module_status`
- `voice_turns`
- `command_queue`
- `conversation_turns`
- `memory_corrections`

daily runner 应产出：

- `stats_json`：结构化统计 JSON，例如传感器样本数、上传延迟、设备在线窗口、语音 turn 数、命令执行数。
- `summary`：自然语言 summary。
- `evidence`：摘要依据，保存来源表、时间窗口、样本 ID 或计数。
- `sample_count`：本次窗口参与统计的样本数。
- `window_start` / `window_end`：日期窗口。
- `confidence`：摘要置信度。
- `status`：默认 `candidate`，人工或后续流程确认后才能成为 `active`。

写入：

- `daily_memory`
- `memory_job_runs`

建议新增或扩展字段：

- `daily_memory.memory_type`：`daily_summary`。
- `daily_memory.input_json`：保存统计输入摘要。
- `daily_memory.raw_json`：保存 LLM 原始输出或 runner 结果。
- `daily_memory.deleted_at`、`daily_memory.delete_reason`：用于 soft delete。
- 如果需要更强结构化，新增 `summary_stats_json`、`window_start`、`window_end`、`sample_count`；这些字段当前未确认，待代码实现。

行为要求：

- `force=false` 时，如果同一 `date + memory_type=daily_summary` 已存在未删除记录，应避免重复生成，并返回已有记录或 skipped job。
- `force=true` 时允许重新生成，但必须只读取未 deleted/inactive 的数据。
- `dry_run=true` 时只返回预估统计和 summary，不写 `daily_memory`，可选择写或不写 `memory_job_runs`；建议不写 job，除非请求显式 `record_job=true`。
- 所有查询必须过滤 deleted/inactive 数据，不能把已删除数据重新纳入总结。
- hard delete 原始数据后，daily runner 不能再基于已删除原始数据生成 summary。

## 5. 每周总结功能设计

接口：

```http
POST /api/jobs/weekly-profile/run
```

请求体建议：

```json
{
  "week_end": "2026-06-09",
  "force": true,
  "dry_run": false
}
```

weekly runner 应读取：

- 最近 7 天未删除的 `daily_memory`。
- 必要时补查未删除的 `sensor_records`、`voice_turns`、`command_queue`、`conversation_turns`。
- `long_term_profile` 中现有 `candidate` 或 `active` 画像。
- `memory_corrections` 中未删除的纠错记录。

weekly runner 应产出：

- weekly summary。
- 用户画像候选。
- 环境画像候选。
- 经验记忆候选。
- 关系记忆候选。
- 设备健康趋势，例如上传延迟、在线状态、模块可用性、语音成功率、命令成功率。

写入：

- 推荐方案 A：写入 `daily_memory`，新增 `memory_type=weekly_summary`。
- 备选方案 B：新增 `weekly_memory` 表。
- `long_term_profile`
- `environment_profile`
- `experience_memory`
- `relation_memory`
- `memory_job_runs`

方案比较：

| 方案 | 优点 | 缺点 | 推荐 |
| --- | --- | --- | --- |
| `daily_memory.memory_type=weekly_summary` | 复用现有 API、表和索引；删除 summaries scope 更简单 | 需要新增 `memory_type` 和周窗口字段，否则 daily/weekly 混杂 | 第一版推荐 |
| 新增 `weekly_memory` | daily/weekly 边界清晰，字段可专门设计 | 新增表、API、测试和删除 scope，工作量更大 | 后续增强可选 |

要求：

- 用户画像默认 `candidate`。
- 不允许一次出现的偏好直接成为永久 `active`。
- 必须保留 evidence，至少包含来源 daily summary、原始数据时间窗口和相关 turn/profile/correction ID。
- 必须过滤 deleted/inactive 数据。
- `force=true` 重新生成时不能复活已删除数据。
- hard delete 原始数据后，weekly runner 不能再基于已删除原始数据生成画像。

## 6. 全量数据删除功能设计

服务器端收集和存储的数据都应被纳入可删除能力，但系统结构、设备能力定义、schema/migration 和服务运行必需配置不能被 `all_user_data` 误删。

建议新增后端模块：

- `src/db/userDataDeletion.js`：表分类、scope 映射、删除审计表、迁移字段。
- `src/services/userDataService.js`：summary、preview、delete、export 逻辑。
- `src/routes/userDataRoutes.js`：HTTP API。

### 6.1 数据概览

```http
GET /api/user-data/summary
```

返回字段：

- `scope`
- `display_name`
- `description`
- `count`
- `date_range`
- `last_updated_at`
- `last_deleted_at`
- `supports_soft_delete`
- `supports_hard_delete`
- `danger_level`

示例结构：

```json
{
  "ok": true,
  "scopes": [
    {
      "scope": "device_history",
      "display_name": "设备运行数据",
      "description": "传感器上传、语音 turn、命令队列、设备状态与模块状态",
      "count": 1200,
      "date_range": {
        "from": "2026-06-01T00:00:00.000Z",
        "to": "2026-06-09T23:59:59.999Z"
      },
      "last_updated_at": "2026-06-09T20:00:00.000Z",
      "last_deleted_at": "",
      "supports_soft_delete": true,
      "supports_hard_delete": true,
      "danger_level": "high"
    }
  ]
}
```

### 6.2 删除预检查

```http
POST /api/user-data/delete/preview
```

请求体：

```json
{
  "scope": "memory | summaries | profiles | conversations | device_history | jobs | all_user_data",
  "mode": "soft_delete | hard_delete",
  "include_audit_logs": false
}
```

返回：

- affected tables
- affected counts
- danger level
- required confirm text

preview 只做统计，不修改业务数据。当前实现会写入 `data_deletion_runs(request_type="preview")` 审计记录，便于追踪 scope、reason、requested_by、include_audit_logs 与 affected counts。

### 6.3 执行删除

```http
POST /api/user-data/delete
```

请求体：

```json
{
  "scope": "memory | summaries | profiles | conversations | device_history | jobs | all_user_data",
  "mode": "soft_delete | hard_delete",
  "confirm": "DELETE",
  "reason": "user_request",
  "include_audit_logs": false
}
```

执行要求：

- `confirm` 必须等于 `DELETE`。
- 必须先计算 affected counts。
- 必须写 `data_deletion_runs`。
- 必须使用事务。
- 失败必须回滚。
- `hard_delete` 不默认 `VACUUM`。
- `all_user_data` 不删除 `device_capabilities`、schema/migration、表结构、索引、白名单、配置。

### 6.4 删除记录

```http
GET /api/user-data/deletion-runs
```

用于未来 Dashboard 展示删除历史。默认返回最近记录，支持 `scope`、`mode`、`status`、`limit` 过滤。

### 6.5 可选导出

```http
GET /api/user-data/export
```

建议作为删除前的安全增强能力。第一版可只支持 JSON 导出，按 scope 输出各表数据。导出功能应使用同一套 scope 映射，且默认不导出 `device_capabilities`、schema/migration 或配置类数据。

## 7. 删除 scope 设计

`summaries`：

- `daily_memory` 中 `memory_type=daily_summary` 的记录。
- `daily_memory` 中 `memory_type=weekly_summary` 的记录。
- 如果当前尚未新增 `memory_type`，第一版需要用 `source` 或新增字段区分；仅靠 `memory_date` 不足以可靠区分，待代码实现。

`profiles`：

- `long_term_profile`
- `environment_profile`
- `experience_memory`
- `relation_memory`

`memory`：

- `summaries`
- `profiles`
- `memory_corrections`
- 推荐默认不包含 `conversation_turns`，避免用户只想删除画像/总结时误删原始对话。

`conversations`：

- `conversation_turns`
- `asr_records`
- `llm_records`

说明：`asr_records`、`llm_records` 是 legacy 记录，也包含用户输入和模型回复，应纳入 conversations 或 legacy_conversations 子类。第一版可在 `GET /api/user-data/summary` 中把它们显示为 `conversations.legacy`。

`device_history`：

- `sensor_records`
- `voice_turns`
- `command_queue`
- `device_status`
- `device_module_status`
- `csi_behavior_events`
- `lcd_status`
- `reminder_rules`
- `reminder_records`
- `emergency_events`

`jobs`：

- `memory_job_runs`
- 其他 job run 表：当前未确认，待代码确认。

`all_user_data`：

- `memory`
- `conversations`
- `device_history`
- `jobs`

`all_user_data` 不得删除：

- `sqlite_sequence`
- `device_capabilities`
- schema/migration
- 表结构
- 索引
- 命令白名单
- 基础配置表

## 8. soft_delete / hard_delete 策略

### soft_delete

soft delete 是默认模式。

要求：

- 有 `status/deleted_at` 字段的表标记 deleted。
- 查询接口默认不返回 deleted。
- prompt 拼接必须忽略 deleted/inactive。
- daily/weekly job 必须忽略 deleted/inactive。
- summary/profile 查询接口默认只返回未删除数据。

推荐统一字段：

- `status`
- `deleted_at`
- `delete_reason`
- `updated_at`

当前表状态：

- 已有 `status` 但无 `deleted_at/delete_reason`：`daily_memory`、`long_term_profile`、`memory_corrections`、`memory_job_runs`、`environment_profile`、`experience_memory`、`relation_memory`、`reminder_rules`、`reminder_records`、`emergency_events`。
- 有 `status` 但 status 用作业务生命周期：`voice_turns`、`command_queue`。这两类表建议新增 `deleted_at/delete_reason`，不要直接把业务 `status` 改成 `deleted`，否则会破坏语音/命令状态语义。
- 无删除字段：`asr_records`、`llm_records`、`sensor_records`、`device_status`、`device_module_status`、`csi_behavior_events`、`lcd_status`。

推荐方案：

- 第一版为所有可删除表补 `deleted_at`、`delete_reason`、`updated_at`。
- 所有 soft_delete 都只写 `deleted_at`、`delete_reason`、`updated_at`。
- 对生命周期表和画像/记忆类表都只依赖 `deleted_at IS NULL` 过滤，不改变原业务 `status`。

### hard_delete

hard delete 是危险操作。

要求：

- 真正执行 SQL `DELETE`。
- 必须 `confirm=DELETE`。
- 必须写 `data_deletion_runs`。
- 必须事务化。
- 失败回滚。
- 不默认 `VACUUM`。
- preview 中必须显示每张表 affected counts。

如果某些表没有软删除字段：

- 方案 A：新增 `deleted_at`、`delete_reason`、`updated_at`，必要时新增 `status`。推荐第一版采用。
- 方案 B：第一版仅支持 hard_delete。风险是不可恢复，不推荐作为默认模式。
- 推荐：统一补字段，soft_delete 先行；hard_delete 作为强确认选项。

## 9. 审计日志设计

建议新增 `data_deletion_runs`。

字段：

- `id`
- `run_id`
- `scope`
- `mode`
- `request_type`
- `reason`
- `requested_by`
- `include_audit_logs`
- `preview_counts_json`
- `affected_counts_json`
- `started_at`
- `completed_at`
- `status`
- `error_message`
- `created_at`
- `updated_at`
- `deleted_at`
- `delete_reason`

说明：

- 每次 preview 和执行删除都必须记录。
- preview 记录 `request_type="preview"`，执行删除记录 `request_type="delete"`。
- hard_delete 后默认仍保留删除审计。
- `include_audit_logs=true` 才允许清理旧审计记录。
- 审计日志自身被删除会降低可追溯性，preview 必须把该风险标成 `danger_level=critical`。
- 如果删除失败，`data_deletion_runs.status` 应记录 `failed`，并保存 `error_message`；事务回滚后仍应保留失败审计。实现上可在业务删除事务外层写入失败审计，具体实现需谨慎设计。

## 10. 安全保护

必须包含：

- `confirm` 必须等于 `DELETE`。
- 删除前必须 preview 或至少统计 affected counts。
- 删除过程必须使用事务。
- 删除失败必须回滚。
- 不允许清表结构。
- 不允许误删 `device_capabilities`。
- 不允许误删 schema/migration 表。
- 不允许默认 `VACUUM`。
- 删除 API 不能无保护开放。
- 如果当前没有用户系统，第一版建议使用 `USER_DATA_DELETE_TOKEN` 或 `ADMIN_TOKEN`。
- 未来 Dashboard 前端也必须走相同权限校验，不能只靠前端确认弹窗保护。

权限建议：

- `GET /api/user-data/summary` 可要求管理员 token。
- `POST /api/user-data/delete/preview` 必须要求管理员 token。
- `POST /api/user-data/delete` 必须要求管理员 token + `confirm=DELETE`。
- `hard_delete` 和 `include_audit_logs=true` 必须返回更严格的 required confirm text，例如 `DELETE ALL USER DATA`，具体文本待代码确认。

## 11. prompt 与 job 过滤要求

prompt 过滤：

- `llmPromptContextService` 当前通过 `deviceContextService` 注入设备、模块、环境、空气状态和新鲜度上下文。
- 后续所有 prompt 拼接只能读取 active/candidate 且未 deleted 的数据。
- 如果长期记忆后续进入 prompt，需要统一过滤 `deleted_at IS NULL`，并排除 `status IN ('deleted','inactive','archived')` 中不应参与 prompt 的记录。

daily runner 过滤：

- 查询原始数据时排除 deleted/inactive。
- `force=true` 只能重新处理未删除数据。
- `dry_run=true` 也必须按同样过滤逻辑计算，避免 preview 与实际生成不一致。

weekly runner 过滤：

- 查询 summary/profile/conversation 时排除 deleted/inactive。
- hard delete 原始数据后，weekly job 不能再基于已删除原始数据生成画像。
- `memory_corrections` 中被删除或 archived 的纠错不能继续影响画像。

统一 helper 建议：

- `buildActiveWhere(tableAlias, options)`：生成 `deleted_at IS NULL` 和 status 过滤条件。
- `getDeletableTablePolicy(tableName)`：声明某表的删除字段、生命周期字段和 active 状态集合。
- 所有 summary、prompt、list API 共用同一套过滤策略，减少遗漏。

## 12. 前端接入预留

本轮不改前端。

当前前端现状：

- `public/index.html` 有 Dashboard、数据变化曲线、命令面板和“清理日志”按钮。
- `public/app.js` 明确“操作记录只保存在当前页面内存中”，`clear-logs` 只清理页面临时记录，服务器记录未变更。
- 当前未发现服务器数据管理入口。

未来 Dashboard 可接入：

- `GET /api/user-data/summary`：数据管理首页。
- `POST /api/user-data/delete/preview`：删除前弹窗。
- `POST /api/user-data/delete`：确认删除。
- `GET /api/user-data/deletion-runs`：删除历史。
- `GET /api/user-data/export`：数据导出。

前端建议展示：

- `scope`
- `display_name`
- `description`
- `count`
- `date_range`
- `danger_level`
- `supports_soft_delete`
- `supports_hard_delete`
- `last_deleted_at`

危险操作：

- 二次确认。
- 用户输入 `DELETE` 或更严格确认文本。
- 明确提示删除范围和不可恢复后果。
- 区分 `soft_delete` 和 `hard_delete`。
- 显示 affected tables 和 affected counts。
- `include_audit_logs=true` 时使用最高危险提示。

## 13. 测试计划

必须包含：

- 使用临时 SQLite DB，不使用真实 `db/database.db`。
- 插入各类可删除表的示例数据。
- 插入 `device_capabilities` 等不可删除表数据。
- `GET /api/user-data/summary` 返回正确分类和数量。
- `POST /api/user-data/delete/preview` 不删除数据。
- soft_delete 后查询接口不再返回已删除数据。
- soft_delete 后 prompt 拼接不再读取已删除数据。
- soft_delete 后 daily runner 不再读取已删除数据。
- soft_delete 后 weekly runner 不再读取已删除数据。
- hard_delete 后数据确实消失。
- `all_user_data` 不影响 `device_capabilities`。
- `all_user_data` 不影响 schema/migration。
- 删除失败事务回滚。
- `data_deletion_runs` 正确记录 affected counts。
- daily summary 生成后可删除。
- weekly summary 生成后可删除。
- 用户画像生成后可删除。
- `npm test` 通过。

建议补充测试分组：

- `assertUserDataSummaryScopes`
- `assertUserDataDeletePreview`
- `assertUserDataSoftDelete`
- `assertUserDataHardDelete`
- `assertUserDataDeleteRollback`
- `assertDailySummarySkipsDeletedData`
- `assertWeeklyProfileSkipsDeletedData`

回归边界：

- `git diff -- public db/database.db` 应为空。
- `node --check` 覆盖新增后端文件。
- `npm test` 继续走临时 DB。

## 14. 实施阶段

Phase 1：只读审计所有表，明确可删/不可删分类。

Phase 2：实现 `GET /api/user-data/summary`、`POST /api/user-data/delete/preview`、`POST /api/user-data/delete`。

Phase 3：实现 `data_deletion_runs` 审计表。

Phase 4：实现 soft_delete/hard_delete 后端逻辑。

Phase 5：改 prompt 拼接过滤 deleted/inactive。

Phase 6：改 daily/weekly runner 过滤 deleted/inactive。

Phase 7：补 daily/weekly summary 真实生成与写库。

Phase 8：补 smoke regression 测试。

Phase 9：更新 `docs/api.md`。

Phase 10：后期可选 Dashboard 前端接入。

阶段约束：

- Phase 1-9 只涉及后端和文档。
- Phase 10 之前不修改 `public/`。
- 任何阶段都不允许修改真实 `db/database.db` 作为测试数据源。

## 15. 风险点

- 误删系统表：`all_user_data` 如果 scope 映射不严，可能误删 `device_capabilities` 或 schema/migration 相关结构。
- hard_delete 后无法恢复：必须 preview、confirm、事务化，并建议提供 export。
- soft_delete 字段不统一导致过滤遗漏：必须用统一 policy/helper，而不是每个查询手写。
- prompt 读取 deleted 数据：会把用户已删除信息重新带入 LLM。
- weekly job 基于已删除数据重新生成画像：会导致 deleted 数据被“复活”为新画像。
- 删除审计日志自身的保留策略：允许删除审计会降低追溯能力，必须显式确认。
- 没有用户系统时权限边界不足：必须先用 `USER_DATA_DELETE_TOKEN` 或 `ADMIN_TOKEN` 保护。
- `all_user_data` 范围过大：必须 preview affected counts，并按 danger_level 提示。
- 业务 status 与删除 status 混淆：`voice_turns`、`command_queue` 的 `status` 有生命周期含义，不能简单改成 `deleted`。
- 真实库与源码 schema 不一致：当前真实库只有三张业务表，后续实现必须通过可重复迁移处理旧库。
- daily/weekly summary 与原始数据删除策略冲突：必须支持只删总结、只删原始数据、全部删除三类策略。

## 16. 下一轮可执行修改命令

下一轮 Codex 修改命令摘要：

```text
cd /Users/zhiqin/Projects/ESP/ESP-server

只改 ESP-server 后端和 docs/api.md，不改 public/，不改真实 db/database.db。

实现：
1. 新增 user-data summary/preview/delete 后端 API。
2. 新增 data_deletion_runs 审计表。
3. 为可删除表增加 soft_delete 所需字段或统一删除策略。
4. 增加 hard_delete 事务逻辑。
5. 修改 prompt 拼接过滤 deleted/inactive。
6. 修改 daily/weekly runner，使其过滤 deleted/inactive，并补真实 summary 生成与写库。
7. 增加 smoke regression 测试。
8. 更新 docs/api.md。

验证：
node --check server.js
npm test
git diff --check
git diff -- public db/database.db
```

下一轮实现前必须再次只读确认：

- 当前真实库 schema。
- 当前源码表定义。
- 当前 docs/api.md 已有接口。
- 当前 smoke-regression 临时 DB 逻辑。
- 当前 public/ 是否仍无数据管理入口。
