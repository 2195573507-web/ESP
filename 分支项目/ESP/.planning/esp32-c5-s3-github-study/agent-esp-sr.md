# esp-sr：ESP32-S3 AFE feed/fetch、调度与资源所有权证据

## 1. 取证边界

- 仓库：`https://github.com/espressif/esp-sr`
- 本地只读浅克隆：`/tmp/esp32-c5-s3-study-root/esp-sr`
- 固定提交：`7ff63a7da40e15e502681be48c4d0e78475544a3`
- 提交主题：`Merge branch 'fix/heap_check' into 'master'`；时间：`2026-05-25T19:03:05+08:00`
- 取证前后工作树均干净；没有构建、没有硬件运行、没有业务代码修改。clone 同时是 sparse + partial clone，只检出 `docs/include/src/test_apps`；`lib/model/esp-tts` 只可从 Git tree 确认路径，无法读取二进制/模型内容。一次对象大小查询触发 promisor fetch，但 DNS 受限而失败，没有取得外部数据。
- 当前 AFE 真正链接的是 S3 预编译 `lib/esp32s3/libesp_audio_front_end.a`；公开 C 源 `src/esp_afe_sr_1mic.ref` 不在组件 `SRCS`，只能作为历史/参考设计，不能用来断言当前二进制内部细节：[CMakeLists.txt L20-L31](https://github.com/espressif/esp-sr/blob/7ff63a7da40e15e502681be48c4d0e78475544a3/CMakeLists.txt#L20-L31)、[L47-L61](https://github.com/espressif/esp-sr/blob/7ff63a7da40e15e502681be48c4d0e78475544a3/CMakeLists.txt#L47-L61)。

## 2. 许可证与商用 caveat

- 根许可证自称 **ESPRESSIF MIT License**，但许可仅授予“在所有 ESPRESSIF SYSTEMS 产品上使用”；这不是无平台限制的标准 MIT 文本：[LICENSE L1-L13](https://github.com/espressif/esp-sr/blob/7ff63a7da40e15e502681be48c4d0e78475544a3/LICENSE#L1-L13)。在 ESP32-S3 上属于字面允许范围，移植到非 Espressif 产品需另行确认。
- 文件级许可并不统一：S3 的若干公开算法头是 Apache-2.0，例如 [include/esp32s3/esp_aec.h L1-L13](https://github.com/espressif/esp-sr/blob/7ff63a7da40e15e502681be48c4d0e78475544a3/include/esp32s3/esp_aec.h#L1-L13)；测试代码则声明 Public Domain 或 CC0 二选一：[test_afe.cpp L1-L8](https://github.com/espressif/esp-sr/blob/7ff63a7da40e15e502681be48c4d0e78475544a3/test_apps/esp-sr/main/test_afe.cpp#L1-L8)。分发时应按文件/二进制资产核查，不能笼统标一个 SPDX。
- README 明确提示示例唤醒词可能涉及第三方名称、标识与品牌，商用前需拥有权利或授权：[README.md L106-L110](https://github.com/espressif/esp-sr/blob/7ff63a7da40e15e502681be48c4d0e78475544a3/README.md#L106-L110)。代码许可不等于唤醒词商标/语料权利。

## 3. S3 支持：可以确认到什么程度

### 3.1 强证据

- README 将 ESP32-S3 明列为 AFE 与 MultiNet 目标；S3 可用 `mn5q8/mn6/mn7` 中英文模型：[README.md L112-L133](https://github.com/espressif/esp-sr/blob/7ff63a7da40e15e502681be48c4d0e78475544a3/README.md#L112-L133)。WakeNet 支持面更广，不能据其总表反推 C5 也支持完整 AFE。
- 构建系统为 `esp32s3` 选择 `include/esp32s3` 和 `lib/esp32s3`，链接 AFE、WakeNet、VADNet、NSNet、MultiNet 等预编译库：[CMakeLists.txt L1-L24](https://github.com/espressif/esp-sr/blob/7ff63a7da40e15e502681be48c4d0e78475544a3/CMakeLists.txt#L1-L24)、[L47-L82](https://github.com/espressif/esp-sr/blob/7ff63a7da40e15e502681be48c4d0e78475544a3/CMakeLists.txt#L47-L82)。
- `test_apps/esp-sr` manifest 启用 S3；CI 在 IDF 5.4、5.5.2、5.5、6.0 构建它，并为 IDF 5.4/5.5 配置 `TEST_TARGET/TEST_ENV=esp32s3` 的 target-test job：[test_apps/.build-rules.yml L1-L4](https://github.com/espressif/esp-sr/blob/7ff63a7da40e15e502681be48c4d0e78475544a3/test_apps/.build-rules.yml#L1-L4)、[.gitlab-ci.yml L140-L147](https://github.com/espressif/esp-sr/blob/7ff63a7da40e15e502681be48c4d0e78475544a3/.gitlab-ci.yml#L140-L147)、[L200-L217](https://github.com/espressif/esp-sr/blob/7ff63a7da40e15e502681be48c4d0e78475544a3/.gitlab-ci.yml#L200-L217)。本次未读取在线 CI 结果，所以这是“存在并持续配置的验证路径”，不是该 SHA 的绿色状态证明。
- pytest 将 AFE 组显式绑定 S3，并给 3600 秒超时：[pytest_esp_sr.py L58-L68](https://github.com/espressif/esp-sr/blob/7ff63a7da40e15e502681be48c4d0e78475544a3/test_apps/esp-sr/pytest_esp_sr.py#L58-L68)。AFE CI 配置固定 S3、启用 16 MB flash、Octal PSRAM、240 MHz、64 KB data cache/64 B line：[sdkconfig.ci.afe L1-L26](https://github.com/espressif/esp-sr/blob/7ff63a7da40e15e502681be48c4d0e78475544a3/test_apps/esp-sr/sdkconfig.ci.afe#L1-L26)。
- 运行时只告警而不强制 S3 推荐配置：CPU 240 MHz、flash/PSRAM 至少 80 MHz、64 KB data cache 与 64 B cache line：[esp_process_sdkconfig.c L11-L32](https://github.com/espressif/esp-sr/blob/7ff63a7da40e15e502681be48c4d0e78475544a3/src/esp_process_sdkconfig.c#L11-L32)。

### 3.2 结论边界

本地证据足以确认 S3 是 AFE 的一等支持目标，并有 build + board-test 配置；不足以验证当前预编译库的内部算法、线程安全、实际延迟、声学效果或指定板卡的 I2S/麦克风链路。文档中的 S3 性能表也是项目给出的历史 benchmark，不是本次测量。

## 4. AFE 数据面与任务分工

```text
I2S/file producer task
  -> get_feed_chunksize x get_feed_channel_num
  -> feed(interleaved int16, 16 kHz)
  -> [opaque AFE pipeline / internal buffering]
  -> fetch or fetch_with_delay
  -> borrowed result: audio + VAD + WakeNet + raw channels + pressure metric
  -> recognizer/application consumer
```

- `feed()` 要求 channel-interleaved、signed 16-bit、16 kHz，帧长必须运行时查询；返回输入 size：[esp_afe_sr_iface.h L62-L100](https://github.com/espressif/esp-sr/blob/7ff63a7da40e15e502681be48c4d0e78475544a3/include/esp32s3/esp_afe_sr_iface.h#L62-L100)。调用方不能硬编码 samples/frame，也不能把字节数当 sample 数。
- `fetch()` 无论输入多少声道都输出目标单声道，默认超时 2000 ms；`fetch_with_delay()` 允许以 ticks 指定等待：[esp_afe_sr_iface.h L102-L124](https://github.com/espressif/esp-sr/blob/7ff63a7da40e15e502681be48c4d0e78475544a3/include/esp32s3/esp_afe_sr_iface.h#L102-L124)。停止路径应优先使用有限等待，避免 consumer 永久卡住。
- fetch result 同时携带 processed audio、VAD cache/state、WakeNet state/index、raw multi-channel data、`ret_value` 和 `ringbuff_free_pct`：[esp_afe_sr_iface.h L27-L51](https://github.com/espressif/esp-sr/blob/7ff63a7da40e15e502681be48c4d0e78475544a3/include/esp32s3/esp_afe_sr_iface.h#L27-L51)。接口没有 result/free API，因此结果及内部 data 指针应按 library-borrowed view 对待；其跨 fetch 的有效期未在头文件写明，产品代码应立即复制需要异步持有的数据。
- API 明确提供 `reset_buffer()`，配置还暴露 `afe_ringbuf_size`，说明当前契约确有内部 ringbuffer；但 ringbuffer 数量、写满策略和同步原语藏在预编译库中，不能由本快照证明：[esp_afe_sr_iface.h L126-L132](https://github.com/espressif/esp-sr/blob/7ff63a7da40e15e502681be48c4d0e78475544a3/include/esp32s3/esp_afe_sr_iface.h#L126-L132)、[esp_afe_config.h L135-L142](https://github.com/espressif/esp-sr/blob/7ff63a7da40e15e502681be48c4d0e78475544a3/include/esp32s3/esp_afe_config.h#L135-L142)。
- `ringbuff_free_pct` 的字段名/注释称“free percent”，却又称 `>0.5` 表示 busy，语义存在自相矛盾；集成时先用可控过载实验标定方向，不能直接把它当“剩余容量百分比”。

## 5. 核亲和性、队列与背压

- 官方文档的 basic example 将外部 feed task 固定 core 0、priority 5、stack 8 KiB，将 fetch task固定 core 1、priority 5、stack 4 KiB：[audio_front_end/README.rst L202-L216](https://github.com/espressif/esp-sr/blob/7ff63a7da40e15e502681be48c4d0e78475544a3/docs/en/audio_front_end/README.rst#L202-L216)。这是示例布局，不是 API 强制。
- 实际 AFE 性能测试却把 feed 与 fetch 都固定在 core 0，priority 5，stack 各 8 KiB：[test_afe.cpp L262-L288](https://github.com/espressif/esp-sr/blob/7ff63a7da40e15e502681be48c4d0e78475544a3/test_apps/esp-sr/main/test_afe.cpp#L262-L288)、[L295-L321](https://github.com/espressif/esp-sr/blob/7ff63a7da40e15e502681be48c4d0e78475544a3/test_apps/esp-sr/main/test_afe.cpp#L295-L321)。因此不能把“feed=core0/fetch=core1”当成性能规范。
- `afe_perferred_core` / `afe_perferred_priority` 配置的是 AFE 内部 speech-enhancement task，不是外部 feed/fetch task：[esp_afe_config.h L135-L142](https://github.com/espressif/esp-sr/blob/7ff63a7da40e15e502681be48c4d0e78475544a3/include/esp32s3/esp_afe_config.h#L135-L142)。产品需要把 I2S ISR/DMA、feed、opaque AFE task、fetch/recognizer和网络任务一起做整机核预算。
- 当前公开测试没有在 feed/fetch 之间创建 FreeRTOS Queue；边界是 AFE API 自身的内部缓冲。生产侧若再加 application queue，必须明确“采集 buffer 归 producer、AFE result 为借用、跨任务 payload 由谁复制/释放”。
- 参考文件展示一种可能设计：input/output 两个 `sr_RingBuf`，feed 和内部处理输出都用 zero-timeout write，内部处理/fetch 用 `portMAX_DELAY` read：[esp_afe_sr_1mic.ref L194-L201](https://github.com/espressif/esp-sr/blob/7ff63a7da40e15e502681be48c4d0e78475544a3/src/esp_afe_sr_1mic.ref#L194-L201)、[L251-L310](https://github.com/espressif/esp-sr/blob/7ff63a7da40e15e502681be48c4d0e78475544a3/src/esp_afe_sr_1mic.ref#L251-L310)。它不参与当前构建，只能提示应实测满缓冲时是 drop、short-write 还是阻塞。

## 6. PSRAM 与模型所有权

- AFE 提供 `MORE_INTERNAL`、`INTERNAL_PSRAM_BALANCE`、`MORE_PSRAM` 三种分配策略：[esp_afe_config.h L41-L45](https://github.com/espressif/esp-sr/blob/7ff63a7da40e15e502681be48c4d0e78475544a3/include/esp32s3/esp_afe_config.h#L41-L45)。S3 AFE benchmark 显示不同管线约占 49-91 KiB internal RAM、740-1239 KiB PSRAM，双麦 SR 单核 feed/fetch 各约 23-25%/22.9%；这些是文档数据，不是容量保证：[benchmark/README.rst L62-L120](https://github.com/espressif/esp-sr/blob/7ff63a7da40e15e502681be48c4d0e78475544a3/docs/en/benchmark/README.rst#L62-L120)。
- `esp_srmodel_init("model")` 找 model partition并 mmap，返回进程级 `static_srmodels` 单例；deinit 会 munmap并释放所有索引对象：[model_path.c L308-L377](https://github.com/espressif/esp-sr/blob/7ff63a7da40e15e502681be48c4d0e78475544a3/src/model_path.c#L308-L377)、[L510-L545](https://github.com/espressif/esp-sr/blob/7ff63a7da40e15e502681be48c4d0e78475544a3/src/model_path.c#L510-L545)。模型分区 mmap 与算法实例的 PSRAM 工作区是不同所有权层。
- 安全释放顺序应是：停止 feed/fetch -> `afe_handle->destroy(afe_data)`（释放 AFE/模型实例）-> `esp_srmodel_deinit(models)`（解除全局 model mapping）。WakeNet 测试也采用“model instance destroy 后 model list deinit”：[test_wakenet.cpp L22-L44](https://github.com/espressif/esp-sr/blob/7ff63a7da40e15e502681be48c4d0e78475544a3/test_apps/esp-sr/main/test_wakenet.cpp#L22-L44)。
- model list 的 refcount 是普通全局整数：每次 init 在查 partition 前先递增；普通 `esp_srmodel_deinit()` 不重置/递减，`esp_srmodel_deinit_with_refcount()` 在计数 `<=1` 真正释放时也不归零：[model_path.c L20-L22](https://github.com/espressif/esp-sr/blob/7ff63a7da40e15e502681be48c4d0e78475544a3/src/model_path.c#L20-L22)、[L510-L554](https://github.com/espressif/esp-sr/blob/7ff63a7da40e15e502681be48c4d0e78475544a3/src/model_path.c#L510-L554)。失败 init、重复 init/deinit、混用两种释放 API或跨任务并发都会让计数失真；应用应集中一个 model manager。

## 7. 错误与停止生命周期

- 正常 micro-benchmark 对每轮执行 `feed -> timed fetch_with_delay -> free input -> destroy AFE`，重复三轮并检查 heap 回到稳定值：[test_afe.cpp L51-L104](https://github.com/espressif/esp-sr/blob/7ff63a7da40e15e502681be48c4d0e78475544a3/test_apps/esp-sr/main/test_afe.cpp#L51-L104)。这是当前最可信的 create/destroy 契约证据。
- 测试 feed task 自行跑 101 帧、释放输入 buffer 后删除；fetch task在 `NULL`/`ESP_FAIL` 时退出并清全局 flag：[test_afe.cpp L208-L260](https://github.com/espressif/esp-sr/blob/7ff63a7da40e15e502681be48c4d0e78475544a3/test_apps/esp-sr/main/test_afe.cpp#L208-L260)。但 flag 只是非 atomic 全局 `int`，task create 返回值未检查，也没有 task notification/EventGroup join。
- 两个性能测试只等 fetch flag，未 join feed task，也没有 `destroy(afe_data)`；传给两任务的 `task_info` 还是测试函数栈对象：[test_afe.cpp L271-L292](https://github.com/espressif/esp-sr/blob/7ff63a7da40e15e502681be48c4d0e78475544a3/test_apps/esp-sr/main/test_afe.cpp#L271-L292)。它适合测试观察，不是可直接复制的生产生命周期。
- 文档示例问题更明确：feed task 是无限循环，而 fetch task遇错直接 destroy 同一 `afe_data`，没有通知 feed 停止/等待其退出；这存在 use-after-free 风险：[audio_front_end/README.rst L158-L200](https://github.com/espressif/esp-sr/blob/7ff63a7da40e15e502681be48c4d0e78475544a3/docs/en/audio_front_end/README.rst#L158-L200)。产品实现必须由 supervisor 统一取消并 join 两个任务，不能让 fetch task单方面销毁共享 AFE。
- 当前接口没有 `stop()`，只有 `destroy()` 与 `reset_buffer()`：[esp_afe_sr_iface.h L195-L235](https://github.com/espressif/esp-sr/blob/7ff63a7da40e15e502681be48c4d0e78475544a3/include/esp32s3/esp_afe_sr_iface.h#L195-L235)。建议状态机：`RUNNING -> STOP_REQUESTED -> feed stopped -> fetch bounded wait/exit -> both joined -> destroy AFE -> deinit models -> STOPPED`；任何 create/task failure都按已取得资源逆序回滚。

## 8. 对 ESP-111 ESPS3 的落地结论

1. AFE 应是独立、低频控制的 subsystem，不要让 CSI/network scheduler 直接持有其内部指针；只通过有界、带 owner 的音频/result 消息交接。
2. feed cadence 由 `get_feed_chunksize()/sample_rate` 决定，不能靠普通 `vTaskDelay` 模拟真实 I2S 节拍；DMA overrun、AFE ring pressure、fetch timeout、drop/late frame需要独立指标。
3. S3 双核分配要实测：官方文档与测试的亲和性布局并不一致；先测 feed/fetch/internal AFE 的 runtime stats，再决定是否拆核。
4. `ringbuff_free_pct` 只做原始 telemetry，经过过载标定后再定义阈值；禁止把 borrowed `res->data/raw_data/vad_cache` 指针跨队列传递，跨任务必须复制或转移显式 owner buffer。
5. 统一 model manager 管理 mmap 单例与引用；停止时先销毁所有 AFE/WakeNet/MultiNet 实例，再解除 model mapping。不要混用普通 deinit 与 refcount deinit。
