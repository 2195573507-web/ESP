# ESPS3 雷达稳定性只读审计 - 2026-07-17

## 审计边界与结论

- 审计对象：`ESPS3/components/radar_ld2450/`、`ESPS3/components/Middlewares/radar_domain/`，以及实际接入点 `gateway_orchestrator`、`local_http_server`。
- 执行方式：静态源码、调用关系和边界条件审查；未启动服务器、未 flash、未 monitor、未执行构建或硬件测试。
- 结论：未发现已证实的 P0（立即触发 Guru Meditation、堆破坏或确定的 double free）。发现 2 项 P1、5 项 P2；其中 P1-1 是配置 API 一旦被调用即会触发的 UART 并发竞态，P1-2 是本地 registry 快照的目标计数与数组不一致。
- 架构边界：本轮未改动 C5、ESP-server 或 S3 gateway 的非雷达业务链路；以下修复均可局限在 ESPS3 雷达组件及其适配层。

## 1. 当前雷达链路架构

```text
LD2450 (UART1, 256000 8N1)
  -> radar_rx task (priority 5, 4 KiB stack)
  -> ld2450_uart: driver event queue / read buffer
  -> ld2450_parser: 30-byte frame reassembly, header + tail validation
  -> radar_service latest-frame deep copy + recovery state
  -> radar_local task (priority 3, 4 KiB stack)
  -> coordinate transform -> zone map -> tracker -> spatial snapshot
  -> registry (mutex-protected value snapshot)
  -> radar_diag task (priority 2, 4 KiB stack) / periodic logs

C51/C52 radar HTTP ingress
  -> bounded request body -> cJSON strict schema parser
  -> identity/session admission -> same registry
```

没有独立的 parser、tracker 或 fusion 任务：parser 在 `radar_rx` 内运行，tracker/spatial state 在 `radar_local` 内运行；仓库当前不存在 active 的 radar fusion 模块。`latest_frame`、registry entry 和对外 spatial snapshot 均为结构体值拷贝，不传递堆指针。

## 2. 已确认问题

| ID | 等级 | 问题 | 触发条件 | 代码位置 |
| --- | --- | --- | --- | --- |
| P1-1 | P1 | 配置命令与 RX 共用 UART，但没有实际互斥/停 RX 协议 | 任意调用者调用已公开的 `ld2450_config_execute()`，同时 `radar_rx` 已运行 | `ld2450_config.c:165`，`radar_service.c:119`，`radar_service.c:109` |
| P1-2 | P1 | 本地 registry 快照用“过滤后数量”配“未过滤原始数组” | 目标被距离、房间或 ignore zone 过滤时 | `radar_local_adapter.c:45` |
| P2-1 | P2 | UART/parser 诊断结构体由 RX 写、adapter/diag 无锁读取 | RX 正在更新诊断时，adapter 读取服务诊断 | `radar_service.c:304`，`ld2450_uart.c:168` |
| P2-2 | P2 | `radar_presence` 未接收任何有效帧，公开 service snapshot 长期为 UNKNOWN | 新调用方使用 `radar_service_get_snapshot()` | `radar_service.c:50`，`radar_service.c:291` |
| P2-3 | P2 | 通用 `%s` 防护只验证地址落点，不验证对象仍存活或 NUL 终止 | 将来把临时/非终止缓冲区传给 `RADAR_DIAG_SAFE_STRING` | `radar_diagnostics.c:33` |
| P2-4 | P2 | 空间坐标平移为有符号 32 位加法，极端安装配置可溢出 | 自定义 config 使 `clamp_i32(rotated_*) + origin_offset_*` 超界 | `radar_coordinate_transform.c:38` |
| P2-5 | P2 | `ld2450_target_distance_mm()` 的平方在转为 `uint64_t` 前完成 | 将该公开函数用于两个 `INT16_MIN` 坐标的非 UART 输入 | `ld2450_parser.c:106` |

### P1-1: 配置命令 API 与 UART RX 竞态

`ld2450_config_execute()` 直接调用 `ld2450_uart_write()` 和 `ld2450_uart_read()`，并在等待 ACK 的过程中消耗 UART 字节。与此同时，`radar_rx_task` 持续 drain event queue、read 和 feed parser。`radar_service.c` 虽有 `s_config_active` 检查，但没有任何写入该变量的 API 或调用点，因此它不能停住 RX。

