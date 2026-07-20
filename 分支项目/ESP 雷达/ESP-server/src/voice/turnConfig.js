const {
    readBooleanFlag,
    readPositiveInteger
} = require("../utils/env");

const DEFAULT_VOLC_GATEWAY_WS_AUDIO_CHUNK_BYTES = 32000;
const VOICE_REQUEST_TIMEOUT_MS = 60000;
const DEFAULT_VOICE_TURN_TIMEOUT_MS = VOICE_REQUEST_TIMEOUT_MS;
const DEFAULT_VOICE_TURN_MAX_CONCURRENT = 1;
const DEFAULT_VOICE_TURN_MAX_BYTES = 4 * 1024 * 1024;
const DEFAULT_LLM_TIMEOUT_MS = 30000;
const DEFAULT_VOICE_TTS_STREAMING_QUEUE_BYTES = 128 * 1024;
const DEFAULT_VOICE_TTS_STREAMING_CHUNK_BYTES = 64 * 1024;
const DEFAULT_VOICE_TTS_STREAMING_TOTAL_BYTES = 4 * 1024 * 1024;
const DEFAULT_VOICE_TTS_WS_BACKLOG_BYTES = 256 * 1024;
const DEFAULT_VOICE_TTS_WS_BACKLOG_MESSAGES = 128;
const DEFAULT_VOICE_TTS_WS_MESSAGE_BYTES = 128 * 1024;

function readVoiceTurnConfig() {
    return {
        timeoutMs: readPositiveInteger(process.env.VOICE_TURN_TIMEOUT_MS, DEFAULT_VOICE_TURN_TIMEOUT_MS),
        maxConcurrent: readPositiveInteger(process.env.VOICE_TURN_MAX_CONCURRENT, DEFAULT_VOICE_TURN_MAX_CONCURRENT),
        mockEnabled: String(process.env.VOICE_TURN_MOCK || "").trim() === "1",
        streamingEnabled: readBooleanFlag(process.env.VOICE_TTS_STREAMING_ENABLED),
        ttsStreamingQueueBytes: readPositiveInteger(
            process.env.VOICE_TTS_STREAMING_QUEUE_BYTES,
            DEFAULT_VOICE_TTS_STREAMING_QUEUE_BYTES
        ),
        ttsStreamingChunkBytes: readPositiveInteger(
            process.env.VOICE_TTS_STREAMING_CHUNK_BYTES,
            DEFAULT_VOICE_TTS_STREAMING_CHUNK_BYTES
        ),
        ttsStreamingTotalBytes: readPositiveInteger(
            process.env.VOICE_TTS_STREAMING_TOTAL_BYTES,
            DEFAULT_VOICE_TTS_STREAMING_TOTAL_BYTES
        ),
        ttsWebSocketBacklogBytes: readPositiveInteger(
            process.env.VOICE_TTS_WS_BACKLOG_BYTES,
            DEFAULT_VOICE_TTS_WS_BACKLOG_BYTES
        ),
        ttsWebSocketBacklogMessages: readPositiveInteger(
            process.env.VOICE_TTS_WS_BACKLOG_MESSAGES,
            DEFAULT_VOICE_TTS_WS_BACKLOG_MESSAGES
        ),
        ttsWebSocketMessageBytes: readPositiveInteger(
            process.env.VOICE_TTS_WS_MESSAGE_BYTES,
            DEFAULT_VOICE_TTS_WS_MESSAGE_BYTES
        ),
        wsAudioChunkBytes: readPositiveInteger(
            process.env.VOLC_GATEWAY_WS_AUDIO_CHUNK_BYTES,
            DEFAULT_VOLC_GATEWAY_WS_AUDIO_CHUNK_BYTES
        )
    };
}

function readVoiceTurnMaxBytes() {
    return readPositiveInteger(process.env.VOICE_TURN_MAX_BYTES, DEFAULT_VOICE_TURN_MAX_BYTES);
}

function readVoiceLlmTimeoutMs() {
    return readPositiveInteger(process.env.LLM_TIMEOUT_MS, DEFAULT_LLM_TIMEOUT_MS);
}

module.exports = {
    VOICE_REQUEST_TIMEOUT_MS,
    readVoiceLlmTimeoutMs,
    readVoiceTurnConfig,
    readVoiceTurnMaxBytes
};
