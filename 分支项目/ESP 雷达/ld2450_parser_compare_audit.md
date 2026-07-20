# LD2450 Parser Compare Audit

审计对象：

- macOS `HLK2450Mac`：`RadarStore.consume()`、`LD2450Protocol.parseReports()`
- ESPS3：`ESPS3/components/radar_ld2450/ld2450_parser.c` 及其 UART RX/recovery 调用链

范围限制：本报告只做源码对比和已有 host test 复核；不修改 UART GPIO、不修改 LD2450 协议、不修改空间算法。

## 结论

两套 **parser 的正常字节流能力基本等价**：都要求 `AA FF 03 00`，固定累计 30 字节，检查末尾 `55 CC`，并能处理半帧、粘包和帧头跨 read 边界。ESPS3 parser 的 host test 也覆盖并通过了这些场景。

ESPS3 与 Mac 的关键运行时差异是：

1. Mac 在 `consume()` 中把每次 read 追加到持久的 `reportBuffer`，不会因为一次 read 没有数据而清空未完成帧。
2. ESPS3 在 `uart_read_bytes()` 返回 `0` 时调用 `ld2450_parser_note_timeout()`；只要 parser 当前有残留字节，就立刻清空 `buffer/length`。因此一个跨 read 的半帧如果中间出现超过 100 ms 的空读，会被 ESPS3 丢掉，而 Mac 会继续等待后续字节。
3. ESPS3 recovery 进入 BACKOFF 时还会执行 `ld2450_uart_deinit()` 后 `parser_reset()`，之后重开 UART；这会再次清空残留帧。Mac 没有对应的 parser recovery reset 路径。
4. ESPS3 的 `resync_count` 不是“坏帧次数”，而是每丢弃一个字节就增加一次；一旦输入字节流没有按预期形成 header，计数会持续快速增加。

因此，`valid_fps_mhz=0`、`discard` 持续增加、`resync` 持续增加，**不能由“ESPS3 不支持半帧/粘包”解释**。源码上更符合以下两类原因：

- ESPS3 实际收到的原始字节没有形成 Mac 所见的同一条 `AA FF 03 00 ... 55 CC` 字节流（例如 baud/电气/来源/模式导致的字节内容或边界不同）；或
- 帧被拆开且两段之间出现了 `100 ms` read timeout，ESPS3 在第二段到达前主动清空了 partial buffer。

在不改 GPIO 和协议的前提下，下一步应以 ESPS3 已有的 `RX raw bytes` hexdump 为事实来源，拼接相邻 read，检查是否真的出现完整的 30 字节序列。

## 六项逐条对比

