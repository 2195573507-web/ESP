# 资源分配与功能链路审计

- 日期：2026-07-22
- 范围：`ESPC51`、`ESPC52`、`ESPS3` 及其现有本地 HTTP/Server 语音边界；未修改 `ESP-server`。
- 状态：源码审计和三个隔离 ESP-IDF 构建通过；未刷机、未接入串口、未进行麦克风、WakeNet、Wi-Fi、BLE、雷达、LCD 或端到端 Server 验收。
- 排除项：数据传输安全、认证、加密和隐私不在本次范围。

## 执行摘要

本次审计以六个独立视角覆盖资源/内存、语音、雷达及核心链路、并发和恢复、架构优化、文档证据。结论不是“所有运行时风险均已消除”：资源放置总体已有明确边界，S3 的大 JSON/HTTP 队列与告警任务已转向 PSRAM，C5 LCD bootstrap 栈已回到 Internal DRAM；但静态源码无法给出实际的 Internal RAM 常驻值、最大连续块或 ESP-SR 模型真实落点。

三个最严重的历史/当前风险为：

1. **已修 P0**：C5 的 `LISTENING` 状态没有开启录音窗口，VAD 即使触发也无法进入独占录音。初始化、完成和取消路径现均显式关闭窗口，只有 `open_recording_window()` 打开它。
2. **P1**：C5 雷达 BLE 的停止路径只清理回调和状态，未见扫描/callout、连接及 NimBLE port 的完整停止/反初始化。重复启停或关闭雷达后仍可能占用 Internal RAM、控制器或主机任务。
3. **P1**：S3 的 WakeNet 已在 C5 之外且由 `voice_proxy` 使用，但每次 HTTP voice turn 创建/销毁模型；没有 AFE、会丢弃不足一个 WakeNet chunk 的尾部，且其 Internal/PSRAM 实际分配未知。只有设备侧监控才能证明实时性、唤醒率和堆余量。

语音职责边界符合目标方向：C5 采集和本地 VAD，只有 VAD 打开录音窗口后才累计并 POST PCM；C5 不再运行 WakeNet；S3 先运行 WakeNet，命中后才代理给 Server。雷达未连接/禁用不再被诊断端误写成 `FAILED`：本地为 `DISABLED_OR_OFFLINE`，远端断开为 `NOT_CONNECTED`。S3 远端雷达是否会因源端停止上报而转为 stale 仍未完全证明。

建议立即保留已实施的 P0 修复；下一轮优先实现并硬件验证 BLE 完整停机、S3 启动失败回滚和远端雷达 stale 判定。不要以移动所有对象到 PSRAM 或增大看门狗超时替代这些所有权问题。

## 审计方法与边界

| 角色 | 覆盖内容 | 结论产物 |
| --- | --- | --- |
| Agent 1，资源与内存 | capability allocator、DMA、PSRAM、任务栈、FreeRTOS 对象和生命周期 | 资源分类、预算边界、碎片与 admission 风险 |
| Agent 2，语音 | C5 采集/VAD/状态机、HTTP PCM、S3 WakeNet/代理 | 实际语音图和 VAD 门控结论 |
| Agent 3，雷达与核心链路 | S3 本地雷达、C5 BLE 雷达、LCD、Wi-Fi/HTTP、BME/Server 输出 | 状态语义和链路退化项 |
| Agent 4，并发与恢复 | 启停幂等性、部分初始化、任务/回调、WDT | P1 回滚和生命周期风险 |
| Agent 5，架构优化 | 固定池、延迟启动、最低余量和长期方案 | 分阶段建议与测量点 |
| Agent 6，文档 | 源码证据、已改行为、构建和未验证项 | 本记录和开发日志索引 |

审计从 `app_main`/启动 orchestrator、模块 init/stop 和主要 HTTP/驱动入口向下追踪。搜索仅用于定位，结论由调用路径和当前源码复核；构建输出只证明配置、编译和链接，不证明板端行为。

## 启动、任务与资源

### 启动顺序

```text
C51/C52 app_main
  -> app_startup_task -> app_orchestrator_start
  -> C51: Wi-Fi / resource-manager / mic+local VAD / server_voice，随后 static Internal lcd_bootstrap
  -> C52: 先调度 static Internal lcd_bootstrap 与 continuation，再继续业务启动
  -> lcd_service -> LCD event/UI workers
  -> radar BLE runtime (按功能启动)

ESPS3 app_main
  -> gateway_startup_task -> gateway_orchestrator_start
  -> Wi-Fi/HTTP/network worker/本地雷达/BME/聚合
  -> audio_wake_gateway_init -> voice_proxy_init
  -> /local/v1/voice/turn handler -> WakeNet -> Server proxy
```

