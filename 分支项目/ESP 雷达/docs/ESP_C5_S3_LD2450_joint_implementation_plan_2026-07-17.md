# ESPC51 / ESPC52 / ESPS3 LD2450 雷达链路联合实施计划

更新日期：2026-07-17  
项目根目录：`/Users/zhiqin/ESP 雷达`  
适用工程：`ESPC51`、`ESPC52`、`ESPS3`  
执行对象：Codex  
实施性质：C5 与 S3 同步修改，一次冻结 C5 -> S3 雷达数据合同；不修改 ESP-server、Dashboard 和其他既有业务链路。

---

## 0. 文档定位

本文用于实施以下分层架构：

```text
LD2450
  ↓ BLE notify byte stream
ESPC51 / ESPC52
  ↓ 固定帧解析与基础校验
轻量目标观测数据
  ↓ POST /local/v1/radar/result
ESPS3 radar_gateway_ingest
  ↓ radar_remote_adapter
统一 radar observation
  ↓ source-specific radar_domain
坐标转换
  ↓
空间区域判断
  ↓
目标关联与跟踪
  ↓
presence / motion / occupancy
  ↓
现有 S3 snapshot / registry / downstream path
```

本计划同时完成两项工作：

1. **C51/C52 雷达边缘接入**：BLE 定向连接、LD2450 固定帧解析、轻量目标结果上传。
2. **S3 统一空间计算**：新增远端雷达 ingest/adapter，并让 `S3_LOCAL`、`C51`、`C52` 使用彼此隔离、语义一致的 radar pipeline。

Codex 必须先读取 live source，再按阶段执行。历史文档、旧路径、旧行号和旧宏只能用于导航，不得代替当前源码。

---

## 1. 最终目标

本轮完成后必须形成以下明确职责边界。

### 1.1 C5 职责

ESPC51、ESPC52 负责：

- 根据配置的雷达 MAC 定向扫描和连接；
- 严格校验 service UUID 和 notify UUID；
- 订阅 notify；
- 将 BLE notification 当作连续字节流处理；
- 解析 LD2450 固定 30 字节实时上报帧；
- 识别帧头、帧尾、半包、粘包和错位；
- 丢弃全零目标槽；
- 提取 `x_mm`、`y_mm`、`speed_cm_s`、`resolution_mm`；
- 安全计算 `distance_mm`；
- 增加本地 frame sequence、request sequence 和 monotonic uptime；
- 记录 BLE 断开、parser 错误和丢帧诊断；
- 将最多 3 个目标压缩为受限的轻量 JSON；
- 通过独立接口上传 S3；
- BLE 断开时上报 `radar_offline`，但不影响 C5 整机在线状态。

### 1.2 S3 职责

ESPS3 负责：

- 提供 `POST /local/v1/radar/result`；
- 校验协议版本、身份、字段类型、数组长度、数值范围、sequence 和 body 上限；
- 将 `local_id=1/2` 映射到固定设备身份；
- 将远端 payload 转换为统一的 `radar_observation_frame_t` 语义；
- 分别维护 `S3_LOCAL`、`C51`、`C52` 的独立上下文；
- 调用已有：
  - `radar_coordinate_transform`
  - `radar_zone_map`
  - `radar_target_tracker`
  - `radar_spatial_state`
- 生成 track ID；
- 完成房间坐标、区域、目标跟踪、presence、motion 和 occupancy；
- 单独维护 radar online/freshness，不影响 device online；
- 继续使用现有 S3 snapshot、registry 或 downstream adapter；
- 不要求 ESP-server 或 Dashboard 修改。

### 1.3 明确不做

本轮不实现：

- C5 侧 zone、房间区域和空间模型；
- C5 侧 track ID；
- C5 侧人数统计；
- C5 侧 occupancy、presence、motion 最终判断；
- C5 侧目标跨帧身份关联；
- C5 侧默认 EMA；
- C5 侧人体类别或置信度模型；
- 多房间目标融合；
- 跨来源人物 ID；
- raw BLE 字节流上传；
- raw 30 字节帧持续上传；
- ESP-server 新接口；
- Dashboard 新页面或字段；
- 数据库变更；
- Kalman、深度学习或大型矩阵库。

---

## 2. 硬性约束

### 2.1 禁止修改

禁止修改：

```text
ESP-server/
ESP-server/public/
ESP-server/db/
Dashboard 前端
managed_components/
第三方组件源码
```

禁止改变以下链路的业务行为、接口和启动顺序：

```text
BME690
voice
wake
mic
speaker
command
现有 Server API
数据库
LCD/LVGL
```

允许在 C5 启动编排中增加独立 radar 模块的最小 init/start hook，但不得重构或改写上述链路。

### 2.2 S3 算法保护

不得推倒重写：

```text
radar_coordinate_transform
radar_zone_map
radar_target_tracker
radar_spatial_state
```

规则：

- 优先保持已有 public API；
- 输入不匹配时新增 adapter；
- 需要增强算法时只做增量修改；
- 必须保留已有测试或补齐回归测试；
- 不得用新框架替换完整 radar_domain；
- 不得把远端 radar 逻辑写进 UART parser；
- 不得为 C51/C52 复制三套算法。

### 2.3 C51/C52 一致性

C51/C52 雷达功能代码必须保持一致。

允许差异仅包括：

```text
local_id
device_id
alias
radar_mac
可选 radar address type
```

强制映射：

```text
local_id=1 -> sensair_shuttle_01
local_id=2 -> sensair_shuttle_02
```

C51/C52 JSON schema、字段、单位、状态枚举、错误码、上传策略和日志格式必须完全一致。

### 2.4 独立故障域

任一雷达断开不得影响：

```text
C5 Wi-Fi
register
heartbeat
status
BME690
voice
command
S3 SoftAP
S3 STA
其他雷达来源
```

状态必须满足：

```text
C5 device online + BLE radar disconnected
= device_online
+ radar_offline
+ radar_state UNKNOWN
```

禁止：

```text
radar_offline -> device_offline
radar_stale -> vacant
一个 source offline -> 清空其他 source
```

---

## 3. 官方协议事实与不可违反边界

LD2450 实时上报帧固定为 30 字节：

```text
Byte 0..3    AA FF 03 00
Byte 4..11   target slot 1
Byte 12..19  target slot 2
Byte 20..27  target slot 3
Byte 28..29  55 CC
```

每个槽固定 8 字节：

```text
X                   u16 little-endian，方向编码
Y                   u16 little-endian，方向编码
Speed               u16 little-endian，方向编码，单位 cm/s
Distance resolution u16 little-endian，单位 mm
```

