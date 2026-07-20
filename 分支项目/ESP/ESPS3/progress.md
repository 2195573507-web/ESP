# ESPS3 CSI 上传优化进度

## 2026-07-10

- 已读取强制流程技能和活动 `/goal`。
- 已确认工作区为 dirty，且其他模块有用户改动，后续严格限定 ESPS3。
- 已开始核对 CSI 融合、server payload 和 network worker 队列链路。
- 已确认上云调用链为 `csi_placeholder_gateway -> sensor_aggregator -> protocol_adapter -> network_worker -> server_client`。
- 已确认完整 link 特征与 canonical fused fact 分层存在，精简 payload 不需要删除内部指标。
- 已定位旧上云 JSON 与目标字段白名单的全部差异，下一步核对 Server 接收合同和队列失败语义。
- 已只读确认当前 Server 严格要求旧 canonical v2，新 payload 部署前需要 Server 兼容，但本轮不修改 Server。
- 已选定单槽 latest 方案：CSI 不再占用普通 upload queue，由现有 upload worker 按 cadence 发送最新快照。
- 已写入并自检本次 ESPS3 设计文档，开始实现。
- 实现范围已收敛为 `protocol_adapter.c` 与 `network_worker.c` 两个运行文件。
- 已完成融合 payload 字段白名单和物理 links 序列化。
- 已将 CSI 从共享 upload queue 移到受锁保护的 latest 单槽，由现有 upload worker 直接发送。
- 已把 CSI HTTP 改为单次请求，普通 Server JSON 的退避重试保持不变。
- `ninja -C ESPS3/build __idf_Middlewares` 通过，三个修改 C 文件均重新编译并成功链接组件静态库。
- ESP-IDF 5.5.4 完整 `idf.py -C ESPS3 build` 通过，生成 `sensair_s3_gateway.bin`；最终大小 `0x1073b0`，最小应用分区约 85% 空闲。
- 共享 upload queue 入队边界已显式拒绝 CSI work item，防止后续调用误把 CSI 放回普通队列。
- 已区分 CSI 本地 HTTP slot busy 与真实 Server 失败；slot busy 丢当前 generation 但不影响 Server health。
- 已让 Server probe 忽略本地 HTTP slot busy，避免语音期间误关整个上云 gate。
- 最终字段白名单、禁止字段、latest-only queue guard、内部调试保留和 `git diff --check -- ESPS3` 检查均通过。
- 两轮并行代码审查均未发现阻断问题；当前 ESP-server 仍需后续适配新 payload，本轮未修改。
- 收尾 `git diff --check -- ESPS3` 通过；规划检查脚本不识别中文编号格式，已人工确认 5 个阶段全部完成。

## 2026-07-10 `/kernel/csi_event` CanonicalEvent v2 兼容修复

- 已确认当前工作树包含大量既有修改，本任务只在 ESPS3 范围内增量工作。
- 已定位生成链：`sensor_aggregator_handle_csi_fact -> protocol_adapter_build_csi_event_v2_json -> network_worker_submit_server_json -> validate_and_log_csi_final_payload -> server_client_post_csi_event_json`。
- 已定位错误根因：ESPS3 当前 6 键业务 body 与 Server 严格 7 键 CanonicalEvent v2 不同，第一层 exact-key 检查即返回 `INVALID_CANONICAL_CSI_EVENT`。
- 已核对 Server 路由直接把 `req.body` 交给 validator，并从可信 gateway header/binding 单独取得 `gateway_id`；不存在 body wrapper 解包层。
- 当前仅完成只读核对和规划记录，尚未修改业务代码。
- 已确认 fusion fact 本身具备 Server 要求的全部 7 字段数据，修改无需进入 CSI 算法或接收链。
- 已确认 `gateway_id` 可由现有 authenticated headers 保留；fusion 内部数值 `schema_version=2` 可继续保留，最终 body 按 Server 要求序列化为 `"v2"`。
- 已确认 latest-only metadata reader 兼容目标字符串 `fused_state`，无需改队列/上传节流语义。
- 已核对 Git 历史：旧 ESPS3 7 字段 builder 与 live Server validator 一致，Server 严格合同自引入后未漂移。
- 阶段 1 已完成；阶段 2 等待确认将 `gateway_id` 保留在 header、内部数值版本保留为 2、最终 body 序列化为 `"v2"`。
- 用户已确认推荐方案；阶段 2 完成，阶段 3 开始，只修改 ESPS3 serializer、final validator 和 schema 字段日志。
- 已修改 `protocol_adapter.c`：恢复 strict 7-key CanonicalEvent v2 body，active links 连续映射为 `link_N`。
- 已修改 `network_worker.c`：final validator 与 Server schema 对齐，并新增 `CSI_SERVER_FINAL_SCHEMA` 字段/类型/值摘要，沿用 2 秒日志节流。
- 定向 `git diff --check` 已通过；latest-only、HTTP client、fusion、C5 和 Server 代码均未改动。
- 独立审查确认 canonical schema、单链映射和 header 身份处理无阻断问题；按审查建议将 `CSI_SERVER_FINAL_PAYLOAD_LINKS` 与 schema/JSON 日志统一到 2 秒节流窗口。
- `ninja -C build __idf_Middlewares` 通过，`protocol_adapter.c` 与 `network_worker.c` 均重新编译并链接。
- 最终 `idf.py build` 通过，生成 `build/sensair_s3_gateway.bin`，大小 `0x1077b0`，最小 app 分区剩余 85%。
- 最终 `git diff --check -- ESPS3` 通过。
- C51/C52/ESP-server 修改前后 diff 指纹完全一致，确认本任务未修改禁止目录。
- 阶段 3、4、5 全部完成；未执行 flash 或硬件/在线 Server 闭环验证。
- planning 完成性脚本仍因中文编号格式显示 `10/0`；已人工核对两组计划共 10 个阶段全部为 complete。
