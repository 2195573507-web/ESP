# ESP BME690 Air Quality Roadmap

本文定义并记录 ESP 本地基于 BME690 的相对空气状态计算方案。2026-06-09 本方案已在 `Whole-project` 固件和 `ESP-server` 后端落地，本文同时作为后续维护说明。

## 实施状态

- 固件已新增 `sensor_domain/bme690/service/bme_air_quality.c` 和 `.h`。
- BME690 服务每次有效采样后调用 `bme_air_quality_update()`，并随 `sensor.bme690` v1 payload 上传原始 BME690 数据和空气状态结果。
- 固件算法版本为 `esp-bme690-relative-v1`，来源字段为 `air_quality_source="esp"`。
- 后端 `sensorBme690Service` 接收、校验、入库空气状态字段，保存 `air_quality_json` 和拆分列。
- 空气状态缺失或非法时，后端可 fallback 补算并标记 `air_quality_source="server_fallback"`；合法 ESP 上传结果不被覆盖。
- LLM prompt context 已明确说明该指标不是国标 AQI，不代表 PM2.5、PM10 或 CO2。
- 本轮没有把空气状态接入风险判断或紧急决策；Server 只接收、校验、存储和用于 context。

## 1. 指标定位

- 本指标不是国标 AQI。
- 本指标不代表 PM2.5、PM10、CO2。
- 本指标是基于 BME690 `gas_resistance_ohm`、`humidity_percent`、`temperature_c` 的相对空气状态估算。
- 主要用于设备本地趋势判断、服务器展示、LLM prompt 上下文。
- 命名使用 `air_quality_score`，不使用 `AQI`、`aqi` 或 `national_aqi`。
- 该分数更适合表达 "相对当前设备基线的空气状态"，不适合跨设备、跨城市、跨传感器直接对比。

## 2. ESP 端职责

- ESP 采集 BME690 原始数据。
- ESP 维护本地 `gas_baseline_ohm`。
- ESP 本地计算 `air_quality_score`。
- ESP 将原始数据和空气质量结果一起放入 `sensor.bme690` payload 上传。
- ESP 不能只上传空气质量结果，必须同时上传原始 BME690 数据。
- ESP 算法必须轻量，避免引入重型 BSEC 或明显增加 flash/ram 压力。
- 第一阶段 baseline 不强制掉电持久化；后续如要提高长期稳定性，可把 baseline 存入 NVS。

## 3. 服务器端职责

- 接收 ESP 上传的空气质量字段。
- 入库 `sensor_records`。
- ESP 上传合法时标记 `air_quality_source="esp"`。
- 如果 ESP 没上传空气质量字段，服务器可使用 fallback 算法补算。
- fallback 结果必须标记 `air_quality_source="server_fallback"`。
- LLM prompt 必须说明该指标来源和非 AQI 属性。
- 服务器不要默认覆盖 ESP 上传的空气质量，除非字段非法或缺失。
- 如果 ESP 上传分数与服务器 fallback 差异极大，第一阶段只记录 warning，不拒收。

## 4. 计算输入

ESP 端输入：

- `gas_resistance_ohm`: BME690 气体电阻，单位 ohm。
- `humidity_percent`: 相对湿度，单位 `%RH`。
- `temperature_c`: 温度，单位 Celsius。
- `gas_baseline_ohm`: ESP 本地维护的清洁空气参考基线。
- `baseline_ready`: 基线是否已有基本可信度。
- `warmup_done`: 传感器是否完成预热。
- `sample_count`: 有效样本数量。

服务器 fallback 输入：

- 最近一条或一段 `sensor.bme690` 原始数据。
- 已入库的 `gas_baseline_ohm` 或短窗口高值近似。
- 如果 baseline 不足，fallback 应输出 `air_quality_confidence="low"` 或 `none`。

## 5. 基线 `gas_baseline_ohm` 计算

第一版采用轻量动态基线，不要求掉电持久化。

### A. 预热期

- 设备启动后前 N 个样本不输出高置信度空气质量。
- 推荐 `warmup_sample_min = 30`。
- 在 `warmup_sample_min` 之前：
  - `air_quality_confidence = "low"`
  - `baseline_ready = false`

### B. 初始 baseline