必须保持：

1. 实时上报帧没有 length 字段。
2. 实时上报帧没有 command 字段。
3. 实时上报帧没有官方 CRC/checksum。
4. 完整性检查依赖固定长度、帧头、帧尾和流重同步。
5. 配置/应答帧不是实时上报帧。
6. 全零 8 字节槽表示无目标。
7. 最多 3 个目标。
8. `slot 1..3` 不是持久人物 ID。
9. LD2450 实时协议没有人体类别字段。
10. LD2450 实时协议没有官方 confidence 字段。
11. `resolution_mm` 不是欧氏距离。
12. `distance_mm` 由 C5 使用 X/Y 安全计算。
13. 坐标正负方向不在 C5 改写，由 S3 安装标定处理。

官方目标槽测试向量：

```text
0E 03 B1 86 10 00 40 01
```

预期：

```text
X = -782 mm
Y = 1713 mm
Speed = -16 cm/s
Resolution = 320 mm
```

---

## 4. 冻结设计决策

### 4.1 删除 `confidence`

C5 -> S3 合同中不传 `confidence`。

原因：

- LD2450 官方实时帧没有 confidence；
- C5 无训练或统计依据生成 0–100 数值；
- 虚构 confidence 会误导 S3 和后续诊断；
- 若未来确需质量字段，必须单独设计 `quality_flags`，且说明来源。

本轮目标字段固定为：

```text
slot
x_mm
y_mm
speed_cm_s
resolution_mm
distance_mm
```

### 4.2 C5 默认不做 EMA

C5 不默认平滑坐标。

原因：

- S3 tracker 已负责跨帧滤波；
- C5 先平滑、S3 再平滑会增加延迟；
- C5 EMA 会改变 association 观测噪声；
- 多人交叉时双重平滑可能增加误关联。

允许保留一个默认关闭的实验开关，但：

- 默认必须关闭；
- 不进入正式上传数据；
- 不得用其替代 S3 tracker；
- 不得在本轮验收中依赖该开关。

### 4.3 C5 只做基础异常过滤

允许：

- 全零槽过滤；
- 固定长度、帧头和帧尾检查；
- 字段解码；
- 64 位中间值距离计算；
- 明显结构错误拒绝；
- parser sequence；
- notification 流重同步；
- body 大小和目标数限制。

禁止未经实测在 C5 加入：

- 房间边界；
- 最大房间距离；
- zone；
- 轨迹预测；
- 人数；
- occupancy；
- motion；
- 自定义人体分类。

### 4.4 S3 为唯一空间语义源

所有来源都必须进入同一 S3 语义流水线：

```text
observation
-> coordinate transform
-> zone map
-> target association/tracker
-> spatial state
```

S3 本地 UART 与 C51/C52 的差异只允许存在于输入 adapter，不允许存在于空间算法副本。

### 4.5 三来源独立

S3 source 固定：

```c
RADAR_SOURCE_S3_LOCAL = 0
RADAR_SOURCE_C51 = 1
RADAR_SOURCE_C52 = 2
```

每个来源独立保存：

```text
identity
coordinate config
zone config
tracker
spatial state
freshness
radar link state
latest observation
latest snapshot
diagnostics
```

禁止跨来源关联和融合。

---

## 5. 必须先确认的硬件合同

### 5.1 已知绑定

固定默认绑定：

```text
C51:
  local_id = 1
  device_id = sensair_shuttle_01
  radar_mac = C1:BC:3C:3C:3D:60

C52:
  local_id = 2
  device_id = sensair_shuttle_02
  radar_mac = 8C:B1:F3:E1:15:41
```

MAC 应以 6 字节二进制存储和比较，字符串仅用于配置和日志。

### 5.2 不得猜测的字段

以下值未获得可靠确认前，不得硬编码猜测：

```text
radar_service_uuid
radar_notify_uuid
radar_address_type
```

必须支持配置：

```c
typedef struct {
    uint8_t radar_mac[6];
    radar_ble_addr_type_t address_type;
    radar_uuid_t service_uuid;
    radar_uuid_t notify_uuid;
} radar_ble_binding_t;
```

### 5.3 UUID 门禁

没有真实 UUID 时，Codex可以完成：

- 配置结构；
- UUID parser；
- scan/filter 状态机；
- GATT discovery 框架；
- mock/单元测试；
- build。

不得声称：

- 已能连接真实 LD2450；
- notify 已订阅；
- 真机 BLE 已验证。

最终状态必须标记：

```text
BLOCKED_BY_RADAR_GATT_UUID
```

直到用户提供并验证 UUID。

### 5.4 地址类型

BLE 地址必须同时考虑：

```text
public
random static
random private/resolvable
```

本轮定向绑定要求：

- 默认使用配置指定 address type；
- 不仅比较显示字符串；
- 不因地址字节序错误连接其他设备；
- 日志使用统一大写冒号格式；
- 不允许名称匹配替代 MAC 绑定；
- MAC 匹配后仍必须校验 service UUID。

---

## 6. 修改范围

### 6.1 ESPC51 / ESPC52 允许范围

预计新增或修改：

```text
components/Middlewares/radar_ble_client/
components/Middlewares/radar_ld2450_edge/
components/Middlewares/radar_gateway_client/
components/Middlewares/radar_config/
components/Middlewares/CMakeLists.txt
components/Middlewares/app_orchestrator/  # 仅最小启动 hook
components/app_config/                    # 仅雷达开关/资源参数
```

实际目录以 live source 为准。若已有雷达草案模块，应优先整理和替换，不得重复创建同职责组件。

允许复用现有通用 C5 -> S3 HTTP 客户端，但不得修改 BME、voice、command 调用行为。

### 6.2 ESPS3 允许范围

预计新增或修改：

```text
components/Middlewares/local_http_server/
components/Middlewares/radar_gateway_ingest/
components/Middlewares/radar_domain/
components/Middlewares/CMakeLists.txt
components/esp111_protocol_common/        # 仅本地 radar route/schema 常量
```

如 active 协议头在三套工程各有副本，新增 local radar 常量必须按当前项目规则同步；不得将 `/api/*` Server 路径下放到 C5。

### 6.3 禁止范围

除上述雷达接入和必要适配外，不修改：

```text
BME690 实现
voice 实现
command 实现
ESP-server
Dashboard
数据库
managed_components
archive
build 产物作为源码
```

---

## 7. C5 模块架构

建议职责划分：

