#ifndef APP_MAIN_CONFIG_H
#define APP_MAIN_CONFIG_H

/**
 * @file app_main_config.h
 * @brief app_main 运行链路开关。
 *
 * 调用方法：
 * 1. MAIN_ENABLE_MIC_CHAIN=1 时启动 C5 -> ESPS3 local gateway 半双工语音链路。
 * 2. MAIN_ENABLE_BME_SERVICE=1 时启动 BME690 周期读取/上传服务。
 * 3. MAIN_ENABLE_SPEAKER_SELF_TEST=1 时启动后播放 1 kHz 自检音，不经过 server voice。
 * 4. MAIN_ENABLE_CSI_SERVICE=1 时在 WiFi 稳定后启动 CSI 本地校准和 feature 输出。
 */

#ifndef MAIN_ENABLE_MIC_CHAIN
/* Mic/local voice 主链路开关：打开后由 voice_chain 编排 PCM 上传和 S3 回传 PCM 播放。 */
#define MAIN_ENABLE_MIC_CHAIN 1
#endif

#ifndef MAIN_ENABLE_BME_SERVICE
/* BME690 周期读取/上传服务开关：打开后由 voice_chain 在 local voice turn 期间 pause/resume。 */
#define MAIN_ENABLE_BME_SERVICE 1
#endif

#ifndef MAIN_ENABLE_SPEAKER_CHAIN
/* 保留兼容开关名；speaker 由 voice_chain/server_voice_client 初始化并播放 S3 回传 PCM。 */
#define MAIN_ENABLE_SPEAKER_CHAIN 1
#endif

#ifndef MAIN_ENABLE_SPEAKER_SELF_TEST
/* Speaker 自检音开关：1 表示 WiFi 稳定后播放 1 kHz/16 kHz/16-bit/mono 自检音。 */
#define MAIN_ENABLE_SPEAKER_SELF_TEST 0
#endif

#ifndef MAIN_ENABLE_CSI_SERVICE
/* CSI 运行总开关：置 0 时不配置 WiFi CSI、不启动 CSI 任务。 */
#define MAIN_ENABLE_CSI_SERVICE 1
#endif

#ifndef CSI_REPORT_INTERVAL_MS
/* CSI edge feature 输出周期，单位 ms；callback 不执行算法，只投递处理事件。 */
#define CSI_REPORT_INTERVAL_MS 100U
#endif

#ifndef CSI_SERVICE_REPORT_INTERVAL_MS
/* 保留旧宏名；默认跟随 CSI_REPORT_INTERVAL_MS。 */
#define CSI_SERVICE_REPORT_INTERVAL_MS CSI_REPORT_INTERVAL_MS
#endif

#ifndef CSI_OUTPUT_ENABLE_LOG
/* CSI feature 本地日志开关；仅打印轻量 feature JSON，不打印 raw CSI。 */
#define CSI_OUTPUT_ENABLE_LOG 1
#endif

#ifndef CSI_OUTPUT_ENABLE_HTTP
/* CSI feature HTTP 上报开关；只 POST 到 ESPS3 /local/v1/csi/result。 */
#define CSI_OUTPUT_ENABLE_HTTP 1
#endif

#ifndef CSI_OUTPUT_ENABLE_DEBUG_METRICS
/* CSI debug payload 开关：默认 HTTP payload 不携带 energy/variance/cv 或 legacy v。 */
#define CSI_OUTPUT_ENABLE_DEBUG_METRICS 0
#endif

#ifndef CSI_ALGORITHM_VERSION
/* CSI 边缘 feature 算法版本；C5 输出本地 IDLE/MOTION，S3 负责双链路融合。 */
#define CSI_ALGORITHM_VERSION "edge_feature_v2"
#endif

#ifndef C5_SCHEDULER_TASK_STACK
/* C5 event dispatcher 栈；业务 HTTP/JSON/传感器路径在 worker 中执行。 */
#define C5_SCHEDULER_TASK_STACK 12288U
#endif

#ifndef C5_SCHEDULER_TASK_PRIORITY
/* C5 event dispatcher 优先级；voice/audio 仍保持独立高优先级链路。 */
#define C5_SCHEDULER_TASK_PRIORITY 4U
#endif

#ifndef C5_WORKER_TASK_STACK
/* C5 CSI/BME/system worker 栈；覆盖各自 HTTP/JSON/传感器业务路径。 */
#define C5_WORKER_TASK_STACK 8192U
#endif