上图是源码依赖图，不表示每一步在实机上均成功。C5 编排器注释明确 voice turn 只面向 S3 `/local/v1/voice/turn`，而非终端直连 Server（`ESPC51/components/Middlewares/app_orchestrator/app_orchestrator.c:463`）。S3 网络/ Wi-Fi 的阶段化初始化仍需建立可验证的反向 rollback 链。

### 任务与栈表

FreeRTOS 的 `xTaskCreate*` 栈深度以 `StackType_t` 为单位；日志中用字节表示时必须乘以 `sizeof(StackType_t)`。下表仅记录源码可确认的重点对象，未把 IDF/Wi-Fi/BLE 系统任务伪装成应用可精确预算。

| 芯片/模块 | 任务或对象 | 放置与大小 | 生命周期/阻塞 | 风险与结论 |
| --- | --- | --- | --- | --- |
| C51/C52 LCD | `lcd_bootstrap` | 静态 `StackType_t`，Internal；由 `xTaskCreateStatic` 创建 | 有界重试后自删 | 已修：C51 不再把 cache-off 敏感 bootstrap 栈放 PSRAM；见 `app_orchestrator.c:66-71,432-452`。LCD-first 仍可能与其他 Internal 栈短时重叠。 |
| C51/C52 voice | `server_voice_rx` | PSRAM 静态分配栈 | task notification 唤醒，读取 S3 HTTP 回应 | 上传与播放解耦；失败创建会释放其栈（`server_voice_client.c:600-613`）。实际高水位需设备测量。 |
| C51/C52 mic | ADC continuous / mic task | DMA 描述符和实时采集路径必须保留 Internal/DMA；控制/较大非 DMA 缓冲可在 PSRAM | DMA 驱动和 VAD 周期 | 不应将 DMA/callback 相关对象泛移 PSRAM；largest DMA block 是启停准入条件。 |
| C51/C52 radar | BLE runtime task/NimBLE | NimBLE 配置为 Internal memory；host stack 4096 | 连接、扫描、通知回调 | P1：runtime task 可删，但 transport stop 不完整。 |
| ESPS3 HTTP/radar | HTTP body、debug JSON | 4096-byte radar debug body 为 `MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT` | 请求期间，正常/错误均释放 | 合理的非 DMA 转移，见 `radar_local_handler.c:259-267,250-255`。 |
| ESPS3 voice | WakeNet model instance | ESP-SR 不透明 allocator，按请求 create/destroy | 单个 `/voice/turn` 处理期间 | P1：分配位置和峰值不能由静态代码证明；create 失败正确返回错误。 |
| ESPS3 network/BME | network worker、告警上报、HTTP 队列 | 修复后大量 body/栈在 PSRAM，连接有 admission | 队列/周期 worker | 已降低历史 DMA/HTTP 峰值；仍要采集内存水位与 WDT。 |

### 任务调度字段补充

| 任务 | 芯片 | 创建入口 | 优先级/Core | 主要等待方式 | 停止/重复创建结论 | Watchdog/实时风险 |
| --- | --- | --- | --- | --- | --- |
| `app_startup_task` / gateway startup | C5/S3 | 各 `app_main` 到 orchestrator | 由项目宏/IDF 调度配置决定；不能从本记录假造固定值 | 启动阶段调用、随后移交 worker | 启动失败必须回滚已建对象；S3 此项仍有 P1 缺口 | 单核 C5 不能使用零 tick 忙轮询；S3 Task WDT 配置为 5 s。 |
| `lcd_bootstrap` | C51/C52 | `c5_schedule_lcd_bootstrap()` | `C5_LCD_BOOTSTRAP_TASK_PRIORITY`；未固定 Core | `vTaskDelay()` 有界重试 | static 创建由 `s_lcd_bootstrap_task` 防重复；结束 `vTaskDelete(NULL)` | 启动期 Internal 峰值与 LCD DMA 需监控。 |
| `server_voice_rx` | C51/C52 | `server_voice_client_init()` | 项目宏；未声称固定 Core | `ulTaskNotifyTake()`/HTTP stream | init 失败释放 PSRAM 栈；turn 状态拒绝并发 | 网络回包慢时应验证取消与播放 abort。 |
| mic/ADC 连续采集 | C51/C52 | mic init/voice chain | 采集优先级由模块宏决定 | DMA 事件、VAD window | resource lease/voice state 应限制重复 turn | 不得让网络发送或队列满阻塞 DMA 采集。 |
| radar BLE runtime | C51/C52 | radar 功能启动 | 任务配置取决于 runtime | scan/connect/notify 回调 | 上层 task 可删除，底层 NimBLE 未完整停机（P1） | BLE 与 Wi-Fi 并存的 Internal 峰值需要实测。 |
| network worker | S3 | gateway/network worker init | 项目宏；通常不应将长 HTTP 放 WDT 敏感循环 | queue/periodic dispatch | 部分 init/stop rollback 未闭环（P1） | 历史上发生过 HTTP 超过 5s 的 WDT 相关风险。 |