```text
radar_config
  -> 读取 local_id/device_id/MAC/UUID/开关
radar_ble_client
  -> scan/connect/discover/subscribe/reconnect
radar_ld2450_stream_parser
  -> notification bytes -> complete raw frame
radar_edge_adapter
  -> decoded slots -> edge sample
radar_gateway_client
  -> latest-only upload / status update
radar_diagnostics
  -> counters / limited logs
```

不得将全部职责塞入一个大型 `.c` 文件。

### 7.1 单一写入者

- BLE callback 只复制 notification bytes 到固定环形缓冲或有界 stream buffer；
- parser worker 是 parser 状态的唯一写入者；
- upload worker 或现有低优先级调度点是 HTTP 状态的唯一写入者；
- BLE callback 中不做 JSON、HTTP、EMA、zone、tracking；
- 不允许多个任务并发修改 parser；
- 不允许 HTTP 请求持有 BLE notification 临时指针。

### 7.2 固定容量

固定上限：

```text
target slots = 3
frame size = 30 bytes
parser storage = 小型固定数组
pending latest result = 1
upload queue depth = 1 或 latest-only slot
HTTP body max = 1024 bytes
```

禁止：

- 无界队列；
- notification 每包动态分配；
- 每帧 malloc；
- 保存无限原始历史；
- 上传原始字节数组；
- callback 中 printf 大量 hex。

---

## 8. C5 BLE 状态机

### 8.1 状态

```c
typedef enum {
    RADAR_BLE_DISABLED = 0,
    RADAR_BLE_SCANNING,
    RADAR_BLE_CONNECTING,
    RADAR_BLE_DISCOVERING,
    RADAR_BLE_SUBSCRIBING,
    RADAR_BLE_READY,
    RADAR_BLE_BACKOFF
} radar_ble_state_t;
```

### 8.2 流程

```text
load binding
-> start scan
-> match MAC + address type
-> stop scan
-> connect
-> discover configured service UUID
-> discover configured notify characteristic UUID
-> verify notify property
-> subscribe CCCD
-> READY
-> receive notification stream
```

任何一步失败：

```text
record reason
-> disconnect/cleanup
-> bounded exponential backoff
-> scan again
```

### 8.3 重连参数

首轮候选：

```text
initial backoff = 500 ms
max backoff = 30 s
successful READY stable window = 10 s 后重置 backoff
scan window/interval = 以当前 NimBLE/IDF 合理默认和资源约束为准
```

不得持续无间隔扫描。

### 8.4 严格校验

连接成功不等于可用。只有全部满足才进入 READY：

```text
MAC matched
address type matched/config accepted
service UUID found
notify UUID found
characteristic supports notify
CCCD subscribe success
first valid LD2450 frame received
```

可以区分：

```text
link_ready
data_ready
```

### 8.5 断开处理

BLE 断开时：

- 清空 parser partial state；
- 保留累计诊断；
- 发布 `sample_valid=0` 的 radar status；
- 不发布 target_count=0 的有效空房间样本；
- 不改变 C5 device online；
- 不停止 BME/voice/command；
- 进入 backoff；
- S3 立即标记该 source radar_offline/UNKNOWN。

---

## 9. C5 LD2450 byte-stream parser

### 9.1 notification 不是 frame

BLE notification 可能：

- 小于 30 字节；
- 恰好 30 字节；
- 大于 30 字节；
- 包含半帧；
- 包含多帧；
- 从帧中间开始；
- 与下一次 notification 拼接。

因此必须把 notify payload 作为连续 byte stream，不能假定一次 notify 等于一帧。

### 9.2 parser 行为

必须支持：

- 固定 30 字节；
- `AA FF 03 00`；
- `55 CC`；
- 逐字节重同步；
- 半包；
- 粘包；
- 错误前缀；
- 坏尾；
- 多帧连续；
- partial frame 有界超时；
- 全零槽；
- 1–3 个有效槽。

### 9.3 parser 输出

```c
typedef struct {
    uint32_t frame_seq;
    uint32_t captured_uptime_ms;
    uint8_t raw_frame[30];       // 仅内部调试，可编译关闭
    uint8_t target_count;
    radar_edge_target_t targets[3];
} radar_edge_frame_t;
```

正式上传不得包含 `raw_frame`。

### 9.4 target

```c
typedef struct {
    uint8_t slot;
    int16_t x_mm;
    int16_t y_mm;
    int16_t speed_cm_s;
    uint16_t resolution_mm;
    uint32_t distance_mm;
} radar_edge_target_t;
```

### 9.5 数值安全

```text
d2 = int64(x_mm) * x_mm + int64(y_mm) * y_mm
distance_mm = integer_sqrt(d2)
```

不得使用 16 位中间值。

### 9.6 parser 计数

至少记录：

```text
notification_count
notification_bytes
valid_frame_count
bad_header_count
bad_tail_count
resync_count
skipped_byte_count
partial_timeout_reset_count
zero_target_frame_count
sequence_count
```

默认日志不打印 raw frame。

---

## 10. C5 轻量上报调度

### 10.1 最新值优先

雷达是最新状态数据，不需要堆积历史。

策略：

```text
parser produces latest edge frame
-> overwrite latest slot
-> upload scheduler reads newest generation
-> older unsent frame is coalesced
```

不得排队大量旧帧。

### 10.2 上传频率

首轮：

```text
maximum normal upload rate = 10 Hz
minimum configurable interval = 100 ms
status heartbeat = 1000 ms
state change = immediate best effort
```

若 LD2450 帧率高于上传频率，C5只上传最新完整帧并增加 coalesce counter。

### 10.3 优先级

radar 上传低于：

```text
voice active path
register/heartbeat
command ACK
关键网络恢复
```

不得修改 voice/BME/command 的现有业务逻辑。

当网络或本地 HTTP忙时：

- 覆盖旧 radar latest；
- 不阻塞 BLE callback；
- 不阻塞 parser；
- 不重试旧位置历史；
- 保留最新 status；
- 记录 drop/coalesce。

### 10.4 语音期间

不修改 voice 链路。

若现有 C5 runtime 在 voice busy 时禁止普通本地 HTTP：

- radar upload 按现有 gate 延迟；
- BLE/parser 可继续运行，资源允许时保持 latest；
- S3 可能将 source 标记 stale/UNKNOWN；
- voice 结束后上传最新 frame/status；
- 不为了 radar 绕过 voice gate。

---

## 11. C5 -> S3 固定接口合同

### 11.1 路由

```http
POST /local/v1/radar/result
Content-Type: application/json
```