后果不是简单的配置失败：ACK 可被数据解析器吃掉，数据帧可被配置等待循环吃掉，parser 会持续 resync；读失败/无有效帧计数还可能把 recovery 误推入 BACKOFF 并删除 UART driver。该问题目前是潜伏问题，因为本次搜索没有找到 `ld2450_config_execute()` 的调用者，但该公共 API 一经接入即成立。

最小修复：由 `radar_service` 独占 UART，新增一个受同一 service mutex 管理的配置事务入口。该入口应：设置配置中状态、让 RX 在确认点停止读取、完成 enable/command/disable/恢复数据帧、reset parser，再恢复 RX。不要让 `ld2450_config.c` 直接从任意 task 操作 UART；同时为执行超时增加上限和结果日志。

### P1-2: registry snapshot 的计数与目标数组不一致

`registry_snapshot_from_spatial()` 赋值 `current_target_count = accepted_target_count`，随后却 `memcpy` 整个 `raw_targets`。因此某一原始目标若因最大距离、room bounds 或 IGNORE zone 被滤掉，registry 仍保存其 `valid=true` 原始坐标，但计数报告为 0 或更小值。下游若按 `current_target_count` 遍历数组，会得到错误的目标；若扫描 `valid` 位，又会绕开空间过滤规则。

最小修复：明确 registry 的契约二选一。

- 若 registry 是兼容 `radar_snapshot_t` 的原始协议视图：使用 `raw_target_count`，并只复制对应原始数组。
- 若 registry 表示空间过滤后的视图：为其定义/使用与 `radar_spatial_target_t` 一致的数组，或者只把 accepted target 投影回 `radar_target_t` 后复制。

两种情况下都要增加“目标被过滤”测试，并保证 `current_target_count`、数组有效位和诊断输出使用同一集合。

## 3. 内存、生命周期与字符串审计

### 已确认安全项

- parser 的内部 buffer 固定为 `LD2450_FRAME_SIZE`，写前检查长度；满时只丢弃一个字节并 `memmove` 剩余长度，半包和粘包不会越界。帧仅在固定 30 字节、4 字节 header 和尾部 `55 CC` 同时成立后发布：`ld2450_parser.c:188-228`。
- 每帧通过栈变量 `radar_frame_t frame` 回调，但回调立即将内容值拷贝到 `s_latest_frame`；没有保存该栈地址：`ld2450_parser.c:212-220`、`radar_service.c:50-63`。
- spatial snapshot、registry entry 和 tracker snapshot 都执行结构体值拷贝。当前模块没有 `malloc`/`free`；HTTP radar body 由 `read_json_body()` 分配后，在成功、解析失败及读取失败路径均释放：`local_http_server.c:907-936`。
- cJSON 节点只在 `radar_protocol_parse_json()` 内使用，所有成功和错误分支均 `cJSON_Delete(root)`；解析结果被复制到普通结构体后才释放树：`radar_protocol.c:118-245`。
- 本次范围内未发现 `strcpy`、`strcat`、`sprintf`，也未发现局部数组地址被返回或存入全局。

### P2-3: `RADAR_DIAG_SAFE_STRING` 不是完整的字符串生命周期证明

`esp_ptr_in_dram/external_ram/drom` 只能判断地址位于可映射区域，不能证明指针没有被释放、内容一定 NUL 终止或对象在日志格式化期间不会变化。因此该 helper 不能作为任意 `%s` 入参的内存安全边界。

当前实际雷达日志的参数是安全的：source/state/recovery/zone 名称均为静态字符串，`device_id`/`room_id` 来自 mutex 保护后复制到本地 `radar_registry_entry_t` 的定长 NUL 终止数组，`esp_err_to_name()` 返回静态名称。所有 `%lu`、`%llu`、`PRIu64`、`%d`、`%f`（本范围没有 `%f`）已逐项与显式转换匹配，未发现格式不匹配。

最小修复：将 helper 定位为 NULL 防护而不是“指针安全”保证；对任何未来的外部文本先复制到有界本地数组（长度由 `strnlen` 限制）再记录。当前字段无需改变 API 或引入动态内存。

### P2-4/P2-5: 算术溢出硬化

- `radar_coordinate_transform_target()` 在 clamp 后立即与 `int32_t origin_offset_*` 相加。默认配置为 0，当前调用只使用默认配置，因此不是当前可达的崩溃路径；但公开 config API 接受任意 `int32_t` offset，C 有符号溢出是未定义行为。
- `ld2450_target_distance_mm()` 使用 `(uint64_t)(x * x) + (uint64_t)(y * y)`。当前 UART decode 的 X 下界为 -32767，因此正常 LD2450 帧仍落在 32 位安全范围；但该函数接受任意 `radar_target_t`，两个 `INT16_MIN` 坐标会在转换前触发有符号溢出。

