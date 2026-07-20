# `78/xiaozhi-esp32` 深读：ESP32-S3 产品级语音终端

## 快照与结论

- 仓库：`78/xiaozhi-esp32`
- 固定提交：[`7b190b78e4f8dfef14126f6cd478c134b3cd3cd8`](https://github.com/78/xiaozhi-esp32/tree/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8)
- 本地镜像：`/tmp/esp32-c5-s3-study-root/xiaozhi-esp32`
- 提交时间：2026-07-07；项目版本：2.2.6。
- 证据等级：A。结论来自固定提交源码、Kconfig、CMake、board manifest 和 sdkconfig；本轮按要求未联网、未构建、未运行硬件。

这不是单板 demo，而是一个多板型语音终端框架：离线唤醒、Opus 双向音频、WebSocket 或 MQTT+UDP、ASR/LLM/TTS 消息、MCP 工具、显示/摄像头、配网、OTA 和可在线更新 assets 都收敛在同一应用外壳。其最值得学习的地方是明确状态表、跨任务回主线程、音频流水线和移动所有权；最需要警惕的是主任务无界调度、若干并发/重连边界以及 OTA 失败清理不完整。

## ESP32-S3 支持验真

支持结论由四层源码相互印证，不依赖 README：

1. S3 有独立 defaults：16 MB flash、240 MHz、Octal PSRAM 80 MHz、96 KB internal reserve、S3 cache 和 4 KB system-event stack。[`sdkconfig.defaults.esp32s3#L2-L26`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/sdkconfig.defaults.esp32s3#L2-L26)
2. 代表板 `lichuang-dev` 的 build manifest 明确写 `"target": "esp32s3"`，并打开 device AEC。[`main/boards/lichuang-dev/config.json#L1-L10`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/boards/lichuang-dev/config.json#L1-L10)
3. Kconfig 只允许该板在 `IDF_TARGET_ESP32S3` 下选择；CMake 将选项映射到 `boards/lichuang-dev`，glob 其板级源码，并为 S3 加入 AFE/custom wake word、camera/video 源码。[`main/Kconfig.projbuild#L240-L245`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/Kconfig.projbuild#L240-L245) [`main/CMakeLists.txt#L154-L158`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/CMakeLists.txt#L154-L158) [`main/CMakeLists.txt#L875-L899`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/CMakeLists.txt#L875-L899) [`main/CMakeLists.txt#L1028-L1038`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/CMakeLists.txt#L1028-L1038)
4. 代表板源码实际配置 24 kHz codec、LCD/touch、ESP32 camera，并把 camera framebuffer 放 PSRAM，最后用 `DECLARE_BOARD` 注册。[`main/boards/lichuang-dev/config.h#L6-L59`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/boards/lichuang-dev/config.h#L6-L59) [`main/boards/lichuang-dev/lichuang_dev_board.cc#L216-L249`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/boards/lichuang-dev/lichuang_dev_board.cc#L216-L249) [`main/boards/lichuang-dev/lichuang_dev_board.cc#L262-L296`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/boards/lichuang-dev/lichuang_dev_board.cc#L262-L296)

本地固定快照中共有 103 个 `config.json` 声明 `target=esp32s3`。Git tree 还包含 `Build Boards` workflow：push 到 `main` 时枚举全部 variant，PR 按改动选择 variant，并以 ESP-IDF 5.5.2 做 `fail-fast: false` 的矩阵构建和产物上传。[`.github/workflows/build.yml#L15-L110`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/.github/workflows/build.yml#L15-L110) 但本轮没有读取该提交对应的 GitHub Actions run/check 结果，仓库也没有独立 test 目录；因此 workflow 只能证明构建覆盖意图，不能证明固定 SHA 的 103 个 S3 配置全部通过。

## 入口与任务拓扑

`app_main()` 只做 NVS 恢复、取得 `Application` singleton、`Initialize()`，然后永久进入 `Run()`。[`main/main.cc#L14-L28`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/main.cc#L14-L28)

应用自己可见的主要执行上下文如下：

| 执行上下文 | 创建/优先级 | 责任 |
|---|---|---|
| IDF main task | `Run()` 将当前任务升到 priority 10 | 消费 event bits、状态副作用、UI、用户命令、网络音频发送和 `Schedule()` callbacks。[`application.cc#L165-L258`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/application.cc#L165-L258) |
| audio input | S3 默认启用 processor；priority 8、stack 6144 bytes、固定 core 0 | codec input、10 ms feed、wake word/AFE、生成 encode work。[`audio_service.cc#L125-L144`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/audio/audio_service.cc#L125-L144) |
| audio output | priority 4、stack 4096 bytes、无 affinity | 消费 PCM playback queue，写 speaker。[`audio_service.cc#L139-L144`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/audio/audio_service.cc#L139-L144) |
| Opus codec | priority 2、stack 24576 bytes、无 affinity | 编码 MIC work、解码网络 packet，连接压缩包队列和 PCM 队列。[`audio_service.cc#L161-L166`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/audio/audio_service.cc#L161-L166) |
| activation | priority 2、stack 8192 bytes、句柄防重 | assets 检查、OTA/version/activation、协议初始化，完成后自删除。[`application.cc#L261-L278`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/application.cc#L261-L278) [`application.cc#L323-L338`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/application.cc#L323-L338) |
| esp_timer task | framework-owned | clock tick 只置 event bit；另有 audio power、Wi-Fi connect timeout、MQTT reconnect callbacks。clock callback 是轻路径。[`application.cc#L23-L47`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/application.cc#L23-L47) |

头文件注释仍写“MIC/Speaker/Processors 一个 task + codec 一个 task”，但实现已经拆成 input/output/codec 三任务；学习和容量核算应以实现为准。[`audio_service.h#L28-L37`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/audio/audio_service.h#L28-L37)

Wi-Fi、MQTT/WebSocket、LVGL 等组件还会有内部任务；它们来自 Component Manager 依赖，不在本仓库实现范围内，不能只靠上述表推断整机总 task 数。

## 应用状态机

状态集合为 `Unknown / Starting / WifiConfiguring / Idle / Connecting / Listening / Speaking / Upgrading / Activating / AudioTesting / FatalError`。[`device_state.h#L4-L16`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/device_state.h#L4-L16)

核心路径：

```text
Unknown -> Starting -> Activating -> [Upgrading] -> Idle
                   \-> WifiConfiguring <-> AudioTesting

Idle -> Connecting -> Listening <-> Speaking
  ^          |            |            |
  +----------+------------+------------+
```

- 转移白名单集中在一个 `switch`，非法转移会拒绝并记录；listener 在调用 `TransitionTo()` 的任务上下文触发。[`device_state_machine.cc#L34-L131`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/device_state_machine.cc#L34-L131)
- listener 列表在 mutex 下复制，回调在锁外执行，避免回调重入锁。[`device_state_machine.cc#L133-L160`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/device_state_machine.cc#L133-L160)
- 状态 listener 只给主 event group 置 `STATE_CHANGED`；真正的 UI、wake word、voice processor 和 decoder 切换集中在主任务。[`application.cc#L88-L94`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/application.cc#L88-L94) [`application.cc#L860-L930`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/application.cc#L860-L930)
- `Schedule()` 用 mutex 将 closure move 进 deque，再置一个 event bit；主任务一次 swap 整批执行，外部回调不直接操作主要业务状态。[`application.cc#L934-L940`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/application.cc#L934-L940)

状态机的两个限制：第一，`current_state_` 虽是 atomic，但“load -> validate -> store”没有 mutex/CAS；activation、timer/network 与 main task 都可能请求转移，两个并发合法转移可基于同一旧状态相互覆盖。第二，任何普通状态的白名单都没有 `FatalError`，所以这个终止态实际上无法通过公开 `TransitionTo()` 进入。

## 音频流水线与队列所有权

数据面分两条：

```text
MIC -> AFE/wake -> encode work(PCM, <=2) -> Opus -> send packets(<=40) -> network
network -> decode packets(<=40) -> Opus -> playback work(PCM, <=2) -> speaker
```

容量来自 [`audio_service.h#L28-L45`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/audio/audio_service.h#L28-L45)。所有 work/packet 都由 `std::unique_ptr` 在 deque 间 move；队列由同一 mutex/condition variable 协调，decode push 可选等待，send pop 明确转移所有权。[`audio_service.h#L163-L175`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/audio/audio_service.h#L163-L175) [`audio_service.cc#L327-L445`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/audio/audio_service.cc#L327-L445) [`audio_service.cc#L484-L528`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/audio/audio_service.cc#L484-L528)

这是成熟的所有权设计，但有三处边界：

1. input task 无锁读取 `audio_testing_queue_.size()`，codec task 在 mutex 下写该 deque，构成 C++ 数据竞争。[读取](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/audio/audio_service.cc#L245-L250) [写入](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/audio/audio_service.cc#L421-L432)
2. 网络音频进入 speaking 状态时用默认 non-wait decode push，但调用方忽略 `false`，满队列会静默丢包且无 drop counter。[`application.cc#L498-L502`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/application.cc#L498-L502) [`audio_service.cc#L506-L518`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/audio/audio_service.cc#L506-L518)
3. encode queue 满时高优先级 input task 在 condition variable 上等待；它保证有界内存，却会把 codec/network 压力反向传到采集 cadence。生产迁移必须同时监控 queue depth、block time 和 audio gap。

## 网络、协议与重连

- `WifiBoard::StartNetwork()` 初始化外部 `esp-wifi-connect` manager，转译统一网络事件，并用 60 秒 one-shot timer 将首次连接失败切到配网模式。[`wifi_board.cc#L29-L104`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/boards/common/wifi_board.cc#L29-L104) [`wifi_board.cc#L151-L197`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/boards/common/wifi_board.cc#L151-L197)
- application 通过 OTA 下发的配置选择 MQTT+UDP 或 WebSocket，并注册 audio/JSON/channel/error callbacks。[`application.cc#L473-L521`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/application.cc#L473-L521)
- MQTT 控制面断线后启动 60 秒 reconnect timer；timer 不直接重建 client，而是 `Schedule()` 回主任务。closure 同时捕获 shared `alive_`，析构先置 false、停删 timer、再释放 UDP/MQTT/event group，能防止排队 closure 使用已析构对象。[`mqtt_protocol.cc#L13-L53`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/protocols/mqtt_protocol.cc#L13-L53) [`mqtt_protocol.cc#L85-L98`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/protocols/mqtt_protocol.cc#L85-L98)
- MQTT audio channel 用 mutex 保护 `udp_` 的 send/reset；服务器 hello 后才创建 UDP、安装解密 callback。[`mqtt_protocol.cc#L166-L213`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/protocols/mqtt_protocol.cc#L166-L213) [`mqtt_protocol.cc#L215-L295`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/protocols/mqtt_protocol.cc#L215-L295)
- WebSocket 不维护常驻 reconnect task；`Start()` 是 no-op，用户开始会话时才创建 socket、发 hello 并最多等待服务器 10 秒。[`websocket_protocol.cc#L23-L26`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/protocols/websocket_protocol.cc#L23-L26) [`websocket_protocol.cc#L83-L200`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/protocols/websocket_protocol.cc#L83-L200)

重连存在一个具体缺口：MQTT timer 是 one-shot；到期时只有 `DeviceState == Idle` 才安排重连，否则直接返回且不重新启动 timer。broker 在 listening/speaking 等状态掉线时，若没有另一路状态变化触发重建，可能永久停止自动重连。[`mqtt_protocol.cc#L16-L33`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/protocols/mqtt_protocol.cc#L16-L33)

另外，`ContinueOpenAudioChannel()` 本身运行在 priority-10 main task，而 MQTT/WebSocket open 都可等待 server hello 10 秒；这期间 `Schedule()`、UI/state side effects 和其他 main events 都被推迟。主调度 deque 又没有容量、drop 或 latency 指标，网络抖动时可能形成无界积压。[`application.cc#L713-L730`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/application.cc#L713-L730) [`mqtt_protocol.cc#L215-L238`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/protocols/mqtt_protocol.cc#L215-L238)

## OTA、恢复与资源生命周期

成熟点：

- bootloader rollback 已启用；启动检查成功后仅在 running partition 为 `PENDING_VERIFY` 时 mark valid。[`sdkconfig.defaults#L6-L22`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/sdkconfig.defaults#L6-L22) [`ota.cc#L247-L265`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/ota.cc#L247-L265)
- activation task 的版本检查最多重试 10 次并指数退避；句柄阻止并发 activation task。协议初始化完成后 `ota_` 立即 reset，释放阶段对象。[`application.cc#L398-L470`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/application.cc#L398-L470) [`application.cc#L299-L315`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/application.cc#L299-L315)
- 固件下载用 4 KB internal buffer、sequential OTA write、`esp_ota_end` image validation 和 boot partition 切换；升级前关闭 channel/停止音频，失败时尝试重启音频。[`ota.cc#L267-L387`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/ota.cc#L267-L387) [`application.cc#L972-L1021`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/application.cc#L972-L1021)

限制：

1. HTTP read 在 `esp_ota_begin()` 之后失败时只 free buffer 并 return，没有 `esp_ota_abort(update_handle)`；write 失败路径才 abort。[`ota.cc#L308-L356`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/ota.cc#L308-L356)
2. MCP 手动升级在 main task 调 `UpgradeFirmware()`；失败后只记录日志。函数重启 audio，却没有从 `Upgrading` 转回 `Idle`，而 activation 自动升级依赖随后 `ACTIVATION_DONE` 才恢复。手动失败可能留下状态与运行资源不一致。[`mcp_server.cc#L151-L169`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/mcp_server.cc#L151-L169) [`application.cc#L989-L1013`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/application.cc#L989-L1013)
3. `AudioService::Stop()` 只置 stop flag、清队列并唤醒任务，没有 join/等待任务自删或清 task handles；OTA 失败后的 `Start()` 假定旧三任务已经退出。析构还关闭 codec/resampler/event group，却未 stop/delete `audio_power_timer_`。[`audio_service.cc#L40-L60`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/audio/audio_service.cc#L40-L60) [`audio_service.cc#L169-L182`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/audio/audio_service.cc#L169-L182)
4. v2 partition table 与 v1 不兼容，不能从 v1 OTA 到 v2，必须手工整片刷写；这是部署边界，不是普通升级路径。[`README.md#L15-L21`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/README.md#L15-L21)

协议输入也偏信任服务端：WebSocket v2/v3 binary parser 在读取 header/payload_size 前没有验证 `len` 是否覆盖 header 和声明 payload，恶意或损坏 frame 可能造成越界读取。[`websocket_protocol.cc#L112-L146`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/protocols/websocket_protocol.cc#L112-L146)

## 许可证、依赖与复现边界

- 根项目为 MIT，允许商用、修改、分发和再许可，但必须保留版权与许可声明。[`LICENSE#L1-L21`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/LICENSE#L1-L21)
- vendored `gifdec` 单独声明 public domain。[`main/display/lvgl_display/gif/LICENSE.txt#L1-L2`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/display/lvgl_display/gif/LICENSE.txt#L1-L2)
- Component Manager manifest 包含 Espressif、78、Waveshare、LVGL、ESPHome 等多方组件；根 MIT 不能替代逐依赖许可证核查。[`main/idf_component.yml#L1-L70`](https://github.com/78/xiaozhi-esp32/blob/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8/main/idf_component.yml#L1-L70)
- 仓库快照没有 `dependencies.lock`，manifest 又大量使用 `^`/`~` 版本范围；固定 Git commit 并不固定完整依赖图。要做可复现产品基线，应保存解析后的 lockfile、ESP-IDF 版本和逐板构建结果。

## 对 ESP-111 的可迁移结论

可直接借鉴：显式状态转移白名单、listener 锁外回调、网络/协议 callback 回主任务、`unique_ptr` 跨队列 ownership、压缩包队列与 PCM work 队列分层、协议析构的 `alive_` guard、OTA rollback/mark-valid。

只适合实验：S3 音频 input priority 8 固定 core 0、24 KB Opus task stack、PSRAM/内部 RAM 阈值、60 秒网络/协议重连周期。这些参数依赖 codec、AFE、Wi-Fi 与显示负载，不能照抄到 ESPS3 网关。

明确不要复制：无界 `Schedule()` deque、在 main task 同步等待网络 hello、one-shot reconnect 的非 idle 丢失窗口、无锁读取共享 deque、忽略 decode queue 满、OTA read-error 不 abort，以及没有完成态恢复的手动升级失败路径。