不得复用：

```text
/local/v1/sensor
/local/v1/csi/result
BME payload
CSI payload
```

### 11.2 请求体

正式 schema：

```json
{
  "p": 2,
  "id": 1,
  "t": "radar",
  "u": 123456,
  "q": 10,
  "v": {
    "link_state": 5,
    "sample_valid": 1,
    "frame_seq": 234,
    "frame_uptime_ms": 123450,
    "target_count": 1,
    "targets": [
      {
        "slot": 0,
        "x_mm": 1200,
        "y_mm": 800,
        "speed_cm_s": 15,
        "resolution_mm": 320,
        "distance_mm": 1442
      }
    ]
  }
}
```

### 11.3 字段定义

| 字段 | 类型 | 必需 | 语义 |
|---|---|---:|---|
| `p` | integer | 是 | 本地 radar schema，固定 2 |
| `id` | integer | 是 | 1=C51，2=C52 |
| `t` | string | 是 | 固定 `"radar"` |
| `u` | uint32 | 是 | C5 请求时 monotonic uptime ms |
| `q` | uint32 | 是 | C5 radar request sequence |
| `v.link_state` | integer | 是 | BLE 状态枚举 |
| `v.sample_valid` | integer 0/1 | 是 | 是否包含有效完整 LD2450 frame |
| `v.frame_seq` | uint32 | 是 | C5 parser 完整帧 sequence |
| `v.frame_uptime_ms` | uint32 | 是 | 该帧完成时 C5 uptime |
| `v.target_count` | integer | 是 | 0..3 |
| `v.targets` | array | 是 | 长度必须等于 target_count |
| `slot` | integer | 是 | 0..2，仅调试槽位，不是人物 ID |
| `x_mm` | integer | 是 | 原始 X |
| `y_mm` | integer | 是 | 原始 Y |
| `speed_cm_s` | integer | 是 | 原始速度 |
| `resolution_mm` | integer | 是 | 官方距离分辨率字段 |
| `distance_mm` | integer | 是 | C5 从 X/Y 派生的欧氏距离 |

### 11.4 状态消息

BLE断开或未获得有效帧：

```json
{
  "p": 2,
  "id": 1,
  "t": "radar",
  "u": 130000,
  "q": 11,
  "v": {
    "link_state": 6,
    "sample_valid": 0,
    "frame_seq": 234,
    "frame_uptime_ms": 123450,
    "target_count": 0,
    "targets": []
  }
}
```

关键语义：

```text
sample_valid=1 + target_count=0
= 雷达在线且当前完整帧无目标

sample_valid=0
= 没有新的有效雷达样本，不能解释为空房间
```

### 11.5 固定限制

S3必须限制：

```text
body <= 1024 bytes
p == 2
t == "radar"
id in {1,2}
target_count in [0,3]
targets.length == target_count
slot in [0,2]
slot 不重复
sample_valid in {0,1}
sample_valid=0 -> target_count=0 and targets=[]
JSON depth 有界
不接受 NaN/Infinity/float 坐标
不接受额外 raw bytes/base64
```

数值范围应以类型和工程安全边界校验，不得擅自把合理的官方负坐标拒绝。

### 11.6 身份交叉校验

若现有通用 HTTP header 包含：

```text
X-Device-Id
X-Gateway-Id
```

S3必须交叉检查：

```text
id=1 <-> X-Device-Id=sensair_shuttle_01
id=2 <-> X-Device-Id=sensair_shuttle_02
```

不一致返回身份错误，不得仅信任 body。

### 11.7 sequence 语义

`q` 是请求幂等 sequence，`frame_seq` 是雷达完整帧 sequence。

S3每个 source 保存：

```text
last_request_q
last_frame_seq
```

规则：

- 相同 `q`：幂等成功，不重复处理；
- 明显旧 `q`：拒绝或记录 stale request；
- `sample_valid=1` 时 `frame_seq` 必须是新帧；
- `sample_valid=0` 状态消息可复用最后 frame_seq；
- 比较必须支持 uint32 wrap；
- C51/C52 sequence 独立。

### 11.8 响应

成功处理新数据：

```json
{"ok":1,"id":1,"accepted":1}
```

重复请求：

```json
{"ok":1,"id":1,"accepted":0,"duplicate":1}
```

错误响应至少区分：

```text
invalid_json
payload_too_large
invalid_schema
invalid_device_id
identity_mismatch
invalid_target_count
invalid_target
sequence_rejected
radar_ingest_disabled
internal_error
```

不得返回大量内部实现细节。

---

## 12. S3 radar ingest 架构

新增窄职责组件：

```text
local_http_server route
  -> radar_gateway_ingest_validate()
  -> radar_remote_adapter_build_observation()
  -> radar_domain_submit(source, observation)
```

### 12.1 `local_http_server`

职责：

- 注册 route；
- body 大小门禁；
- Content-Type；
- 读取完整 body；
- 调用 ingest；
- 返回有限 JSON。

禁止：

- 在 handler 内做坐标转换；
- 在 handler 内做 tracker；
- 在 handler 内做 zone；
- 在 handler 内持有长期状态；
- 在 handler 内访问 Server。

### 12.2 `radar_gateway_ingest`

职责：

- JSON schema；
- 身份映射；
- sequence；
- link state；
- target arrays；
- per-source ingest counters；
- status/frame 区分；
- 生成纯内部 remote sample。

### 12.3 `radar_remote_adapter`

职责：

将远端 sample 转换为与 S3本地 UART adapter 相同语义的：

```c
typedef struct {
    radar_source_id_t source;
    uint32_t frame_seq;
    int64_t captured_at_ms;
    uint32_t source_age_ms;
    bool source_valid;
    uint8_t raw_target_count;
    radar_observation_target_t targets[3];
} radar_observation_frame_t;
```

时间规则：

- C5 `frame_uptime_ms` 仅用于传输延迟和顺序诊断；
- S3 使用接收时 monotonic time 作为本地 freshness 基准；
- 不把 C5 uptime 当 S3时间；
- 不要求绝对墙钟同步；
- 不把网络延迟补偿成虚构坐标时间。

### 12.4 source 映射

```text
local id 1 -> RADAR_SOURCE_C51
local id 2 -> RADAR_SOURCE_C52
S3 UART -> RADAR_SOURCE_S3_LOCAL
```

不得通过 body 指定任意 source。

---

## 13. S3 radar source context

