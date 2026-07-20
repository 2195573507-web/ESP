# 主项目与 ESPLCD LCD 迁移对比审计

日期：2026-07-20
范围：只读比较 `/Users/zhiqin/ESP 部分开发` 主项目与
`分支项目/ESP`（下称 ESPLCD 分支）的 ESP32-C5 LCD 实现。本文不修改固件，也不把
已有构建工件或源码分析表述为实机验收。

## 结论

应将 ESPLCD 分支的 `ESPC52/components/lcd` 作为 LCD 基线，迁移到主项目的
`ESPC51` 与 `ESPC52`，再由主项目既有 `screen_service` 接入命令链路。不要整体复制
ESPC51 的 LCD 仪表盘实现：它引入 CST816T 触摸、`ui/boot_screen`、BME/CSI/WiFi/语音
状态拼装和点击唤醒，超过“迁移 LCD”的必要范围。

WiFi CSI 必须留在分支。ESPLCD 的 C5 默认配置开启 CSI；主项目 C5 明确关闭 CSI，且
主项目已改用雷达职责。迁移 LCD 不需要，也不能重新打开 CSI Kconfig、回调、任务或
上传链路。语音保留主项目现有实现，不从分支覆盖；LCD 仅消费已发布的只读状态，且
不能占用或暂停语音资源。

## 项目边界

| 维度 | 主项目 ESPC51/ESPC52 | ESPLCD ESPC52 最小 LCD | ESPLCD ESPC51 仪表盘 |
| --- | --- | --- | --- |
| LCD 现状 | 没有真实面板驱动；`screen_service` 仅转发占位 bridge | `lcd.c` + `lcd_lvgl_service.c` | 基础 LCD 加完整仪表盘、触摸和动画 |
| 面板/总线 | 无 | ST7789、SPI2、四线 SPI | 同一面板基线 |
| UI 依赖 | 无 LVGL managed component | LVGL 9.2.2、esp_lvgl_port 2.6.2 | 同左，另依赖 `IIC`、`ui` |
| CSI | C5 Kconfig 关闭 | 不需要 CSI；静态文本也声明独立 | 仪表盘直接读取 `csi_service` |
| 语音 | 已有 Mic/VAD/S3 voice proxy/speaker 链 | 不改变语音 | 显示 voice 状态，猫咪点击请求本地唤醒 |
| 推荐 | 目标 | 迁移基线 | 仅作 UI 设计参考，不直接迁移 |

主项目现有显示命令入口不能丢失。`screen_service` 目前只调用 `ai_screen_bridge`，不驱动
真实 LCD（`ESPC51/components/Middlewares/display_placeholder/screen_service.c:1-27`）。
上游已经接受 `display.show_text` 和 `lcd.show_text`，并调用 `screen_service_show_text()`
（`.../system_command/system_server_client.c:708-727`）；`system_service_init()` 已初始化该
服务（`.../system_service.c:105-123`）。因此正确的迁移位置是替换或扩展 screen bridge
的实现，而不是再新增第二条命令入口。

## LCD 可复用单元

### 基础驱动

应复用的核心文件是 `分支项目/ESP/ESPC52/components/lcd/lcd.c` 与 `lcd.h`。其初始化顺序
是：`spi_bus_initialize()` -> `esp_lcd_new_panel_io_spi()` -> `esp_lcd_new_panel_st7789()`
-> reset/init -> 色彩、方向、偏移配置 -> DMA 行缓存 -> display on
（`lcd.c:347-533`）。对外基础 API 是初始化、清屏、矩形、字符/字符串、位图与颜色测试，
并将硬件配置收敛于 `lcd.h`。

| 参数 | ESPLCD 值 | 迁移含义 |
| --- | ---: | --- |
| 控制器 | `SPI2_HOST` | 要先确认主项目 SPI2 未被实际硬件使用 |
| SCLK / MOSI / CS / DC | GPIO24 / 23 / 25 / 26 | 源码扫描未见主项目占用；仍必须由原理图与实板确认 |
| MISO / RST / 背光 | `GPIO_NUM_NC` | 只能表示分支板上未由 MCU 控制，不能照此推断目标板接线 |
| 面板 | ST7789、RGB565、240 x 284 | 面板型号、可视区、X/Y gap、反色和字节序均需实板复核 |
| SPI | mode 0、20 MHz、队列深度 10 | 首次点亮可降至 10 MHz，稳定后再恢复 20 MHz |
| 基础 legacy 行缓存 | 240 x 20 x 2 = 9,600 B | `MALLOC_CAP_DMA`；只在 `lcd_init()` 至 LVGL display 注册间存在 |

