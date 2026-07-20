# `esp-matter`：ESP32-C5 / ESP32-S3 与 Light 调度深读

> 本地只读研究快照：2026-07-10（Asia/Shanghai）。仓库：[`espressif/esp-matter`](https://github.com/espressif/esp-matter)，固定提交 [`0b82a30549606689e6ea050d7579b382406316dc`](https://github.com/espressif/esp-matter/tree/0b82a30549606689e6ea050d7579b382406316dc)（提交时间 2026-07-08，`Merge branch 'door_lock/add_aliro_support' into 'main'`）。以下链接均固定到该 SHA；未联网、未编译、未上板。

## 1. 结论先行

- **项目定位**：Espressif 在上游 Matter SDK 之上的设备端框架，封装动态数据模型、平台/网络初始化、commissioning、OTA、NVS 和板级 HAL；官方文档直接说明它建立在 `connectedhomeip` 之上（[`docs/en/introduction.rst#L28-L36`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/docs/en/introduction.rst#L28-L36)）。
- **C5 / S3 支持强度**：最新文档目标列表同时包含 `esp32c5` 和 `esp32s3`（[`docs/_static/esp_sdk_matter_version.js#L5-L20`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/docs/_static/esp_sdk_matter_version.js#L5-L20)）；`examples/light` 的 CMake 对两者都选择专用 DevKit HAL（[`examples/light/CMakeLists.txt#L22-L31`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/examples/light/CMakeLists.txt#L22-L31)），且各有目标默认文件（[`sdkconfig.defaults.esp32c5#L1`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/examples/light/sdkconfig.defaults.esp32c5#L1)、[`sdkconfig.defaults.esp32s3#L1`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/examples/light/sdkconfig.defaults.esp32s3#L1)）。这是“源码/配置支持”，不是本次硬件验证。
- **网络能力不要混同**：S3 属于 Wi-Fi Matter 设备路径；C5 同时具备 Wi-Fi 与 802.15.4，可跑 Wi-Fi 或 Thread（[`docs/en/introduction.rst#L28-L31`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/docs/en/introduction.rst#L28-L31)、[`docs/en/developing.rst#L350-L355`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/docs/en/developing.rst#L350-L355)）。Light 还提供 C5 的 Thread-only 与 Wi-Fi+Thread 双接口预设。
- **代表样例**：选择 `examples/light`。它用同一应用层覆盖 C5/S3，创建 Extended Color Light endpoint，把 Matter 属性更新同步到 LED，并演示 commissioning 生命周期、延迟持久化、按钮本地控制和 factory reset。
- **核心调度模型**：不是“`app_main` 自己收业务队列”，而是 **ESP-IDF `app_main` 任务 + CHIP event-loop task + 可选 OpenThread task**。`ScheduleWork()` 把 server 初始化串行投递到 CHIP 线程；调用者用 FreeRTOS task notification 等待。事件经 `PlatformMgr().PostEvent()` 入 CHIP 平台事件队列，再依次进入内部及应用回调。

## 2. 芯片支持证据与边界

| 项目 | ESP32-C5 | ESP32-S3 | 证据与解释 |
|---|---|---|---|
| 文档目标 | 是 | 是 | `latest`、`release-v1.5`、`release-v1.4.2` 均列出二者（[`version.js#L5-L13`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/docs/_static/esp_sdk_matter_version.js#L5-L13)）。 |
| Light 构建选择 | `esp32c5_devkit_c` | `esp32s3_devkit_c` | [`examples/light/CMakeLists.txt#L22-L31`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/examples/light/CMakeLists.txt#L22-L31)；两块板都声明 WS2812 + iot button（[`C5 HAL#L1-L11`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/device_hal/device/esp32c5_devkit_c/esp_matter_device.cmake#L1-L11)、[`S3 HAL#L1-L11`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/device_hal/device/esp32s3_devkit_c/esp_matter_device.cmake#L1-L11)）。 |
| Thread | 原生，仓库有明确预设 | 无原生 802.15.4 设备路径 | C5 Thread-only 打开 OpenThread、关闭 Wi-Fi station（[`c5_thread#L6-L19`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/examples/light/sdkconfig.defaults.c5_thread#L6-L19)、[`#L32-L40`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/examples/light/sdkconfig.defaults.c5_thread#L32-L40)）；双接口配置增加第 3 个动态 endpoint 并同时开启 Wi-Fi（[`c5_wifi_thread#L24-L44`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/examples/light/sdkconfig.defaults.c5_wifi_thread#L24-L44)）。 |
| 板级 IO | GPIO28 button / GPIO27 RGB | 由 S3 HAL 配置层提供 | C5 预设见 [`c5_thread#L42-L59`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/examples/light/sdkconfig.defaults.c5_thread#L42-L59)；应用不硬编码引脚，而从 `device_hal` 取配置。 |

**CI caveat**：当前 `examples/.build-rules.yml` 的通用 Light 构建白名单没有 C5 或 S3，并明确写着其他目标“not tested yet”（[`examples/.build-rules.yml#L24-L28`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/examples/.build-rules.yml#L24-L28)）。测试文件虽然带 `esp32s3` 标记，但参数是 `esp32h2|esp32s3`，其中 Light 是 H2、S3 是 Thread Border Router（[`pytest_esp_matter_light.py#L238-L256`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/examples/pytest_esp_matter_light.py#L238-L256)）；不能把它当作 S3 Light 的闭环测试。C5/S3 文档会构建（[`.gitlab-ci.yml#L807-L810`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/.gitlab-ci.yml#L807-L810)），但这也不等于固件构建/上板通过。

## 3. Light 的功能闭环

1. `app_main` 初始化 NVS、LED/button、reset 回调，创建 Matter node 和 Extended Color Light endpoint；初始属性包含 OnOff、LevelControl 和 ColorControl（[`app_main.cpp#L166-L204`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/examples/light/main/app_main.cpp#L166-L204)）。
2. 远程写属性先进入 `PRE_UPDATE`，应用把 endpoint/cluster/attribute 路由给驱动（[`app_main.cpp#L149-L163`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/examples/light/main/app_main.cpp#L149-L163)）；驱动再映射 power/brightness/hue/saturation/color-temperature/XY 到 LED HAL（[`app_driver.cpp#L31-L64`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/examples/light/main/app_driver.cpp#L31-L64)、[`#L81-L111`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/examples/light/main/app_driver.cpp#L81-L111)）。PRE_UPDATE 返回驱动错误，因此硬件拒绝更新时可阻止数据模型提交。
3. 本地按钮回调读取 `OnOff`、取反并调用 `attribute::update()`，因此本地动作也走相同的数据模型/回调/报告路径（[`app_driver.cpp#L66-L79`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/examples/light/main/app_driver.cpp#L66-L79)）。
4. 创建非易失 attribute 时框架先尝试从 NVS 恢复（[`esp_matter_data_model.cpp#L530-L550`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/components/esp_matter/data_model/esp_matter_data_model.cpp#L530-L550)）；`esp_matter::start(app_event_cb)` 后，Light 把当前数据模型值下发到 LED，然后 `app_main` 只保留 10 秒一次的 heap 观测循环（[`app_main.cpp#L243-L270`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/examples/light/main/app_main.cpp#L243-L270)）。

## 4. 任务、事件与队列

### 4.1 启动时序

```text
ESP-IDF app_main task
  -> 创建 node / light endpoint / drivers
  -> esp_matter::start(app_event_cb)
       -> 创建 ESP 默认 event loop，初始化 Wi-Fi（若启用）
       -> InitChipStack + StartEventLoopTask              [CHIP task]
       -> 注册 internal callback + app_event_cb
       -> 初始化 Thread stack 并 StartThreadTask（若启用） [OT task]
       -> ScheduleWork(esp_matter_chip_init_task)
       -> xTaskNotifyWait(portMAX_DELAY)                   [app_main 阻塞]
CHIP task
  -> 初始化 Server/Fabric/DataModel/endpoint
  -> xTaskNotifyGive(app_main)
app_main
  -> 下发恢复后的属性到 LED
  -> 周期 heap 日志，不承担协议事件泵
```

- CHIP task 的创建、回调注册、`ScheduleWork` 和通知同步位于 [`esp_matter_core.cpp#L316-L352`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/components/esp_matter/esp_matter_core.cpp#L316-L352)；server 初始化完成后 `xTaskNotifyGive`（[`#L177-L223`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/components/esp_matter/esp_matter_core.cpp#L177-L223)、[`#L246-L250`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/components/esp_matter/esp_matter_core.cpp#L246-L250)）。
- C5 Thread 会额外初始化 Thread stack、选择设备类型并启动 Thread task（[`esp_matter_core.cpp#L287-L313`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/components/esp_matter/esp_matter_core.cpp#L287-L313)）。Light 的原生 Thread port 配置明确给 `netif_queue_size=10`、`task_queue_size=10`（[`app_priv.h#L83-L97`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/examples/light/main/app_priv.h#L83-L97)）。

### 4.2 事件与背压语义

- commissioning/fabric delegate 不直接调用应用，而是构造 `ChipDeviceEvent` 并 `PlatformMgr().PostEvent()`；入队失败只记录错误、事件被丢弃，无重试（[`esp_matter_core.cpp#L79-L107`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/components/esp_matter/esp_matter_core.cpp#L79-L107)、[`#L110-L130`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/components/esp_matter/esp_matter_core.cpp#L110-L130)）。
- 内部回调先处理 IP、DNS-SD、OTA/binding 和 commissioning 完成后的 BLE 释放；BLE 释放仍通过 `ScheduleWork()` 串行回 CHIP task（[`esp_matter_core.cpp#L253-L280`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/components/esp_matter/esp_matter_core.cpp#L253-L280)）。应用回调只观察生命周期并做少量控制，不应在其中长时间阻塞。
- Light **没有自建 `QueueHandle_t`**。这里存在三种不同机制：CHIP 平台事件队列、CHIP `ScheduleWork` 工作投递、OpenThread 的 netif/task queue；不能用 C5 的两个“10”推断 CHIP event queue 容量。

## 5. Commissioning 与 fabric 生命周期

- 应用记录 IP 变化、commissioning start/stop/complete/fail-safe、window open/close、fabric add/update/remove 和 BLE deinit 等事件（[`app_main.cpp#L66-L138`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/examples/light/main/app_main.cpp#L66-L138)）。
- 删除最后一个 fabric 后，如果 window 未开，应用打开 **300 秒 Basic Commissioning Window，且只做 DNS-SD 广播**；源码明确说明保留已有 Wi-Fi credentials/IP 连通性（[`app_main.cpp#L99-L115`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/examples/light/main/app_main.cpp#L99-L115)）。这不是 BLE 重新配网流程。
- C5 Thread-only 预设仍打开 NimBLE，并设 `CONFIG_USE_BLE_ONLY_FOR_COMMISSIONING=n`（[`c5_thread#L6-L14`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/examples/light/sdkconfig.defaults.c5_thread#L6-L14)）；是否在入网后释放 BLE 由框架配置/内部事件决定。应用 README 也提醒可在不再需要时禁用 BLE（[`README.md#L40`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/examples/light/README.md#L40)）。
- 配置把长按阈值设为 5000 ms（[`sdkconfig.defaults#L30-L32`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/examples/light/sdkconfig.defaults#L30-L32)），松开后才触发 factory reset：button 回调调用 `esp_matter::factory_reset()`（[`app_reset.cpp#L20-L47`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/examples/common/app_reset/app_reset.cpp#L20-L47)）；框架先擦除自己的 KVS namespace，再调 Matter Server 安排全量 reset/restart（[`esp_matter_core.cpp#L409-L437`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/components/esp_matter/esp_matter_core.cpp#L409-L437)）。

## 6. 持久化

- 根配置定义 `nvs` 为非易失属性分区，延迟写默认 3000 ms，目的就是避免高频属性磨损 flash（[`components/esp_matter/Kconfig#L17-L28`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/components/esp_matter/Kconfig#L17-L28)）。Light 对 `CurrentLevel`、`CurrentX/Y` 和 `ColorTemperatureMireds` 启用延迟持久化（[`app_main.cpp#L206-L215`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/examples/light/main/app_main.cpp#L206-L215)）。
- 属性更新先执行应用 PRE_UPDATE；提交内存值后，非易失属性若带 deferred flag 就只在 timer 未激活时启动一次 timer，否则立即写 NVS（[`esp_matter_data_model.cpp#L667-L700`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/components/esp_matter/data_model/esp_matter_data_model.cpp#L667-L700)、[`#L752-L768`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/components/esp_matter/data_model/esp_matter_data_model.cpp#L752-L768)）。这实际是“首个变更后固定延迟写当前值”，不是每次变更都重置的 debounce。
- NVS key 由 endpoint/cluster/attribute 生成，统一写 `esp_matter_kvs`；读取不到新 key 时会尝试旧 namespace/key 并迁移（[`esp_matter_nvs.cpp#L280-L318`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/components/esp_matter/data_model/private/esp_matter_nvs.cpp#L280-L318)）。分区表还包含 `nvs`、encrypted `nvs_keys`、`esp_secure_cert` 与 `fctry`（[`partitions.csv#L1-L10`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/examples/light/partitions.csv#L1-L10)）。

## 7. 错误处理审计

**做得较稳的路径**

- node/endpoint/Matter start/加密 OTA 初始化失败用 `ABORT_APP_ON_FAILURE`：记录日志、等 5 秒、`abort()`（[`app_main.cpp#L180-L201`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/examples/light/main/app_main.cpp#L180-L201)、[`#L243-L255`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/examples/light/main/app_main.cpp#L243-L255)、宏定义 [`common_macros.h#L37-L43`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/examples/common/utils/common_macros.h#L37-L43)）。
- `start()` 防重复启动，允许默认 ESP event loop 已存在，Wi-Fi/CHIP 初始化失败会返回并触发应用 abort（[`esp_matter_core.cpp#L369-L385`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/components/esp_matter/esp_matter_core.cpp#L369-L385)）。
- 平台 task/callback 创建失败会停止 event-loop task、释放 CHIP memory 后返回（[`esp_matter_core.cpp#L316-L349`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/components/esp_matter/esp_matter_core.cpp#L316-L349)）。

**静态可见的缺口（未做故障注入）**

1. `nvs_flash_init()`、driver/reset 注册、deferred flag 设置和 `app_driver_light_set_defaults()` 的返回值均未检查（集中见 [`app_main.cpp#L170-L178`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/examples/light/main/app_main.cpp#L170-L178)、[`#L206-L215`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/examples/light/main/app_main.cpp#L206-L215)、[`#L249-L250`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/examples/light/main/app_main.cpp#L249-L250)）。按钮回调也忽略 `get_val/update` 结果（[`app_driver.cpp#L73-L79`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/examples/light/main/app_driver.cpp#L73-L79)）。
2. `InitializeStaticResourcesBeforeServerInit()` 失败会直接 `return`，没有执行末尾 `xTaskNotifyGive()`（[`esp_matter_core.cpp#L177-L188`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/components/esp_matter/esp_matter_core.cpp#L177-L188)）；调用方却在 `xTaskNotifyWait(..., portMAX_DELAY)` 无限等待（[`#L344-L351`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/components/esp_matter/esp_matter_core.cpp#L344-L351)）。**静态推断：该罕见失败路径可能死锁启动任务。**
3. Thread stack 启动函数本身返回错误，但 `chip_init()` 没检查其返回值（[`esp_matter_core.cpp#L287-L313`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/components/esp_matter/esp_matter_core.cpp#L287-L313)、[`#L342`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/components/esp_matter/esp_matter_core.cpp#L342)）。Server/Fabric/endpoint 初始化若失败，多数也只日志后继续并最终通知成功（[`#L212-L230`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/components/esp_matter/esp_matter_core.cpp#L212-L230)）。
4. deferred timer 回调和立即持久化路径都忽略 `store_val_in_nvs()` 返回值（[`esp_matter_data_model.cpp#L657-L665`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/components/esp_matter/data_model/esp_matter_data_model.cpp#L657-L665)、[`#L752-L765`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/components/esp_matter/data_model/esp_matter_data_model.cpp#L752-L765)）；NVS helper 也不检查 `nvs_commit()`（[`esp_matter_nvs.cpp#L180-L198`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/components/esp_matter/data_model/private/esp_matter_nvs.cpp#L180-L198)、[`#L262-L264`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/components/esp_matter/data_model/private/esp_matter_nvs.cpp#L262-L264)）。掉电/flash 故障可能表现为 RAM 已更新但重启后回退，应用层没有明确告警闭环。

## 8. 对 ESP-111 的可迁移启发

- 让协议栈拥有单一串行执行上下文，业务线程只用 `ScheduleWork`/event 投递；跨线程启动用明确 completion 信号。ESP-111 若采用这种模型，必须让 completion 同时携带 status，并保证每个 early-return 都通知，避免本项目第 7.2 条的死等。
- 高频状态持久化应做延迟合并并区分 RAM truth / flash checkpoint；但需要写失败计数、重试上限和可观测告警，不能像样例一样丢弃返回码。
- commissioning/fabric 事件适合作为状态机输入，不适合在回调内做重活。事件队列满时要定义丢弃/合并/重试策略；本项目仅 log-and-drop，不宜直接照搬到需要强一致的网关状态。
- 板级差异放在 HAL/CMake 选择层是可复用做法：上层 Light 数据模型完全共用；C5/S3 只在 radio preset 与 device HAL 分叉。

## 9. 许可证、来源完整性与核验边界

- 根仓库 `LICENSE` 是 Apache License 2.0（[`LICENSE#L1-L5`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/LICENSE#L1-L5)）；`NOTICE` 明确项目为 Apache-2.0，同时声明 Matter 认证、商标和潜在第三方权利并不随源码许可自动授予（[`NOTICE#L1-L16`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/NOTICE#L1-L16)、[`#L18-L45`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/NOTICE#L18-L45)）。
- `examples/light/main/*.cpp` 文件头另标“Public Domain (or CC0)”（例如 [`app_main.cpp#L1-L7`](https://github.com/espressif/esp-matter/blob/0b82a30549606689e6ea050d7579b382406316dc/examples/light/main/app_main.cpp#L1-L7)）。复用时应按具体文件与依赖逐项保留许可/NOTICE，不能只用根许可概括全部第三方内容。
- 本地是 shallow、blobless clone；`connectedhomeip/connectedhomeip` gitlink 固定为 `c992539e738378e6e9a13c2f8cd47b1c878d069b`，但子模块未检出。因此本文能证明 esp-matter 包装层如何调用 `PlatformMgr`，**不能证明上游 CHIP event queue 的实际长度、FreeRTOS task priority/core affinity 或内部出队公平性**。
- 未执行 `idf.py build`：固定快照缺少 `connectedhomeip` 子模块，且任务明确不联网。未做 C5/S3 编译、烧录、commissioning、断电恢复、队列饱和或故障注入；相关结论均严格标为源码/配置证明或静态推断。
