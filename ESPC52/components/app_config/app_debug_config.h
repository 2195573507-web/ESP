#ifndef APP_DEBUG_CONFIG_H
#define APP_DEBUG_CONFIG_H

#include "esp_log.h"

/**
 * @file app_debug_config.h
 * @brief 项目统一调试开关配置。
 *
 * 调用方法：
 * 1. 需要排查 Mic / VAD / local gateway voice turn 时，只修改本文件里的 APP_DEBUG_* 宏。
 * 2. 普通运行保持默认值即可：关键状态和错误日志常显，高噪声 hex / payload / PCM
 *    每包统计默认关闭。
 * 3. 各模块头文件只保留业务参数，实际调试开关值统一来自这里。
 */

/* ============================== Mic 识别准确度调试入口 ==============================
 * 1. 触发太灵敏/不灵敏：优先调 APP_VOICE_VAD_SPEECH_START_RMS / PEAK 和 APP_VOICE_VAD_SPEECH_END_RMS。
 * 2. 句首丢字：优先调 APP_VOICE_PRE_SPEECH_PACKETS；句尾丢字：优先调 APP_VOICE_POST_ROLL_MS。
 * 3. 需要看音频质量：打开 APP_DEBUG_VOICE_PCM_PACKET_STATS_LOG，再看 pcm_rms/p2p/clipping。
 * 4. 音量过小或削顶：再到 mic_adc_pcm.h 调 MIC_ADC_PCM_GAIN / DC 跟踪参数。
 */

/* 高频音频/Mic/IIS 诊断总开关：默认关闭，错误日志不受影响。 */
#define ENABLE_VERBOSE_AUDIO_LOG                   0

/* Mic ADC 调试：循环与栈水位日志默认关闭，错误日志不受这些开关影响。 */
#define APP_DEBUG_MIC_ADC_LOOP_LOG                 ENABLE_VERBOSE_AUDIO_LOG // ADC 采样循环普通诊断日志。
#define APP_DEBUG_MIC_ADC_STACK_LOG                ENABLE_VERBOSE_AUDIO_LOG // ADC 任务栈水位诊断日志。

/* Local gateway voice turn 关键日志：默认只保留主流程一句话日志，细节由调试开关打开。 */
#define APP_DEBUG_VOICE_SESSION_LOG                  0  // Server voice turn 细节日志。
#define APP_DEBUG_VOICE_FINAL_LOG                    0  // 保留兼容名；server-only 链路不产生本地识别文本。
#define APP_DEBUG_VOICE_VAD_KEY_LOG                  0  // VAD 起止/post-roll 细节日志。
#define APP_DEBUG_VOICE_VAD_STATE_LOG                0  // silence_ms 等连续状态日志，默认关闭避免串口刷屏。

/* VAD/local voice 默认参数：集中在这里方便现场调试，不改变 WiFi、ADC 或 PCM 转换链路。 */
#define APP_VOICE_AUDIO_PACKET_MS                    100  // local voice 音频包时长，当前 100 ms。
#define APP_VOICE_VAD_SPEECH_START_RMS              1800 // 起始 RMS 阈值，降低正常说话音量触发门槛。
#define APP_VOICE_VAD_SPEECH_START_PEAK             4500 // 起始 peak 阈值，保留峰值过滤以减少环境噪声误触发。
#define APP_VOICE_VAD_SPEECH_END_RMS                850  // 低于该 RMS 累计静音，认为接近说话结束。
#define APP_VOICE_VAD_START_FRAMES                  2    // 200 ms 窗口下 400 ms 起始确认，落在 500 ms pre-roll 覆盖范围内。
#define APP_VOICE_VAD_START_COOLDOWN_MS             1200 // local voice done 后 VAD 起始冷却，防止刚恢复就再次触发。
#define APP_VOICE_VAD_SILENCE_END_MS                1500 // 静音累计 1500 ms 后结束本轮音频。
#define APP_VOICE_VAD_MIN_RECORD_MS                 800  // 防止过短语音误触发结束。
#define APP_VOICE_VAD_MAX_RECORD_MS                15000 // 命令最长讲话 15 s；到期仍收尾提交有效 PCM。
#define APP_VOICE_PRE_SPEECH_PACKETS                5    // 句首预缓存 5 个 100 ms local voice PCM 包。
#define APP_VOICE_POST_ROLL_MS                      700  // VOICE_END 后继续发送 700 ms PCM，再 finalize。

