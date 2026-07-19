const fs = require("fs");
const path = require("path");
const crypto = require("crypto");
const {
    isSilentPcmBuffer
} = require("./ttsAudio");

const DEFAULT_PROMPT_KEY = "wake_ack_zh";
const PROMPT_CACHE_FORMAT = "pcm_s16le_mono_16k";
const PROMPT_CACHE_CONTENT_TYPE = "audio/L16; rate=16000; channels=1";
const PROMPT_CACHE_SAMPLE_RATE = 16000;
const PROMPT_CACHE_CHANNELS = 1;
const PROMPT_CACHE_ENCODING = "s16le";

function safePromptKey(value) {
    const key = String(value || DEFAULT_PROMPT_KEY).trim();
    if (!/^[a-zA-Z0-9_.-]{1,80}$/.test(key)) {
        return DEFAULT_PROMPT_KEY;
    }

    return key;
}

function promptPaths(promptKey) {
    const safeKey = safePromptKey(promptKey);
    const cacheDir = process.env.VOICE_PROMPT_CACHE_DIR
        ? path.resolve(process.env.VOICE_PROMPT_CACHE_DIR)
        : path.join(__dirname, "..", "..", "cache", "voice_prompts");
    return {
        promptKey: safeKey,
        cacheDir,
        pcmPath: path.join(cacheDir, `${safeKey}_16000_s16le.pcm`),
        metaPath: path.join(cacheDir, `${safeKey}_16000_s16le.json`)
    };
}

function sha256(buffer) {
    return `sha256:${crypto.createHash("sha256").update(buffer).digest("hex")}`;
}

function readJson(filePath) {
    try {
        return JSON.parse(fs.readFileSync(filePath, "utf8"));
    } catch (_) {
        return null;
    }
}

function validatePromptPcm(buffer) {
    return Buffer.isBuffer(buffer) &&
        buffer.length > 0 &&
        buffer.length % 2 === 0 &&
        !isSilentPcmBuffer(buffer);
}

function readPromptCache(promptKey = DEFAULT_PROMPT_KEY) {
    const paths = promptPaths(promptKey);
    if (!fs.existsSync(paths.pcmPath) || !fs.existsSync(paths.metaPath)) {
        return null;
    }

    const pcm = fs.readFileSync(paths.pcmPath);
    const meta = readJson(paths.metaPath);
    if (!validatePromptPcm(pcm) || !meta || meta.enabled === false) {
        return null;
    }
    if (meta.byte_length !== pcm.length ||
        meta.audio_format !== PROMPT_CACHE_FORMAT ||
        meta.sample_rate !== PROMPT_CACHE_SAMPLE_RATE ||
        meta.channels !== PROMPT_CACHE_CHANNELS) {
        return null;
    }
    if (meta.checksum && meta.checksum !== sha256(pcm)) {
        return null;
    }

    return {
        promptKey: paths.promptKey,
        pcm,
        meta
    };
}

function writePromptCache(promptKey, text, pcm, extra = {}) {
    if (!validatePromptPcm(pcm)) {
        const error = new Error("Prompt PCM is empty, odd length, or silent");
        error.code = "VOICE_PROMPT_CACHE_INVALID_PCM";
        throw error;
    }

    const paths = promptPaths(promptKey);
    fs.mkdirSync(paths.cacheDir, { recursive: true });
    const now = new Date().toISOString();
    const checksum = sha256(pcm);
    const meta = {
        prompt_key: paths.promptKey,
        text,
        locale: "zh-CN",
        audio_format: PROMPT_CACHE_FORMAT,
        sample_rate: PROMPT_CACHE_SAMPLE_RATE,
        channels: PROMPT_CACHE_CHANNELS,
        encoding: PROMPT_CACHE_ENCODING,
        file_path: path.relative(path.join(__dirname, "..", ".."), paths.pcmPath),
        content_type: PROMPT_CACHE_CONTENT_TYPE,
        byte_length: pcm.length,
        version: extra.prompt_version || `${paths.promptKey}:v1`,
        prompt_version: extra.prompt_version || `${paths.promptKey}:v1`,
        voice_config_hash: extra.voice_config_hash || "",
        checksum,
        created_at: extra.created_at || now,
        updated_at: now,
        enabled: true,
        provider: extra.provider || extra.tts_provider || "",
        voice_id: extra.voice_id || extra.tts_voice || "",
        speaker_id: extra.speaker_id || "",
        speed: Number.isFinite(extra.speed) ? extra.speed : 1.0,
        pitch: Number.isFinite(extra.pitch) ? extra.pitch : 1.0,
        volume: Number.isFinite(extra.volume) ? extra.volume : 1.0,
        format: extra.format || PROMPT_CACHE_ENCODING,
        tts_provider: extra.tts_provider || extra.provider || "",
        tts_voice: extra.tts_voice || extra.voice_id || ""
    };
    const tmpPcm = `${paths.pcmPath}.tmp`;
    const tmpMeta = `${paths.metaPath}.tmp`;
    fs.writeFileSync(tmpPcm, pcm);
    fs.writeFileSync(tmpMeta, `${JSON.stringify(meta, null, 2)}\n`);
    fs.renameSync(tmpPcm, paths.pcmPath);
    fs.renameSync(tmpMeta, paths.metaPath);
    return {
        promptKey: paths.promptKey,
        pcm,
        meta
    };
}

function sendPromptCachePcm(res, cacheRecord, cacheState, serverTimeMs = Date.now()) {
    const meta = cacheRecord.meta || {};
    res
        .status(200)
        .set({
            "Content-Type": PROMPT_CACHE_CONTENT_TYPE,
            "X-Prompt-Key": cacheRecord.promptKey,
            "X-Prompt-Cache": cacheState,
            "X-Audio-Format": PROMPT_CACHE_FORMAT,
            "X-Audio-Sample-Rate": String(PROMPT_CACHE_SAMPLE_RATE),
            "X-Audio-Channels": String(PROMPT_CACHE_CHANNELS),
            "X-Audio-Version": meta.prompt_version || meta.version || "",
            "X-Prompt-Version": meta.prompt_version || meta.version || "",
            "X-Voice-Config-Hash": meta.voice_config_hash || "",
            "X-Sample-Rate": String(PROMPT_CACHE_SAMPLE_RATE),
            "X-Channels": String(PROMPT_CACHE_CHANNELS),
            "X-Server-Time-Ms": String(serverTimeMs),
            "Cache-Control": "public, max-age=86400",
            "X-Content-Type-Options": "nosniff"
        })
        .end(cacheRecord.pcm);
}

module.exports = {
    DEFAULT_PROMPT_KEY,
    PROMPT_CACHE_CONTENT_TYPE,
    PROMPT_CACHE_FORMAT,
    readPromptCache,
    safePromptKey,
    sendPromptCachePcm,
    writePromptCache
};
