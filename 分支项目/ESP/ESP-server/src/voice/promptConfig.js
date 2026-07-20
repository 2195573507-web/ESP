const crypto = require("crypto");
const fs = require("fs");
const path = require("path");
const {
    readVolcGatewayConfig
} = require("./gatewayConfig");

const DEFAULT_WAKE_PROMPT_TEXT = "我在，你说";
const DEFAULT_WAKE_PROMPT_PROVIDER = "volc";
const DEFAULT_WAKE_PROMPT_SAMPLE_RATE = 16000;
const DEFAULT_WAKE_PROMPT_FORMAT = "s16le";
const DEFAULT_WAKE_PROMPT_AUDIO_FORMAT = "pcm_s16le_mono_16k";
const DEFAULT_WAKE_PROMPT_CHANNELS = 1;

let promptConfigState = {
    wake_prompt_text: DEFAULT_WAKE_PROMPT_TEXT,
    provider: "",
    voice_id: "",
    speaker_id: "",
    speed: 1.0,
    pitch: 1.0,
    volume: 1.0,
    sample_rate: DEFAULT_WAKE_PROMPT_SAMPLE_RATE,
    format: DEFAULT_WAKE_PROMPT_FORMAT,
    channels: DEFAULT_WAKE_PROMPT_CHANNELS,
    updated_at_ms: Date.now()
};
let promptConfigLoaded = false;

function configPath() {
    if (process.env.VOICE_PROMPT_CONFIG_PATH) {
        return path.resolve(process.env.VOICE_PROMPT_CONFIG_PATH);
    }
    const cacheDir = process.env.VOICE_PROMPT_CACHE_DIR
        ? path.resolve(process.env.VOICE_PROMPT_CACHE_DIR)
        : path.join(__dirname, "..", "..", "cache", "voice_prompts");
    return path.join(cacheDir, "wake_prompt_config.json");
}

function loadPromptConfigFromDisk() {
    if (promptConfigLoaded) {
        return;
    }
    promptConfigLoaded = true;

    try {
        const saved = JSON.parse(fs.readFileSync(configPath(), "utf8"));
        promptConfigState = normalizePromptConfig({
            ...promptConfigState,
            ...saved
        });
    } catch (_) {
        promptConfigState = normalizePromptConfig(promptConfigState);
    }
}

function savePromptConfigToDisk(config) {
    const filePath = configPath();
    fs.mkdirSync(path.dirname(filePath), { recursive: true });
    const tmpPath = `${filePath}.tmp`;
    fs.writeFileSync(tmpPath, `${JSON.stringify(config, null, 2)}\n`);
    fs.renameSync(tmpPath, filePath);
}

function normalizeString(value, fallback = "") {
    const text = String(value ?? "").trim();
    return text || fallback;
}

function normalizeNumber(value, fallback) {
    const number = Number(value);
    return Number.isFinite(number) && number > 0 ? number : fallback;
}

function normalizeInteger(value, fallback) {
    const number = Number(value);
    return Number.isInteger(number) && number > 0 ? number : fallback;
}

function normalizePromptConfig(input = {}, gatewayConfig = readVolcGatewayConfig()) {
    const tts = gatewayConfig.tts || {};
    const provider = normalizeString(input.provider, normalizeString(process.env.VOICE_PROMPT_PROVIDER, DEFAULT_WAKE_PROMPT_PROVIDER));
    const voiceId = normalizeString(input.voice_id, normalizeString(tts.voice, "server_prompt_v1"));
    const speakerId = normalizeString(input.speaker_id, "");

    const rawFormat = normalizeString(input.format, DEFAULT_WAKE_PROMPT_FORMAT);
    const format = rawFormat === DEFAULT_WAKE_PROMPT_AUDIO_FORMAT ? DEFAULT_WAKE_PROMPT_FORMAT : rawFormat;

    return {
        wake_prompt_text: normalizeString(input.wake_prompt_text, DEFAULT_WAKE_PROMPT_TEXT),
        provider,
        voice_id: voiceId,
        speaker_id: speakerId,
        speed: normalizeNumber(input.speed, 1.0),
        pitch: normalizeNumber(input.pitch, 1.0),
        volume: normalizeNumber(input.volume, 1.0),
        sample_rate: normalizeInteger(input.sample_rate, tts.sampleRate || DEFAULT_WAKE_PROMPT_SAMPLE_RATE),
        format,
        channels: normalizeInteger(input.channels, DEFAULT_WAKE_PROMPT_CHANNELS),
        updated_at_ms: normalizeInteger(input.updated_at_ms, Date.now())
    };
}

