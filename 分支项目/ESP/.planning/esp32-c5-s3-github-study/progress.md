# ESP32-C5 / ESP32-S3 GitHub 学习进度

## 会话：2026-07-10 三小时长线学习

### 阶段 1：研究设计与项目发现
- **状态：** complete；当前进入阶段 4（阶段 2/3 已完成研究与专题成稿）
- **开始时间：** 2026-07-10 12:52:58 +0800
- **目标收口时间：** 2026-07-10 15:52:58 +0800
- 执行的操作：
  - 确认活动 goal 与用户原文一致，承接现有目标。
  - 完整读取 `using-superpowers`、`planning-with-files-zh` 和 `brainstorming`。
  - 恢复根规划上下文，确认旧任务混杂且工作区大量既有改动。
  - 读取项目硬规则、Git 状态、近期提交和现有文档目录。
  - 建立独立作用域计划、证据等级、筛选维度和六阶段研究路线。
- 创建/修改的文件：
  - `.planning/.active_plan`
  - `.planning/esp32-c5-s3-github-study/task_plan.md`
  - `.planning/esp32-c5-s3-github-study/findings.md`
  - `.planning/esp32-c5-s3-github-study/progress.md`
  - `docs/esp32-c5-s3-open-source-study/`（交付目录，尚未成稿）

## 批次时间记录

| 批次 | 时间区间 | 主题 | 产出 |
|------|----------|------|------|
| 0 | 12:52-13:00 | 上下文恢复与研究设计 | 作用域计划和证据标准 |
| 1A | 13:00-13:03 | GitHub API 项目发现 | 30 个初始候选、官方锚点与验真门槛 |
| 1B | 13:03-13:06 | `esp-csi` 固定提交深读 | C5/S3 配置差异、callback/queue 正反例、采样功耗取舍 |
| 1C | 13:06-13:09 | `esp-idf` FreeRTOS/事件/WDT 基线 | 单核/双核差异、默认 loop 拓扑、post 所有权、WDT user |
| 1D | 13:09-13:13 | C5 OpenThread/BLE/低功耗路径 | 支持分级、PM lock 生命周期、测试覆盖缺口 |
| 1E | 13:13-13:19 | `esp-matter` / `esp-iot-bridge` 深读与根复核 | 协议任务拓扑、持久化、控制面事件与错误路径 |
| 1F | 13:18-13:19 | 外部模式回映 ESP-111 live source | 8 项 direct/experiment/do-not-copy 对照 |
| 2A | 13:19-13:22 | `esp-adf` audio pipeline 深读 | element task、ringbuffer、event queue、abort/stop、core/PSRAM |
| 2B | 13:22-13:39 | SmartKnob 与 `esp-brookesia` 调度深读 | depth-1 overwrite、双核 ownership、Asio worker/strand、ServiceManager lifecycle、profiler |
| 2C | 13:39-13:42 | `esp-sr` 报告复核与 `esp-thread-br` C5 深读 | AFE feed/fetch、borrowed result、外置 RCP、OpenThread lock handoff、RCP 恢复 |
| 2D | 13:42-13:45 | `esp-who` / `xiaozhi-esp32` 报告复核与合并 | latest-frame 视觉流、有损 idle 恢复、S3 语音任务、状态机、有界音频队列与重连/OTA 缺口 |
| 2E | 13:45-13:51 | `esp-hal` / `esp-zigbee-sdk` 深读收口 | Rust executor/radio RTOS、bounded pubsub、C5 Zigbee owner task、commissioning、NVS/deep-sleep |
| 3A | 13:51-13:52 | 手册入口、项目索引、C5/S3 专题初稿 | 30 项阅读地图、13 个固定快照、芯片专题与实验清单 |

## 验证结果

| 检查 | 预期结果 | 实际结果 | 状态 |
|------|---------|---------|------|
| 目标状态 | 与用户原文一致且 active | 已确认 | 通过 |
| 业务代码边界 | 不编辑现有业务代码 | 尚未编辑 | 通过 |
| 规划隔离 | 不覆盖根旧计划 | 使用独立 `.planning` 目录 | 通过 |
| 项目广度 | 候选不少于 24 | 30 个候选 | 通过 |
| 固定提交深读 | 不少于 12 | 13 个项目 | 通过 |

## 错误日志

| 时间戳 | 错误 | 尝试次数 | 解决方案 |
|--------|------|---------|---------|
| 12:53 | `create_goal` 因已有活动目标失败 | 1 | 读取并承接同一目标 |
| 12:54 | 并行长输出被截断 | 1 | 分段读取并降低单次输出量 |
| 13:00 | Web 搜索接口无法解码四查询返回流 | 1 | 改走官方仓库直达、GitHub API 和 shallow clone |
| 13:01 | Web 官方仓库直开及单查询仍为同一解码错误 | 3 | 达到三次失败阈值，停用 Web 工具；GitHub API 可用 |
| 13:12 | 网络沙箱阻止 ESP-IDF sparse checkout 补拉缺失 blob | 1 | 不请求扩大权限，继续使用已落盘的一手源码并降低结论范围 |
| 13:16 | 新浅克隆的 sparse 子树首次展开均被 DNS 沙箱拒绝 | 1 | 获批只读 `git sparse-checkout` 后批量恢复 |
| 13:17 | BLE 补记补丁误用 findings 作为 task-plan 锚点 | 1 | 验证无部分写入后拆成三个定向补丁 |
| 13:44 | 批次 2D 首次日志误写了未来的预计结束时间 | 1 | 立即读取系统时钟并改为真实批次结束时间；后续结束后再落盘 |

## 五问重启检查

| 问题 | 答案 |
|------|------|
| 我在哪里？ | 阶段 1：建立候选项目池和深读清单 |
| 我要去哪里？ | C5、S3、调度横向、ESP-111 映射、验证收口 |
| 目标是什么？ | 三小时 GitHub 开源项目长线学习并形成中文手册 |
| 我学到了什么？ | 见 `findings.md` |
| 我做了什么？ | 见本文件批次记录 |