`sdkconfig` 显示 C5 NimBLE 内存模式为 Internal、host stack 为 4096；S3 开启 external stack support，LwIP TCP/IP 栈为 3072，Task WDT 为 5 秒。它们是配置事实，不是对应用任务栈足够的证明。

### FreeRTOS/驱动对象审计

| 类别 | 观察结果 | 生命周期结论 |
| --- | --- | --- |
| Task notification | `server_voice_client_finish_turn()` 在成功提交请求后用 `xTaskNotifyGive()` 唤醒 response task（`server_voice_client.c:756-762`） | 单一接收任务配合状态检查；应在板端压测 cancel 与 response 竞态。 |
| 静态任务控制块/栈 | LCD bootstrap 的控制块和栈为静态对象 | 避免启动阶段分散堆分配；自删只释放内核任务状态，不释放静态数组，这是预期行为。 |
| DMA driver | C5 ADC continuous 的 DMA 缓冲/驱动对象 | 只在成功 init 后启动；失败路径及反初始化必须继续以 IDF 返回码为准。 |
| HTTP stream | C5 `s_voice.stream` | 上传完成后释放 upload buffer，接收任务负责 stream 响应；cancel 会请求 abort | 资源状态检查防止第二 turn 重入，见 `server_voice_client.c:635-695,698-776`。 |
| BLE host/scan/callout | `radar_ble_transport_stop()` | 仅清理回调/状态，未见 port stop/deinit | 已确认 P1，不能把删除上层 runtime task 等同于关停 NimBLE。 |

## Internal RAM 与资源预算

### 分类结论

| 对象类型 | 必须 Internal | 必须 DMA | 可放 PSRAM | 审计结论 |
| --- | ---: | ---: | ---: | --- |
| C5 cache-off/ISR/ADC driver 控制栈和 descriptor | 是或须由 IDF 要求确认 | 部分是 | 否 | 不可因总 PSRAM 充足而迁移。 |
| LCD DMA draw buffer | 是 | 是 | 否 | 是 LCD 启动早期的关键连续块，应在并发大对象前做 admission。 |
| C5 LCD bootstrap static stack | 是 | 否 | 否 | 已修为 Internal static。 |
| C5 PCM 上传、预录、响应任务栈 | 否（非 DMA 路径） | 否 | 是 | 已优先使用 PSRAM；保留数据所有权与 cache-off 限制。 |
| S3 HTTP JSON/雷达 debug body/告警 payload | 否 | 否 | 是 | 已有 PSRAM 分配，需确保 enqueue 时复制和失败释放。 |
| Wi-Fi/LwIP/BLE/ESP-SR 内部对象 | 实际上会消耗 Internal | 部分 | 不能假定 | 只能用 capability heap、map 和运行日志定量。 |

### 资源热点明细

