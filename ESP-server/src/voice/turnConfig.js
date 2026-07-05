const {
    readPositiveInteger
} = require("../utils/env");

const DEFAULT_VOLC_GATEWAY_WS_AUDIO_CHUNK_BYTES = 32000;
const DEFAULT_VOICE_TURN_TIMEOUT_MS = 45000;
const DEFAULT_VOICE_TURN_MAX_CONCURRENT = 1;
const DEFAULT_VOICE_TURN_MAX_BYTES = 4 * 1024 * 1024;
const DEFAULT_LLM_TIMEOUT_MS = 30000;

function readVoiceTurnConfig() {
    return {
        timeoutMs: readPositiveInteger(process.env.VOICE_TURN_TIMEOUT_MS, DEFAULT_VOICE_TURN_TIMEOUT_MS),
        maxConcurrent: readPositiveInteger(process.env.VOICE_TURN_MAX_CONCURRENT, DEFAULT_VOICE_TURN_MAX_CONCURRENT),
        mockEnabled: String(process.env.VOICE_TURN_MOCK || "").trim() === "1",
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
    readVoiceLlmTimeoutMs,
    readVoiceTurnConfig,
    readVoiceTurnMaxBytes
};
