# Home AI Agent v2.0 实施跟踪

## 目标

以 `docs/Home_AI_Agent_Repositioned_Development_Plan_v2.0.md` 为唯一产品实施依据，按阶段完成 C51、C52、S3、ESP-server 与 Web 的增量开发；每阶段完成构建、测试、warning 检查和 `git diff --check`，不执行烧录、串口监视或生产服务操作。

## 阶段

1. [completed] Phase 0：按 v2.0 逐项重新核验真实接口、资源、分区和已有能力
2. [completed] Phase 1：跨端协议、Server 数据底座、C5 语音租约和 S3 会话锁（已有实现待缺口修复后重验）
3. [completed] Phase 2：S3 三房间状态与防抖/unknown 语义（已有实现待重验）
4. [completed] Phase 3：规则引擎、用户覆盖、运行时接线和规则同步（已有实现待重验）
5. [completed] Phase 4：虚拟灯/空调/风扇执行与命令 ACK（已有实现待重验）
6. [completed] Phase 5：语音仲裁、条件播报和紧急抢占（已有实现待缺口修复后重验）
7. [completed] Phase 6：离线历史、补传、持久化和容量策略（已有实现待重验）
8. [completed] Phase 7：Server 规则控制面、部署状态和增量 Web
9. [completed] Phase 8：分层提示词、Tool Registry 与 intent/plan/action/speech（已有实现待重验）
10. [completed] Phase 9：天气、新闻与场景简报（已有实现待缺口修复后重验）
11. [completed] Phase 10：Feedback、Memory、Habit、试运行与自动回滚
12. [completed] 最终验收：全端回归、资源报告、风险与硬件验收清单
13. [completed] 完成审计修复：紧急提醒生命周期、语音会话生产状态、主动唤醒静音接线与最终门禁
14. [completed] 最终完成审计：逐项证明 v2.0 第 13/14/15/17 章及全部跨端职责已满足
15. [completed] Phase 15：完成性复核与联网恢复/TTS 短期缓存缺口修复

## Phase 15 当前复核范围

- `LINK_STABLE` 首次进入必须立即排入一次 Home AI 规则全量检查，并复用现有 command worker 的去重与资源门控。
- 动态 Home AI `prompt_text` 只允许有界进程内短期缓存；静态唤醒提示音的既有可配置缓存保持兼容。
- 为新增行为补充测试、构建、warning、资源和空白门禁；硬件/生产验收仍不在本轮执行。

## Phase 15 完成证据

- `LINK_STABLE` 首次转换已在既有 command worker 中排入 Home AI full sync；同一 pending flag 仍去重，BME replay 和本地 SoftAP ingest 链路未改变。
- SoftAP 映射的 C5 断线在资源释放成功后立即释放对应语音 owner；普通 C5 release 仍要求 session id/generation。
- 动态 `home_ai_text_*` TTS 使用 15 分钟、16 项、8 MiB 上限的进程内 LRU/TTL 缓存并返回 `Cache-Control: no-store`；静态 wake prompt 兼容路径保持原行为。原始 voice PCM 在 parser、校验拒绝、配置拒绝、并发拒绝、成功、失败、超时和断开路径清零。
- 新增联网恢复契约、语音 owner 掉线、规则资源超限、动态提示音保留策略和原始音频生命周期测试；Server、S3 host、既有雷达/BME/alarm/Web 回归以及三端 fresh build/size 均通过。
- 最终 Phase 15 资源：S3 `0x130790`，DIRAM `307399/341760`、IRAM `16384/16384`；C51/C52 均 `0x1ad2b0`，HP SRAM `188762/320928`。未执行 flash、串口 monitor、生产 Server 或生产数据库操作。

## Phase 14 完成证据

- C51、C52、S3 已按 ESP-IDF 5.5.4 在全新隔离目录构建并执行 `idf.py size`；三端日志和 size 输出均无 `warning:`、`error:`、`fatal error` 或 `FAILED`。
- Server 六组测试、99 个 JavaScript 文件 `node --check`、Home AI 15 次 host 测试执行、环境告警、雷达、虚拟设备 gateway 和 Debug parser 回归均通过。
- C51/C52 命令执行源码逐字一致；C51/C52 协议头一致，S3 共享宏与 Server-only 扩展分层明确；9 种 C51/C52 紧急 presence 组合均有测试覆盖。
- `git diff --check` 和新增实现/测试/报告文件空白检查通过；未执行 flash、串口 monitor、生产 Server 或生产数据库操作。

