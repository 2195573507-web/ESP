# C5 N16R8 LCD/LVGL Theme Fix Audit

日期：2026-07-20

## 1. 当前崩溃根因

ESPC51 与 ESPC52 均使用 LVGL 9.2.2 和 esp_lvgl_port 2.6.2。LVGL9 的
`lv_display_create()` 会在返回前自动调用 `lv_theme_default_init()`；应用层
没有第二次 theme 初始化，也没有 LVGL8 API 混用。

两端将 LVGL builtin bootstrap pool 配置为 2 KiB，但既有的 96 KiB UI
PSRAM arena（其中 92 KiB 作为 LVGL pool）此前在 `lcd_ui_start()` 才
`lv_mem_add_pool()`。启动顺序却先进入 `lvgl_port_add_disp()`，所以 display、
layer、timer 和 default theme 都只能使用 2 KiB bootstrap pool。
`lv_theme_default_init()` 在 theme 分配失败后会解引用 NULL，因而表现为
`Load access fault`，调用链落在 theme init。

## 2. ESPLCD 对照结果

ESPLCD 的 display 参数模型与当前工程相同：ST7789/SPI、240x284、RGB565、
10-line single DMA buffer、partial render 与 esp_lvgl_port 自动创建 display/theme。
其可工作的重要差异是 builtin LVGL pool 更大（C51 为 14 KiB，C52 为 64 KiB），
并非手动 theme API 或不同 display 生命周期。

本次未复制 ESPLCD 的内存配置。迁移的是正确的时序原则：在 LVGL 已初始化、
但第一块 display 尚未创建时，先使现有 PSRAM LVGL pool 可用。

## 3. 修改文件列表

两端同步修改以下文件：

| 文件 | 原因 |
| --- | --- |
| `ESPC51/components/lcd/lcd_driver.h`、`ESPC52/components/lcd/lcd_driver.h` | 为 port-init 后的 pool prepare/release 增加无反向组件依赖的回调接口。 |
| `ESPC51/components/lcd/lcd_driver.c`、`ESPC52/components/lcd/lcd_driver.c` | 在 LVGL port lock 内执行 prepare，再创建 display；记录 theme/display 阶段，并保证失败及停止时在 `lv_deinit()` 后再释放外部 arena。 |
| `ESPC51/components/lcd_ui/lcd_ui.h`、`ESPC52/components/lcd_ui/lcd_ui.h` | 声明 UI pool prepare/release 生命周期接口。 |
| `ESPC51/components/lcd_ui/lcd_ui.c`、`ESPC52/components/lcd_ui/lcd_ui.c` | 将既有 arena 分配与 `lv_mem_add_pool()` 拆到 prepare；display 创建成功后才绑定 UI 并创建控件。保留 `LCD_FAULT_UI_ARENA` 在实际 arena 分配之前。 |
| `ESPC51/components/lcd_ui/lcd_service.c`、`ESPC52/components/lcd_ui/lcd_service.c` | 服务层把 UI pool 回调交给 driver 编排，并继续在持有 LVGL lock 时创建 UI 对象。 |

新增的启动诊断为：

```
LVGL_INIT_STAGE before_display_create
LVGL_INIT_STAGE before_theme_init
LVGL_INIT_STAGE after_theme_init
LVGL_INIT_STAGE after_display_create
```

每条日志和失败日志均输出 display 指针、theme 指针、LVGL 版本和 draw-buffer
指针。主题是 `lv_display_create()` 内部步骤，因此前两条日志在调用前、后两条
在其返回后记录。

## 4. 内存影响分析

本修复不增加 LVGL bootstrap pool，不修改 `sdkconfig`，不修改 DMA buffer、
Span、internal RAM admission、radar、voice、WiFi 或 BME。

LCD steady-state draw buffer 仍为单 10-line RGB565 internal-DMA buffer（4800 B）。
UI arena 仍为既有的 96 KiB PSRAM allocation，LVGL 可用子池仍为 92 KiB；变化
仅是其注册由 display/theme 创建后提前到创建前。这样 default theme 与首批
LVGL display objects 从既有 PSRAM pool 获得空间，而不是依赖 2 KiB bootstrap
pool。

工作区中先前已有的 DMA、admission 与 task-stack 脏改动未由本次修复调整或回退。

## 5. 静态验证

- C51/C52 的 `lcd_driver.c/.h`、`lcd_ui.c/.h` 和 `lcd_service.c` 逐字一致。
- 全局搜索确认只有这一个 `lvgl_port_add_disp()` 路径；没有手动
  `lv_theme_default_init()`、`lv_theme_set_default()` 或 `lv_display_set_theme()`。
- `git diff --check -- ESPC51/components/lcd ESPC51/components/lcd_ui ESPC52/components/lcd ESPC52/components/lcd_ui` 无输出。
- 本任务按限制未执行编译、烧录、启动设备或修改硬件配置。

## 6. 后续烧录验证步骤

1. 使用与目标板一致的 N16R8 固件构建并烧录 ESPC51，首次启动确认四个
   `LVGL_INIT_STAGE` 日志按顺序出现，`after_theme_init` 的 theme 指针非 NULL。
2. 确认不再出现 `lv_theme_default_init` 的 Load access fault，并核对
   draw-buffer 指针仍为 internal-DMA capable、大小仍为 4800 B。
3. 观察 LCD 首屏、刷新、触摸、启动失败重试与停止/再启动；确认没有
   use-after-free、LVGL lock timeout 或 PSRAM pool release 警告。
4. 对 ESPC52 重复相同验证。两端都通过后，再进行与 voice、radar、BME、WiFi
   并行运行的设备级回归。
