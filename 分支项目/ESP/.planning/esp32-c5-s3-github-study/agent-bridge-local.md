# esp-iot-bridge：C5/S3 支持与 Wi-Fi Router 调度证据

## 1. 取证边界

- 官方仓库：`https://github.com/espressif/esp-iot-bridge`
- 本地只读浅克隆：`/tmp/esp32-c5-s3-study-root/esp-iot-bridge`
- 固定提交：`667766b0feefb199afc5de7c59fc330048641fcb`
- 提交主题：`Merge branch 'feature/support_idf_6.0' into 'master'`；提交时间：`2026-07-07T20:06:19+08:00`
- 取证时 `git status --porcelain=v1` 为空，`git rev-parse --is-shallow-repository` 为 `true`。本文只陈述该快照；没有联网、没有执行 CI、没有下载依赖、没有本地编译或硬件实测。
- 许可范围必须精确表述：`components/iot_bridge/license.txt` 是 Apache License 2.0，本文深读的 C 源文件也声明 `SPDX-License-Identifier: Apache-2.0`（[license.txt L2-L4](https://github.com/espressif/esp-iot-bridge/blob/667766b0feefb199afc5de7c59fc330048641fcb/components/iot_bridge/license.txt#L2-L4)、[bridge_common.c L1-L5](https://github.com/espressif/esp-iot-bridge/blob/667766b0feefb199afc5de7c59fc330048641fcb/components/iot_bridge/src/bridge_common.c#L1-L5)、[bridge_wifi.c L1-L5](https://github.com/espressif/esp-iot-bridge/blob/667766b0feefb199afc5de7c59fc330048641fcb/components/iot_bridge/src/bridge_wifi.c#L1-L5)）。仓库根没有统一 `LICENSE`；例如 Linux host driver 子树另有 GPL-2.0（[host driver LICENSE L1-L7](https://github.com/espressif/esp-iot-bridge/blob/667766b0feefb199afc5de7c59fc330048641fcb/examples/spi_and_sdio_host/host_driver/linux/host_driver/esp32/LICENSE#L1-L7)），因此不能把整个仓库笼统标成纯 Apache-2.0。

## 2. C5 与 S3 的实际支持证据

### 2.1 结论

| 目标 | 源码证据能证明什么 | 不能证明什么 |
|---|---|---|
| ESP32-C5 | Wi-Fi Router README 明列 C5；CI 配置为 C5 建立独立 job，以 `--preview` 在 ESP-IDF 5.5、6.0、6.1 上构建全部五个主示例及 iperf；Kconfig 有 C5 专属 SDIO、GPIO/SPI 默认值。 | 本次未看到该提交对应的 CI 运行结果，也未本地构建或上板；不能声称 5.2-5.4 支持，CI 明确从 5.5 起。 |
| ESP32-S3 | README 明列 S3；CI 在 ESP-IDF 5.2-6.1 上构建全部主示例、iperf，并额外覆盖 BLE、SPI/modem/USB 组合；仓库有 `sdkconfig.defaults.esp32s3`。 | 未读取在线 job 状态，也未验证具体 S3 板卡、USB PHY 或外设接线。 |

### 2.2 可复核锚点

- Wi-Fi Router 的支持表同时列出 ESP32-C5 与 ESP32-S3：[examples/wifi_router/README.md L1-L8](https://github.com/espressif/esp-iot-bridge/blob/667766b0feefb199afc5de7c59fc330048641fcb/examples/wifi_router/README.md#L1-L8)。
- 公共 CI 宏会依次构建 `wifi_router`、`wireless_nic`、`wired_nic`、`4g_hotspot`、`4g_nic`：[.gitlab-ci.yml L114-L155](https://github.com/espressif/esp-iot-bridge/blob/667766b0feefb199afc5de7c59fc330048641fcb/.gitlab-ci.yml#L114-L155)。
- S3 job 的矩阵是 IDF 5.2/5.3/5.4/5.5/6.0/6.1，并调用全部示例和 S3 专项组合：[.gitlab-ci.yml L278-L298](https://github.com/espressif/esp-iot-bridge/blob/667766b0feefb199afc5de7c59fc330048641fcb/.gitlab-ci.yml#L278-L298)；专项组合覆盖 BLE，以及 SPI + modem + USB：[.gitlab-ci.yml L93-L112](https://github.com/espressif/esp-iot-bridge/blob/667766b0feefb199afc5de7c59fc330048641fcb/.gitlab-ci.yml#L93-L112)。
- C5 job 的矩阵是 IDF 5.5/6.0/6.1，设置 `compile_option: --preview`，并调用全部示例、默认扩展组合和 iperf：[.gitlab-ci.yml L322-L343](https://github.com/espressif/esp-iot-bridge/blob/667766b0feefb199afc5de7c59fc330048641fcb/.gitlab-ci.yml#L322-L343)。这是比 README 更强的“维护者持续编译意图”证据，但不是本次运行成功证明。
- 组件清单全局要求 IDF `>=5.2`；S3 选择 USB DTE/TinyUSB，C5 落入非 S2/S3/S31 的 `esp_modem` 分支：[components/iot_bridge/idf_component.yml L8-L22](https://github.com/espressif/esp-iot-bridge/blob/667766b0feefb199afc5de7c59fc330048641fcb/components/iot_bridge/idf_component.yml#L8-L22)。实践上应以目标 CI 矩阵为准，C5 不应由此推断为 IDF 5.2 即可用。
- Kconfig 允许 C5 使用 SDIO，而 S3 的 USB data-forwarding 是专属能力之一：[components/iot_bridge/Kconfig L47-L60](https://github.com/espressif/esp-iot-bridge/blob/667766b0feefb199afc5de7c59fc330048641fcb/components/iot_bridge/Kconfig#L47-L60)、[Kconfig L112-L139](https://github.com/espressif/esp-iot-bridge/blob/667766b0feefb199afc5de7c59fc330048641fcb/components/iot_bridge/Kconfig#L112-L139)。
- S3 有目标专属 defaults（TinyUSB、240 MHz、USB 缓冲/URB）；C5 没有 `examples/wifi_router/sdkconfig.defaults.esp32c5`，会使用通用 defaults。这是配置策略差异，不是 C5 不支持：[sdkconfig.defaults.esp32s3 L1-L37](https://github.com/espressif/esp-iot-bridge/blob/667766b0feefb199afc5de7c59fc330048641fcb/examples/wifi_router/sdkconfig.defaults.esp32s3#L1-L37)。

## 3. Wi-Fi Router 的真实拓扑与启动顺序

```text
app_main
  -> NVS -> esp_netif -> default esp_event loop
  -> esp_bridge_create_all_netif()
       -> SoftAP: LAN/data-forwarding + DHCPS + NAPT
       -> Station: WAN/external + DHCPC
  -> configure SoftAP -> esp_wifi_connect(STA)
  -> button/timer -> optional web server / BLE provisioning

IP_EVENT_STA_GOT_IP
  -> propagate WAN DNS to all LAN netifs
  -> detect WAN/LAN subnet collision
  -> if collision: stop LAN DHCPS -> assign new LAN subnet -> restart DHCPS
  -> SoftAP callback deauths clients so they renew addresses
```

- 示例 README 的功能定义就是 SoftAP 通过 NAT 转发 Station 流量：[README L6-L10](https://github.com/espressif/esp-iot-bridge/blob/667766b0feefb199afc5de7c59fc330048641fcb/examples/wifi_router/README.md#L6-L10)。
- `app_main` 不创建业务 FreeRTOS task：它初始化 NVS、netif 和默认事件循环，创建全部 netif，设置 AP，发起 STA 连接，再启动按钮和可选配网服务：[examples/wifi_router/main/app_main.c L107-L137](https://github.com/espressif/esp-iot-bridge/blob/667766b0feefb199afc5de7c59fc330048641fcb/examples/wifi_router/main/app_main.c#L107-L137)。
- `esp_bridge_create_all_netif()` 按编译期 Kconfig 创建所有已启用接口；SoftAP 在前、Station 在末：[bridge_common.c L1054-L1111](https://github.com/espressif/esp-iot-bridge/blob/667766b0feefb199afc5de7c59fc330048641fcb/components/iot_bridge/src/bridge_common.c#L1054-L1111)。角色是编译期/创建参数固定的，不存在 Wi-Fi Router 运行时把 Station 与 SoftAP 互换 WAN/LAN 的逻辑。
- WAN/LAN 枚举只是 `WAN=0`、`LAN=1`：[esp_bridge_config.h L44-L49](https://github.com/espressif/esp-iot-bridge/blob/667766b0feefb199afc5de7c59fc330048641fcb/components/iot_bridge/include/esp_bridge_config.h#L44-L49)。运行期发生的是 LAN **地址段重分配**，不是 netif **角色切换**。

## 4. `bridge_common.c`：控制面与地址调度

### 4.1 netif registry

- 全局单链表保存 `esp_netif_t*`、DNS/DHCPS 回调和冲突检查状态；add 用 `malloc`，防重复，remove 只 `free` 包装节点：[bridge_common.c L48-L57](https://github.com/espressif/esp-iot-bridge/blob/667766b0feefb199afc5de7c59fc330048641fcb/components/iot_bridge/src/bridge_common.c#L48-L57)、[L117-L177](https://github.com/espressif/esp-iot-bridge/blob/667766b0feefb199afc5de7c59fc330048641fcb/components/iot_bridge/src/bridge_common.c#L117-L177)。链表没有 mutex；设计前提显然是启动期串行增删、运行期只遍历/改字段，而不是任意任务并发管理。
- `esp_bridge_netif_list_remove()` 不销毁底层 `esp_netif_t`，找不到也返回 `ESP_OK`。它是 registry detach，不是完整 teardown。
- 自定义网段检查器也用永久链表注册；分配失败仍返回 `true`，且没有 unregister/free API：[bridge_common.c L378-L398](https://github.com/espressif/esp-iot-bridge/blob/667766b0feefb199afc5de7c59fc330048641fcb/components/iot_bridge/src/bridge_common.c#L378-L398)。
- `_esp_bridge_netif_list_add()` 分配节点后没有清零，也没有初始化 `conflict_check`；正常 DHCPS 路径随后由 `esp_bridge_netif_set_ip_info()` 赋值，但“已 add、分配 IP 失败”的异常路径可能留下不确定值。动态复用时应在节点构造处显式初始化：[bridge_common.c L131-L149](https://github.com/espressif/esp-iot-bridge/blob/667766b0feefb199afc5de7c59fc330048641fcb/components/iot_bridge/src/bridge_common.c#L131-L149)。

### 4.2 网段分配与冲突迁移

- 默认池从 `192.168.0.0` 开始，并以 `/24` 作为每个 LAN 的网段大小；前缀可持久化到 NVS。设置时会拒绝无效掩码及与 0/8、127/8、169.254/16、224/4 交叠的范围：[bridge_common.c L37-L63](https://github.com/espressif/esp-iot-bridge/blob/667766b0feefb199afc5de7c59fc330048641fcb/components/iot_bridge/src/bridge_common.c#L37-L63)、[L270-L327](https://github.com/espressif/esp-iot-bridge/blob/667766b0feefb199afc5de7c59fc330048641fcb/components/iot_bridge/src/bridge_common.c#L270-L327)。
- 分配器按 subnet size 遍历候选网段，避开特殊地址、已有 netif 和用户检查器；成功时把网关设为 LAN 接口自身：[bridge_common.c L472-L508](https://github.com/espressif/esp-iot-bridge/blob/667766b0feefb199afc5de7c59fc330048641fcb/components/iot_bridge/src/bridge_common.c#L472-L508)。这是一轮同步线性扫描，没有异步 scheduler。
- 外部 DHCPC 获址后，冲突检查遍历所有 DHCPS LAN；若重叠，则申请新段、停止 DHCPS、写 IP、重启 DHCPS并调用变更回调：[bridge_common.c L555-L660](https://github.com/espressif/esp-iot-bridge/blob/667766b0feefb199afc5de7c59fc330048641fcb/components/iot_bridge/src/bridge_common.c#L555-L660)。SoftAP 回调会 deauth 全部客户端，迫使其重新关联/续租：[bridge_wifi.c L283-L292](https://github.com/espressif/esp-iot-bridge/blob/667766b0feefb199afc5de7c59fc330048641fcb/components/iot_bridge/src/bridge_wifi.c#L283-L292)。
- `esp_bridge_netif_set_ip_info()` 对 DHCPS 采用 stop/change/start；设置或重启失败时尽力恢复旧 IP/DHCPS，对 DHCPC 静态地址则停止客户端并设置静态 DNS：[bridge_common.c L939-L1049](https://github.com/espressif/esp-iot-bridge/blob/667766b0feefb199afc5de7c59fc330048641fcb/components/iot_bridge/src/bridge_common.c#L939-L1049)。
- NVS 向后兼容存在细节缺陷：读取不到 `%.8s_check` 时先写 `*conflict_check=true`，紧接着又无条件用初始值 `0` 覆盖，因此旧记录缺少该键时实际会关闭冲突检查：[bridge_common.c L856-L865](https://github.com/espressif/esp-iot-bridge/blob/667766b0feefb199afc5de7c59fc330048641fcb/components/iot_bridge/src/bridge_common.c#L856-L865)。

### 4.3 DNS 事件与背压

- DHCP/lwIP hook 不直接做全量同步，而是把 `{esp_netif}` 投递到自定义 `BRIDGE_EVENT`；默认事件循环中的 handler 再把 WAN DNS 扇出给所有 LAN 接口：[bridge_common.c L731-L794](https://github.com/espressif/esp-iot-bridge/blob/667766b0feefb199afc5de7c59fc330048641fcb/components/iot_bridge/src/bridge_common.c#L731-L794)、[esp_bridge_events.h L17-L28](https://github.com/espressif/esp-iot-bridge/blob/667766b0feefb199afc5de7c59fc330048641fcb/components/iot_bridge/include/esp_bridge_events.h#L17-L28)。
- 背压策略是 `esp_event_post(..., portMAX_DELAY)`：默认事件队列满时无限等待；没有 drop、coalescing、覆盖旧状态或超时。因为调用点是 DHCP DNS hook，这会把压力反传到调用该 hook 的 lwIP 路径。对 ESP-111 的高频状态事件不宜照搬，至少应区分“必须送达”与“只保留最新”。

## 5. `bridge_wifi.c`：事件处理与重连

- 初始化以全局 EventGroup 是否存在作为一次性 guard，创建 EventGroup、初始化 Wi-Fi、选 RAM storage、设 `WIFI_MODE_NULL` 并启动 Wi-Fi：[bridge_wifi.c L28-L34](https://github.com/espressif/esp-iot-bridge/blob/667766b0feefb199afc5de7c59fc330048641fcb/components/iot_bridge/src/bridge_wifi.c#L28-L34)、[L135-L151](https://github.com/espressif/esp-iot-bridge/blob/667766b0feefb199afc5de7c59fc330048641fcb/components/iot_bridge/src/bridge_wifi.c#L135-L151)。`xEventGroupCreate()` 和 `esp_wifi_start()` 的返回值未检查。
- STA got-IP handler 同步 DNS、触发网段冲突迁移、置 connected bit；disconnect handler默认立即调用一次 `esp_wifi_connect()` 并清 bit：[bridge_wifi.c L157-L178](https://github.com/espressif/esp-iot-bridge/blob/667766b0feefb199afc5de7c59fc330048641fcb/components/iot_bridge/src/bridge_wifi.c#L157-L178)。仓库中没有等待这个 bit 的消费者；它目前主要兼任初始化 guard/状态痕迹。
- 重连没有 reason 分类、次数上限、延迟、指数退避、jitter，也不检查 `esp_wifi_connect()` 返回值；唯一策略开关是 `CONFIG_BRIDGE_STATION_CANCEL_AUTO_CONNECT_WHEN_DISCONNECTED`，默认关闭取消，即默认自动重连：[Kconfig L25-L33](https://github.com/espressif/esp-iot-bridge/blob/667766b0feefb199afc5de7c59fc330048641fcb/components/iot_bridge/Kconfig#L25-L33)。持续认证失败时可能形成高频重试/日志抖动。
- Station 只接受 external/DHCPC 角色；SoftAP 只接受 data-forwarding/DHCPS 角色：[bridge_wifi.c L185-L201](https://github.com/espressif/esp-iot-bridge/blob/667766b0feefb199afc5de7c59fc330048641fcb/components/iot_bridge/src/bridge_wifi.c#L185-L201)、[L299-L324](https://github.com/espressif/esp-iot-bridge/blob/667766b0feefb199afc5de7c59fc330048641fcb/components/iot_bridge/src/bridge_wifi.c#L299-L324)。SoftAP start 时选择当前 external netif、同步 DNS 并启用 NAPT：[bridge_wifi.c L239-L268](https://github.com/espressif/esp-iot-bridge/blob/667766b0feefb199afc5de7c59fc330048641fcb/components/iot_bridge/src/bridge_wifi.c#L239-L268)。
- Station/SoftAP 创建都向默认事件循环注册 handler，但把 instance 输出参数传 `NULL`，模块也没有 unregister/deinit；重复 create 会重复注册且覆盖静态 netif 指针：[bridge_wifi.c L228-L232](https://github.com/espressif/esp-iot-bridge/blob/667766b0feefb199afc5de7c59fc330048641fcb/components/iot_bridge/src/bridge_wifi.c#L228-L232)、[L348-L355](https://github.com/espressif/esp-iot-bridge/blob/667766b0feefb199afc5de7c59fc330048641fcb/components/iot_bridge/src/bridge_wifi.c#L348-L355)。这套 API 适合“启动一次、活到重启”，不适合热重载。
- 配置路径会以 INFO 级别打印 SSID 和明文 password，并在 AP 配置变化时 deauth 所有客户端：[bridge_wifi.c L36-L70](https://github.com/espressif/esp-iot-bridge/blob/667766b0feefb199afc5de7c59fc330048641fcb/components/iot_bridge/src/bridge_wifi.c#L36-L70)。迁移到产品固件时应删除密码日志。

## 6. 示例自身的资源生命周期

- NVS 初始化对“无空页/版本变化”执行 erase 后重试，其他错误由调用者当前未检查的 `esp_storage_init()` 返回值承载：[app_main.c L37-L49](https://github.com/espressif/esp-iot-bridge/blob/667766b0feefb199afc5de7c59fc330048641fcb/examples/wifi_router/main/app_main.c#L37-L49)、[L107-L114](https://github.com/espressif/esp-iot-bridge/blob/667766b0feefb199afc5de7c59fc330048641fcb/examples/wifi_router/main/app_main.c#L107-L114)。
- 按钮长按启动 one-shot timer，抬起时 stop；到期擦 NVS 并重启：[app_main.c L51-L79](https://github.com/espressif/esp-iot-bridge/blob/667766b0feefb199afc5de7c59fc330048641fcb/examples/wifi_router/main/app_main.c#L51-L79)。timer 在 button 创建前分配；若 button 创建失败，timer 保留，且正常路径没有 delete，因为设计生命周期就是整机运行期：[L81-L105](https://github.com/espressif/esp-iot-bridge/blob/667766b0feefb199afc5de7c59fc330048641fcb/examples/wifi_router/main/app_main.c#L81-L105)。
- 正向资源管理较完整的局部是 NVS handle 和临时 `asprintf` key：各错误路径普遍 close/free；但 bridge registry、custom-check 节点、Wi-Fi EventGroup、事件 handler、netif、button/timer均无对称全局 teardown。该示例是 appliance-style 固件，不是动态服务容器。

## 7. 对 ESP-111 C5/S3 调度的可迁移结论

1. 可复用的是拓扑边界：C5/S3 上把 WAN 获取、LAN 转发、DNS 扇出、地址冲突迁移拆成事件驱动控制面；业务代码不必轮询 Wi-Fi 状态。
2. 不应直接复制其背压：`portMAX_DELAY` 的控制事件适合低频 DNS 变更，不适合 CSI/STATE 高频流。ESP-111 应继续使用优先级队列、STATE coalescing、有限等待和 drop/latency 观测。
3. 重连应补齐状态机：按 disconnect reason 区分可重试/配置错误，加入 bounded exponential backoff + jitter，并让新凭据或人工命令可中断等待。
4. 若 ESP-111 需要运行期重建 netif，必须把 handler instance、EventGroup、netif、registry 和 timer/button ownership 做成成对 init/deinit；本仓库 Wi-Fi Router 的“一次启动直到重启”前提不满足热切换。
5. “角色切换”和“地址迁移”要分开建模：此处 Station/WAN、SoftAP/LAN 角色固定；外网网段变化只触发 LAN IP/DHCPS/NAPT 更新。S3 网关不应把一次 LAN 地址迁移误判为链路角色重选。