#ifndef C5_WORKER_TASK_PRIORITY
/* C5 CSI/BME/system worker 优先级，低于 dispatcher 与 voice/audio 链路。 */
#define C5_WORKER_TASK_PRIORITY 3U
#endif

#ifndef MAIN_SPEAKER_SELF_TEST_DURATION_MS
/* Speaker 自检音时长，单位 ms。 */
#define MAIN_SPEAKER_SELF_TEST_DURATION_MS 1500
#endif

#ifndef MAIN_IDLE_DELAY_MS
/* app_startup_task 完成启动后进入低频空闲循环，后台任务继续运行。 */
#define MAIN_IDLE_DELAY_MS 1000
#endif

#ifndef APP_STARTUP_TASK_STACK
/* 复杂 WiFi/HTTP/audio 启动链路任务栈，单位字节；避免压占 ESP-IDF main_task 栈。 */
#define APP_STARTUP_TASK_STACK 12288U
#endif

#ifndef APP_STARTUP_TASK_PRIORITY
/* 启动编排任务优先级：高于后台命令/BME，低于 WiFi 重连和音频实时任务。 */
#define APP_STARTUP_TASK_PRIORITY 3U
#endif

#if MAIN_ENABLE_MIC_CHAIN != 0 && MAIN_ENABLE_MIC_CHAIN != 1
#error "MAIN_ENABLE_MIC_CHAIN must be 0 or 1"
#endif

#if MAIN_ENABLE_BME_SERVICE != 0 && MAIN_ENABLE_BME_SERVICE != 1
#error "MAIN_ENABLE_BME_SERVICE must be 0 or 1"
#endif

#if MAIN_ENABLE_SPEAKER_CHAIN != 0 && MAIN_ENABLE_SPEAKER_CHAIN != 1
#error "MAIN_ENABLE_SPEAKER_CHAIN must be 0 or 1"
#endif

#if MAIN_ENABLE_SPEAKER_SELF_TEST != 0 && MAIN_ENABLE_SPEAKER_SELF_TEST != 1
#error "MAIN_ENABLE_SPEAKER_SELF_TEST must be 0 or 1"
#endif

#if MAIN_ENABLE_CSI_SERVICE != 0 && MAIN_ENABLE_CSI_SERVICE != 1
#error "MAIN_ENABLE_CSI_SERVICE must be 0 or 1"
#endif

#if CSI_OUTPUT_ENABLE_LOG != 0 && CSI_OUTPUT_ENABLE_LOG != 1
#error "CSI_OUTPUT_ENABLE_LOG must be 0 or 1"
#endif

#if CSI_OUTPUT_ENABLE_HTTP != 0 && CSI_OUTPUT_ENABLE_HTTP != 1
#error "CSI_OUTPUT_ENABLE_HTTP must be 0 or 1"
#endif

#if CSI_OUTPUT_ENABLE_DEBUG_METRICS != 0 && CSI_OUTPUT_ENABLE_DEBUG_METRICS != 1
#error "CSI_OUTPUT_ENABLE_DEBUG_METRICS must be 0 or 1"
#endif

#if MAIN_SPEAKER_SELF_TEST_DURATION_MS <= 0
#error "MAIN_SPEAKER_SELF_TEST_DURATION_MS must be greater than 0"
#endif

#if CSI_SERVICE_REPORT_INTERVAL_MS < 100
#error "CSI_SERVICE_REPORT_INTERVAL_MS must be at least 100ms"
#endif

#if C5_SCHEDULER_TASK_STACK < 8192
#error "C5_SCHEDULER_TASK_STACK must be at least 8192"
#endif

#if C5_SCHEDULER_TASK_PRIORITY <= 0
#error "C5_SCHEDULER_TASK_PRIORITY must be greater than 0"
#endif

#if C5_WORKER_TASK_STACK < 6144
#error "C5_WORKER_TASK_STACK must be at least 6144"
#endif

#if C5_WORKER_TASK_PRIORITY <= 0
#error "C5_WORKER_TASK_PRIORITY must be greater than 0"
#endif

#if APP_STARTUP_TASK_STACK < 8192
#error "APP_STARTUP_TASK_STACK must be at least 8192"
#endif

#if APP_STARTUP_TASK_PRIORITY <= 0
#error "APP_STARTUP_TASK_PRIORITY must be greater than 0"
#endif

#endif /* APP_MAIN_CONFIG_H */