上述 GPIO 来自 `分支项目/ESP/ESPC52/components/lcd/lcd.h:28-174`；DMA 缓存分配和释放
来自 `lcd.c:494-550`。SPI 和 I2S 是不同外设，不存在“端口互斥”，但其 DMA 缓冲都消耗
主项目宝贵的 internal/DMA heap。

### 最小 LVGL 层

ESPC52 的 `lcd_lvgl_service.c` 只有三条状态文本，不读取 CSI、BME、网络或语音状态。
它是适合第一阶段迁移的 UI 外壳：

- `lcd_service_start()` 先初始化面板，再创建 LVGL port，随后释放 9,600 B legacy 行缓存；
- LVGL 使用单缓冲 240 x 10 x 2 = 4,800 B，`buff_dma=true`、`buff_spiram=false`；
- LVGL task 优先级 1、栈 4,096 B，明确为 `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`；
- 无双缓冲、无全屏刷新、无软件旋转。

证据在 `分支项目/ESP/ESPC52/components/lcd/lcd_lvgl_service.c:10-124`。也就是说稳态
DMA 缓冲不是 legacy 9,600 B 与 LVGL 4,800 B 的相加；启动成功后目标是仅保留后者。
LVGL 对象、字体和 ESP-IDF/LVGL 运行期内存不应由这两个常量推算，必须通过迁移后的
heap 日志和 `idf.py size` 实测。

该组件自己的 manifest 固定了 `lvgl/lvgl 9.2.2` 与 `espressif/esp_lvgl_port 2.6.2`
（`.../ESPC52/components/lcd/idf_component.yml:1-9`），而主项目的
`ESPC51/dependencies.lock` / `ESPC52/dependencies.lock` 仅直接声明 `esp-sr`，没有 LVGL。
迁移时应让新增 LCD component 的 manifest 解析这两个依赖；不得手工复制分支
`managed_components` 目录或混入无关包版本。

## 明确不迁移的分支内容

### WiFi CSI

ESPLCD C5 的 `sdkconfig.defaults` 启用 `CONFIG_ESP_WIFI_CSI_ENABLED=y`
（`分支项目/ESP/ESPC51/sdkconfig.defaults:23`），并有 `sensor_domain/csi_phase_a`、
`csi_placeholder`、`csi_edge_detector` 与启动代码。主项目同一默认配置明确为
`# CONFIG_ESP_WIFI_CSI_ENABLED is not set`（`ESPC51/sdkconfig.defaults:34-35`），当前
`sdkconfig` 也关闭该选项。

禁止复制：CSI 目录、CSI CMake source、`csi_service_init/start()` 调用、`app_lcd_motion_state()`、
`lcd_dashboard_snapshot_t.csi_*` 字段，或任何 WiFi CSI callback 配置。LCD 第一版只显示
基础状态与命令文本；若以后显示雷达，必须通过主项目既有雷达已发布快照单独接入，而不是
复活 CSI 管线。

### 语音与触摸耦合 UI

ESPLCD C51 LCD CMake 额外编译 `touch/touch_cst816t.c`，并依赖 `IIC ui`
（`.../ESPC51/components/lcd/CMakeLists.txt:1-5`）。它的 1,300 余行 LVGL 服务包含 BME、
CSI、WiFi、voice/speaker 状态、多个 timer、猫咪帧动画和 `voice_chain_request_local_wake()`
耦合（`lcd_lvgl_service.c:17-124`、`app_orchestrator.c:87-137`）。这些都不是 LCD 驱动层。

第一版不迁移 CST816T，也不迁移 `ui/boot_screen`、猫咪资源、触摸坐标映射或触摸唤醒。
若以后需要触摸，CST816T 地址 `0x15` 使用现有 I2C0（`touch_cst816t.c:6-88`），而 BME 已在
GPIO2/3、400 kHz 的同一 I2C0（`ESPC51/components/BSP/IIC/iic.h:11-42`）。这需要独立的
地址、电平、上拉和 BME/触摸并发压力测试，不能被“LCD 能显示”替代。

语音链路也不从分支复制覆盖。主项目已有：Mic ADC GPIO6、12 KiB PSRAM-capable 任务栈
（`mic_adc_test.h:13-43`）；voice-chain 4 KiB internal 栈（`voice_chain.h:16-25`）；
server voice 响应任务 8 KiB PSRAM static（`server_voice_client.h:19-28`）；speaker writer
6 KiB PSRAM static（`speaker_player.h:66-83`）。LCD 只能读取已发布状态，并且绝不在
LVGL timer 中发 HTTP、读 BME I2C、读雷达/BLE，或等待语音锁。