- 从启动后有效样本中选择较高 `gas_resistance_ohm` 值作为初始 baseline。
- 推荐：

```text
gas_baseline_ohm = max(gas_resistance_ohm in recent valid samples)
```

- 或者使用轻量滑动窗口高分位近似。
- 初始 baseline 只接受正数，并应设置最小值保护。

### C. 运行期更新

只允许 baseline 缓慢上升或轻微下降，避免污染环境下 baseline 快速下滑。

推荐指数平滑：

```text
如果当前 gas_resistance_ohm > gas_baseline_ohm:
  gas_baseline_ohm = gas_baseline_ohm * 0.99 + gas_resistance_ohm * 0.01

如果当前 gas_resistance_ohm <= gas_baseline_ohm:
  gas_baseline_ohm = gas_baseline_ohm * 0.999 + gas_resistance_ohm * 0.001
```

说明：

- 上升快一点，便于学习更干净空气参考。
- 下降非常慢，避免污染环境把 baseline 拉低。
- `gas_baseline_ohm` 必须有最小值保护，避免除零。
- 推荐最小保护值可先设为 `1000.0` ohm，具体阈值后续按实测调整。

### D. baseline 有效性

- `gas_baseline_ohm <= 0` 时不可计算 `gas_ratio`。
- 有效样本不足时 `confidence=low`。
- 可选后续：把 baseline 存入 NVS，但第一阶段不强制。

## 6. `gas_ratio` 计算公式

```text
gas_ratio = gas_resistance_ohm / gas_baseline_ohm
gas_ratio_clamped = clamp(gas_ratio, 0.0, 1.5)
```

其中：

- `gas_ratio` 接近 1 表示接近当前基线空气状态。
- `gas_ratio` 低表示气体电阻低，可能存在 VOC、异味、还原性气体增加或环境变化。
- `gas_ratio > 1` 可视为接近或优于当前基线，但评分上限仍限制为 100。

## 7. `gas_score` 计算公式

第一版推荐简单线性评分：

```text
gas_score = clamp(gas_ratio_clamped * 100, 0, 100)
```

说明：

- `gas_ratio=1.0` 时 `gas_score=100`。
- `gas_ratio=0.75` 时 `gas_score=75`。
- `gas_ratio=0.40` 时 `gas_score=40`。
- `gas_ratio>1.0` 时仍不超过 100。
- 可选后续非线性版本只作为未来优化，不作为第一阶段实现。

## 8. `humidity_score` 计算公式

湿度最佳区间以 40% 到 60% 为参考，中心值 50%。

第一版推荐：

```text
humidity_deviation = abs(humidity_percent - 50.0)
humidity_score = clamp(100.0 - humidity_deviation * 2.5, 0, 100)
```

示例：

- `humidity=50%`，`humidity_score=100`。
- `humidity=40%` 或 `60%`，`humidity_score=75`。
- `humidity=30%` 或 `70%`，`humidity_score=50`。
- `humidity=10%` 或 `90%`，`humidity_score=0`。

如果 `humidity_percent` 无效：

- `humidity_score = 50`
- `air_quality_confidence` 降级为 `"low"`

## 9. temperature 修正规则

第一阶段不建议复杂温度补偿，只做有效性和置信度修正。

建议：

- `temperature_c` 有效范围：`-10C` 到 `60C`。
- 超出范围时：
  - 不直接让公式崩溃。
  - `air_quality_confidence` 降级。
  - `air_quality_level` 可保持根据 gas/humidity 计算，但 hint 需要说明温度异常。
- 后续可加入温度补偿，但第一阶段不做复杂模型。

## 10. 最终 `air_quality_score` 公式

第一版推荐权重：

```text
air_quality_score_raw = gas_score * 0.75 + humidity_score * 0.25
```

最终公式：

```text
air_quality_score = round(
  clamp(
    gas_score * 0.75 + humidity_score * 0.25,
    0,
    100
  )
)
```

说明：

- `gas_resistance` 是主指标，权重 75%。
- `humidity` 是辅助补偿，权重 25%。
- 不引入 `pressure_hpa` 到第一版评分。
- `pressure_hpa` 仍上传给服务器，用于环境上下文和后续分析。

## 11. `air_quality_level` 分级

