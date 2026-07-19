# C5 BLE LD2450 实施报告

## 修改范围

- 新增 `ESPC51/ESPC52/components/Middlewares/radar_ble/`：固定流、NimBLE Central 传输、运行时任务和空绑定配置。
- 修改 C51/C52 `components/Middlewares/CMakeLists.txt`、`app_orchestrator.c`、`radar_service.[ch]`、`sdkconfig.defaults`/`sdkconfig`。
- 将 C51/C52 parser、parser header、types 与当前 ESPS3 只读实现同步；仅把超时调用适配为 C5 测试接口。
- 新增 `tools/check_c5_radar_parity.sh`、`tools/test_c5_radar_ble_stream.[ch|sh]`。

## 验证结果

- C51 parser/presence golden-vector host test：PASS。
- C52 parser/presence golden-vector host test：PASS。
- BLE stream fixed-buffer host test：PASS。
- C51/C52 parity：PASS；公共 BLE/parser/presence 文件字节级一致，只有 `radar_ble_binding_config.h` 作为身份配置白名单差异。
- C51 build：PASS，隔离目录 `build-radar-c51`。
- C52 build：PASS，隔离目录 `build-radar-c52`。
- `RADAR_BLE_BINDING_ENABLED=1` 的 C51/C52 `radar_ble_transport.c` 活跃分支：目标编译检查 PASS；默认绑定仍为 0。
- 原有 `build/` 因缓存指向 `/Users/zhiqin/ESP-111` 未使用，未执行清理；正确工作区使用独立 `build-radar-*`。

## 未完成与硬件待验证

当前默认 MAC、地址类型、服务 UUID、Notify UUID 和 Write UUID 为空，因此运行时静态状态为 `unavailable/retry`，不会自动连接附近同名雷达。尚未进行真机 BLE 连接、Notify 实测、三房间检测、BLE/Wi-Fi 共存或长期稳定性验收；不得据此宣称在线或稳定。

S3 未修改。C5 -> S3 代码复用了既有 `/local/v1/radar/state` 客户端/字段定义；本轮未进行实机 BLE 或 C5 -> S3 全链路验证。

## 工作区边界复核

- 错误工作区 `/Users/zhiqin/ESP-111` 中本轮误加的 C5 `radar_ble` 源码目录、空目录和 `build/` 下雷达对象已清除；其余已有未提交改动保留。
- 正确工作区 `/Users/zhiqin/ESP 雷达` 之外没有新增本任务的源码。
- ESPS3 仍按只读约束处理。任务开始时记录的哈希与结束哈希仅差一个任务开始后变更的未跟踪文件 `ESPS3/components/radar_ld2450/include/radar_config.h`；本轮未修改该文件，不能将结果表述为“哈希无差异”。

## 复核后的边界

- C51/C52 的解析器和 presence 模块沿用当前 S3 LD2450 协议与参数，但 C5 当前实现没有引入 S3 的完整 spatial tracker/zone 模块，不能声称三端完整空间算法已经等价。
- BLE 默认绑定仍为空；配置启用分支已加入固定流并发保护、通知超时驱动 parser timeout、上限 30 秒指数退避以及写特征 API。启用分支的 NimBLE 编译和真机行为仍需硬件配置后验证。