| 模块 | 对象/大小 | 当前位置 | 生命周期 | 必须 Internal | 必须 DMA | 可迁移 PSRAM | 风险与建议 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| C5 启动 | `app_startup`，16 KiB | Internal | 启动任务全程 | 是/cache-off | 否 | 否 | 保留；由 `main/main.c:68-74`、`app_main_config.h:65-70` 证明。 |
| C5 runtime | dispatcher，12 KiB 及静态队列 | Internal | 常驻 | 是 | 否 | 不应直接迁 | C5 最大应用层常驻 Internal 栈之一；需纳入最低余量。 |
| C5 mic | mic task 12 KiB、ADC driver store 4 KiB、raw 512 B | task 栈可 PSRAM；driver/DMA Internal | mic 服务启停 | driver/DMA 是 | DMA 是 | 栈/预录可 | `mic_adc_test.h:31-39`、`mic_adc_test.c:152-156,1603-1658,1946-1983`；只对 DMA largest 做准入。 |
| C5 voice | response 栈 8 KiB；upload 16 KiB 起步、倍增至 320 KiB | PSRAM | service 常驻/每 turn upload | response 执行路径需实测 | 否 | 是 | 逐轮 `realloc` 有 PSRAM 碎片风险；加 reserve 或固定 session pool。 |
| C5 LCD | UI arena 96 KiB；draw DMA 4,800 B；bootstrap 4 KiB | arena PSRAM；draw/bootstrap Internal/DMA | UI 常驻；bootstrap 一次性 | draw/bootstrap 是 | draw 是 | arena 是 | LCD handoff 短时可能有旧/新 DMA 双占用；见 `lcd_ui.c:334-384`、`lcd_driver.c:339-371,446-484`。 |
| C5 speaker | writer 6,144 B、DMA staging 1,024 B、IIS DMA 约 8 KiB、ring | writer/DMA Internal；ring PSRAM | speaker 服务 | writer/DMA 是 | DMA 是 | ring 是 | 不迁移实时 DMA；以 `speaker_player.h:46-71`、`speaker_player.c:785-866` 为准。 |
| C5 radar | BLE 2 KiB、worker 4 KiB、upload 3 KiB | 应用栈/队列 PSRAM；NimBLE Internal | radar 启停 | NimBLE 是 | 否 | 应用层可 | stop 不完整 P1；实际 host/controller 不能从源码精算。 |
| S3 startup | gateway startup 8 KiB | Internal | 启动任务 | cache-off 是 | 否 | 否 | `main/main.c:69-76`；保留并记录高水位。 |
| S3 network | coordinator 16 KiB；upload/command/snapshot 约 40 KiB | coordinator Internal；队列/body PSRAM | worker 常驻、请求临时 | coordinator/IDF 网络对象 | 部分 | body/队列是 | init 失败 rollback P1；见 `network_worker.c:48-78,2457-2597`。 |
| S3 voice proxy | worker 8 KiB；body 0-384 KiB | PSRAM 优先，失败会普通 8-bit heap fallback | 每个 voice HTTP turn | fallback 会消耗 | 否 | 应只允许 PSRAM | fallback 可能吞 Internal，且绕过统一 admission；见 `voice_proxy.c:125-188,257-296`、`gateway_config.h:22-31`，列为 P1。 |
| S3 radar/alarm | UART ring 2 KiB；radar Internal 栈 4/8/6/6 KiB；alarm reporter 5,120 B | ring/工作区 PSRAM；实时栈 Internal；alarm storage/栈按现实现 | 常驻/周期上报 | UART/实时栈按驱动要求 | UART DMA 依实现 | history/storage 可 | alarm 部分失败回滚未闭合（`environment_alarm_reporter.c:512-560`）。 |

### 可确认预算、未知量与阈值

没有本轮设备快照时，不能从 C/CMake 静态加总出“当前常驻 Internal RAM”或 ESP-SR 的精确峰值。本项目必须分别记录 `internal_free`、`internal_minimum_free_size`、`internal_largest_free_block`、`dma_free`、`dma_largest_free_block`、`psram_free` 和每个长期任务的 high-water mark；总 free 不能代替 largest block。

| 芯片 | 已知压力来源 | 推荐启动准入/告警策略 | 不能由源码确认的量 |
| --- | --- | --- | --- |
| C51/C52 | ADC/LCD DMA、Wi-Fi、NimBLE、mic Internal 栈、cache-off 控制栈 | 在创建 mic、LCD、BLE 或 Wi-Fi 并存大对象前检查 DMA largest 与 Internal largest；任一关键申请失败应拒绝非关键功能并记录 capability 指标 | 常驻 Internal、Wi-Fi/BLE 瞬态、ADC DMA 实际连续块、栈真实高水位。 |
| ESPS3 | Wi-Fi/LwIP/TLS、HTTP、ESP-SR、告警/雷达任务 | 保持 HTTP 并发 admission；低于当前功能配置定义的 capability 水位时推迟 best-effort 请求，不能让 TLS/Wi-Fi 继续抢到耗尽 | WakeNet model allocator、AFE 预算、双终端并发音频和实际 Server 延迟。 |

推荐把阈值定义为“下一个关键分配的字节数 + 安全余量”，而不是在本记录中捏造一个跨芯片通用常数。基线采集应在：启动后、LCD 运行后、Wi-Fi/BLE 共存、开始/结束 voice turn、HTTP 并发和雷达断连重连后各记录一次；若 `minimum_free` 或 `largest` 单调下降，应按资源所有权追踪，而非只重启或增大 WDT。

### 分芯片预算矩阵（源码可确认项与候选阈值）

| 芯片 | 常驻 Internal（可确认部分） | 动态峰值/语音预留 | DMA、Wi-Fi/BLE、LCD、雷达 | 最低余量/告警候选 | 禁止新功能候选 |
| --- | --- | --- | --- | --- | --- |
| C51/C52 | app startup 16 KiB、dispatcher 12 KiB、LCD bootstrap 4 KiB、speaker writer 6,144 B，以及 IDF/NimBLE/驱动对象；总量不可静态精确 | mic driver store 4 KiB、speaker staging/IIS DMA、C5 voice upload 16-320 KiB **PSRAM**；无已证实的 Internal voice reserve | LCD draw DMA 4,800 B；LCD 切换候选连续块 12 KiB；Mic admission 候选 DMA largest >=4 KiB；NimBLE/Wi-Fi 由 IDF 管理 | `internal_free < 32 KiB` 或 `internal_largest < 16 KiB` 作为“可选功能告警”候选；实机校准 | 低于上述任一值拒绝 BLE/雷达、LCD 重建或非关键任务；关键采集失败应显式降级 |
| ESPS3 | gateway startup 8 KiB、network coordinator 16 KiB、雷达多个 Internal 栈、IDF/Wi-Fi/LwIP/TLS/ESP-SR；总量不可静态精确 | voice body 0-384 KiB 应只允许 PSRAM；WakeNet/AFE allocator 未知；HTTP/告警/雷达工作区大多 PSRAM | HTTP admission 当前 class 为 core 12/6/8/4 KiB、telemetry 20/10/16/8 KiB、best-effort 28/12/24/12 KiB（Internal free/largest、DMA free/largest），inflight=1 | 以 class admission 的 free/largest 作为请求告警；另需记录 WakeNet create 前后 Internal/PSRAM | PSRAM body 不足时不得 fallback 到大块 Internal；拒绝 best-effort HTTP，保留核心控制/采集 |

