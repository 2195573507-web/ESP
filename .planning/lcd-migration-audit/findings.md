# LCD 迁移审计发现

## 已确认范围

- 主项目根目录为 `/Users/zhiqin/ESP 部分开发`，其 `.git` 已有大量用户改动。
- `分支项目/ESP` 含 `ESPC51/components/lcd`、`ESPC52/components/lcd` 及对应
  `build-lcd-clean-*` 构建目录，是当前最符合“ESPLCD”描述的候选分支。
- LCD 是针对 ESP32-C5 的 ST7789 四线 SPI 实现，固定使用 SPI2，候选引脚为
  SCLK=GPIO24、MOSI=GPIO23、DC=GPIO26、CS=GPIO25；RST 与背光在分支定义中均为
  `GPIO_NUM_NC`，不能推断目标板具有相同电气连接。
- LCD 不使用整屏帧缓存：240 x 10 x RGB565 的单块可复用 DMA 行缓存为 4,800 B；
  SPI 事务队列深度为 10。LVGL 服务是独立的增强层，不能与基础点亮路径混为一项。
- 分支的 C5 固件仍包含 WiFi CSI 与语音组件；实际 SDK 配置中
  `CONFIG_ESP32_WIFI_CSI_ENABLED` 为未启用状态，说明源码存在不代表 CSI 运行时占用。

## 关键更正与完整结论

- 分支 C5 `sdkconfig.defaults` 实际启用 `CONFIG_ESP_WIFI_CSI_ENABLED=y`；主项目对应
  配置关闭它。此前“分支实际配置未启用”的初步表述不完整，已在交付文档中更正。
- C52 LCD 是应迁移的最小基线：legacy DMA 缓冲为 9,600 B，启动后释放，稳态 LVGL 单 DMA
  buffer 为 4,800 B，LVGL task 为 4,096 B internal。
- C51 dashboard 额外依赖 IIC 与 UI，含触摸、CSI/BME/WiFi/voice 状态和点击唤醒；不属于
  LCD 驱动迁移范围。
- 主项目已有 `screen_service` 和 `display.show_text`/`lcd.show_text` 命令链，迁移必须用
  adapter 接入该路径，不能再创建平行 command entry。
- 根目录原有 `task_plan.md` 是已完成的雷达任务，不能复用或覆盖。

## 证据原则

- 本文档只记录源码、配置和已存构建工件可证明的事实。
- GPIO 实际接线、面板型号、背光极性、并发显示与 CSI/语音稳定性均需目标板实测。