## 硬约束

- 唯一产品依据是用户指定的 v2.0 计划；本文件只记录执行状态。
- C5 只承担采集、雷达基础解析、唤醒、录音和播放，C51/C52 保持镜像。
- 不重写既有网络、语音、雷达、BME690、告警、命令队列、SQLite、SSE 和 Web 首页。
- S3 新模块固定容量，复用现有 scheduler/network/replay task，不新增第二套关键链路。
- 跨端协议同步修改双方；硬件成功不得由构建或 host 测试代替。
- 保留所有既有用户改动，不 reset、checkout 或恢复删除内容。

## 错误记录

| 错误 | 次数 | 处理 |
|---|---:|---|
| 一次性 runtime/orchestrator patch 因预期 include 不存在而失败 | 1 | 确认 patch 未落盘，改为小步创建和接线 |
| Phase 6 历史补丁在局部变量声明前引用变量，host 编译失败 | 1 | 调整声明顺序并新增 24h/72h/优先级/重启 host tests，全部通过 |
| Phase 6 fresh S3 构建使用的 `/Users/zhiqin/esp/esp-idf/export.sh` 不存在 | 1 | 已定位 `/Users/zhiqin/.espressif/v5.5.4/esp-idf/export.sh` 并完成 fresh build |
| Phase 7 首次 schema patch 因同一上下文包含已移动的预期行而未匹配 | 1 | 确认补丁未落盘，按实际行号拆分后成功应用 |
| Phase 7 浏览器测试用系统 Python 启动失败：缺少 `playwright` 模块 | 1 | 临时 Server 已正常关闭；改为定位 Codex 随附浏览器运行时，不重复使用系统 Python |
| Phase 7 Node Playwright 首次启动失败：默认 Chromium 缓存不存在 | 1 | 临时 Server 已正常关闭；改为显式绑定本机浏览器或 Codex 内置浏览器，不下载项目依赖 |
| Phase 7 浏览器脚本固定等待 300 ms 时过早读取保存提示 | 1 | 页面和 API 已进入实际交互；改为等待明确 notice 状态，不再使用固定时间猜测 |
| Phase 7 房间表单未发 PUT：1.5 秒数值被原生整数 step 校验拦截 | 1 | number 输入增加 `step=0.1`，与 0.5 秒 Server 下限及现有默认值一致 |
| Phase 7 移动浏览器验收直接点击收起侧栏中的 S3 链接 | 1 | 测试先走现有移动菜单按钮，再选择 S3，不改变导航实现 |
| Phase 7 浏览器 console 出现 `/favicon.ico` 404 | 1 | 首页声明空 data favicon，消除无关网络请求，不改变现有布局或功能 |
| 最终资源诊断的一次性 host `sizeof` 命令缺少雷达 include 路径 | 1 | 不重复该不完整命令；改用 fresh S3 map/size 报告提供目标架构真实资源数字 |
| 最终产物检查误以为 SPIFFS 会生成独立 `home_ai.bin` | 1 | 以 partition table 的 2 MiB 分区定义和 app fresh build 为准；未把该路径错误视为构建失败 |
| 最终 C5 镜像检查首次使用了错误的 voice-session 源码路径 | 1 | 用 `components/Middlewares/home_ai/` 真实路径重跑，C51/C52 对应文件 SHA-256 一致 |
| untracked whitespace 脚本首次使用 zsh 只读变量 `status` | 1 | 改用普通变量 `rc`；实现文件检查完成，脚本错误未冒充项目结果 |
| 全 untracked 扫描命中原始计划的 Markdown 强制换行尾空格 | 1 | 保持唯一依据只读；排除该输入文件后，本轮全部实现/测试/报告新文件检查通过 |
| 尾空格检查的临时清理命令被环境安全策略拒绝 | 1 | 未改写任何文件；改用无临时文件的只读 while/rg 扫描并通过 |
| 最终完成标记后源码复核发现 runtime 指针 `sizeof` 清零、动态 mutex、终端配置未消费、播放 ACK generation、weather 非法字段等缺口 | 1 | 作废先前最终完成状态，按 v2.0 逐项矩阵重新核验、修复并执行全部门禁 |
| 读取 voice router 头文件时误用不存在的 `include/` 子目录 | 1 | 已由 `rg` 确认头文件直接位于 `home_ai/`；后续使用真实路径，不重复错误命令 |
| 搜索 C5 语音链时误用不存在的 `Middlewares/voice` 与 `voice_chain` 目录 | 1 | 已确认真实目录是 `Middlewares/voice_domain/`，后续只用真实路径 |
| 读取规则同步时误用不存在的 `runtime/network_worker.c` 路径 | 1 | 已确认 `network_worker.c` 位于独立 `Middlewares/network_worker/`，后续使用真实目录 |
| zsh 通配符 `ESPS3/*/*.csv` 无匹配时触发 `nomatch` | 1 | 已由 `rg --files` 确认仅两个 partition CSV，后续不用未保护通配符 |
| Phase 0 三端基线构建首次未启动：`export.sh` 按系统 Python 3.9 寻找不存在的 `idf5.5_py3.9_env` | 1 | 已确认可用环境为 `idf5.5_py3.14_env`；下一次显式设置 `IDF_PYTHON_ENV_PATH` 后重跑，不把环境初始化失败记为源码构建失败 |
| Phase 0 资源 `idf.py size` 首次从顶层目录执行，找不到顶层 `CMakeLists.txt` | 1 | fresh build 产物不受影响；改为分别在 ESPS3/ESPC51 项目目录对隔离 build 目录执行 size |
| Phase 4 新 smart-home gateway host test 首次缺少间接 `radar_registry.h` include 路径 | 1 | 产品源码未参与失败；测试脚本补充现有 radar_domain/radar_ld2450 include 后重跑 |
| Phase 8 续接记录补丁使用了旧进度行作为上下文，未匹配当前文件 | 1 | 确认补丁原子失败且未落盘，改用当前 `## 2026-07-20` 段末精确上下文追加 |
| Phase 11 C5 client 大补丁上下文与现有 acquire/renew 条件不匹配 | 1 | 原子失败未落盘，拆为结构/序列化/调用点小步补丁后成功 |
| Phase 11 首次 Home AI 回归的 temporary-awake 旧断言失败 | 1 | 保留只在夜间静音退出的产品语义，修正测试前置状态并补完整会话状态机测试 |
| Phase 11 紧急协调器 host 目标缺少 radar 间接 include 路径 | 1 | 产品源码未进入链接；测试命令补齐现有 radar_domain/radar_ld2450 include 后重跑 |
| Phase 11 紧急协调器 host 链接缺少 timer/semaphore 桩 | 1 | 补齐独立 host 进程桩；不改变产品同步实现 |
| Phase 15 全量 JS 扫描首次用 zsh 空格分词把多行文件列表拼成一个 Node 路径 | 1 | 改用逐行 `while IFS= read -r` 扫描，101 个文件全部通过 |
| Phase 15 Web 技能首次调用 `python` 命令不可用，系统 Python 也无 Playwright | 1 | 按技能先运行 `python3 ... --help`，再使用本机已有 Playwright + Edge 二进制完成隔离浏览器回归 |
| Phase 15 C51/C52 镜像检查首次受 zsh 未启用字符串分词影响，将 8 个路径当作一个路径 | 1 | 改用 heredoc 逐行 `cmp`，8 个镜像文件全部通过 |
| 最终复跑再次直接执行无 executable bit 的 Home AI host 脚本，返回 exit 126 | 1 | 未修改文件权限；立即改用项目既有 `sh run_host_tests.sh` 入口，13 类/17 次全部通过 |
| 最终未跟踪文件空白扫描包含临时 `rm -f`，被命令安全策略拒绝 | 1 | 命令未执行、文件未变化；改为无临时清理的只读逐文件 `rg` 扫描并通过 |