以上“候选”不是实机验收阈值。它们来自当前宏/日志和下一次关键分配的量级，必须用 capability heap minimum、largest 和栈 high-water 校准；不能将 32 KiB、16 KiB 或 12 KiB 表述为所有硬件批次的保证。

## 功能链路结果

### 语音：存在已修 P0，仍有 P1 验证项

```text
C51/C52 麦克风
  -> ADC/DMA 采集 -> 本地轻量 VAD
  -> recording-window / VOICE_EXCLUSIVE 状态
  -> PSRAM PCM upload buffer
  -> POST /local/v1/voice/turn (16 kHz, s16le, mono)
  -> ESPS3 voice_proxy HTTP handler
  -> audio_wake_gateway_detect_pcm (S3 WakeNet)
      -> 未命中: 204 No Content
      -> 命中: proxy 到既有 Server voice turn，PCM 回传 C5 response task
```

证据如下：

- C5 唤醒兼容层在初始化、完成和取消时关闭 `s_recording_window_open`，只有 `open_recording_window()` 打开它，避免 `LISTENING` 卡死在不允许录音的状态（`ESPC51/components/Middlewares/wake/local_wake_word.c:10-22`；C52 文件逐字节一致）。
- C5 `start_turn()` 拒绝并发 turn，获取 resource lease 后分配上传 buffer；仅在 streaming 状态 append PCM（`server_voice_client.c:635-695`）。`finish_turn()` 使用既有 `/local/v1/voice/turn` 且在提交后释放 upload buffer、通知 response task（`:698-762`）。因此 idle/VAD 未开窗时不会持续送 PCM。
- `voice_chain` 只在 `LISTENING -> VOICE_WAKE_ACK` 后取得资源租约、暂停 mic 并排入本地 wake 事件；`RECORDING` 才开始 turn、gateway ready 才 append，finish 后转 `WAITING_RESPONSE`（`ESPC51/components/Middlewares/voice_domain/voice_chain.c:724-868`）。这是 VAD 门控的上游状态机证据。
- S3 `audio_wake_gateway_init()` 是幂等 model-list 初始化；`audio_wake_gateway_detect_pcm()` 检查 16 kHz/mono 和 chunk size，检测后 destroy model（`ESPS3/components/Middlewares/audio_wake_gateway/audio_wake_gateway.c:14-75`）。`voice_proxy` 在转给 Server 前调用它；无 wake 返回 204（`voice_proxy.c:343-360`）。

`voice_proxy` 当前先接收整个 HTTP request body，优先采用 SPIRAM、必要时退回 Internal，然后再调用 detector 和 Server proxy（`ESPS3/components/Middlewares/voice_proxy/voice_proxy.c:123-188,311-421`）。这满足“无 VAD 不发、S3 负责 WakeNet”，但不是 packet-level continuous stream：大 VAD 段仍会产生一个完整缓冲。任何声学 pre-roll、跨段状态或 AFE 结论都不能超出这条代码事实。

这不是 AFE 或连续流重组实现：请求内不足一个 chunk 的尾样本不送 detector，且模型逐请求重建。是否出现开头裁剪、VAD 收尾过长、模型占用 Internal RAM、S3 CPU 抖动或 HTTP 慢速拖累 C5，必须用真实 PCM 和 heap/task telemetry 验证。

### 雷达：状态语义已修，freshness/停机仍为 P1