```c
typedef struct {
    radar_source_id_t source;
    const char *device_id_static;
    uint8_t local_id;
    bool enabled;

    radar_link_state_t radar_link;
    uint32_t last_ingest_ms;
    uint32_t last_valid_frame_ms;
    uint32_t last_request_q;
    uint32_t last_frame_seq;

    radar_pipeline_config_t config;
    radar_target_tracker_t tracker;
    radar_spatial_state_context_t spatial;
    radar_source_diagnostics_t diagnostics;
    radar_snapshot_t latest_snapshot;
} radar_source_context_t;
```

要求：

```text
S3_LOCAL enabled = 按当前本地 UART配置
C51 enabled = C51 radar ingest启用
C52 enabled = C52 radar ingest启用
```

每个 source：

- 独立 mutex 或由单一 radar pipeline owner 串行处理；
- 独立 tracker；
- 独立 zone map config；
- 独立坐标 config；
- 独立 freshness；
- 独立 diagnostics；
- 独立 occupancy/motion；
- 独立 reset。

禁止：

- 三个 source 共用一个 tracker；
- 远端 frame 清空 S3_LOCAL；
- C51 status 更新 C52；
- source index 越界；
- device_id 动态悬空指针。

---

## 14. S3 既有算法接入

统一顺序固定：

```text
radar_observation_frame
-> raw target validation
-> radar_coordinate_transform
-> radar_zone_map
-> radar_target_tracker
-> radar_spatial_state
-> source snapshot
```

### 14.1 adapter 原则

若现有算法输入结构不同：

- 新增 `radar_observation_to_domain_adapter`；
- 保留 raw 与 calibrated 坐标；
- 不修改算法使其理解 JSON；
- 不让算法依赖 local_id；
- 不让算法依赖 BLE；
- 不让算法依赖 HTTP。

### 14.2 坐标配置

每个雷达安装位置不同，必须 source-specific：

```text
flip_x
flip_y
rotation
offset_x
offset_y
max_distance
room bounds
zone map
```

初始不得猜测。

默认：

```text
保持当前 S3_LOCAL配置
C51/C52 使用未标定默认配置并明确标记 calibration_pending
```

在未标定时：

- 可以处理原始坐标；
- 不得声称房间位置准确；
- zone结果如依赖边界，必须使用明确默认或关闭；
- 最终报告标记 `HARDWARE_CALIBRATION_PENDING`。

### 14.3 slot 与 track

`slot` 只用于：

- frame diagnostics；
- slot->track mapping；
- parser 测试。

`track_id` 只由 `radar_target_tracker`生成。

---

## 15. S3 tracker 增量优化

本轮可以与远端接入同步完成，但必须在 adapter 接入和回归测试后实施。

### 15.1 不重写

保留现有 tracker public API，或增加兼容 wrapper。

### 15.2 目标

将固定位置贪心最近邻增量增强为：

```text
prediction
-> dynamic gate
-> 3x3 fixed cost matrix
-> deterministic global assignment
-> lifecycle
-> adaptive EMA
```

### 15.3 生命周期

```text
EMPTY
TENTATIVE
CONFIRMED
HOLD
```

首轮参数：

```text
confirm = 连续 2 帧
business HOLD = 500 ms
track retention/release = 1800 ms
```

TENTATIVE 不参与正式 occupancy。

### 15.4 预测与 gate

候选首轮：

```text
gate = max(800 mm, speed * dt + 400 mm)
gate <= 1200 mm
```

参数必须集中配置。

### 15.5 全局分配

最多三目标，枚举合法 one-to-one assignment，不引入 Hungarian 库。

tie-break 必须确定：

1. 最小总 cost；
2. 更多 confirmed/hold 匹配；
3. 更低 track_id；
4. 更低 slot。

### 15.6 EMA

候选：

```text
静止 alpha = 0.25
正常移动 alpha = 0.50
快速移动 alpha = 0.60
```

C5不做默认 EMA，避免双重滤波。

### 15.7 异常点

gate 外 measurement：

- 不移动已有 confirmed track；
- 旧 track进入 HOLD；
- measurement可创建 TENTATIVE；
- 连续两帧后才确认；
- 单帧异常快速释放。

---

## 16. Presence / Motion / Occupancy

必须区分：

```text
device_online
radar_link_online
radar_sample_fresh
occupancy
motion
```

### 16.1 source状态

```text
C5 heartbeat online + radar sample stale
-> device_online
-> radar_offline/stale
-> occupancy UNKNOWN
```

### 16.2 occupancy

```text
source invalid/stale/recovering -> UNKNOWN
valid + confirmed visible -> PRESENT
valid + confirmed短暂丢失 <=500ms -> HOLD
valid + 无 confirmed 且HOLD过期 -> VACANT_INFERRED
```

### 16.3 motion

仅 confirmed visible stable track 可触发。

候选：

```text
abs(reported speed) >= 15 cm/s
或
filtered displacement >= 120 mm/正常周期
```

滞回：

```text
连续3帧进入 MOVING
连续8帧低运动退出
```

禁止：

- tentative触发；
- gate外异常触发；
- radar offline输出 IDLE；
- HOLD继续累加运动；
- `speed==0` 作为静止人体唯一依据。

---

## 17. S3 downstream 边界

本轮不修改 ESP-server 或 Dashboard。

### 17.1 允许

- 更新现有 S3 radar latest snapshot；
- 更新现有 registry 中雷达模块状态；
- 使用现有下游结构能够容纳的字段；
- 保持当前 S3_LOCAL 下游行为；
- 为 C51/C52 保存本地 source snapshot。

### 17.2 禁止

- 新增 `/api/radar`；
- 修改 server ingest schema；
- 修改 Dashboard JSON；
- 修改数据库；
- 将远端 radar JSON 直接转发 Server；
- 发明 server字段。

### 17.3 下游缺口

如果 live source 当前没有可兼容的多雷达下游合同：

- 本轮停止在 S3 latest snapshot/registry；
- 记录 `BLOCKED_BY_EXISTING_DOWNSTREAM_CONTRACT`；
- 不自行修改 Server；
- 不声称 Dashboard 已显示 C51/C52 雷达。

---

## 18. 日志与诊断

### 18.1 C5 必需日志

状态变化立即输出，周期日志限频。

```text
radar_ble_state
radar_ble_connected
radar_gatt_validated
radar_notify_subscribed
radar_frame_received
radar_target_count
radar_upload_ok
radar_upload_coalesced
radar_ble_disconnected
```

每条至少带：

```text
local_id
device_id
state/reason
frame_seq（适用时）
```

禁止默认打印大量 raw frame。