| 项目 | macOS HLK2450Mac | ESPS3 | 审计结论 |
|---|---|---|---|
| 1. 跨 read 缓存 | `RadarStore.reportBuffer` 持久保存；`consume()` 先 `reportBuffer += bytes`。见 `Sources/Stores/RadarStore.swift:20-23,149-152`。 | `ld2450_parser_t.buffer[30] + length` 持久保存；`ld2450_parser_feed()` 每次继续喂入同一个 parser。见 `ESPS3/components/radar_ld2450/include/ld2450_parser.h:26-34`、`ld2450_parser.c:231-248`。 | 都支持。差异在 ESPS3 的 timeout/recovery 会主动清空缓存。 |
| 2. 半帧 | 找到 header 但不足 30 字节时保留从 header 开始的残留，等待下一次 `parseReports()`。见 `LD2450Protocol.swift:11-15`。 | `length < LD2450_FRAME_SIZE` 时返回，已累计字节保留在 parser buffer。见 `ld2450_parser.c:193-208`。 | parser 层都支持。ESPS3 的 `read_len == 0` 分支随后会调用 timeout reset，见 `radar_service.c:254-258`。 |
| 3. 粘包 | `while let start = find(...)` 循环消费完整帧；一批 bytes 中可消费多帧。见 `LD2450Protocol.swift:9-39`。 | `ld2450_parser_feed()` 逐字节循环；成功帧将 `length=0` 后继续处理本次输入剩余字节。见 `ld2450_parser.c:217-247`。 | 都支持；ESPS3 host test 明确验证两帧连续输入，见 `tests/test_radar_core.c:127-139`。 |
| 4. 帧头跨边界 | 每次先追加到 `reportBuffer`，所以 header 的 4 个字节可分布在多个 read 中；`find()` 在合并后的缓存中搜索。见 `RadarStore.swift:149-152`、`LD2450Protocol.swift:91-95`。 | 每个 byte 进入 `consume_byte()`；`buffer_is_header_prefix()` 保留 `AA`、`AA FF`、`AA FF 03` 的前缀，见 `ld2450_parser.c:176-205`。 | 逻辑上都支持。ESPS3 不是按 read 搜索，而是按 byte 增量搜索。 |
| 5. recovery 时清空 parser 状态 | 没有对应的 UART recovery reset；断开处理只清理连接/命令状态，不清理 `reportBuffer`。见 `RadarStore.swift:30-36`。 | 进入 recovery BACKOFF 时 deinit UART 后调用 `parser_reset()`；`ld2450_parser_reset()` 将 `length=0` 并清 buffer。见 `radar_service.c:116-140`、`ld2450_parser.c:154-161`。 | ESPS3 会清空；Mac 不会。 |
| 6. 帧头搜索逻辑 | `find()` 搜索完整 4 字节 header；header 前噪声一次性移除；坏尾只前移一个字节再搜。见 `LD2450Protocol.swift:11-19,91-95`。 | 若当前前缀不匹配，`discard_first_byte()` 一字节滑动；header 前缀匹配则等待；坏尾同样丢一个字节后重试。见 `ld2450_parser.c:163-226`。 | 搜索语义一致；ESPS3 额外把每个丢弃字节计入 `discarded_bytes` 和 `resync_count`。 |

## 关键源码链

### Mac

`RadarStore.consume()` 的顺序是：

```text
reportBuffer += bytes
parseReports(from: &reportBuffer)
```

`parseReports()` 找到 header 后：

1. 不足 30 字节：保留残留并退出；
2. 足够 30 字节但尾部不是 `55 CC`：从当前 header 起向前移动一个字节；
3. 完整有效帧：消费 `start + 30` 个字节，然后继续 while 循环。

这就是持久 byte-stream buffer，read 边界不会成为协议边界。

### ESPS3 parser

`consume_byte()` 的状态推进为：

```text
append byte
while buffer not empty:
    header 不完整且是合法前缀 -> 等待
    header 不匹配 -> 丢 1 byte，resync
    少于 30 byte -> 等待
    30 byte 且尾部 55 CC -> 发布并清空已消费帧
    尾部错误 -> invalid_tail++，丢 1 byte，继续搜索
```

因此在没有 timeout/recovery 的前提下，它同样能跨 read、半帧、粘包和坏尾恢复。

### ESPS3 运行时的破坏性路径

RX task 每次最多读取 128 bytes，read timeout 为 100 ms：

- `radar_service.c:227-228` 调用 `ld2450_uart_read(..., 100 ms)`；
- `radar_service.c:254-258` 在 `read_len == 0` 时调用 `parser_note_timeout()`；
- `ld2450_parser.c:251-258` 对非空 partial parser 执行 `partial_timeouts++` 和完整 reset；
- recovery 达到静默阈值 30 次后进入 BACKOFF，`radar_service.c:121-129` 再次 deinit UART 并 reset parser；
- recovery 重建后必须连续 3 帧有效帧才进入 `VALID`，见 `radar_uart_recovery.c:131-145`。

这会造成如下现象链：

```text
partial/read gap 或原始字节不匹配
    -> 无完整 valid frame
    -> invalid_tail / discarded_bytes / resync_count 增长
    -> valid_frame_rate_millihz 没有有效帧可计
    -> 连续 no-valid 达 30 次
    -> BACKOFF + parser reset
    -> 重开后继续等待 3 个 valid frame
```

