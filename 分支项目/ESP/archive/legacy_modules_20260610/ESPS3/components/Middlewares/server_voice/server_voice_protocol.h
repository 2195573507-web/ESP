#ifndef SERVER_VOICE_PROTOCOL_H
#define SERVER_VOICE_PROTOCOL_H

#ifdef __cplusplus
extern "C" {
#endif

/* 语音 turn 协议参数：只描述 ESP-server 接口，不包含云端 ASR/LLM/TTS 配置。 */
#ifndef SERVER_VOICE_TURN_ENDPOINT
#define SERVER_VOICE_TURN_ENDPOINT "/api/voice/turn" // 语音 turn 接口路径。
#endif

#ifndef SERVER_VOICE_REQUEST_CONTENT_TYPE
#define SERVER_VOICE_REQUEST_CONTENT_TYPE "audio/L16; rate=16000; channels=1" // 上传 PCM 格式。
#endif

#ifndef SERVER_VOICE_RESPONSE_CONTENT_TYPE
#define SERVER_VOICE_RESPONSE_CONTENT_TYPE "audio/L16" // 期望服务器返回裸 PCM。
#endif

#ifndef SERVER_VOICE_AUDIO_FORMAT
#define SERVER_VOICE_AUDIO_FORMAT "pcm_s16le_mono_16k" // X-Audio-Format header。
#endif

#ifndef SERVER_VOICE_SAMPLE_RATE_HZ
#define SERVER_VOICE_SAMPLE_RATE_HZ 16000 // 上传/返回 PCM 采样率。
#endif

#ifndef SERVER_VOICE_HTTP_TIMEOUT_MS
#define SERVER_VOICE_HTTP_TIMEOUT_MS 30000 // 语音 turn HTTP 超时，单位 ms。
#endif

#ifdef __cplusplus
}
#endif

#endif /* SERVER_VOICE_PROTOCOL_H */