### 18.2 S3 必需日志

```text
radar_ingest_ok
source
device_id
request_q
frame_seq
targets
radar_link
tracking_state
presence_state
motion_state
```

默认 summary 不高于 1 Hz，状态改变立即输出。

### 18.3 诊断计数

C5：

```text
scan_cycles
connect_attempts
gatt_failures
subscribe_failures
disconnects
notifications
valid_frames
parser_errors
coalesced_frames
upload_success
upload_failures
```

S3：

```text
ingest_requests
ingest_accepted
duplicates
sequence_rejects
schema_rejects
identity_mismatches
source_stale
tracking_creates
tracking_confirms
tracking_releases
association_rejects
```

### 18.4 字符串安全

- 枚举转静态字符串；
- 不保存临时 `%s` 指针；
- 不对未终止缓冲使用 `%s`；
- 异步状态保存数值枚举；
- device_id使用静态常量或深拷贝；
- 日志不得改变算法状态。

---

## 19. 资源与实时性

### 19.1 C5

硬性要求：

```text
BLE callback 零 heap
parser 每帧零 heap
edge adapter 每帧零 heap
latest slot 固定容量
不创建无界队列
不保存 raw历史
不上传 raw
```

新增常驻任务原则上不超过：

```text
1 个 parser/worker任务
```

若 NimBLE回调和现有调度能安全承担，不新增第二个任务。

新增静态/动态资源必须报告：

```text
task stack
queue/stream buffer
static BSS
heap delta
largest block
```

### 19.2 S3

硬性要求：

```text
ingest parse有明确body上限
每source固定3 targets/3 tracks
association零heap
tracker零heap
spatial零heap
不新增网络上传任务
```

优先复用现有 HTTP server task 和 radar pipeline调度。

### 19.3 性能门槛

纯算法：

```text
三目标单帧 S3 pipeline < 5 ms（关闭详细日志）
```

C5：

```text
BLE callback 不执行阻塞操作
parser/adapter处理显著低于帧间隔
HTTP失败不阻塞 BLE输入
```

---

## 20. 实施阶段

## P0：三工程 live source 基线

只读任务：

1. 确认 `/Users/zhiqin/ESP 雷达` 真实目录。
2. 确认各工程 git/dirty 状态。
3. 查找 C51/C52 是否已有雷达草案。
4. 查找 S3现有 radar模块和调用链。
5. 查找 C5 NimBLE组件和已有 BLE使用。
6. 查找 C5通用 HTTP本地客户端。
7. 查找启动编排和 voice gate。
8. 查找三份协议头现状。
9. 查找 S3 local HTTP注册方式。
10. baseline build C51/C52/S3。
11. 记录旧 warning和失败。
12. 不修改源码。

输出：

```text
docs/radar-c5-s3-baseline-2026-07-17.md
```

门禁：

- 未完成 live source 审计不得编码；
- baseline失败必须区分旧失败和本轮失败。

## P1：冻结共享合同

任务：

- source enum；
- BLE state enum；
- edge target；
- request schema；
- error enum；
- route常量；
- body上限；
- sequence规则；
- identity映射；
- C51/C52 parity规则。

必须先写：

```text
docs/radar-local-contract-v2.md
```

门禁：

- C5结构字段与S3 parser逐字段对应；
- 单位明确；
- 无 confidence；
- 有 sample_valid；
- 有 resolution_mm；
- 目标上限3；
- C51/C52格式一致。

## P2：C5 parser纯函数

任务：

- 固定30字节 parser；
- byte stream feed；
- 官方解码；
- half/sticky/resync；
- target结构；
- distance；
- counters；
- 单元测试。

先在 C51完成后同步到 C52，或使用项目允许的共享机制；最终代码必须 parity。

门禁：

- parser测试全通过；
- 每帧零heap；
- 不接 BLE；
- 不接 HTTP；
- 不做EMA/zone/track。

## P3：C5 BLE client

任务：

- binding config；
- MAC/address type；
- UUID；
- state machine；
- scan；
- connect；
- discovery；
- subscribe；
- backoff；
- callback -> parser feed；
- status events。

门禁：

- 无UUID时 build/mock通过并标记 blocked；
- 不通过名称连接；
- MAC后仍校验UUID；
- callback无HTTP/JSON；
- 断开不影响其他链路。

## P4：C5 radar gateway client

任务：

- schema codec；
- latest-only；
- 10Hz上限；
- 1Hz status；
- immediate disconnect status；
- 通用本地HTTP；
- response/error；
- coalesce；
- voice gate兼容；
- startup最小hook。

门禁：

- body<=1024；
- 不复用sensor/csi；
- 不上传raw；
- C51/C52 payload golden完全一致，仅id不同；
- 网络失败不阻塞 BLE。

## P5：S3 route 与 ingest

任务：

- route；
- body门禁；
- JSON schema；
- header/body identity；
- sequence；
- link status；
- source映射；
- per-source state；
- error response；
- tests。

门禁：

- id 1/2正确映射；
- 超3目标拒绝；
- sample_valid语义正确；
- duplicate幂等；
- radar断开不影响device online；
- 不访问Server。

## P6：S3 remote adapter 与统一 pipeline

任务：

- remote sample -> observation；
- S3_LOCAL继续使用本地adapter；
- 三source context；
- source-specific config；
- mutex/single owner；
- freshness；
- latest snapshot；
- existing radar_domain调用。

门禁：

- 三source互不覆盖；
- 本地UART行为无回归；
- C51/C52同一算法；
- 无JSON进入算法层；
- source stale -> UNKNOWN。

## P7：S3 tracker/spatial增量优化

任务：

- prediction；
- dynamic gate；
- deterministic 3x3 assignment；
- lifecycle；
- adaptive EMA；
- abnormal point；
- count语义；
- occupancy/motion边界。

门禁：

- 不重写四个已有模块；
- adapter优先；
- 旧API兼容；
- 单帧杂波不成为业务目标；
- slot swap不直接换ID；
- offline不VACANT。

## P8：诊断、资源与安全

任务：

- C5/S3 counters；
- rate limit；
- static strings；
- stack；
- processing time；
- heap scans；
- protected paths；
- source reset reasons。

门禁：

- 默认无raw刷屏；
- 无瞬态字符串风险；
- 无新增无界queue；
- 无每帧heap；
- 无无关链路修改。

## P9：软件测试与三工程 build

任务：

