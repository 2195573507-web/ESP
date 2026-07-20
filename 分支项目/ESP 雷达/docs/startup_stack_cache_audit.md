# ESPS3 启动栈与 Cache-Freeze 审计

日期：2026-07-19  
范围：`ESPS3` 启动阶段；不修改 WiFi、NVS、partition 或 Flash 配置。

## 结论

启动断言的高风险点是 `gateway_startup_task` 使用了
`xTaskCreateWithCaps(..., MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`。启动任务随后进入
`gateway_wifi_start()`，其中 `nvs_flash_init()` 可能走 partition/`spi_flash_mmap` 的
cache-off 路径；此时任务栈必须位于内部 RAM。已改为
`MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`，并将 `APP_STARTUP_TASK_STACK` 的编译期下限
固定为 8192 字节。

## 启动任务栈

| 任务/逻辑阶段 | 创建位置 | 配置栈 | 栈分配 | HWM 输出 |
| --- | --- | ---: | --- | --- |
| `gateway_startup_task` | `ESPS3/main/main.c:64` | 8192 bytes | Internal RAM | `TASK_STACK_REPORT task=gateway_startup_task ... high_water_bytes=...` |
| `gateway_orchestrator`（逻辑阶段，不是独立任务） | `gateway_orchestrator_start()` | 复用上行启动任务 | Internal RAM | `gateway_orchestrator` 阶段日志使用当前启动任务 HWM |
| `network_worker` | `network_worker.c:2378` | 16384 bytes | PSRAM-capable | worker entry `TASK_STACK_REPORT` |
| `upload_worker` | `network_worker.c:2391` | 12288 bytes | PSRAM-capable | worker entry `TASK_STACK_REPORT` |
| `command_worker` | `network_worker.c:2404` | 16384 bytes | PSRAM-capable | worker entry `TASK_STACK_REPORT` |
| `snapshot_worker` | `network_worker.c:2417` | 12288 bytes | PSRAM-capable | worker entry `TASK_STACK_REPORT` |
| `radar_rx` (`radar_ld2450`) | `radar_service.c:430` | 4096 bytes | PSRAM-capable | 当前实现未改变 |
| `radar_local` | `radar_local_adapter.c:279` | 8192 bytes | PSRAM-capable | entry/periodic `TASK_STACK_REPORT` |
| `radar_diag` (`radar_diagnostics`) | `radar_diagnostics.c:457` | 4096 bytes | PSRAM-capable | 当前实现未改变 |
| `s3_scheduler` | `s3_scheduler.c:1463` | 12288 bytes | PSRAM-capable | entry `TASK_STACK_REPORT` |
| `protocol_worker` | `s3_scheduler.c:1434` | 10240 bytes | PSRAM-capable | entry `TASK_STACK_REPORT` |
| `stream_worker` | `s3_scheduler.c:1447` | 8192 bytes | PSRAM-capable | entry `TASK_STACK_REPORT` |

`high_water_bytes` 是 FreeRTOS `uxTaskGetStackHighWaterMark()` 返回值换算后的字节数，
不是静态猜测值。每个任务在 entry 处报告一次，长期任务按现有周期监控继续报告。

## 局部变量检查

- `gateway_startup_task`：只有参数和栈监控调用，无数组、`malloc` 或格式化缓存。
- `gateway_orchestrator_start`：只有标量 `esp_err_t` 返回值；无大数组、`snprintf` 或动态
  缓冲区。
- `gateway_wifi_start`：使用 ESP-IDF 的 `wifi_init_config_t`/`wifi_config_t` 栈对象，未发现
  1 KB 级自定义 `char[]` 或 `uint8_t[]`；现有 WiFi 逻辑未改动。
- `STARTUP_MEMORY_CHECK` 只调用 `heap_caps_get_free_size()` 和
  `heap_caps_get_largest_free_block()`（按 Internal/PSRAM capability 查询），不创建局部
  buffer、不调用 `snprintf`、不调用 `malloc`。

## 启动链验证标准

静态链路保持：

`gateway_startup_task` -> `gateway_orchestrator_start` -> `gateway_wifi_start` ->
`network_worker` -> `radar_ld2450`/`radar_local` -> `radar_diagnostics`。

构建必须覆盖 `ESPS3`，并检查生成日志中依次出现：

1. `TASK_STACK_REPORT task=gateway_startup_task stack_bytes=8192 ...`
2. `STARTUP_MEMORY_CHECK module=wifi.after ...`
3. `STARTUP_MEMORY_CHECK module=network_worker.after ...`
4. `TASK_STACK_REPORT task=network_worker ...`
5. `TASK_STACK_REPORT task=radar_local ...`
6. `TASK_STACK_REPORT task=radar_diag ...`

本次环境可执行静态检查和 ESP-IDF 构建；没有连接设备，因此无法把构建成功等同于
硬件已经通过 `esp_cache_freeze_caches_disable_interrupts` 或已经完整跑到上述运行时日志。
设备验收仍需串口启动日志确认 `gateway_wifi_start` 返回后继续进入各 worker。

## 变更文件

- `ESPS3/main/main.c`：启动任务栈改为 Internal RAM capability。
- `ESPS3/components/app_config/app_main_config.h`：启动栈小于 8192 时编译失败。