最小修复：先转换每个操作数，再计算；平移用 `int64_t` 中间量并 clamp 到 `int32_t`。加入 INT16/INT32 极值单元测试。

## 4. ESP-IDF 资源、阻塞与看门狗审计

### 已确认安全项

- 三个任务都检查 `xTaskCreate()` 返回值；RX 创建失败会清空 task handle 并 deinit UART：`radar_service.c:249-258`。adapter 和 diag 也返回 `ESP_ERR_NO_MEM`：`radar_local_adapter.c:160-172`、`radar_diagnostics.c:337-346`。
- UART driver install、set pin、param config 的错误路径会返回；driver delete 失败会计数。静态 mutex 的生命周期与系统等长，没有重复 delete/free 路径。
- RX 的 UART read 有 100 ms timeout；adapter 100 ms delay；diag 250 ms delay。没有本范围内的无延迟永久轮询。recovery backoff 最大 8 s 且倍增饱和，`uint64_t` 单调时间比较不会发生 32 位 wrap：`radar_uart_recovery.c:57-171`。
- JSON body 限制为 768 字节，target_count 限制为 3，远端内容不会导致数组越界：`radar_protocol.h:14-30`、`radar_protocol.c:205-242`。

### 剩余风险和验证项

- `ld2450_config_execute()` 最坏会进行三段 `timeout_ms` 阻塞读。修复 P1-1 时应限制调用任务及总耗时，不能从 HTTP handler 或高优先级任务直接执行。
- RX、adapter、diag 都固定为 4096-byte stack；代码没有 stack watermark 或溢出 hook 证据。diag 的完整 summary 同时持有 3 个 registry entry 和一个 large spatial snapshot，因此“当前没有栈不足证据”不能替代真机验证。应在后续验证版采集 `uxTaskGetStackHighWaterMark()`，并用 3 targets + 3 tracks + full diagnostics 的最坏日志场景确认余量。
- 当前 radar core/domain 自身不做堆分配，因此未发现隐藏泄漏或碎片化来源。UART driver、FreeRTOS task 和 HTTP server 的分配需要运行时 heap trace 才能证实；日志所示 free=16 MB 只能说明快照，不能证明长期无泄漏。

## 5. 并发、快照与状态机审计

### 已确认安全项

- `s_latest_frame` 由 service mutex 保护，读者取得的是结构体副本：`radar_service.c:273-284`。
- recovery/presence 也在同一 mutex 内读写；registry 使用独立 mutex；对外 spatial snapshot 和 adapter diagnostics 用 `portMUX` 下的深拷贝。没有发现 ring buffer 索引跨 task 共享或任务间共享 `cJSON*`。
- `radar_spatial_state_on_frame()` 以 frame sequence 去重，adapter 的 100 ms poll 不会重复把同一帧输入 tracker：`radar_spatial_state.c:132-165`。
- 状态机为 `OFFLINE -> WAITING_VALID -> VALID`，连续 error/no-valid 才进入 BACKOFF；成功重建后必须获得连续三帧有效数据才 VALID。`sensor_state` 将 OFFLINE/BACKOFF 设为 offline，WAITING_VALID/超时设为 stale，VALID 且帧新鲜才 valid：`radar_uart_recovery.c:85-149`、`radar_spatial_state.c:167-185`。

### P2-1: 诊断快照的无锁读取

`radar_service_get_diagnostics()` 在 service mutex 外复制 parser diagnostics，且 `ld2450_uart_get_diagnostics()` 直接复制 UART 全局 diagnostics。RX 同时会修改这些字段。ESP32-S3 上 64 位 `last_valid_frame_ms` 可能被撕裂，32 位计数也可能形成非同一时刻组合；当前这些值只驱动日志、registry 诊断计数和对外 spatial diagnostics，未直接控制 recovery，因此属于数据准确性和观测稳定性问题，而非已证实内存破坏。

最小修复：为 parser/UART diagnostics 添加独立短临界区，或在 service mutex 内维护一份由 RX 单次提交的诊断副本；不要在 parser feed 全程持锁。对应单元测试应并发读取快照并验证 64 位时间不回退、计数不倒退。

### P2-2: 旧 presence 状态机处于失联状态