/* Command capture is already open before VAD detects speech.  These values
 * only label speech and close it after silence; they never gate PCM TX. */
#define APP_VOICE_COMMAND_VAD_SPEECH_START_RMS      350
#define APP_VOICE_COMMAND_VAD_SPEECH_START_PEAK     900
#define APP_VOICE_COMMAND_VAD_SPEECH_END_RMS        220
#define APP_VOICE_COMMAND_VAD_START_FRAMES          1
#define APP_VOICE_COMMAND_VAD_SILENCE_END_MS        1500
#define APP_VOICE_COMMAND_VAD_MAX_RECORD_MS       15000

/* 唤醒确认后的命令采集分阶段预算；它们不改变 VAD 阈值或 PCM 链路。 */
#define APP_VOICE_COMMAND_WAIT_SPEECH_TIMEOUT_MS    8000  // 仅限制等待用户开口。
#define APP_VOICE_COMMAND_FINALIZE_TIMEOUT_MS       5000  // VAD 收尾、上传提交的本地上限。
#define APP_VOICE_COMMAND_SERVER_RESPONSE_TIMEOUT_MS 90000 // 与当前 voice request 总预算一致。

/* PCM 音质调试：默认关闭，排查静音或削波时再打开。 */
#define APP_DEBUG_VOICE_PCM_PACKET_STATS_LOG         0  // pcm_min/max/avg/rms/zero_cross 等质量统计。
#define APP_DEBUG_VOICE_PCM_EVERY_PACKET_STATS       0  // 1 表示每包统计；0 表示按间隔统计。
#define APP_DEBUG_VOICE_PCM_HEX_DUMP                 0  // 每个音频包前 N 字节 PCM hex，默认关闭。

/* Speaker/IIS 调试：默认关闭，不影响 Mic/server voice 主链路串口输出。 */
#define APP_DEBUG_BSP_IIS_LOG                    ENABLE_VERBOSE_AUDIO_LOG // IIS/PDM 底层 GPIO、DMA 配置诊断。
#define APP_DEBUG_SPEAKER_PLAYER_LOG             ENABLE_VERBOSE_AUDIO_LOG // speaker 播放器 ringbuffer/DMA/heap 诊断。

/* Bridge 调试：活动固件只通过 server_voice_client 与 ESPS3 local gateway 通信。 */
#define APP_DEBUG_SCREEN_BRIDGE                    0  // screen bridge/LCD 命令日志。

/* 调试预览长度和节流参数：只影响日志大小，不改变 server voice PCM 上传内容。 */
#define APP_DEBUG_VOICE_PCM_HEX_PREVIEW_BYTES        16  // PCM hex 预览字节数。
#define APP_DEBUG_VOICE_PCM_STATS_INTERVAL_PACKETS   100 // PCM 质量统计默认每 100 包打印一次。

#if APP_DEBUG_VOICE_PCM_HEX_PREVIEW_BYTES < 0
#error "APP_DEBUG_VOICE_PCM_HEX_PREVIEW_BYTES must not be negative"
#endif

#if APP_DEBUG_VOICE_PCM_STATS_INTERVAL_PACKETS <= 0
#error "APP_DEBUG_VOICE_PCM_STATS_INTERVAL_PACKETS must be greater than 0"
#endif

#if APP_VOICE_AUDIO_PACKET_MS <= 0
#error "APP_VOICE_AUDIO_PACKET_MS must be greater than 0"
#endif

