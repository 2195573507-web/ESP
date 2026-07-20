# 主项目 ESPLCD LCD 迁移实施计划

日期：2026-07-20
前置文档：`docs/主项目_ESPLCD_LCD迁移对比审计_2026-07-20.md`
实施目标：把 ST7789 SPI LCD 的最小、可降级实现加入主项目 ESPC51/ESPC52，同时保留
现有命令入口、关闭 WiFi CSI、保持现有语音链路与资源仲裁边界。

## 决策与范围

### 采用方案

采用“最小 C52 LCD component + 主项目 display adapter + 只读状态提供者”的三层结构：

```text
system command (display.show_text / lcd.show_text)
                    |
             screen_service
                    |
        LCD adapter / bounded latest state
                    |
       lcd service -> LVGL task -> ST7789 SPI2
```

基础驱动来自 `分支项目/ESP/ESPC52/components/lcd`，而不是 C51 仪表盘。ESPC51 与 ESPC52
均使用相同的 C5 硬件能力，因此先以 C51 实现并构建，再向 C52 保持同构迁移；两板只允许
保留既有设备身份/绑定差异。

### 本计划不包含

- 不迁移或恢复 WiFi CSI、CSI HTTP、CSI 算法、CSI Kconfig 或 CSI dashboard 字段。
- 不修改 ESPS3、ESP-server、雷达算法、BME 传输协议、语音协议或音频格式。
- 第一版不迁移 CST816T 触摸、`ui/boot_screen`、猫咪动画、触摸唤醒、BME/CSI 直接轮询。
- 不更改 `partitions.csv`；无构建体积证据前不擅自压缩 model/storage。

## 阶段 0：冻结基线与接线确认

1. 记录主项目 C51/C52 当前 `idf.py size`、internal/DMA/PSRAM 启动日志、task high-water
   和 Git 状态；只将新增 LCD 文件纳入本任务变更。
2. 由原理图和实物确认 ST7789 的 SCLK=24、MOSI=23、CS=25、DC=26，RST/BL 的真实连线、
   供电、地、面板分辨率和控制器。禁止仅依据分支 `GPIO_NUM_NC` 假设目标板 RST/BL 未连接。
3. 静态复核 GPIO23-26 没有与 boot strap、外接 flash/PSRAM 或当前板级复用冲突；确认 SPI2
   没有在 board bring-up 中被未检索到的二进制/外部组件占用。
4. 保留主项目 `CONFIG_ESP_WIFI_CSI_ENABLED` 为关闭；校验新增组件和 CMake 改动没有把
   `sensor_domain/csi_*` 或 CSI managed dependency 引入目标。

退出条件：硬件接线已确认，基线文件已保存，CSI 仍关闭。未满足时只停在文档和接线核验，
不进入驱动迁移。

## 阶段 1：迁移最小 LCD 组件与受控依赖

1. 为 `ESPC51/components/lcd` 和 `ESPC52/components/lcd` 迁入 C52 基线的 `lcd.c`、`lcd.h`、
   `lcd_lvgl_service.c`、`lcd_service.h`、`CMakeLists.txt`、`idf_component.yml`。
2. 保留组件 manifest 的 IDF >= 5.5、LVGL 9.2.2、esp_lvgl_port 2.6.2 声明，让 component
   manager 更新 lock 与 managed components；不复制分支的整个 `managed_components`。
3. 固定第一版资源：legacy DMA 行缓存 9,600 B，初始化成功后释放；LVGL 单 draw buffer
   4,800 B，`buff_dma=true`、`buff_spiram=false`、无 double-buffer/full-refresh；LVGL task
   为 4,096 B internal、优先级 1。
4. 把 LCD 配置集中在板级头文件或 Kconfig 默认值中，使 C51/C52 共享面板参数；不要在
   多个业务模块散落 GPIO、旋转、颜色或频率宏。
5. 在 LCD 初始化、legacy buffer 释放、LVGL display 注册、失败回滚处打印统一的
   `LCD_MEM`（internal/DMA/PSRAM free 与 largest block）和错误码。所有失败路径必须清理
   已创建 handle，并返回错误供调用方降级。

退出条件：两个板均能配置与编译；生成依赖锁可复现；`sdkconfig` 不出现 CSI 启用。

## 阶段 2：接入主项目启动与命令入口

1. 保持 `system_service_init()` 对 `screen_service_init()` 的既有调用。将 placeholder bridge
   改为可延迟绑定的 LCD adapter，而非并行注册一个 command consumer。
2. 在 `app_orchestrator` 中选择明确的 best-effort 启动点：WiFi/gateway/system、BME、雷达、
   scheduler 已完成初始启动之后，voice chain 启动之前调用 `lcd_service_start()`。此调用返回
   非 `ESP_OK` 时只记录 `LCD_START_FAILED` 并继续启动 voice；不得使用 `ESP_ERROR_CHECK`。
