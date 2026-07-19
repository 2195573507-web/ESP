# esp-zigbee-sdk：ESP32-C5 Zigbee 主循环、commissioning 与持久化边界

## 1. 取证边界

- 仓库：`https://github.com/espressif/esp-zigbee-sdk`
- 本地只读快照：`/tmp/esp32-c5-s3-study-root/esp-zigbee-sdk`
- 固定提交：`7eff0fbe19bcf2acd112ba0b5f080530efc49626`
- 提交主题：`Merge branch 'release/esp-zigbee-sdk-v2.0.3' into 'main'`；提交时间：`2026-07-10T12:48:11+08:00`
- 本次没有运行仓库脚本、构建、pytest、安装或硬件测试。固定 commit 与代表性 blob permalink 在线返回 HTTP 200；运行行为均标明是源码合同、CI 配置还是待上板验证。
- 深读对象是明确列出 C5 的 `home_automation_devices/on_off_light`，以及 CI 把 C5 纳入 build/pytest、但 README 仍只列 H2/C6 的 `sleepy_devices/deep_sleep_end_device`。后者属于“C5 验证路径已配置、文档尚未对齐”，不能写成已在本轮实测通过。

## 2. 许可与预编译栈边界

- 根目录和 `components/esp-zigbee-lib` 均提供 Apache-2.0 文本，示例源码自身多标 CC0-1.0：[root LICENSE L2-L4](https://github.com/espressif/esp-zigbee-sdk/blob/7eff0fbe19bcf2acd112ba0b5f080530efc49626/LICENSE#L2-L4)、[component LICENSE L2-L4](https://github.com/espressif/esp-zigbee-sdk/blob/7eff0fbe19bcf2acd112ba0b5f080530efc49626/components/esp-zigbee-lib/LICENSE#L2-L4)、[on_off_light.c L1-L5](https://github.com/espressif/esp-zigbee-sdk/blob/7eff0fbe19bcf2acd112ba0b5f080530efc49626/examples/home_automation_devices/on_off_light/main/on_off_light.c#L1-L5)。本快照没有发现单独的二进制许可证文件，但正式分发仍应以对应 Component Registry 版本元数据为准。
- README 明确说明 v2.x 使用 **Espressif proprietary Zigbee stack**，并以 `esp-zigbee-lib` 预编译库交付：[README.md L9-L28](https://github.com/espressif/esp-zigbee-sdk/blob/7eff0fbe19bcf2acd112ba0b5f080530efc49626/README.md#L9-L28)。这里的 proprietary 直接意味着核心实现源码不可审计，不能据 Apache 头文件把内部调度实现视为开源。
- 组件 CMake 只编译少量日志、mbedTLS 适配和可选 Spinel UART 源码；核心按 target、ZC/ZR 或 ZED、native 或 remote、debug 或 release 选择三组 `.a`：[CMakeLists.txt L1-L67](https://github.com/espressif/esp-zigbee-sdk/blob/7eff0fbe19bcf2acd112ba0b5f080530efc49626/components/esp-zigbee-lib/CMakeLists.txt#L1-L67)。C5 快照同时具有 ZC/ZR、ZED、native/remote 的 debug/release 资产，但主循环、内部队列、锁、路由、重试和 NVS 后端实现仍不可见。

## 3. C5 支持证据

- 根 README 将 ESP32-C5-DevKitM-1 列为参考硬件，on/off light README 将 C5 列为支持 target：[README.md L34-L50](https://github.com/espressif/esp-zigbee-sdk/blob/7eff0fbe19bcf2acd112ba0b5f080530efc49626/README.md#L34-L50)、[on_off_light README L1-L15](https://github.com/espressif/esp-zigbee-sdk/blob/7eff0fbe19bcf2acd112ba0b5f080530efc49626/examples/home_automation_devices/on_off_light/README.md#L1-L15)。组件 Kconfig 在有原生 802.15.4 的 target 上默认 native radio，并可选 ZC/ZR 或 ZED：[Kconfig L16-L57](https://github.com/espressif/esp-zigbee-sdk/blob/7eff0fbe19bcf2acd112ba0b5f080530efc49626/components/esp-zigbee-lib/Kconfig#L16-L57)。
- 动态 CI 的 default/LTS pipeline 均生成 C5 build 与 pytest job；`tools/ci/build_apps.py` 把 on/off light、deep/light sleep 等列进 C5 generic pytest app，公共 decorator 也带 `pytest.mark.esp32c5`：[pipeline L21-L33](https://github.com/espressif/esp-zigbee-sdk/blob/7eff0fbe19bcf2acd112ba0b5f080530efc49626/.gitlab/ci/generate_build_test_jobs.yml#L21-L33)、[build_apps.py L12-L34](https://github.com/espressif/esp-zigbee-sdk/blob/7eff0fbe19bcf2acd112ba0b5f080530efc49626/tools/ci/build_apps.py#L12-L34)、[zigbee_common.py L24-L50](https://github.com/espressif/esp-zigbee-sdk/blob/7eff0fbe19bcf2acd112ba0b5f080530efc49626/examples/zigbee_common.py#L24-L50)。CI 常量记录 C5 从 IDF `v5.5.2` 起支持，仓库 README 推荐 `v5.5.4`：[constants.py L8-L23](https://github.com/espressif/esp-zigbee-sdk/blob/7eff0fbe19bcf2acd112ba0b5f080530efc49626/tools/ci/dynamic_pipelines/constants.py#L8-L23)。
- 这些是维护者的支持与验证配置，不是本 SHA 的绿色 job 证明。本轮没有读取 GitLab job 结果，也没有本地 C5 toolchain build。

## 4. 代表性 C5 Coordinator：任务与数据模型

```text
app_main
  -> init default NVS + 16 KiB zb_storage NVS
  -> xTaskCreate Zigbee_main (stack 4096, priority 5, unpinned)
       -> esp_zigbee_init
       -> register signal handler + channel/security policy
       -> create device -> endpoint 10 -> Basic/Identify/Groups/Scenes/OnOff clusters
       -> esp_zigbee_start(false)
       -> esp_zigbee_launch_mainloop()
       -> callbacks execute on the Zigbee main-loop path
```

- `esp_zigbee_start(false)` 只安排启动 callback 并返回，应用必须随后进入 main loop；示例将完整 Zigbee owner 放在一个 4 KiB、priority 5 的 FreeRTOS task 中，主循环返回后才 deinit/self-delete：[esp_zigbee.h L126-L170](https://github.com/espressif/esp-zigbee-sdk/blob/7eff0fbe19bcf2acd112ba0b5f080530efc49626/components/esp-zigbee-lib/include/esp_zigbee.h#L126-L170)、[on_off_light.c L166-L190](https://github.com/espressif/esp-zigbee-sdk/blob/7eff0fbe19bcf2acd112ba0b5f080530efc49626/examples/home_automation_devices/on_off_light/main/on_off_light.c#L166-L190)。C5 为单核时，该 task 会与 Wi-Fi、timer、GPIO 和业务 worker 共用 core 0，不能照搬优先级而不测 high-water 与 callback latency。
- Zigbee API 默认不线程安全：main-loop callback 内可直接调用；其他 task 应先取得 Zigbee lock，或用 `esp_zigbee_task_queue_post()` 投递到 Zigbee task：[esp_zigbee.h L172-L209](https://github.com/espressif/esp-zigbee-sdk/blob/7eff0fbe19bcf2acd112ba0b5f080530efc49626/components/esp-zigbee-lib/include/esp_zigbee.h#L172-L209)、[developing.rst L361-L392](https://github.com/espressif/esp-zigbee-sdk/blob/7eff0fbe19bcf2acd112ba0b5f080530efc49626/docs/en/developing.rst#L361-L392)。公开接口没有给出 task queue 深度、满队列策略或执行 deadline；v2 还移除了旧 queue-size setter，因此必须把 `ESP_FAIL` 当作真实背压信号，而不能假设必达。
- ZCL callback 直接在回调路径驱动 LED；它只按 cluster ID 分支，没有验证 endpoint、cluster role、attribute ID/type、`message->info.status` 或 value 指针：[on_off_light.c L108-L136](https://github.com/espressif/esp-zigbee-sdk/blob/7eff0fbe19bcf2acd112ba0b5f080530efc49626/examples/home_automation_devices/on_off_light/main/on_off_light.c#L108-L136)。产品代码应在 callback 内完成严格校验并投递小事件，耗时 I/O 不应占住 Zigbee main loop。

## 5. Signal handler、commissioning 与延迟动作

- Coordinator 的 BDB 状态机是：`SKIP_STARTUP -> INITIALIZATION`；factory-new 成功后 `FORMATION -> NETWORK_STEERING`；非 factory-new 则复用 NVS 网络并开放 180 秒；设备 announce 同时覆盖首次 commission 和 rejoin：[on_off_light.c L41-L105](https://github.com/espressif/esp-zigbee-sdk/blob/7eff0fbe19bcf2acd112ba0b5f080530efc49626/examples/home_automation_devices/on_off_light/main/on_off_light.c#L41-L105)。失败统一固定 1 秒重试，没有 reason 分类、指数 backoff、jitter、上限或失败计数。
- v2.0.3 已移除 `esp_zb_scheduler_alarm*`、iteration API 和 scheduler queue-size setter：[migration common.rst L125-L140](https://github.com/espressif/esp-zigbee-sdk/blob/7eff0fbe19bcf2acd112ba0b5f080530efc49626/docs/en/migration-guide/v2.x/common.rst#L125-L140)。示例的 `alarm_timer_schedule()` 每次 heap 分配一个 one-shot `esp_timer`，触发时先 delete timer，再执行用户 callback，最后 free；create/start 失败路径会释放资源：[alarm_timer.c L19-L68](https://github.com/espressif/esp-zigbee-sdk/blob/7eff0fbe19bcf2acd112ba0b5f080530efc49626/examples/utils/alarm_timer/src/alarm_timer.c#L19-L68)。
- 调用方忽略 `alarm_timer_schedule()` 返回值，helper 又不返回可取消 handle，也没有 retry 去重；OOM、timer 创建失败会让恢复静默消失，重复 signal 则可能挂多个相同重试。定时 callback 还用 `portMAX_DELAY` 等 Zigbee lock：[on_off_light.c L34-L39](https://github.com/espressif/esp-zigbee-sdk/blob/7eff0fbe19bcf2acd112ba0b5f080530efc49626/examples/home_automation_devices/on_off_light/main/on_off_light.c#L34-L39)。产品化应采用 generation/token 去重、有限 lock wait、bounded backoff 和可观测失败计数。

## 6. Endpoint/cluster 所有权与容量

- Device/endpoint descriptor 都是 opaque `void *`，create API 明确可能失败，并分别提供 free；`device_add_endpoint_desc()` 只写“links”，`device_desc_register()` 没有声明是否转移所有权：[af.h L141-L168](https://github.com/espressif/esp-zigbee-sdk/blob/7eff0fbe19bcf2acd112ba0b5f080530efc49626/components/esp-zigbee-lib/include/ezbee/af.h#L141-L168)、[L301-L376](https://github.com/espressif/esp-zigbee-sdk/blob/7eff0fbe19bcf2acd112ba0b5f080530efc49626/components/esp-zigbee-lib/include/ezbee/af.h#L301-L376)。示例创建 device/endpoint/cluster 后不检查 invalid handle、不保留 handle、不显式 free，实质采用“注册后随 stack 生命周期”前提；中途失败由 `ESP_ERROR_CHECK` 终止，而不是逆序回滚。[on_off_light.c L138-L153](https://github.com/espressif/esp-zigbee-sdk/blob/7eff0fbe19bcf2acd112ba0b5f080530efc49626/examples/home_automation_devices/on_off_light/main/on_off_light.c#L138-L153)
- Coordinator 显式限制 `max_children=10`，endpoint 固定为 10；公开 API允许配置 maximum endpoint 数和 buffer/address/neighbor/route/binding tables，但 0 表示使用闭源栈默认值：[on_off_light.h L12-L26](https://github.com/espressif/esp-zigbee-sdk/blob/7eff0fbe19bcf2acd112ba0b5f080530efc49626/examples/home_automation_devices/on_off_light/main/on_off_light.h#L12-L26)、[af.h L285-L309](https://github.com/espressif/esp-zigbee-sdk/blob/7eff0fbe19bcf2acd112ba0b5f080530efc49626/components/esp-zigbee-lib/include/ezbee/af.h#L285-L309)、[core.h L51-L85](https://github.com/espressif/esp-zigbee-sdk/blob/7eff0fbe19bcf2acd112ba0b5f080530efc49626/components/esp-zigbee-lib/include/ezbee/core.h#L51-L85)。示例未调用 `ezb_config_memory()`，所以实际默认表容量、每项内存和满表淘汰策略不能由本快照证明。

## 7. NVS、rejoin 与 C5 deep sleep

- v2 将 Zigbee persistent datasets 从 FAT blob 改到 NVS；示例有普通 `nvs`、16 KiB `zb_storage` 和 1 KiB `zb_fct`，并在建 task 前初始化两套 NVS：[migration overview L73-L88](https://github.com/espressif/esp-zigbee-sdk/blob/7eff0fbe19bcf2acd112ba0b5f080530efc49626/docs/en/migration-guide/v2.x/overview.rst#L73-L88)、[partitions.csv L1-L8](https://github.com/espressif/esp-zigbee-sdk/blob/7eff0fbe19bcf2acd112ba0b5f080530efc49626/examples/home_automation_devices/on_off_light/partitions.csv#L1-L8)、[on_off_light.c L185-L190](https://github.com/espressif/esp-zigbee-sdk/blob/7eff0fbe19bcf2acd112ba0b5f080530efc49626/examples/home_automation_devices/on_off_light/main/on_off_light.c#L185-L190)。示例没有处理 `ESP_ERR_NVS_NO_FREE_PAGES/NEW_VERSION_FOUND` 的 erase-reinit 路径，README 只要求人工 erase-flash；损坏 NVS 可能直接落入 `ESP_ERROR_CHECK` 重启/abort。
- 公共 dataset API暴露 common、parent、child、group、bind、counter、key、reporting、scene 等 key，并可返回 `NO_MEM`，但 key 到 NVS namespace、事务/掉电原子性、磨损和回收实现都在预编译层：[datasets.h L19-L113](https://github.com/espressif/esp-zigbee-sdk/blob/7eff0fbe19bcf2acd112ba0b5f080530efc49626/components/esp-zigbee-lib/include/ezbee/platform/datasets.h#L19-L113)。`esp_zigbee_factory_reset()` 会清 datasets 并重启；BDB local reset 则保留 outgoing NWK frame counter，两者不能混为普通配置清空：[esp_zigbee.h L220-L227](https://github.com/espressif/esp-zigbee-sdk/blob/7eff0fbe19bcf2acd112ba0b5f080530efc49626/components/esp-zigbee-lib/include/esp_zigbee.h#L220-L227)、[bdb.h L214-L229](https://github.com/espressif/esp-zigbee-sdk/blob/7eff0fbe19bcf2acd112ba0b5f080530efc49626/components/esp-zigbee-lib/include/ezbee/bdb.h#L214-L229)。
- Deep-sleep ZED 配置 `RxOnWhenIdle=false`、end-device timeout 64 分钟、keep-alive 4 秒；factory-new 走 steering，NVS 恢复后的 `DEVICE_REBOOT` 表示已按配置网络信息 join/rejoin，再安排睡眠：[deep_sleep_end_device.h L19-L46](https://github.com/espressif/esp-zigbee-sdk/blob/7eff0fbe19bcf2acd112ba0b5f080530efc49626/examples/sleepy_devices/deep_sleep_end_device/main/deep_sleep_end_device.h#L19-L46)、[deep_sleep_end_device.c L205-L250](https://github.com/espressif/esp-zigbee-sdk/blob/7eff0fbe19bcf2acd112ba0b5f080530efc49626/examples/sleepy_devices/deep_sleep_end_device/main/deep_sleep_end_device.c#L205-L250)、[app_signals.h L173-L208](https://github.com/espressif/esp-zigbee-sdk/blob/7eff0fbe19bcf2acd112ba0b5f080530efc49626/components/esp-zigbee-lib/include/ezbee/app_signals.h#L173-L208)。
- 该示例在发出异步 match/bind 请求后，只给固定 5 秒 awake window，timer 到期便直接 deep sleep，没有等待 bind callback或取消未完成请求：[deep_sleep_end_device.c L114-L203](https://github.com/espressif/esp-zigbee-sdk/blob/7eff0fbe19bcf2acd112ba0b5f080530efc49626/examples/sleepy_devices/deep_sleep_end_device/main/deep_sleep_end_device.c#L114-L203)。pytest 只验证首次连接、进入 deep sleep、醒后 announce/rejoin与连接，不验证 binding 完成：[pytest L136-L154](https://github.com/espressif/esp-zigbee-sdk/blob/7eff0fbe19bcf2acd112ba0b5f080530efc49626/examples/pytest_esp_zigbee_open_examples.py#L136-L154)。网络拥塞下 binding 是否被睡眠截断仍需故障注入。
- README 明确说 deep sleep 由应用管理、每次醒来重启并 re-attach，适合超过约 30 分钟的长睡眠；示例用 20 秒只是功能演示。v2 light sleep 则交给 IDF tickless idle + PM，不再由旧 Zigbee sleep API控制：[deep-sleep README L81-L91](https://github.com/espressif/esp-zigbee-sdk/blob/7eff0fbe19bcf2acd112ba0b5f080530efc49626/examples/sleepy_devices/deep_sleep_end_device/README.md#L81-L91)、[migration common L92-L106](https://github.com/espressif/esp-zigbee-sdk/blob/7eff0fbe19bcf2acd112ba0b5f080530efc49626/docs/en/migration-guide/v2.x/common.rst#L92-L106)。

## 8. 可见故障边界与一个文档/实现冲突

- 所有代表性示例都忽略 `xTaskCreate()` 返回值；main loop 与 deinit 返回值也未检查。task OOM 时 app_main 会正常返回而 Zigbee 永不启动，init/start失败则 `ESP_ERROR_CHECK` 直接终止，没有 supervisor、局部 teardown 或重建策略：[on_off_light.c L166-L190](https://github.com/espressif/esp-zigbee-sdk/blob/7eff0fbe19bcf2acd112ba0b5f080530efc49626/examples/home_automation_devices/on_off_light/main/on_off_light.c#L166-L190)。
- `switch_driver.h` 声称 callback 在 ISR 中执行且禁止阻塞；实现实际上 ISR 只把 pointer 放入 depth-10 queue，priority 10 的 `button_detected` task 才调用 callback：[switch_driver.h L38-L66](https://github.com/espressif/esp-zigbee-sdk/blob/7eff0fbe19bcf2acd112ba0b5f080530efc49626/examples/utils/switch_driver/include/switch_driver.h#L38-L66)、[switch_driver.c L23-L55](https://github.com/espressif/esp-zigbee-sdk/blob/7eff0fbe19bcf2acd112ba0b5f080530efc49626/examples/utils/switch_driver/src/switch_driver.c#L23-L55)。deep-sleep callback 正因实际在 task context，才会用 `portMAX_DELAY` 获取 Zigbee lock并发命令：[deep_sleep_end_device.c L81-L96](https://github.com/espressif/esp-zigbee-sdk/blob/7eff0fbe19bcf2acd112ba0b5f080530efc49626/examples/sleepy_devices/deep_sleep_end_device/main/deep_sleep_end_device.c#L81-L96)。产品不能依赖错误头注释，且 C5 上 priority 10 button task等待 priority 5 Zigbee owner 的锁是否有优先级继承，闭源锁实现不可见，必须测量。
- switch driver 还在创建 queue/task 后才安装 ISR，未检查 task create；后续 ISR 安装失败会遗留 task/queue，deinit 也不删除它们：[switch_driver.c L57-L116](https://github.com/espressif/esp-zigbee-sdk/blob/7eff0fbe19bcf2acd112ba0b5f080530efc49626/examples/utils/switch_driver/src/switch_driver.c#L57-L116)。这是“启动一次活到重启”的示例假设，不适合作为热重建组件模板。

## 9. 对 ESP-111 的可借鉴与未验证项

1. C5 Zigbee 应维持单一 stack-owner task；非 Zigbee callback 只通过有限 lock或 `esp_zigbee_task_queue_post()` 进入协议栈，CSI/sensor/network callback 不直接串行执行 Zigbee重逻辑。
2. 将 signal/ZCL callback 收窄为校验、复制最小值和投递 typed event；`STATE` 可 coalesce，但 commissioning、leave、factory-reset、bind result属于控制事件，必须确认投递成功并可重试。
3. 不复制固定 1 秒无限重试。至少记录 signal/status、attempt、queue/lock failure，并使用有上限的指数 backoff + jitter + generation 去重。
4. `zb_storage` 需要版本、容量水位、NVS 错误恢复、迁移和 factory-reset 合同；不能只依赖人工 erase-flash，也不能把 BDB local reset 当成完全擦除。
5. C5 单核上要同时 profile Zigbee main task、Wi-Fi/CSI、esp_timer与 GPIO worker 的 stack high-water、callback latency和 radio coexistence；本仓库 pure-Zigbee 示例不能证明 ESP-111 并发负载下的实时性。
6. 未验证：该 SHA 的 GitLab job 绿色状态、C5 真机 commission/rejoin、掉电中的 NVS 原子性、深睡前 dataset flush、表满行为、task queue容量/公平性、锁实现与优先级继承、native radio buffer、descriptor register后的所有权、Wi-Fi/802.15.4 共存时延、长时间功耗和 RF 稳定性。