- S3 本地 UART `OFFLINE` 的调试状态已映射为 `DISABLED_OR_OFFLINE`（`ESPS3/components/Middlewares/local_http_server/radar_local_handler.c:72-80`），不会把用户未开启雷达误报为 `FAILED`。
- C51/C52 远端来源无连接时，debug 输出为 `NOT_CONNECTED`（`radar_local_handler.c:281-352`）；本地/远端 source context 按 source ID 分开输出，单路不应覆盖其他来源。
- 远端 `radar_online` 仍来自最后可用 gateway output。若 C5 停止上报且没有 source-side stale 派生，页面可能长期显示 online。需在 ingest 状态根据 `updated_at_ms` 显式推导 `stale`，并补首帧、disabled、disconnected、stale、format-error 的稳定词汇。
- C5 `radar_ble_runtime_stop()` 删除上层任务后调用 transport stop，但后者只有回调/状态清理（`ESPC51/components/Middlewares/radar_ble/radar_ble_runtime.c:93-101`，`radar_ble_transport.c:581-589`）。完整停止应停止 scan/callout、断开连接，等待 `nimble_port_stop()`，再 `nimble_port_deinit()`，并处理幂等与失败回滚；C52 同路径。

### LCD：正常的源码路径，需硬件验证

LCD bootstrap 是静态 Internal 栈、有界重试、自删，不再临时从 PSRAM 分配（`ESPC51/components/Middlewares/app_orchestrator/app_orchestrator.c:66-71,400-452`）。这保留开机动画和状态显示的优先启动意图；但是 LCD-first 与 mic/Wi-Fi/BLE 的 Internal 峰值没有显式全局 admission gate，故仍为 P2 硬件风险。

### Wi-Fi/HTTP：高风险但已有防护

ESPS3 已有 HTTP 内存 admission、有限并发/回退和 PSRAM body 转移，解决历史 TLS/LwIP/DMA 雪崩方向正确。残余风险是 network worker 的部分启动失败没有完整 teardown，Wi-Fi 分阶段 init 的中间失败也缺少统一 deinit/rollback。需要在每个失败点确认“已创建什么、谁释放、是否允许再次 init”，不能以继续运行的日志代替恢复正确性。

`network_worker_init()` 依次创建锁、PSRAM 队列和多个任务，后续步骤失败直接返回而没有逆序释放（`ESPS3/components/Middlewares/network_worker/network_worker.c:2457-2584`）。`gateway_wifi_start()` 同样会在 NVS、netif、event loop、AP/STA netif、Wi-Fi 和事件 handler 的中间错误点返回，未持有完整 handler instance/对称销毁链（`ESPS3/components/Middlewares/gateway_wifi/gateway_wifi.c:769-859`）。相对地，network worker 先于 Wi-Fi 启动，Wi-Fi callback 只用零等待队列投递，属于当前可确认的合理初始化边界（`gateway_orchestrator.c:72-96`、`gateway_wifi.c:223-328`）。

### BME690/传感器与 Server/API：代码边界可确认，实机未验证

本轮未修改 BME690 算法或 Server 数据结构。S3 传感器/告警链路沿现有网络 admission 运行；告警堆栈和 payload 已做 PSRAM/栈压力修复，仍要确认传感器在网络退避、HTTP 队列满和 LCD/语音并发时不丢失所有权。Server 未改动，因此没有 Server build；C5 到 Server 的语音只经 S3 本地接口，符合当前 gateway 边界。

## 问题清单