- parser tests；
- codec golden tests；
- ingest schema tests；
- mapping tests；
- source isolation；
- tracker/spatial replay；
- C51 build；
- C52 build；
- S3 build；
- C51/C52 parity diff；
- warning diff；
- route negative scan。

门禁：

- 三工程build；
- 无本轮新增warning；
- protected paths零修改；
- 不执行flash/monitor；
- 硬件未测明确pending。

## P10：交付

输出：

```text
docs/radar-c5-s3-execution-log.md
docs/radar-local-contract-v2.md
docs/radar-c5-parser-test-report.md
docs/radar-s3-ingest-test-report.md
docs/radar-s3-replay-test-report.md
docs/radar-resource-report.md
docs/radar-hardware-test-checklist.md
```

---

## 21. 测试矩阵

### 21.1 C5 parser

| 编号 | 场景 | 预期 |
|---|---|---|
| C5-PAR-01 | 官方槽 | 数值准确 |
| C5-PAR-02 | 单帧 | 1 frame |
| C5-PAR-03 | 逐字节 | 与单帧一致 |
| C5-PAR-04 | 两帧粘包 | 2 frames |
| C5-PAR-05 | 半帧 | 补齐后输出 |
| C5-PAR-06 | 垃圾前缀 | 重同步 |
| C5-PAR-07 | 坏尾 | 拒绝并恢复 |
| C5-PAR-08 | 帧头跨notify | 正确 |
| C5-PAR-09 | 全零 | target_count=0 |
| C5-PAR-10 | 三目标 | target_count=3 |
| C5-PAR-11 | 负坐标/速度 | 正确 |
| C5-PAR-12 | 距离大值 | 无溢出 |

### 21.2 BLE状态机

| 编号 | 场景 | 预期 |
|---|---|---|
| BLE-01 | 非目标MAC | 不连接 |
| BLE-02 | MAC匹配服务不匹配 | 断开/backoff |
| BLE-03 | notify不存在 | 断开/backoff |
| BLE-04 | subscribe失败 | 重试 |
| BLE-05 | READY后断开 | status offline |
| BLE-06 | 高频断开 | 有界backoff |
| BLE-07 | notify半帧 | parser拼接 |
| BLE-08 | notify多帧 | parser多输出 |

### 21.3 Codec与接口

| 编号 | 场景 | 预期 |
|---|---|---|
| API-01 | C51 golden | id=1字段正确 |
| API-02 | C52 golden | id=2字段正确 |
| API-03 | 0目标有效帧 | sample_valid=1 |
| API-04 | BLE断开 | sample_valid=0 |
| API-05 | 4目标 | C5不生成/S3拒绝 |
| API-06 | confidence字段 | 不生成 |
| API-07 | raw字段 | 不生成/S3拒绝 |
| API-08 | targets长度不符 | S3拒绝 |
| API-09 | identity mismatch | S3拒绝 |
| API-10 | duplicate q | 幂等 |

### 21.4 Source隔离

| 编号 | 场景 | 预期 |
|---|---|---|
| SRC-01 | C51 frame | 只更新C51 |
| SRC-02 | C52 frame | 只更新C52 |
| SRC-03 | S3 UART frame | 只更新S3_LOCAL |
| SRC-04 | C51 offline | C52/S3不变 |
| SRC-05 | C52 sequence旧 | 只拒绝C52 |
| SRC-06 | C51 tracker reset | 其他不清空 |

### 21.5 Tracker/spatial

至少覆盖：

```text
单帧杂波
静止抖动
单人走动
快速走动
两人平行
两人交叉
slot交换
短暂遮挡
三目标
source短时stale
source断开
source恢复
```

---

## 22. 验收指标

### 22.1 C5

必须报告：

```text
BLE连接尝试
GATT校验失败
notify订阅
有效帧
parser错误
coalesce
upload成功/失败
callback最大耗时
parser最大耗时
新增task stack
新增queue/stream buffer
heap变化
```

### 22.2 S3

必须报告：

```text
ingest accepted/rejected
identity mismatch
duplicate/stale sequence
每source freshness
track create/confirm/release
false confirmed track
ID switch
occupancy/motion delay
pipeline avg/max time
静态内存变化
每帧heap allocation
```

### 22.3 硬门槛

```text
C5/S3每帧算法零heap
C5 callback不阻塞
body<=1024
targets<=3
三工程build
C51/C52协议完全一致
单source故障不影响其他链路
radar offline不等于device offline
radar stale不等于vacant
无raw上传
无confidence
无slot-as-track-id
无新增Server/Dashboard修改
```

---

## 23. Build与静态检查

根据实际环境执行等价命令。

```bash
source /Users/zhiqin/.espressif/v5.5.4/esp-idf/export.sh >/dev/null

cd "/Users/zhiqin/ESP 雷达/ESPC51"
idf.py build

cd "/Users/zhiqin/ESP 雷达/ESPC52"
idf.py build

cd "/Users/zhiqin/ESP 雷达/ESPS3"
idf.py build
```

禁止执行：

```text
flash
monitor
erase-flash
fullclean
启动ESP-server
写真实数据库
```

除非用户后续明确授权。

建议扫描：

```bash
rg -n "confidence|raw_frame|base64" ESPC51/components/Middlewares/radar* ESPC52/components/Middlewares/radar* ESPS3/components/Middlewares/radar*
rg -n "/api/|/local/v1/sensor|/local/v1/csi" ESPC51/components/Middlewares/radar* ESPC52/components/Middlewares/radar*
rg -n "malloc|calloc|realloc|free" ESPC51/components/Middlewares/radar* ESPC52/components/Middlewares/radar* ESPS3/components/Middlewares/radar*
rg -n "xTaskCreate|xTaskCreateWithCaps" ESPC51/components/Middlewares/radar* ESPC52/components/Middlewares/radar* ESPS3/components/Middlewares/radar*
rg -n "CRC|checksum" ESPC51/components/Middlewares/radar* ESPC52/components/Middlewares/radar*
```

命中必须人工解释，不能机械判定。

---

## 24. 硬件验收

Codex只生成清单，用户执行。

### HW-C51-01

- C51只连接 `C1:BC:3C:3C:3D:60`；
- UUID校验成功；
- notify稳定；
- frame计数增长；
- C51 JSON id=1。

### HW-C52-01

- C52只连接 `8C:B1:F3:E1:15:41`；
- UUID校验成功；
- notify稳定；
- frame计数增长；
- C52 JSON id=2。

### HW-S3-01

- S3同时接收C51/C52；
- source不覆盖；
- `radar_ingest_ok`；
- target字段一致；
- tracker独立。