## 对三个现场指标的解释

### `valid_fps_mhz=0`

`valid_frame_rate_millihz` 只在累计时间窗口达到 1000 ms 时由 `note_candidate()` 更新，且只统计通过尾部校验并发布的帧，见 `ld2450_parser.c:29-51`。如果 parser 从未发布完整有效帧，或者观察窗口尚未满 1 秒，该值都可能为 0。

在用户描述的“持续为 0”场景中，它更可能是前面没有 valid frame 的结果，而不是帧率计算本身拒绝 10 Hz。

### `discard` 持续增加

`discarded_bytes` 在 `discard_first_byte()` 中每丢一个 byte 增加，见 `ld2450_parser.c:163-174`。它覆盖两种情况：

- 当前 4 字节开头不是 `AA FF 03 00`；
- 已有 header 但固定第 28/29 字节不是 `55 CC`，随后逐字节重同步。

所以它不是“帧数”，不能直接等同于坏帧数。

### `resync` 持续增加

ESPS3 当前实现中 `resync_count` 与 `discarded_bytes` 同步增加（同一处代码同时递增）。因此只要输入持续不是可对齐的 header，resync 就会持续增加；这与 Mac 只是丢弃缓存前缀的行为语义相同，但 Mac 没有同样细粒度的计数器。

## 现阶段最可信的根因排序

1. **ESPS3 实际收到的 bytes 与 Mac 所见 bytes 不同**：这是持续 discard/resync 的首要解释。需要查看 `radar_service.c:242-252` 打出的每次 read 原始 hexdump，确认是否能跨相邻 read 拼出 `AA FF 03 00` 开头、总长 30、末尾 `55 CC` 的块。
2. **跨 read 的间隔触发 100 ms timeout**：如果 header/半帧已经进入 parser，但下一段超过 100 ms 才到，ESPS3 会在第二段前 reset；Mac 不会。正常 256000 baud 下单帧传输时间约 1.2 ms，因此若现场真发生该情况，应重点看调度、UART 输入是否间歇、或 read 返回行为，而不是空间算法。
3. **恢复状态机放大症状**：一旦连续 30 次无 valid，BACKOFF 会清空 parser 并重开 UART；它解释为什么问题会反复从空状态开始，但不是最初字节不匹配的根因。
4. **配置命令与 RX 并发**：仓库已有审计指出 `ld2450_config_execute()` 直接读写同一 UART，而 `s_config_active` 没有实际调用点；若现场同时执行配置，ACK/报告帧可能被另一条读路径消费，造成 resync。该风险只在配置 API 被调用时成立，不能仅凭当前指标断定。

## 验证状态

- ESPS3：`sh ESPS3/components/radar_ld2450/tests/run_host_tests.sh` -> `radar core host tests: PASS`
- Mac：`swiftc ... LD2450Protocol.swift ...` 后 protocol checks -> `PASS: protocol parser and command checks`
- 未执行 flash、monitor 或真机验收；host test 只能证明 parser 对构造字节流的行为，不能证明 ESPS3 UART 实际收到的物理字节与 Mac 相同。

## 审计结论

针对用户列出的六点，答案是：

- 1 跨 read 缓存：**都有**；
- 2 半帧：**parser 都支持**，但 ESPS3 read timeout 会主动丢 partial；
- 3 粘包：**都有**；
- 4 帧头跨边界：**都有**；
- 5 recovery 清空 parser：**ESPS3 会，Mac 没有对应路径**；
- 6 帧头搜索：**核心匹配规则一致**，ESPS3 是逐字节增量状态机，Mac 是合并数组搜索。

所以当前证据不支持“ESPS3 parser 不支持 30 字节 AA FF 03 00 ... 55 CC 帧”这一判断；支持的是“ESPS3 运行时在 raw stream 不匹配或 read gap 时有额外的 destructive timeout/recovery 行为”。