## 资源分配与冲突审计

### GPIO、外设与总线

| 资源 | 主项目占用 | LCD 需求 | 结论与动作 |
| --- | --- | --- | --- |
| GPIO1 | speaker PA enable | 无 | 无源码冲突 |
| GPIO2/3 | I2C0 BME690 | 最小基线不用 | 触摸延期，避免首版共享 I2C |
| GPIO6 | Mic ADC | 无 | 无源码冲突 |
| GPIO7/8、I2S0 | PDM speaker | 无 | 无 GPIO/外设冲突 |
| GPIO23-26、SPI2 | 源码扫描未发现定义/初始化 | LCD MOSI/SCLK/CS/DC | 源码层暂未冲突，必须核对原理图、飞线和 boot/strap 约束 |
| internal/DMA heap | I2S DMA、WiFi/驱动与控制块 | LCD 4,800 B 稳态 draw buffer | 有容量竞争，需用 largest-free-block 门槛管理 |
| PSRAM | Mic/ring/scratch/任务栈 | LCD 不应使用作 DMA draw buffer | PSRAM 不能替代 LCD DMA buffer |

主项目 speaker 为 I2S0 PDM，GPIO7/8、PA GPIO1，DMA 最低配置为 8 descriptors x 512 mono
16-bit frames，约 8,192 B（`ESPC51/components/BSP/IIS/iis.h:26-111`）。LCD 的 4,800 B
与这部分都要求 DMA 可访问内存，故风险是 heap 余额与碎片，而非 GPIO 重复。

主项目已有三个能力分类：internal-DMA、internal-control、PSRAM
（`ESPC51/components/Middlewares/memory/c5_memory.c:10-76`）。LCD draw buffer 应按
internal-DMA 分配，LVGL task 保持 internal-control；大图、动画和整屏缓存都不能在
第一版加入。SDK 当前为 PSRAM 40 MHz，普通 `malloc` 16 KiB 阈值、internal reserve 32 KiB
（`ESPC51/sdkconfig:1700-1724`），因此 LCD 的小 DMA 缓冲更不应依赖“PSRAM 已开启”。

### 任务与调度

主项目 C5 资源管理器的 voice-exclusive ACK 仅为 HTTP、BME、worker
（`c5_resource_manager.h:44-48`），不应把 LCD 加入语音暂停名单。建议的 LCD 策略是：

1. LVGL task 维持分支的优先级 1、4 KiB internal stack；它低于 Mic/speaker/voice task。
2. 使用单缓冲、latest-only 状态快照，刷新频率从 1 Hz 开始；命令文本通过队列或 LVGL
   lock 串行执行，绝不从 command poll task 直接画屏。
3. LVGL timer 只拷贝已发布内存快照，禁止网络、I2C、BLE、雷达或阻塞等待。
4. LCD 初始化失败、DMA 缓存不足或 LVGL 创建失败只记录并降级为无显示；不能让 WiFi、
   system command、BME、雷达、scheduler 或语音启动失败。

分区不需要预先修改。两侧都是 16 MiB flash，factory app 为 5 MiB、model 为 2 MiB、storage
为 8.9375 MiB（`ESPC51/partitions.csv:1-5` 与分支同型）。LVGL 与字体会增加 app flash，
但分支旧 build 和主项目当前 build 的功能集合不同，不能据此计算可信增量。迁移后要用同一
源码基线的前后 `idf.py size` 比较，并以 factory 5 MiB 为硬上限。

## 可验证性边界

现有分支有 `build-lcd-clean-c51` / `build-lcd-clean-c52` 工件，能证明该分支曾生成 LCD
目标，但它们不等价于本机新构建，也不证明目标板接线、面板颜色、触摸、电源或并行语音
稳定性。验收必须分为：

- 构建证据：C51/C52 独立 `idf.py build`、组件依赖锁定、`idf.py size` 前后对比；
- 固件诊断证据：启动前后 internal/DMA/PSRAM free 与 largest block、LVGL task 高水位、
  LCD init/flush 错误计数；
- 实机显示证据：ST7789 ID/颜色顺序、反色、方向、240x284 可视区、20 MHz 信号完整性；
- 并发实机证据：BME、雷达/BLE、voice record/playback、WiFi 重连和命令文本刷新同时运行；
- 后续触摸证据：仅在触摸范围获批后，验证 I2C 共享、地址、电平与坐标映射。