### HW-FAULT-01

断开C51雷达：

```text
C51 device_online
C51 radar_offline
C51 occupancy UNKNOWN
C52/S3_LOCAL正常
BME/voice/command正常
```

### HW-CAL-01

每个雷达分别做已知点标定：

```text
正前方
左前方
右前方
近点
远点
区域边界
```

未完成前不得冻结坐标参数。

### HW-MULTI-01

两人交叉，记录：

```text
slot
track_id
raw位置
filtered位置
ID switch
official tool raw count
```

所有硬件项未执行时标记：

```text
HARDWARE_VALIDATION_PENDING
```

---

## 25. 回滚

实施前：

- 各工程若为git仓库，记录独立status和checkpoint；
- 顶层非git时生成文件清单和patch；
- 不依赖build目录回滚；
- 不混入无关修改。

每阶段可独立回滚：

```text
P2 parser
P3 BLE
P4 C5 upload
P5 S3 ingest
P6 adapter
P7 algorithm enhancement
```

如果P7回归：

- 保留已验证的C5/S3合同和adapter；
- 回滚tracker/spatial增强；
- 不回退到raw上传；
- 不删除诊断以掩盖问题。

---

## 26. 最终交付要求

Codex最终必须列出：

1. C51修改文件；
2. C52修改文件；
3. S3修改文件；
4. 新增文件；
5. C51/C52 parity结果；
6. 冻结JSON合同；
7. MAC/UUID配置来源；
8. 未确认UUID门禁；
9. parser测试；
10. BLE mock测试；
11. codec golden；
12. S3 ingest测试；
13. source隔离测试；
14. tracker/spatial回放；
15. 三工程build；
16. warning差异；
17. task/stack/queue变化；
18. heap/static memory变化；
19. protected paths零修改；
20. 硬件pending；
21. downstream未修改说明；
22. 风险与回滚。

不得声称：

- UUID已知但实际未提供；
- 真机BLE通过但未测试；
- confidence来自官方；
- C5完成occupancy；
- slot是人物ID；
- 三房间融合完成；
- Dashboard已支持新字段；
- Server已修改；
- radar断开代表无人。

---

## 27. 完成定义

软件阶段只有满足以下条件才可完成：

```text
C51/C52/S3 live baseline完成
+ local contract v2冻结
+ C51/C52 parser一致
+ BLE client框架完成
+ UUID未知状态诚实标记
+ C51/C52轻量上传完成
+ S3独立route/ingest完成
+ local_id映射正确
+ remote adapter完成
+ 三source独立context完成
+ 既有radar_domain复用
+ tracker/spatial回归通过
+ offline/UNKNOWN语义正确
+ 三工程build通过
+ 无新增warning
+ 每帧零heap
+ protected paths零修改
+ 硬件验收pending
```

---

## 28. Codex最终执行命令

```text
在 /Users/zhiqin/ESP 雷达 中严格执行《ESPC51 / ESPC52 / ESPS3 LD2450 雷达链路联合实施计划》。

本轮必须同步修改 ESPC51、ESPC52 与 ESPS3，一次冻结并双方实现 C5 -> S3 radar local contract。禁止只改一端。禁止修改 ESP-server、Dashboard、数据库、managed_components，以及 BME690、voice、command 等现有业务链路。

先读取三工程 live source，完成 dirty状态、调用链、任务所有权、NimBLE能力、HTTP客户端、协议头、S3 local HTTP、现有 radar_domain 和 baseline build审计。不得用历史文档替代源码。

C51固定 local_id=1、device_id=sensair_shuttle_01、radar_mac=C1:BC:3C:3C:3D:60；C52固定 local_id=2、device_id=sensair_shuttle_02、radar_mac=8C:B1:F3:E1:15:41。radar_service_uuid、radar_notify_uuid和BLE address type必须配置并严格校验，未知时不得猜测，最终标记 BLOCKED_BY_RADAR_GATT_UUID。

C5只负责BLE定向连接、service/notify校验、notify byte stream接收、LD2450固定30字节帧解析、全零槽过滤、X/Y/Speed/Resolution提取、64位安全distance计算、frame sequence、uptime、丢帧诊断和轻量上传。C5不得做zone、房间区域、track_id、人数、occupancy、presence、motion、空间模型或默认EMA。不得生成或上传confidence，不得上传raw BLE bytes或raw 30-byte frame。

新增独立 POST /local/v1/radar/result，严格使用本文contract v2：p/id/t/u/q/v、link_state、sample_valid、frame_seq、frame_uptime_ms、target_count和最多3个targets；每个target仅含slot、x_mm、y_mm、speed_cm_s、resolution_mm、distance_mm。body上限1024 bytes。C51/C52格式必须完全一致，仅身份和MAC不同。使用latest-only/coalesce，正常上传不超过10Hz，状态1Hz并在断开时立即best-effort上报。

S3新增 radar_gateway_ingest 和 radar_remote_adapter。local_http_server只负责route/body读取；ingest负责schema、identity、sequence和状态；adapter负责转换成统一 radar_observation。local_id 1映射RADAR_SOURCE_C51，2映射RADAR_SOURCE_C52，S3 UART映射RADAR_SOURCE_S3_LOCAL。三source必须分别维护坐标、zone、tracker、spatial、freshness、snapshot和diagnostics，禁止跨房间融合或共享tracker。

复用现有 radar_coordinate_transform、radar_zone_map、radar_target_tracker、radar_spatial_state。接口不匹配时新增adapter，不重写完整算法。完成远端接入和回归后，可按计划增量实现预测、动态gate、定长全局分配、TENTATIVE/CONFIRMED/HOLD生命周期、自适应EMA和异常点隔离，但必须保持public API兼容并通过回放测试。

严格区分device_online、radar_online、freshness、occupancy和motion。雷达断开或stale必须是radar_offline/UNKNOWN，不能把C5标为device_offline，不能输出VACANT。任一雷达故障不得影响BME690、voice、command或其他雷达source。

完成C5 parser、BLE状态机mock、JSON codec golden、S3 schema/identity/sequence、source隔离、tracker/spatial回放、资源和日志安全测试。三工程分别build，检查C51/C52 parity、protected paths零修改、每帧零heap、无raw上传、无confidence、无slot-as-track-id、无新增Server/Dashboard接口。

禁止flash、monitor、erase-flash、fullclean、启动ESP-server或声称硬件通过。所有真机BLE、坐标、多目标和故障测试交给用户执行，并标记 HARDWARE_VALIDATION_PENDING。
```
