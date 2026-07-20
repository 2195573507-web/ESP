# `esp-who`：ESP32-S3 视觉流水线源码深读

> 本地只读快照：2026-07-10（Asia/Shanghai）。仓库：[`espressif/esp-who`](https://github.com/espressif/esp-who)，固定提交 [`2475f1456e49492d71e2e20e499377a8fd747ae4`](https://github.com/espressif/esp-who/tree/2475f1456e49492d71e2e20e499377a8fd747ae4)（2026-06-25）。只研究现有 sparse/blobless clone；未联网、未构建、未上板。所有源码链接固定到该 SHA。

## 1. 结论

- **定位**：ESP-WHO 是建立在 ESP-DL 上的图像处理/AI 应用框架，当前主线强调 camera 与模型异步运行以提高 FPS（[`README.md#L1-L16`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/README.md#L1-L16)）。代表样例选 `examples/human_face_recognition`，因为它完整连接 S3 camera、检测/识别、LCD 与按钮交互。
- **S3 支持是直接源码路径**：README 列出 ESP32-S3-EYE、ESP32-S3-Korvo-2 的 camera/LCD 能力（[`README.md#L25-L31`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/README.md#L25-L31)）；样例 target preset 是 `esp32s3`，依赖锁也固定 `target: esp32s3`（[`sdkconfig.bsp.esp32_s3_eye#L1-L23`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/examples/human_face_recognition/sdkconfig.bsp.esp32_s3_eye#L1-L23)、[`dependencies.lock#L227-L245`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/examples/human_face_recognition/dependencies.lock.esp32_s3_eye#L227-L245)）。
- **双核划分**：`FrameCapFetch` 与 `LCDDisp` 固定 core 0；`Detect` 与 `Recognition` 固定 core 1；四者 priority 都为 2。额外的 `Yield2Idle` 是最高优先级、无 affinity 的监控任务；`app_main` 自身提到 priority 5（[`app_main.cpp#L9-L36`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/examples/human_face_recognition/main/app_main.cpp#L9-L36)、[`who_recognition_app_lcd.cpp#L74-L83`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/components/who_app/who_recognition_app/who_recognition_app_lcd.cpp#L74-L83)）。
- **通信不是传统多级 queue pipeline**：S3 代表路径只有一个 Fetch node；camera frame 保存在长度 4 的自定义 ring buffer，`NEW_FRAME` EventGroup bit 唤醒 Detect/LCD。组件库支持相邻 node 间深度 1 的 FreeRTOS queue，但 S3 此路径未创建相邻 node，不能误画出不存在的 decode queue。
- **所有权依赖时序而非强约束**：consumer 从 ring 中拿裸 `cam_fb_t *` 后 mutex 已释放，没有 refcount/lease；最旧 frame 随后可能被 Fetch 归还 camera driver。配置用 6 个 camera FB、4 格 ring 和显示滞后 2 帧来留处理窗口，但这不是内存安全证明。

## 2. S3 支持证据

| 层 | 固定证据 | 含义 |
|---|---|---|
| 硬件 | ESP32-S3-EYE 文档写明 S3R8、8 MB Octal PSRAM、camera 与 LCD（[`guide#L40-L40`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/docs/en/get-started/ESP32-S3-EYE_Getting_Started_Guide.md#L40-L40)、[`#L107-L120`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/docs/en/get-started/ESP32-S3-EYE_Getting_Started_Guide.md#L107-L120)、[`#L137-L142`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/docs/en/get-started/ESP32-S3-EYE_Getting_Started_Guide.md#L137-L142)）。 | 真实 BSP 目标，不只是宏兼容。 |
| 构建 | S3 preset 开启 240 MHz、Octal 80 MHz SPIRAM、XIP、camera PSRAM DMA（[`sdkconfig#L4-L23`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/examples/human_face_recognition/sdkconfig.bsp.esp32_s3_eye#L4-L23)）。 | 视觉负载明确依赖外部 RAM 与 cache 配置。 |
| camera | `CONFIG_IDF_TARGET_ESP32S3` 选择 `WhoS3Cam`（[`who_cam.hpp#L1-L7`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/components/who_peripherals/who_cam/who_cam.hpp#L1-L7)），CMake 仅在 S3 编译该目录（[`who_cam/CMakeLists.txt#L14-L20`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/components/who_peripherals/who_cam/CMakeLists.txt#L14-L20)）。 | 编译期专用实现。 |
| 依赖 | lock 固定 ESP-IDF 5.5.2、esp32-camera 2.1.4、S3-EYE BSP 5.0.2、ESP-DL 3.2.4（[`dependencies.lock#L36-L61`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/examples/human_face_recognition/dependencies.lock.esp32_s3_eye#L36-L61)、[`#L72-L117`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/examples/human_face_recognition/dependencies.lock.esp32_s3_eye#L72-L117)、[`#L227-L245`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/examples/human_face_recognition/dependencies.lock.esp32_s3_eye#L227-L245)）。 | 可复现依赖版本；依赖源码不在本次 sparse checkout。 |

## 3. Camera → Inference → Display

```text
core 0, prio 2                    core 1, prio 2
┌─────────────────┐ NEW_FRAME bit ┌────────────────┐
│ FrameCapFetch   │───────────────>│ Detect         │── model->run(frame)
│ esp_camera_get  │                └──────┬─────────┘
│ ring[4] / pool6 │                       │ callback (same Detect task)
└────────┬────────┘                ┌──────▼─────────┐
         │ NEW_FRAME bit           │ Recognition   │ mode coordinator
         ▼                         │ recognize/enroll callback swap
┌─────────────────┐                └────────────────┘
│ LCDDisp         │<── timestamped detect-result std::queue
│ ring index 1    │── LVGL canvas + overlay + label
└─────────────────┘

Yield2Idle: max-priority, no affinity; monitors both cores and briefly pauses a
starving core's ESP-WHO tasks so the FreeRTOS idle task gets execution time.
```

1. S3 `app_main` 选择 DVP pipeline，构造 LCD recognition app 并 `run()`（[`app_main.cpp#L27-L36`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/examples/human_face_recognition/main/app_main.cpp#L27-L36)）。
2. pipeline 以 `MODEL_TIME=3` 创建 `MODEL_TIME+3=6` 个 RGB565 camera FB；唯一 node 是 `WhoFetchNode`，其 ring 长度由 `fb_count-2` 得到 4（[`frame_cap_pipeline.cpp#L7-L29`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/examples/human_face_recognition/main/frame_cap_pipeline.cpp#L7-L29)、[`who_frame_cap_node.hpp#L44-L59`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/components/who_frame_cap/who_frame_cap_node.hpp#L44-L59)）。
3. Fetch 循环直接 `esp_camera_fb_get()`；ring 满时先 pop/return 最旧 FB，再 push 新 FB，然后给活跃订阅者置 `NEW_FRAME` bit（[`who_frame_cap_node.cpp#L99-L144`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/components/who_frame_cap/who_frame_cap_node.cpp#L99-L144)、[`#L162-L174`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/components/who_frame_cap/who_frame_cap_node.cpp#L162-L174)）。
4. Detect 等待可合并的 EventGroup bit，peek ring 最新帧并同步执行 `m_model->run(img)`；回调也在 Detect task 内执行（[`who_detect.cpp#L59-L95`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/components/who_detect/who_detect.cpp#L59-L95)）。所以慢模型不会积压“每帧消息”，而是自然跳到下一次可见的最新 frame。
5. Recognition task 本身不持续做人脸特征推理。按钮给它置 `RECOGNIZE/ENROLL/DELETE` bit；前两者临时替换 Detect result callback，下一次 Detect 回调里同步执行 recognize/enroll，然后恢复普通 callback（[`who_recognition.cpp#L46-L108`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/components/who_recognition/who_recognition.cpp#L46-L108)、[`who_recognition_button.cpp#L7-L13`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/components/who_app/who_recognition_app/who_recognition_button.cpp#L7-L13)）。
6. LCDDisp peek ring index 1；Detect peek `-1`（最新）。对 4 格 ring，这让显示落后检测帧 2 帧，源码明确要求 ring 覆盖模型耗时以让框与画面同步（[`frame_cap_pipeline.cpp#L15-L20`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/examples/human_face_recognition/main/frame_cap_pipeline.cpp#L15-L20)、[`who_recognition_app_lcd.cpp#L9-L14`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/components/who_app/who_recognition_app/who_recognition_app_lcd.cpp#L9-L14)）。

## 4. Queue、ring buffer 与背压

- **node queue**：通用 `WhoFrameCap::add_node()` 在相邻 node 之间创建深度 1、元素为 `cam_fb_t *` 的 FreeRTOS queue（[`who_frame_cap.hpp#L16-L31`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/components/who_frame_cap/who_frame_cap.hpp#L16-L31)）。node 默认 `xQueueOverwrite`，即 latest-wins；也可选 `xQueueSend(..., portMAX_DELAY)` 形成强背压（[`who_frame_cap_node.cpp#L119-L140`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/components/who_frame_cap/who_frame_cap_node.cpp#L119-L140)）。S3 本例只有 Fetch node，因此这里没有实际 inter-node queue。
- **frame ring**：自定义 `RingBuf<T>` 只管理指针槽，不管理对象生命周期；越界/空/满仅 log、仍继续访问或修改（[`who_ringbuf.hpp#L13-L43`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/components/who_frame_cap/who_ringbuf.hpp#L13-L43)）。当前 caller 在 push 前显式 pop，正常路径避免超容，但该容器本身不是防御式 API。
- **event bit**：`NEW_FRAME`、按钮命令、pause/stop 都用 EventGroup bits。相同 bit 多次 set 会合并，不保留计数；Detect 落后时主动丢帧。若多个 recognition 命令同时置位，wait 会一次性 clear，而代码优先处理 `RECOGNIZE` 后 `continue`，其余同批命令会丢失，这是控制面 coalescing（[`who_recognition.cpp#L46-L81`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/components/who_recognition/who_recognition.cpp#L46-L81)）。
- **result queue**：Detect result 被复制进受 mutex 保护的 `std::queue`；LCD 按 timestamp 丢弃/吸收不晚于当前显示帧的结果，从而选最近 overlay（[`who_detect_result_handle.cpp#L143-L183`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/components/who_app/who_app_common/who_detect_result_handle/who_detect_result_handle.cpp#L143-L183)）。该 queue **没有容量上限**；若 LCD 长时间停滞而 Detect 继续，内存可持续增长。

## 5. Core affinity 与调度保护

- 所有 `WhoTask` 必须显式指定 core；基类用 `xTaskCreatePinnedToCore`，task 创建失败只 log 并返回 `false`（[`who_task.cpp#L10-L26`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/components/who_task/who_task.cpp#L10-L26)、[`#L142-L153`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/components/who_task/who_task.cpp#L142-L153)）。
- `Yield2Idle` 以 `configMAX_PRIORITIES-1`、`tskNO_AFFINITY` 运行，给 core 0/1 注册 idle hook；若一个观测周期某 core 从未进入 idle，就选中该 core 上的 ESP-WHO tasks，pause、cleanup，延时 10 ms，再 resume（[`who_yield2idle.cpp#L85-L98`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/components/who_task/who_yield2idle.cpp#L85-L98)、[`#L118-L166`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/components/who_task/who_yield2idle.cpp#L118-L166)）。这既给 idle/WDT 喂执行机会，也会清空 frame/result 状态；不是无损调度。
- 监控周期由 `ESP_TASK_WDT_TIMEOUT` 与 `MAX_TASK_LOOP_TIME` 推导，配置说明要求后者覆盖最慢 task 单轮耗时（[`who_task/Kconfig#L1-L6`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/components/who_task/Kconfig#L1-L6)、[`who_yield2idle.cpp#L118-L123`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/components/who_task/who_yield2idle.cpp#L118-L123)）。模型变慢但未同步调大配置，会触发额外 pause/reset。

## 6. PSRAM 与 frame 所有权

- S3 preset 开启 `CONFIG_CAMERA_PSRAM_DMA`，`WhoS3Cam` 又调用 `esp_camera_set_psram_mode(true)`，camera 以 `CAMERA_GRAB_LATEST`、6 FB 初始化（[`who_s3_cam.cpp#L10-L29`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/components/who_peripherals/who_cam/who_s3_cam/who_s3_cam.cpp#L10-L29)）。实际 camera buffer allocator 在锁定的外部 `esp32-camera` 组件，不在本地源码；这里只能证明配置意图，不能证明每块 buffer 的 heap capability。
- `cam_fb_t` 只是 wrapper：`buf` 指向 camera buffer，`ret` 保存原 `camera_fb_t *`（[`who_cam_define.hpp#L80-L100`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/components/who_peripherals/who_cam/who_cam_define.hpp#L80-L100)）。Fetch ring 是 camera FB 的临时 owner；淘汰/cleanup 时调用 `esp_camera_fb_return()`（[`who_s3_cam.cpp#L36-L47`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/components/who_peripherals/who_cam/who_s3_cam/who_s3_cam.cpp#L36-L47)、[`who_frame_cap_node.cpp#L152-L174`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/components/who_frame_cap/who_frame_cap_node.cpp#L152-L174)）。
- `cam_fb_peek()` 只在取指针时持 ring mutex，返回后不 pin frame（[`who_frame_cap_node.cpp#L53-L75`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/components/who_frame_cap/who_frame_cap_node.cpp#L53-L75)）。**静态风险**：若 Detect/LCD 使用时间超过该 frame 留在 ring 的窗口，Fetch 可能先 return buffer，camera DMA 再复用它，形成画面撕裂或 use-after-return。ring 长度是概率/时序裕量，不是 ownership protocol。
- LVGL path 的 draw buffer 明确在 internal RAM（`MALLOC_CAP_INTERNAL`、`buff_spiram=false`），canvas 则临时引用 camera `fb->buf`（[`who_lvgl_lcd.hpp#L9-L13`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/components/who_peripherals/who_lcd/who_lvgl_lcd.hpp#L9-L13)、[`who_lvgl_lcd.cpp#L24-L46`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/components/who_peripherals/who_lcd/who_lvgl_lcd.cpp#L24-L46)、[`who_frame_lcd_disp.cpp#L58-L70`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/components/who_frame_lcd_disp/who_frame_lcd_disp.cpp#L58-L70)）。无图形库路径则因 S3 不支持 PSRAM DMA display，显式拷贝到 internal DMA buffer（[`who_lcd.cpp#L16-L41`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/components/who_peripherals/who_lcd/who_lcd.cpp#L16-L41)）。

## 7. 启动、停止与错误恢复

**正常机制**

- 文件系统、SD、LED、camera、LCD/button 初始化大量使用 `ESP_ERROR_CHECK`，失败即 abort/reboot 路径，而非局部降级（[`app_main.cpp#L11-L25`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/examples/human_face_recognition/main/app_main.cpp#L11-L25)、[`who_s3_cam.cpp#L17-L28`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/components/who_peripherals/who_cam/who_s3_cam/who_s3_cam.cpp#L17-L28)、[`who_recognition_button.cpp#L20-L47`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/components/who_app/who_recognition_app/who_recognition_button.cpp#L20-L47)）。
- stop/pause 先给所有 task 置 bit，再逐一无限等待 acknowledged state，最后统一 cleanup；frame node 若阻塞在输入 queue，会尝试塞入 null sentinel 唤醒（[`who_task.cpp#L203-L247`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/components/who_task/who_task.cpp#L203-L247)、[`who_frame_cap_node.cpp#L29-L50`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/components/who_frame_cap/who_frame_cap_node.cpp#L29-L50)）。Fetch cleanup 将 ring 中所有 FB 归还 camera driver（[`who_frame_cap_node.cpp#L147-L160`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/components/who_frame_cap/who_frame_cap_node.cpp#L147-L160)）。

**静态缺口（未故障注入）**

1. `WhoS3Cam::cam_fb_get()` 没检查 `esp_camera_fb_get()` 是否返回 null，立即解引用（[`who_s3_cam.cpp#L36-L42`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/components/who_peripherals/who_cam/who_s3_cam/who_s3_cam.cpp#L36-L42)）；Detect/LCD 也不检查 `cam_fb_peek()` 的 null 返回（[`who_detect.cpp#L78-L81`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/components/who_detect/who_detect.cpp#L78-L81)、[`who_frame_lcd_disp.cpp#L58-L66`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/components/who_frame_lcd_disp/who_frame_lcd_disp.cpp#L58-L66)）。camera 暂时取帧失败可直接崩溃。
2. 四个 worker 的 `run()` 用 `&=` 聚合失败，但样例忽略最终 bool（[`who_recognition_app_lcd.cpp#L74-L83`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/components/who_app/who_recognition_app/who_recognition_app_lcd.cpp#L74-L83)、[`app_main.cpp#L33-L36`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/examples/human_face_recognition/main/app_main.cpp#L33-L36)）。部分 task 创建失败时可能留下半启动系统，没有 rollback/retry。
3. `WhoTaskGroup::stop/pause()` 使用 `portMAX_DELAY`。Fetch 是首 node、没有 input queue；若底层 `esp_camera_fb_get()` 永久卡住，null sentinel 分支无法唤醒它，整个 stop/pause/Yield2Idle 都可能无限等待。这是基于控制流的静态推断。
4. 样例 `new WhoRecognitionAppLCD` 后不保存/删除，按设备生命周期永久运行。若外部代码运行期 delete，derived destructor 会先删除 result/button helper，而 `WhoApp` base destructor 才停止 worker（[`who_recognition_app_lcd.cpp#L64-L72`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/components/who_app/who_recognition_app/who_recognition_app_lcd.cpp#L64-L72)、[`who_app.cpp#L6-L10`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/components/who_app/who_app_common/who_app.cpp#L6-L10)）。**静态推断：callbacks 可能在 helper 释放后仍被 worker 调用；安全销毁应先 stop，再拆 callback 对象。**
5. S3 LVGL `WhoLCD::deinit()` 仍是 TODO（[`who_lvgl_lcd.cpp#L49-L52`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/components/who_peripherals/who_lcd/who_lvgl_lcd.cpp#L49-L52)），因此 stop/destroy 后的 display 资源回收与同进程 restart 没闭环。

## 8. 可迁移到 ESP-111 的原则

- 双核按“采集/显示”和“推理/决策”分区是清晰基线，但 core affinity、priority、frame budget 应由实测延迟驱动；不要把本例的 `0/1, prio 2` 当通用常量。
- 实时视频适合 latest-wins：深度 1 overwrite queue + coalesced event 可限制 backlog；控制命令与持久状态则不能复用同样的丢失语义。
- frame ring 必须补显式所有权：lease/refcount、generation token，或 consumer 完成 ack 后再归还 camera。仅靠“ring 足够长”无法证明 DMA buffer 未被复用。
- result queue 需要 hard cap、drop metric 与“按 timestamp 取最近值”；stop/pause 要有 deadline、失败状态和强制恢复，不能无限等待。
- `Yield2Idle` 的好处是发现 core starvation；其 cleanup/restart 是有损的。ESP-111 更适合先做运行时预算/看门狗 telemetry，再把有损恢复作为明确状态转换。

## 9. 许可证与核验边界

- 根许可证名为 **ESPRESSIF MIT License**，但授权明确限定“use on all ESPRESSIF SYSTEMS products”（[`LICENSE#L1-L13`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/LICENSE#L1-L13)）。它不是无产品范围限制的标准 MIT 文本；移植到非 Espressif 产品前需单独法务确认。
- 样例锁定 ESP-DL、esp32-camera、LVGL、BSP、face model 等 managed components（[`dependencies.lock#L238-L245`](https://github.com/espressif/esp-who/blob/2475f1456e49492d71e2e20e499377a8fd747ae4/examples/human_face_recognition/dependencies.lock.esp32_s3_eye#L238-L245)）。这些依赖各自许可证不在本报告范围，分发固件/源码时必须生成逐组件清单，不能只引用根许可。
- 本地 clone 是 shallow + sparse + blobless；只检出 `components/docs/examples`，`.gitlab/ci/*.yml` 虽在 Git tree 中但 blob 不在本地，因此无法在不联网条件下核验 S3 CI matrix。`dependencies.lock` 证明版本/target，不证明该 SHA 的 CI 已通过。
- 未执行 build、烧录、camera/LCD 实测、FPS/heap/PSRAM 测量、慢模型/断 camera/queue 饱和/stop 故障注入。本文严格区分源码事实与标注为“静态推断”的风险。