| 优先级 | 问题、触发与根因 | 当前/正确行为 | 证据 | 已修复 | 硬件验证 |
| --- | --- | --- | --- | --- | --- |
| P0 | C5 `LISTENING` 未开 recording window，VAD 后无法进入录音独占状态。 | 当前仅 VAD start 开窗，finish/cancel/init 关窗；正确状态机可开始下一 turn。 | `local_wake_word.c:10-22` | 是 | 需要麦克风 VAD start/end/cancel 循环。 |
| P0 | 旧 C5 UDP 音频完成信号与 HTTP 回包路径脱节，可能完成竞态。 | 当前回到 C5 缓冲后 POST S3 HTTP，并等待既有 response task。 | `server_voice_client.c:635-762` | 是 | 需要真实 S3/Server TTS 回包。 |
| P1 | BLE stop 未停扫描、callout、连接及 NimBLE host/port。 | 当前仅清理局部 callback/status；正确行为为全部资源按反序停机并可重启。 | `radar_ble_transport.c:581-589` | 否 | 必须多次开关、断连和 heap/task 对比。 |
| P1 | S3 remote radar output 未必按 `updated_at_ms` 变 stale。 | 当前最后状态可能长期 online；正确行为为 freshness 超时后独立标 stale。 | `radar_local_handler.c:334-353` | 否 | 断开 C5 上报并观察。 |
| P1 | S3 network/Wi-Fi 局部 init 失败缺少完整 rollback/deinit。 | 当前可出现部分资源留下；正确行为为按已建资源逆序释放、重试可幂等。 | `network_worker`、`wifi_manager` 初始化错误分支 | 否 | 注入失败/反复 init-stop。 |
| P1 | S3 WakeNet 无 AFE，逐 request 创建模型，尾部不足 chunk 不检测。 | 当前满足 detector 位于 S3；正确目标需以模型/API 支持和实测确定连续会话策略。 | `audio_wake_gateway.c:39-66` | 否，架构项 | PCM 回放、heap、CPU、唤醒率。 |
| P1 | S3 voice body 在 PSRAM 失败时回退普通 8-bit heap，单请求上限可到 384 KiB。 | 当前可能直接挤压 Internal 并绕过统一 HTTP admission；正确行为应拒绝/退避，或使用受限单实例 PSRAM reserve。 | `voice_proxy.c:134-149`、`gateway_config.h:29` | 否 | 失败注入和 capability heap。 |
| P1 | C5 语音只有服务启动，缺少明确的服务级 stop/deinit。 | 单次 lease 释放不等于关闭语音服务；正确行为应释放 response task、PSRAM 栈、mic callback/stream 并支持重开。 | `voice_chain.c:1186-1327`，`server_voice_client.c:556-614` | 否 | 运行时开关语音、反复启停、heap/task 对比。 |
| P2 | LCD-first 启动无显式 Internal admission gate。 | 可能在短窗口与 mic/Wi-Fi/BLE 内部栈并存；正确行为为按 capability 余量拒绝/延后非关键项。 | `app_orchestrator.c:432-452` | 否 | 冷启动压力、PSRAM/cache-off。 |
| P2 | C5 内存日志没有 S3 同等的 `minimum_free_size` 持续证据。 | 当前不能证明碎片趋势；正确行为为 capability 分类最小值/最大块/水位齐全。 | C5 `c5_mem_log` 调用点 | 否 | 长时运行采样。 |
| P2 | C5 radar stop 等待 in-flight callback 没有截止时间；部分 voice wait 使用 `pdMS_TO_TICKS(1)`，而 tick rate 是 100 Hz。 | 当前 1 ms 转换为 0 tick，且异常 callback 会使 stop 无界等待；正确行为为有界 deadline 和至少一个实际 tick 的等待策略。 | `radar_worker.c:306-330`，`voice_chain.c:1123-1126`，`sdkconfig:1972` | 否 | callback 卡住/stop 压力和 WDT。 |
| P2 | C5 voice upload 以 16 KiB 起始并倍增至 320 KiB 的整段 `realloc`，注释与实际固定 body 提交不一致。 | 当前数据在 PSRAM 但仍会造成最大连续块压力；正确行为应采用固定 session pool/reserve，长期再做真正 chunked upload。 | `server_voice_client.c:174-201,726-755` | 否 | 长 utterance、多轮启停和 largest block 曲线。 |
| P2 | S3 alarm reporter storage/lock/queue/task 部分失败时回滚不完整。 | 当前可能留下前序资源；正确行为为 rollback_init 和 stop/deinit。 | `environment_alarm_reporter.c:512-560` | 否 | 失败注入、重复 init/stop。 |
| P2 | BLE control command 仍是硬件协议待确认的占位/未接线风险。 | 不应把没有真实设备 ACK 的路径表述为工作。 | `radar_ble_transport` control path | 否 | 真实 HLK/目标 BLE 硬件。 |
| P3 | 雷达 debug vocabulary 仍需要 stable first-frame/stale/format-error 契约。 | 当前已正确区分 disabled/unconnected；应扩展而不破坏现有 UI contract。 | `radar_local_handler.c:72-80,281-352` | 部分 | UI/HTTP contract regression。 |

## 已实施修改与回滚

| 文件 | 修改前 | 修改后 | 回滚方式与风险 |
| --- | --- | --- | --- |
| `ESPC51/ESPC52/components/Middlewares/wake/local_wake_word.c` | 录音窗口状态不能可靠地在终止路径关闭 | init/finish/cancel 关闭，open 函数开启 | 恢复前版本会重新引入 P0；两端文件必须同步。 |
| `ESPC51/ESPC52/components/Middlewares/server_voice/server_voice_client.c` | 旧 UDP completion/响应边界可能脱节 | VAD 片段 PSRAM 缓冲，POST 既有 S3 `/local/v1/voice/turn`，response task 等回包 | 可按文件回退；风险是依赖 S3 HTTP 响应时延，需设备验证。 |
| `ESPS3/components/Middlewares/audio_wake_gateway/audio_wake_gateway.c` | 新 UDP gateway 与实际返回协议不一致 | 幂等 model-list init 和 PCM detector helper | 可回退 helper；不要回退到 C5 WakeNet。 |
| `ESPS3/components/Middlewares/voice_proxy/voice_proxy.c` | 请求会直接转 Server，未先由 S3 作 WakeNet 决策 | WakeNet 未命中返回 204，命中后才 proxy | 回退会破坏目标职责边界。 |
| `ESPC51/components/Middlewares/app_orchestrator/app_orchestrator.c` | bootstrap 任务栈从 PSRAM 动态取得 | 4 KiB static Internal 栈 | 回退会恢复 cache-off 未证实安全性风险。 |
| `ESPS3/components/Middlewares/local_http_server/radar_local_handler.c` | 未启用/断开被表述为失败 | 使用 `DISABLED_OR_OFFLINE` / `NOT_CONNECTED` | 回退会使诊断误导，影响恢复判断。 |