function hashPayload(config) {
    return {
        wake_prompt_text: config.wake_prompt_text,
        provider: config.provider,
        voice_id: config.voice_id,
        speaker_id: config.speaker_id,
        speed: config.speed,
        pitch: config.pitch,
        volume: config.volume,
        sample_rate: config.sample_rate,
        format: config.format,
        channels: config.channels
    };
}

function computeVoiceConfigHash(config) {
    const json = JSON.stringify(hashPayload(config));
    return crypto.createHash("sha256").update(json).digest("hex");
}

function buildPromptVersion(config) {
    return `wake:${computeVoiceConfigHash(config).slice(0, 16)}:${config.updated_at_ms}`;
}

function enrichPromptConfig(config) {
    const normalized = normalizePromptConfig(config);
    const voiceConfigHash = computeVoiceConfigHash(normalized);
    return {
        ...normalized,
        prompt_version: buildPromptVersion(normalized),
        voice_config_hash: voiceConfigHash
    };
}

function readVoicePromptConfig() {
    loadPromptConfigFromDisk();
    const gatewayConfig = readVolcGatewayConfig();
    const merged = {
        ...promptConfigState,
        provider: normalizeString(promptConfigState.provider, DEFAULT_WAKE_PROMPT_PROVIDER),
        voice_id: normalizeString(promptConfigState.voice_id, normalizeString(gatewayConfig.tts.voice, "server_prompt_v1")),
        sample_rate: promptConfigState.sample_rate || gatewayConfig.tts.sampleRate,
        format: promptConfigState.format || DEFAULT_WAKE_PROMPT_FORMAT,
        channels: promptConfigState.channels || DEFAULT_WAKE_PROMPT_CHANNELS
    };
    return enrichPromptConfig(merged);
}

function updateVoicePromptConfig(patch = {}) {
    loadPromptConfigFromDisk();
    const allowed = [
        "wake_prompt_text",
        "provider",
        "voice_id",
        "speaker_id",
        "speed",
        "pitch",
        "volume",
        "sample_rate",
        "format",
        "channels"
    ];
    const next = { ...promptConfigState };
    for (const key of allowed) {
        if (Object.prototype.hasOwnProperty.call(patch, key)) {
            next[key] = patch[key];
        }
    }
    next.updated_at_ms = Date.now();
    promptConfigState = normalizePromptConfig(next);
    savePromptConfigToDisk(promptConfigState);
    return readVoicePromptConfig();
}

function promptConfigMatches(cacheMeta, config) {
    return !!cacheMeta &&
        cacheMeta.voice_config_hash === config.voice_config_hash &&
        cacheMeta.prompt_version === config.prompt_version &&
        cacheMeta.text === config.wake_prompt_text &&
        cacheMeta.provider === config.provider &&
        cacheMeta.voice_id === config.voice_id &&
        cacheMeta.speaker_id === config.speaker_id &&
        cacheMeta.speed === config.speed &&
        cacheMeta.pitch === config.pitch &&
        cacheMeta.volume === config.volume &&
        cacheMeta.sample_rate === config.sample_rate &&
        cacheMeta.format === config.format &&
        cacheMeta.channels === config.channels;
}

module.exports = {
    DEFAULT_WAKE_PROMPT_AUDIO_FORMAT,
    DEFAULT_WAKE_PROMPT_CHANNELS,
    DEFAULT_WAKE_PROMPT_FORMAT,
    DEFAULT_WAKE_PROMPT_SAMPLE_RATE,
    DEFAULT_WAKE_PROMPT_TEXT,
    computeVoiceConfigHash,
    promptConfigMatches,
    readVoicePromptConfig,
    updateVoicePromptConfig
};