3. 增加 `screen_service_attach_lcd()` 或等价的内部绑定：LCD 未启动时 `show_text/clear` 返回
   可观察的降级状态；LCD 启动后，把命令文本写入有界 latest-only 消息槽，供 LVGL task
   在 lock 内渲染。不得在 command poll task 直接调用 LVGL 或 SPI API。
4. 实现命令 TTL：同一消息槽保存文本、标题、到期时间；LVGL timer 到期恢复默认页。新命令
   覆盖旧命令，避免累计队列和字体/对象泄漏。
5. 初始默认页只显示静态“ready”、连接/雷达状态占位或无数据，不读取 CSI。之后若需要
   BME、雷达、网络、语音状态，定义一个只读 snapshot provider：调用方复制已发布状态，
   不做 I2C、HTTP、BLE、雷达解析、锁等待或唤醒语音。

退出条件：现有 `display.show_text` 与 `lcd.show_text` 仍经同一 ACK 语义工作；LCD 失败时
命令链与网络/语音链保持可用。

## 阶段 3：资源保护与观测

1. 用主项目 `c5_memory` 的能力分类分配 LCD 内存：draw buffer 是
   `MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT`；LVGL task 是 internal control；
   不把 draw buffer 放入 PSRAM。
2. LVGL 维持低优先级、单缓冲和 1 Hz 状态刷新。voice-exclusive 期间 LCD 不抢占 I2S/ADC，
   但不纳入 `c5_resource_manager` 的 HTTP/BME/worker 暂停 ACK；它只显示最后已发布状态。
3. 在启动阶段记录 `before_lcd`、`after_lcd_panel`、`after_lvgl_display`、`after_voice` 的
   internal free/largest、DMA free/largest、PSRAM free/largest。显示它们和主项目已有
   Mic/speaker 资源日志的时间序列，排查内部堆碎片而不是仅看总 free。
4. 为 LVGL task、Mic、speaker writer、voice response task 输出 high-water；阈值由迁移后
   实测确定，不在没有数据时削减已有语音栈。
5. 比较同一板、同一源码基线的迁移前后 `idf.py size`，特别是 factory app 的 5 MiB 空间、
   DRAM/IRAM、flash rodata/text。若超预算，先删减 UI/字体/图片，不能迁移为整屏 PSRAM
   framebuffer 来规避问题。

退出条件：启动及 voice record/playback 前后 DMA largest block 不低于项目确认的阈值，且无
LCD、I2S、ADC 或 LVGL allocation failure；build size 有可追溯前后报告。

## 阶段 4：分层验证

| 层级 | 检查 | 通过标准 | 不代表 |
| --- | --- | --- | --- |
| 静态 | C51/C52 组件/CMake/Kconfig diff | 无 CSI 目录、CSI Kconfig 或 UI 耦合被带入 | 能在硬件上显示 |
| 构建 | 两板独立 `idf.py build` | 依赖锁定、无未定义 LCD/LVGL 符号，size 报告生成 | 面板接线正确 |
| 固件日志 | LCD init、heap/DMA、task high-water | legacy buffer 被释放，单缓冲和失败降级生效 | 长时稳定 |
| 实机点亮 | 颜色测试、方向、可视区、SPI 频率 | 红绿蓝黑白、反色/byte-swap、X/Y gap 正确 | 并发稳定 |
| 并发实机 | WiFi、BME、雷达/BLE、voice record/playback、命令文本 | 不丢语音、不阻塞 heartbeat、无 DMA/heap 失败 | 触摸可用 |
| 可选触摸 | CST816T 与 BME 同总线 | 地址/上拉/坐标/并发读通过 | CSI 需要恢复 |

构建使用 ESP-IDF 5.5.4 的隔离目录，避免两个 Ninja 目录竞争。当前工作区已有分支的
clean-build 工件，但它们只能作源码历史佐证；正式结论以迁移后在本机重建的 C51/C52
结果和目标板实机日志为准。

## 实施顺序与回滚

推荐提交顺序是：

1. 仅新增最小 LCD component 与 managed dependency lock；
2. C51/C52 构建、size 与静态无-CSI检查；
3. screen_service adapter 与 best-effort startup；
4. 命令文本/TTL 与资源日志；
5. 实机点亮与并发压力；
6. 单独评审触摸或更丰富 dashboard。

每一步都可按组件或 adapter 回退，且不会要求回退现有 WiFi、CSI 已移除状态、语音、雷达
或 server 协议。若 LCD 初始化失败，运行时回退是“无显示但主业务继续”；若 size 或
internal/DMA 预算失败，代码回退是移除 LCD component 与 adapter，不触碰既有语音资源。