```text
if score >= 90:
  air_quality_level = "excellent"
elif score >= 75:
  air_quality_level = "good"
elif score >= 55:
  air_quality_level = "moderate"
elif score >= 30:
  air_quality_level = "poor"
else:
  air_quality_level = "bad"
```

如果输入无效或 baseline 不可用：

```text
air_quality_level = "unknown"
```

## 12. `air_quality_confidence` 规则

`confidence = "none"`：

- `gas_resistance_ohm` 无效。
- `gas_baseline_ohm` 无效。
- 无法计算 score。

`confidence = "low"`：

- `warmup_done=false`。
- `sample_count < warmup_sample_min`。
- `baseline_ready=false`。
- `humidity_percent` 无效。
- `temperature_c` 明显异常。
- 设备刚启动不久。

`confidence = "medium"`：

- `sample_count` 足够。
- baseline 已建立。
- 输入都有效。
- 但 baseline 未经过长时间学习。

`confidence = "high"`：

- 后续可选。
- 需要较长时间运行和稳定 baseline。
- 第一阶段可以不输出 high，最多到 medium。

## 13. ESP 上传字段

最终 `sensor.bme690` payload：

```json
{
  "sensor_id": "bme690_01",
  "temperature_c": 29.57,
  "humidity_percent": 30.29,
  "pressure_hpa": 986.26,
  "gas_resistance_ohm": 35164.0,
  "air_quality_score": 72,
  "air_quality_level": "moderate",
  "air_quality_confidence": "low",
  "air_quality_algo_version": "esp-bme690-relative-v1",
  "air_quality_source": "esp",
  "gas_baseline_ohm": 82000.0,
  "gas_ratio": 0.43,
  "gas_score": 43,
  "humidity_score": 87,
  "baseline_ready": false,
  "warmup_done": false,
  "sample_count": 12
}
```

字段类型：

- `air_quality_score`: integer 0-100 or null。
- `air_quality_level`: `excellent|good|moderate|poor|bad|unknown`。
- `air_quality_confidence`: `none|low|medium|high`。
- `air_quality_algo_version`: 第一版固定 `esp-bme690-relative-v1`。
- `air_quality_source`: ESP 上传时固定 `esp`。
- `gas_baseline_ohm`: number or null。
- `gas_ratio`: number or null。
- `gas_score`: integer 0-100 or null。
- `humidity_score`: integer 0-100 or null。
- `baseline_ready`: boolean。
- `warmup_done`: boolean。
- `sample_count`: integer。

## 14. 服务器校验规则

- `air_quality_score` 必须是 0-100 数字，否则置 null 或 fallback。
- `air_quality_level` 必须在枚举范围内。
- `air_quality_confidence` 必须在枚举范围内。
- `air_quality_source` 来自 ESP 时记录为 `"esp"`。
- ESP 不允许上传 `"AQI"` 字段作为可信字段。
- 如果 ESP 上传字段缺失，服务器可补算并标记 `"server_fallback"`。
- 如果 ESP 上传分数与服务器 fallback 差异极大，可记录 warning，但第一阶段不拒收。
- 服务器不应覆盖合法 ESP 分数；只在缺失或非法时 fallback。

## 15. LLM prompt 使用规则

- prompt 中应写：`ESP 本地基于 BME690 计算的相对空气状态`。
- 必须说明：`不是国标 AQI，不代表 PM2.5、PM10 或 CO2`。
- `confidence=low` 时，LLM 回答必须保守。
- 数据过期时，LLM 不得当成实时空气状态。
- 如果 score 缺失或 `level=unknown`，只说明原始 BME690 数据，不给空气质量结论。
- 如果 `air_quality_source="server_fallback"`，prompt 必须说明它不是 ESP 直接计算结果。

## 16. 第一阶段验收标准

- 已完成：ESP 上传原始 BME690 数据和空气状态字段。
- 已完成：后端 `sensor_records` 能同时保存旧映射列和新增空气状态列。
- 已完成：smoke test 覆盖 v1 BME ingest、空气状态入库、fallback/context 文案和 legacy 查询兼容。
- 已完成：LLM prompt 明确该指标不是国标 AQI，不代表 PM2.5、PM10 或 CO2。
- 后续：Dashboard 若迁移展示空气状态，必须标注 `ESP 本地 BME690 相对空气状态估算，不是国标 AQI`。
