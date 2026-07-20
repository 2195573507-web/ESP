# 研究发现

## 资料范围

- 官方资料根目录：`/Users/zhiqin/Documents/ESP 资料`。
- LD2450 资料包括协议 V1.03、模组说明书、教程、透传说明、Windows `HLK-2450_TOOL`、Android `hlkradartool-android-`、iOS 蓝牙 Demo、STM32/C 解析工程和 PDF 解析参考。
- `HLK-2450_TOOL` 目录含 EXE、`ICLM_DataProcess.dll`、`Radar_Config.ini` 等，能做可见能力盘点，但无可读算法源码。

## 项目范围

- ESPS3：UART/LD2450 固定帧解析、恢复、目标过滤、坐标变换、zone map、tracker、spatial state、registry、local adapter、HTTP/调试输出。
- ESPC51/C52：BLE 接收、固定设备绑定、LD2450 解析/过滤、结果编码、latest-only/资源适配与上传。
- macOS：日志解析、回放、雷达画布与仪表盘，用于工程调试，不是雷达固件检测算法。

## 待补证据

- 官方 Android `DataAnalysisHelper.kt`、`ReceiveInfo.kt`、`CreateControlData.kt`、BLE 连接流程。
- 官方 STM32 `User/main.c` 和协议 PDF 的帧/字段说明。
- ESP 项目 tracker/spatial/zone/codec/adapter 的准确行号。

## 官方源码已确认

- STM32 `Hardware/Serial.c`：USART1 `256000, 8N1`；中断状态机寻找 `AA FF 03 00`，收 24 字节载荷，再检查 `55 CC`；三个槽各 8 字节解析 X/Y/Speed/DistanceResolution；未显示跨帧 ID、平滑、预测、丢失保持或人体语义。
- STM32 `User/main.c`：对每个槽直接用 `sqrt(x*x+y*y)` 计算距离，用 `-atan2(x,y)` 计算角度；打印速度、分辨率、距离、角度；示例级别，没有目标关联和人数状态机。
- Android `DataAnalysisHelper.kt`：按 MAC 建立 `DeviceState`，串行化数据分析并用 EventBus 分发；`DeviceState.kt` 维护最多三个 LD2450 槽、单/多目标模式、三矩形区域过滤配置、MAC/版本/蓝牙配置和应答状态。
- Android `BaseVolume.kt`：明确存在 LD2450 单目标 `0x0080`、多目标 `0x0090`、查询追踪模式 `0x0091`、蓝牙开关、读取 MAC、区域过滤查询/设置命令；同时混合包含 LD2410/LD2412/LD2451/LD6002 命令，不能把全工程能力归给 LD2450。
- Android BLE 流程：服务发现后匹配预设 Service/Read/Write UUID，订阅通知，写特征使用 no-response；连接失败会有限次重连。源码证明了工具的传输/配置流程，不证明 Windows 工具内部算法。
- Android 的可见坐标实现：按槽数据计算 x/y、距离、角度和显示坐标；区域过滤是三矩形的 include/exclude 配置；未见跨帧 track ID 关联或轨迹生命周期算法。

## ESP 项目已确认

- ESPS3 parser：`ld2450_parser.c` 完整固定 30 字节、逐字节重同步、半帧保留、坏头/坏尾/跳过字节/有效帧/帧率/超时等诊断；解析三个槽并计算整数距离。
- ESPS3 service：UART RX task 独占读取与解析，latest-frame 深拷贝；与恢复状态机、配置互斥和诊断连接。
- ESPS3 spatial：先保留 raw targets，再做全零/零分辨率/sentinel/距离/房间边界准入；保存 `raw_target_count` 和 `accepted_target_count` 两层数量。
- ESPS3 tracker：最多三个 track；预测位置 + 全局 assignment 搜索；距离 gate、速度异常、EMA、置信度、tentative/confirmed/HOLD 生命周期；confirmed track 产生单调 `track_id`。
- ESPS3 spatial semantics：区分 `sensor_state`、`occupancy_state` 和 `motion_state`，支持 PRESENT/HOLD/VACANT_INFERRED/UNKNOWN 与运动进入/退出滞回。
- ESPS3 zone map：房间矩形、最多 8 个业务区域、ignore/active/entry、历史区域滞回、zone changed/left/dwell。
- ESPC51/C52：BLE 只做固定设备绑定、通知接收、LD2450 原始帧解析/基础过滤、结果编码/上传；S3 负责空间语义和跨帧轨迹。远端 payload 有 schema、local id、sequence、uptime、link/sample validity 和目标数组校验。
- macOS debugger：串口/日志解析、回放、轨迹画布和状态展示，属于调试/可视化层；不等于雷达固件算法。

## 关键对比判断

- 官方硬件/协议输出最多三个“当前帧目标槽”，没有协议级持久人体 ID；官方示例和 Android 可读实现主要是槽位显示/配置。
- ESP 项目在官方输出之上增加了可测试的跨帧 track ID、滤波、速度/跳变拒绝、空间区域、占用/运动状态、链路健康和多源身份/序列保护。
- 官方区域过滤是模块配置能力，ESP zone map 是主控业务语义；两者不能直接视为同一层。
- 人数应明确区分 `raw_target_count`、`accepted_target_count`、`visible/confirmed active_track_count`；官方工具显示的数量层级不能仅凭 UI 反推。
- 官方工具显示“稳定”不能证明使用 EMA/Kalman/最近邻；闭源 Windows DLL/EXE 不可作算法证据。