#if APP_VOICE_VAD_SPEECH_START_RMS <= 0
#error "APP_VOICE_VAD_SPEECH_START_RMS must be greater than 0"
#endif

#if APP_VOICE_VAD_SPEECH_START_PEAK <= 0
#error "APP_VOICE_VAD_SPEECH_START_PEAK must be greater than 0"
#endif

#if APP_VOICE_VAD_SPEECH_END_RMS < 0
#error "APP_VOICE_VAD_SPEECH_END_RMS must not be negative"
#endif

#if APP_VOICE_VAD_START_FRAMES <= 0
#error "APP_VOICE_VAD_START_FRAMES must be greater than 0"
#endif

#if APP_VOICE_VAD_START_COOLDOWN_MS < 0
#error "APP_VOICE_VAD_START_COOLDOWN_MS must not be negative"
#endif

#if APP_VOICE_VAD_SILENCE_END_MS <= 0
#error "APP_VOICE_VAD_SILENCE_END_MS must be greater than 0"
#endif

#if APP_VOICE_VAD_MIN_RECORD_MS < 0
#error "APP_VOICE_VAD_MIN_RECORD_MS must not be negative"
#endif

#if APP_VOICE_VAD_MAX_RECORD_MS <= 0
#error "APP_VOICE_VAD_MAX_RECORD_MS must be greater than 0"
#endif

#if APP_VOICE_VAD_MAX_RECORD_MS < APP_VOICE_VAD_MIN_RECORD_MS
#error "APP_VOICE_VAD_MAX_RECORD_MS must be greater than or equal to APP_VOICE_VAD_MIN_RECORD_MS"
#endif

#if APP_VOICE_PRE_SPEECH_PACKETS < 3 || APP_VOICE_PRE_SPEECH_PACKETS > 5
#error "APP_VOICE_PRE_SPEECH_PACKETS must be between 3 and 5"
#endif

#if APP_VOICE_POST_ROLL_MS < 0 || APP_VOICE_POST_ROLL_MS > 1000
#error "APP_VOICE_POST_ROLL_MS must be between 0 and 1000"
#endif

#if APP_VOICE_COMMAND_WAIT_SPEECH_TIMEOUT_MS <= 0 || \
    APP_VOICE_COMMAND_FINALIZE_TIMEOUT_MS <= 0 || \
    APP_VOICE_COMMAND_SERVER_RESPONSE_TIMEOUT_MS <= 0
#error "command capture phase timeouts must be greater than 0"
#endif

#if APP_VOICE_COMMAND_VAD_SPEECH_START_RMS <= 0 || \
    APP_VOICE_COMMAND_VAD_SPEECH_START_PEAK <= 0 || \
    APP_VOICE_COMMAND_VAD_SPEECH_END_RMS < 0 || \
    APP_VOICE_COMMAND_VAD_START_FRAMES <= 0 || \
    APP_VOICE_COMMAND_VAD_SILENCE_END_MS <= 0 || \
    APP_VOICE_COMMAND_VAD_MAX_RECORD_MS <= 0
#error "command VAD configuration must be valid"
#endif

/**
 * @brief 应用默认日志等级。
 *
 * 调用方法：app_main() 最早调用一次。普通运行保持底层库只输出 warn/error，
 * 项目自己的主流程 INFO 仍可见；需要细节时打开上面的 APP_DEBUG_* 宏。
 */
static inline void app_debug_apply_log_levels(void)
{
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("wifi_init", ESP_LOG_WARN);
    esp_log_level_set("phy_init", ESP_LOG_WARN);
    esp_log_level_set("pp", ESP_LOG_WARN);
    esp_log_level_set("net80211", ESP_LOG_WARN);
    esp_log_level_set("esp_netif_handlers", ESP_LOG_WARN);
    esp_log_level_set("transport_base", ESP_LOG_WARN);
    esp_log_level_set("esp_http_client", ESP_LOG_WARN);
}

#endif // APP_DEBUG_CONFIG_H
