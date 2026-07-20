const {
    buildGatewayHttpUrl,
    buildGatewayRealtimeUrl,
    buildGatewayTtsUrl,
    normalizeBaseUrl,
    readBooleanFlag,
    readFirstGatewayPathEnv,
    readFirstTrimmedEnv,
    readGatewayPathEnv,
    readPositiveInteger,
    readTrimmedEnv
} = require("../utils/env");
const {
    summarizeSecret
} = require("../utils/logging");

const DEFAULT_LLM_MODEL = "Doubao-Seed-1.6-flash";
const DEFAULT_VOLC_GATEWAY_WS_BASE_URL = "wss://ai-gateway.vei.volces.com";
const DEFAULT_VOLC_GATEWAY_HTTP_BASE_URL = "https://ai-gateway.vei.volces.com";
const DEFAULT_VOLC_GATEWAY_REALTIME_PATH = "/v1/realtime";
const DEFAULT_VOLC_GATEWAY_CHAT_PATH = "/v1/chat/completions";
const DEFAULT_VOLC_GATEWAY_ASR_MODEL = "bigmodel";
const DEFAULT_VOLC_GATEWAY_CHAT_MODEL = DEFAULT_LLM_MODEL;
const DEFAULT_VOLC_GATEWAY_ASR_FORMAT = "pcm";
const DEFAULT_VOLC_GATEWAY_ASR_CODEC = "raw";
const DEFAULT_VOLC_GATEWAY_ASR_RESOURCE_ID = "volc.bigasr.sauc.duration";
const DEFAULT_VOLC_GATEWAY_TTS_SAMPLE_RATE = 16000;
const DEFAULT_VOLC_GATEWAY_TTS_FORMAT = "pcm_s16le_mono_16k";
const DEFAULT_VOLC_GATEWAY_SAMPLE_RATE = 16000;

function readVolcGatewayConfig() {
    const apiKey = readTrimmedEnv("VOLC_GATEWAY_API_KEY");
    const wsBaseUrl = normalizeBaseUrl(
        "VOLC_GATEWAY_WS_BASE_URL",
        DEFAULT_VOLC_GATEWAY_WS_BASE_URL,
        ["ws:", "wss:"]
    );
    const httpBaseUrl = normalizeBaseUrl(
        "VOLC_GATEWAY_HTTP_BASE_URL",
        DEFAULT_VOLC_GATEWAY_HTTP_BASE_URL,
        ["http:", "https:"]
    );
    const realtimePath = readGatewayPathEnv("VOLC_GATEWAY_REALTIME_PATH", DEFAULT_VOLC_GATEWAY_REALTIME_PATH);
    const chatPath = readGatewayPathEnv("VOLC_GATEWAY_CHAT_PATH", DEFAULT_VOLC_GATEWAY_CHAT_PATH);
    const asrModel = readTrimmedEnv("VOLC_GATEWAY_ASR_MODEL", DEFAULT_VOLC_GATEWAY_ASR_MODEL);
    const chatModel = readTrimmedEnv("VOLC_GATEWAY_CHAT_MODEL", DEFAULT_VOLC_GATEWAY_CHAT_MODEL);
    const ttsModel = readFirstTrimmedEnv([
        "VOLC_GATEWAY_TTS_MODEL",
        "LLM_MODEL_TTS",
        "LLM_GATEWAY_TTS_MODEL",
        "TTS_MODEL"
    ]);
    const ttsVoice = readFirstTrimmedEnv([
        "VOLC_GATEWAY_TTS_VOICE",
        "LLM_TTS_VOICE",
        "TTS_VOICE",
        "TTS_VOICE_TYPE",
        "VOICE_TYPE"
    ]);
    const ttsPath = readFirstGatewayPathEnv([
        "VOLC_GATEWAY_TTS_PATH",
        "VOLC_GATEWAY_TTS_REALTIME_PATH",
        "LLM_TTS_PATH",
        "TTS_PATH",
        "TTS_URL",
        "TTS_ENDPOINT"
    ], ttsModel ? realtimePath : "");
    const asrSampleRate = readPositiveInteger(
        process.env.VOLC_GATEWAY_ASR_SAMPLE_RATE,
        DEFAULT_VOLC_GATEWAY_SAMPLE_RATE
    );
    const asrBits = readPositiveInteger(process.env.VOLC_GATEWAY_ASR_BITS, 16);
    const asrChannels = readPositiveInteger(process.env.VOLC_GATEWAY_ASR_CHANNELS, 1);
    const ttsSampleRate = readPositiveInteger(
        process.env.VOLC_GATEWAY_TTS_SAMPLE_RATE,
        DEFAULT_VOLC_GATEWAY_TTS_SAMPLE_RATE
    );
    const ttsFormat = readTrimmedEnv("VOLC_GATEWAY_TTS_FORMAT", DEFAULT_VOLC_GATEWAY_TTS_FORMAT);
    const ttsResourceId = readTrimmedEnv("VOLC_GATEWAY_TTS_RESOURCE_ID");
    const ttsSpeed = Number.parseFloat(process.env.VOLC_GATEWAY_TTS_SPEED || "1");
    const ttsPitch = Number.parseFloat(process.env.VOLC_GATEWAY_TTS_PITCH || "1");
    const ttsVolume = Number.parseFloat(process.env.VOLC_GATEWAY_TTS_VOLUME || "1");

    return {
        apiKey,
        keySummary: summarizeSecret(apiKey),
        wsBaseUrl,
        httpBaseUrl,
        realtimePath,
        chatPath,
        asr: {
            model: asrModel,
            url: buildGatewayRealtimeUrl(wsBaseUrl, realtimePath, asrModel),
            sampleRate: asrSampleRate,
            bits: asrBits,
            channels: asrChannels,
            format: readTrimmedEnv("VOLC_GATEWAY_ASR_FORMAT", DEFAULT_VOLC_GATEWAY_ASR_FORMAT),
            codec: readTrimmedEnv("VOLC_GATEWAY_ASR_CODEC", DEFAULT_VOLC_GATEWAY_ASR_CODEC),
            useResourceId: readBooleanFlag(process.env.VOLC_GATEWAY_USE_RESOURCE_ID),
            resourceId: readTrimmedEnv("VOLC_GATEWAY_ASR_RESOURCE_ID", DEFAULT_VOLC_GATEWAY_ASR_RESOURCE_ID)
        },
        chat: {
            baseUrl: httpBaseUrl,
            path: chatPath,
            endpoint: buildGatewayHttpUrl(httpBaseUrl, chatPath),
            model: chatModel
        },
        tts: {
            model: ttsModel,
            voice: ttsVoice,
            path: ttsPath,
            url: ttsModel && ttsPath ? buildGatewayTtsUrl(wsBaseUrl, httpBaseUrl, ttsPath, ttsModel) : "",
            sampleRate: ttsSampleRate,
            format: ttsFormat,
            speed: Number.isFinite(ttsSpeed) && ttsSpeed > 0 ? ttsSpeed : 1.0,
            pitch: Number.isFinite(ttsPitch) && ttsPitch > 0 ? ttsPitch : 1.0,
            volume: Number.isFinite(ttsVolume) && ttsVolume > 0 ? ttsVolume : 1.0,
            useResourceId: readBooleanFlag(process.env.VOLC_GATEWAY_TTS_USE_RESOURCE_ID),
            resourceId: ttsResourceId
        }
    };
}