有效 frame 回调只更新 recovery 和 `s_latest_frame`，没有调用 `radar_presence_on_frame()`；也没有周期调用 `radar_presence_poll()`。故 `radar_service_get_snapshot()` 的 state/frame_fresh/targets 除 UART error 外始终保留初始化值。当前 local adapter 正确地以 latest frame 驱动 spatial state，且仓库内未找到该 service snapshot 的调用者，所以当前主链不受影响；但该公开 API 已经返回错误状态。

最小修复：二选一并写明契约。

- 仍支持旧 service snapshot：在 handle_frame 中更新 presence，并在 RX timeout/poll 路径刷新它。
- 彻底由 spatial state 取代：移除/弃用该公开 getter 和不再使用的 presence 状态，避免未来调用者误接入。

## 6. LD2450 协议和远端 ingest 审计

- 半包：parser 保存不足 30 字节的 prefix；UART timeout 才 reset partial buffer，安全但会在 100 ms 无字节间隙时丢弃尚未完成的帧。对 256000 baud 的正常 30-byte frame 不是问题；异常串口延迟需真机验证。
- 粘包：`ld2450_parser_feed()` 逐字节循环，每发布一个帧即清空已消费帧并继续处理输入，安全。
- 帧长：本地协议是固定 30 字节，没有不可信 length field；尾部 `55 CC` 是完整性校验，但没有 CRC。错误尾部逐字节 resync，故不会越界，但随机噪声仍有极低概率伪造 header+tail 后形成错误目标。
- 目标上限：本地只循环 3 个 slot，远端 JSON `target_count` 和 array size 都限制为 3。
- 坐标：local transform 过滤距离、房间和 ignore zone；见 P1-2，过滤结果还未完全一致地投影到 registry。
- 时间：核心与 spatial state 使用 `uint64_t esp_timer` 毫秒；差值前大多检查 `now >= timestamp`。registry 在时钟回退时不刷新旧 entry，presence 会记 time rollback 并回 UNKNOWN；真实 64 位 timer wrap 不具备可操作的近期触发条件。

远端 JSON 的所有 cJSON 指针均在 delete 前转换为值类型。协议层仍未检查 state/target_count/frame_fresh 的跨字段语义（例如 `motion` + 0 target）；这会产生远端业务数据不一致，但不会越界。建议在 P2-2 后按 C5 既有协议决定是否新增仅拒绝明显矛盾组合的 P2 校验，不能擅自改变 C5 协议。

## 7. 修复优先级与实施计划

1. **P1-1，先修配置所有权。** 在 ESPS3 的 `radar_service` 内引入唯一 UART transaction gate；让 config 调用者经 service 排队，RX 可控暂停并在恢复时 reset parser。增加“RX 正在收帧时执行 config”的并发 host test，验证无 parser resync storm、无误入 BACKOFF。
2. **P1-2，冻结本地 snapshot 语义。** 选择 raw 或 accepted 的唯一含义，令 count/array/日志三者一致；对超距离、room 外和 IGNORE zone 分别加测试。该改动局限 adapter/registry contract，不触碰 gateway route、C5 或 server。
3. **P2-1，诊断快照原子化。** 只保护 diagnostics copy，不扩展 parser/UART 的长临界区；并发测试校验时间/计数单调。
4. **P2-2，处理死的 presence API。** 按实际消费者决定恢复更新或弃用；若恢复，保持其仅为兼容视图，spatial state 继续是本地状态事实来源。
5. **P2-3/P2-4/P2-5，防御性硬化。** 收紧日志 helper 的承诺，使用 64 位中间算术及 clamp，补充极值和非终止字符串的测试。
6. **运行验证（后续单独授权）。** 仅在允许真机时采集 stack watermark、heap trace、UART overflow/recovery loop、长时间稳定帧率和 config transaction；不能以当前静态审计或 16 MB 空闲 heap 代替。

## 8. 不影响既有链路的修改建议

- 修改只落在 `ESPS3/components/radar_ld2450/` 和 `ESPS3/components/Middlewares/radar_domain/`；remote HTTP route 只在明确需要 schema 语义校验时做最小补充。
- 保持 UART1/TX18/RX17/256000 8N1、原始坐标默认值、S3 gateway 启动顺序及 C51/C52 radar 禁用状态不变。
- 不改 C5 payload 格式、不改 ESP-server、Dashboard、BME、CSI、voice、network worker 或 scheduler 的业务行为。
- 所有新增诊断应沿用当前“状态变化立即记录、summary 低频记录”的策略；避免每帧日志和动态分配。
- 先用现有 radar core/domain host tests 扩展边界用例，再在获授权后执行 `idf.py -C ESPS3 build`；本次未执行这些命令，也未宣称真机验收。

