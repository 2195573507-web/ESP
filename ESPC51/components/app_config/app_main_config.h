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
 * 4. MAIN_ENABLE_CSI_SERVICE=1 时在 WiFi 稳定后启动 CSI raw 接收和本地摘要输出。
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
/* CSI 摘要输出周期，单位 ms；只输出 occupancy/motion_score 等轻量结果。 */
#define CSI_REPORT_INTERVAL_MS 1000U
#endif

#ifndef CSI_SERVICE_REPORT_INTERVAL_MS
/* 保留旧宏名；默认跟随 CSI_REPORT_INTERVAL_MS。 */
#define CSI_SERVICE_REPORT_INTERVAL_MS CSI_REPORT_INTERVAL_MS
#endif

#ifndef CSI_OUTPUT_ENABLE_LOG
/* CSI 摘要本地日志开关；仅打印轻量 summary JSON，不打印 raw CSI。 */
#define CSI_OUTPUT_ENABLE_LOG 1
#endif

#ifndef CSI_OUTPUT_ENABLE_HTTP
/* CSI 摘要 HTTP 上报开关；只 POST 到 ESPS3 /local/v1/csi/result。 */
#define CSI_OUTPUT_ENABLE_HTTP 1
#endif

#ifndef CSI_ALGORITHM_VERSION
/* CSI 阶段 A 摘要算法版本；用于 S3 区分后续算法演进。 */
#define CSI_ALGORITHM_VERSION "phase_a_v1"
#endif

#ifndef CSI_SERVICE_WINDOW_SAMPLES
/* CSI 滑动窗口帧数上限，必须不超过 csi_phase_a 的固定数组上限。 */
#define CSI_SERVICE_WINDOW_SAMPLES 32U
#endif

#ifndef CSI_SERVICE_TASK_STACK
/* CSI 低优先级上报任务栈，HTTP 上传和算法摘要在任务中执行，不在 WiFi callback 中执行。 */
#define CSI_SERVICE_TASK_STACK 4096U
#endif

#ifndef CSI_SERVICE_TASK_PRIORITY
/* CSI 任务优先级低于启动、语音和 WiFi 主链路，避免抢占 BME/voice/command。 */
#define CSI_SERVICE_TASK_PRIORITY 2U
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

#if MAIN_SPEAKER_SELF_TEST_DURATION_MS <= 0
#error "MAIN_SPEAKER_SELF_TEST_DURATION_MS must be greater than 0"
#endif

#if CSI_SERVICE_REPORT_INTERVAL_MS < 1000
#error "CSI_SERVICE_REPORT_INTERVAL_MS must be at least 1000"
#endif

#if CSI_SERVICE_WINDOW_SAMPLES < 8 || CSI_SERVICE_WINDOW_SAMPLES > 64
#error "CSI_SERVICE_WINDOW_SAMPLES must be between 8 and 64"
#endif

#if CSI_SERVICE_TASK_STACK < 3072
#error "CSI_SERVICE_TASK_STACK must be at least 3072"
#endif

#if CSI_SERVICE_TASK_PRIORITY <= 0
#error "CSI_SERVICE_TASK_PRIORITY must be greater than 0"
#endif

#if APP_STARTUP_TASK_STACK < 8192
#error "APP_STARTUP_TASK_STACK must be at least 8192"
#endif

#if APP_STARTUP_TASK_PRIORITY <= 0
#error "APP_STARTUP_TASK_PRIORITY must be greater than 0"
#endif

#endif /* APP_MAIN_CONFIG_H */