function validateUrl(value, protocols, code, envName) {
    let parsed;
    try {
        parsed = new URL(value);
    } catch (_) {
        return {
            status: 503,
            code,
            message: `${envName} must be an absolute http(s) URL`
        };
    }

    if (!protocols.includes(parsed.protocol)) {
        return {
            status: 503,
            code,
            message: `${envName} must use ${protocols.join(" or ").replace(/:/g, "")}`
        };
    }

    return null;
}

function validateVoiceAsrConfig(config) {
    if (!config.apiKey || !config.asr.model) {
        return {
            status: 503,
            code: "VOICE_ASR_NOT_CONFIGURED",
            message: "VOLC_GATEWAY_API_KEY and VOLC_GATEWAY_ASR_MODEL must be configured when VOICE_TURN_MOCK is not 1"
        };
    }

    const urlError = validateUrl(config.asr.url, ["ws:", "wss:"], "VOICE_ASR_NOT_CONFIGURED", "VOLC_GATEWAY_WS_BASE_URL/VOLC_GATEWAY_REALTIME_PATH");
    if (urlError) {
        return urlError;
    }

    if (config.asr.sampleRate !== DEFAULT_VOLC_GATEWAY_SAMPLE_RATE ||
        config.asr.bits !== 16 ||
        config.asr.channels !== 1 ||
        config.asr.format !== DEFAULT_VOLC_GATEWAY_ASR_FORMAT ||
        config.asr.codec !== DEFAULT_VOLC_GATEWAY_ASR_CODEC) {
        return {
            status: 503,
            code: "VOICE_ASR_NOT_CONFIGURED",
            message: "VOLC_GATEWAY_ASR_* must describe pcm_s16le_mono_16k raw PCM"
        };
    }

    return null;
}

function validateVoiceChatConfig(config) {
    if (!config.apiKey || !config.chat.model) {
        return {
            status: 503,
            code: "VOICE_LLM_FAILED",
            message: "VOLC_GATEWAY_API_KEY and VOLC_GATEWAY_CHAT_MODEL must be configured when VOICE_TURN_MOCK is not 1"
        };
    }

    return validateUrl(config.chat.endpoint, ["http:", "https:"], "VOICE_LLM_FAILED", "VOLC_GATEWAY_HTTP_BASE_URL/VOLC_GATEWAY_CHAT_PATH");
}

function validateVoiceTtsConfig(config) {
    if (!config.apiKey || !config.tts.model || !config.tts.voice || !config.tts.path) {
        return {
            status: 503,
            code: "VOICE_TTS_NOT_CONFIGURED",
            message: "VOLC_GATEWAY_TTS_MODEL, VOLC_GATEWAY_TTS_VOICE, and VOLC_GATEWAY_TTS_PATH must be configured when VOICE_TURN_MOCK is not 1"
        };
    }

    const urlError = validateUrl(config.tts.url, ["ws:", "wss:", "http:", "https:"], "VOICE_TTS_NOT_CONFIGURED", "VOLC_GATEWAY_WS_BASE_URL/VOLC_GATEWAY_TTS_PATH");
    if (urlError) {
        return urlError;
    }

    if (config.tts.format !== DEFAULT_VOLC_GATEWAY_TTS_FORMAT) {
        return {
            status: 503,
            code: "VOICE_TTS_NOT_CONFIGURED",
            message: `VOLC_GATEWAY_TTS_FORMAT must be ${DEFAULT_VOLC_GATEWAY_TTS_FORMAT}`
        };
    }

    if (config.tts.sampleRate !== DEFAULT_VOLC_GATEWAY_SAMPLE_RATE) {
        return {
            status: 503,
            code: "VOICE_TTS_NOT_CONFIGURED",
            message: `VOLC_GATEWAY_TTS_SAMPLE_RATE must be ${DEFAULT_VOLC_GATEWAY_SAMPLE_RATE}`
        };
    }

    return null;
}

module.exports = {
    readVolcGatewayConfig,
    validateVoiceAsrConfig,
    validateVoiceChatConfig,
    validateVoiceTtsConfig
};