所有已改 C51/C52 文件需保持同源行为；本次 final parity 检查已覆盖 `local_wake_word.c` 与 `server_voice_client.c`。

### 放弃的方案

1. **不继续使用 C5 到 S3 的新 UDP 音频完成协议。** 它与现有 C5 HTTP response playback 断开，且会引入 completion race；当前恢复至已经存在的 HTTP voice-turn 回应路径。
2. **不把 cache-off 敏感栈、DMA buffer 或不透明 ESP-SR state 一律迁移 PSRAM。** 这不能证明安全，反而会把连续 Internal/DMA 问题隐藏起来。
3. **不在本轮强行加入 AFE、第二个 WakeNet instance 或全局任务合并。** 它们需要 API/模型资源测量与设备验证，超出明确安全修复的范围。

## 后续计划

| 阶段 | 工作 |
| --- | --- |
| 立即执行 | 完成 BLE transport 全量 stop/deinit；给远端雷达 output 加 freshness/stale；为 Wi-Fi/network 分阶段初始化补对称 rollback；为 C5 memory log 加 minimum/largest capability 指标。 |
| 下一阶段 | 实现 C5 服务级语音 stop/deinit，并为 radar/voice 停止使用有界 tick/deadline；移除或隔离 S3 voice Internal fallback，使用固定、可计量的音频 session 预算验证 WakeNet，确认是否可引入 AFE/持续 model 会话；为 LCD/mic/Wi-Fi/BLE 建 capability admission gate；对 HTTP/告警队列做失败注入。 |
| 长期 | 建立模块资源清单与 Resource Manager：声明 Internal/DMA/PSRAM 预算、启动依赖、stop ownership 和每项最小余量；将雷达状态契约固定为 disabled/unconnected/first-frame/stale/format-error/valid。 |

## 构建和验证

使用 ESP-IDF 5.5.4、隔离 build 目录，环境为：

```sh
export IDF_PYTHON_ENV_PATH=/Users/zhiqin/.espressif/tools/python_env/idf5.5_py3.14_env
source /Users/zhiqin/.espressif/v5.5.4/esp-idf/export.sh
idf.py -C ESPS3 -B /tmp/esp-audit-esps3-build build
idf.py -C ESPC51 -B /tmp/esp-audit-espc51-build build
idf.py -C ESPC52 -B /tmp/esp-audit-espc52-build build
```

| 目标 | 结果 | 产物/注意事项 |
| --- | --- | --- |
| ESPS3 | 通过 | `/tmp/esp-audit-esps3-build/sensair_s3_gateway.bin`；未见本次改动相关编译警告 |
| ESPC51 | 通过 | `/tmp/esp-audit-espc51-build/00_Learn.bin`；sdkconfig.defaults 提示旧 `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY` 已被 `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM` 替代，且 `radar_domain` 目录缺少 CMakeLists 被跳过；均为现有工程配置/目录提示，非本次源码回归 |
| ESPC52 | 通过 | `/tmp/esp-audit-espc52-build/00_Learn.bin`；同上配置兼容提示和 `radar_domain` 目录跳过提示，非本次源码回归 |
| Server | 未构建 | 本次未修改 Server 接口或数据结构。 |

`git diff --check` 在源码修改后通过。本记录不会把这些 host builds 解释为 flash、设备、网络、音频、WakeNet、LCD、BLE、雷达或 Server 端到端验收；这些都仍由用户的硬件验证承担。

一次在本记录编写过程中尝试的**三路并行**重建，在 ESP-IDF `export.sh` 激活 virtualenv 时被宿主系统以 `SIGKILL` 终止，三个进程均未运行 `idf.py build`。该环境级失败不改变隔离构建的通过记录；随后已改用串行激活/构建，并以三个二进制产物和增量 Ninja 检查确认通过。

## 审计追踪

检查过的重点入口包括：C5 `app_orchestrator`、`local_wake_word`、`voice_chain`、`server_voice_client`、`mic`、`radar_ble_transport/runtime`；S3 `gateway_orchestrator`、`voice_proxy`、`audio_wake_gateway`、`local_http_server/radar_local_handler`、`network_worker`、`wifi_manager`；以及对应 `sdkconfig` 的 PSRAM、NimBLE、LwIP、Task WDT 配置。C51/C52 中只允许身份差异，任何后续语音/资源改动必须重新作双树差异核验。
