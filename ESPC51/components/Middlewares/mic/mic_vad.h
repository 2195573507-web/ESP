#ifndef MIC_VAD_H
#define MIC_VAD_H

#include <stdint.h>

#include "app_debug_config.h"

/**
 * @file mic_vad.h
 * @brief 独立的最小语音活动检测模块。
 *
 * 本模块不读取 ADC、不转换 PCM、不依赖 FreeRTOS；只根据一帧统计值判断是否有人声活动。
 */

/* VAD 参数：当前 MIC_ADC 每帧 3200 samples / 16000 Hz = 200 ms。 */
#define MIC_VAD_FRAME_MS           200 // 每帧统计对应的时间。
#define MIC_VAD_START_RMS          APP_VOICE_VAD_SPEECH_START_RMS // 连续超过该 PCM RMS 才认为开始说话。
#define MIC_VAD_START_PEAK         APP_VOICE_VAD_SPEECH_START_PEAK // 连续超过该 PCM peak 才认为开始说话。
#define MIC_VAD_END_RMS            APP_VOICE_VAD_SPEECH_END_RMS   // 连续低于该 PCM RMS 才认为说话结束。
#define MIC_VAD_START_FRAMES       APP_VOICE_VAD_START_FRAMES // 连续达到配置帧数才开始。
#define MIC_VAD_END_FRAMES         ((APP_VOICE_VAD_SILENCE_END_MS + MIC_VAD_FRAME_MS - 1) / MIC_VAD_FRAME_MS) // 连续静音达到配置时长才结束。
#define MIC_VAD_MIN_SPEECH_FRAMES  2   // 小于 400 ms 的语音忽略。
#define MIC_VAD_MAX_SPEECH_FRAMES  ((APP_VOICE_VAD_MAX_RECORD_MS + MIC_VAD_FRAME_MS - 1) / MIC_VAD_FRAME_MS) // 最长录音时长，防止卡死。

/**
 * @brief VAD 状态机状态
 *
 * 调用方法：上层一般只读 state 做调试，不需要直接修改。
 * 串口状态码：0=IDLE，1=SPEECH，2=HANGOVER。
 */
typedef enum {
    MIC_VAD_STATE_IDLE = 0,      // 空闲，当前没有语音。
    MIC_VAD_STATE_SPEECH = 1,    // 说话中。
    MIC_VAD_STATE_HANGOVER = 2,  // 已变安静，等待连续结束帧。
} mic_vad_state_t;

/**
 * @brief VAD 输出事件。
 *
 * 调用方法：mic_vad_process() 每处理一帧返回一个事件，上层按事件打印日志。
 * 串口事件码：0=NONE，1=VOICE_START，2=VOICE_END。
 */
typedef enum {
    MIC_VAD_EVENT_NONE = 0,         // 没有新事件。
    MIC_VAD_EVENT_VOICE_START,      // 本帧检测到说话开始。
    MIC_VAD_EVENT_VOICE_END,        // 本帧检测到说话结束。
} mic_vad_event_t;

/**
 * @brief VAD 输入特征。
 *
 * 调用方法：ADC 统计窗口算出 adc_rms/adc_p2p/pcm_rms/pcm_p2p/pcm_peak/clipped 后填入本结构。
 */
typedef struct {
    uint32_t adc_rms;  // ADC 去直流 RMS。
    uint32_t adc_p2p;  // ADC 峰峰值。
    uint32_t pcm_rms;  // PCM 去直流 RMS。
    uint32_t pcm_p2p;  // PCM 峰峰值。
    uint32_t pcm_peak; // PCM 正负绝对峰值。
    uint32_t clipped;  // 1 表示 ADC 或 PCM 有削顶。
} mic_vad_features_t;

/**
 * @brief VAD 状态机上下文。
 *
 * 调用方法：定义一个 mic_vad_t 变量，先调用 mic_vad_init()，再逐帧调用 mic_vad_process()。
 */
typedef struct {
    mic_vad_state_t state;  // 当前状态。
    int start_count;        // 连续达到开始阈值的帧数。
    int end_count;          // 连续达到结束阈值的帧数。
    int speech_frames;      // 当前语音段已持续的帧数。
} mic_vad_t;

/**
 * @brief 初始化 VAD 状态机。
 *
 * 调用方法：Mic ADC 任务启动时调用一次。
 *
 * @param vad VAD 状态机，不能为空。
 */
void mic_vad_init(mic_vad_t *vad);

/**
 * @brief 处理一帧统计值并更新 VAD 状态。
 *
 * 调用方法：每输出一帧 MIC_ADC 统计时调用一次。
 *
 * @param vad VAD 状态机，不能为空。
 * @param features 当前帧统计值，不能为空。
 * @return 本帧产生的 VAD 事件。
 */
mic_vad_event_t mic_vad_process(mic_vad_t *vad, const mic_vad_features_t *features);

#endif // MIC_VAD_H
