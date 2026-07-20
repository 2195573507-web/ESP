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
const EPHEMERAL_PROMPT_PREFIX = "home_ai_text_";
const DEFAULT_EPHEMERAL_TTL_MS = 15 * 60 * 1000;
const DEFAULT_EPHEMERAL_MAX_ENTRIES = 16;
const DEFAULT_EPHEMERAL_MAX_BYTES = 8 * 1024 * 1024;
const DEFAULT_EPHEMERAL_MAX_ITEM_BYTES = 2 * 1024 * 1024;

const ephemeralPromptCache = new Map();
let ephemeralPromptCacheBytes = 0;

function boundedIntegerEnv(name, fallback, min, max) {
    const parsed = Number.parseInt(process.env[name] || "", 10);
    if (!Number.isSafeInteger(parsed)) {
        return fallback;
    }
    return Math.max(min, Math.min(max, parsed));
}

function ephemeralPromptPolicy() {
    const maxBytes = boundedIntegerEnv(
        "VOICE_DYNAMIC_PROMPT_CACHE_MAX_BYTES",
        DEFAULT_EPHEMERAL_MAX_BYTES,
        64 * 1024,
        32 * 1024 * 1024
    );
    return {
        ttlMs: boundedIntegerEnv(
            "VOICE_DYNAMIC_PROMPT_CACHE_TTL_MS",
            DEFAULT_EPHEMERAL_TTL_MS,
            1000,
            24 * 60 * 60 * 1000
        ),
        maxEntries: boundedIntegerEnv(
            "VOICE_DYNAMIC_PROMPT_CACHE_MAX_ENTRIES",
            DEFAULT_EPHEMERAL_MAX_ENTRIES,
            1,
            64
        ),
        maxBytes,
        maxItemBytes: Math.min(maxBytes, boundedIntegerEnv(
            "VOICE_DYNAMIC_PROMPT_CACHE_MAX_ITEM_BYTES",
            DEFAULT_EPHEMERAL_MAX_ITEM_BYTES,
            32 * 1024,
            8 * 1024 * 1024
        ))
    };
}

function safePromptKey(value) {
    const key = String(value || DEFAULT_PROMPT_KEY).trim();
    if (!/^[a-zA-Z0-9_.-]{1,80}$/.test(key)) {
        return DEFAULT_PROMPT_KEY;
    }

    return key;
}

function promptKeyForText(text) {
    const normalized = String(text || "").trim();
    const digest = crypto.createHash("sha256").update(normalized, "utf8").digest("hex").slice(0, 32);
    return `home_ai_text_${digest}`;
}

function isEphemeralPromptKey(promptKey) {
    return safePromptKey(promptKey).startsWith(EPHEMERAL_PROMPT_PREFIX);
}

function removeEphemeralPrompt(promptKey) {
    const existing = ephemeralPromptCache.get(promptKey);
    if (!existing) {
        return false;
    }
    ephemeralPromptCache.delete(promptKey);
    ephemeralPromptCacheBytes = Math.max(0, ephemeralPromptCacheBytes - existing.pcm.length);
    return true;
}

function pruneEphemeralPromptCache(nowMs = Date.now(), policy = ephemeralPromptPolicy()) {
    for (const [promptKey, record] of ephemeralPromptCache.entries()) {
        if (record.expiresAtMs <= nowMs) {
            removeEphemeralPrompt(promptKey);
        }
    }
    while (ephemeralPromptCache.size > policy.maxEntries ||
           ephemeralPromptCacheBytes > policy.maxBytes) {
        const oldestKey = ephemeralPromptCache.keys().next().value;
        if (!oldestKey) {
            break;
        }
        removeEphemeralPrompt(oldestKey);
    }
}

function clearEphemeralPromptCache() {
    ephemeralPromptCache.clear();
    ephemeralPromptCacheBytes = 0;
}

function getEphemeralPromptCacheStats(nowMs = Date.now()) {
    const policy = ephemeralPromptPolicy();
    pruneEphemeralPromptCache(nowMs, policy);
    return {
        entries: ephemeralPromptCache.size,
        bytes: ephemeralPromptCacheBytes,
        ttl_ms: policy.ttlMs,
        max_entries: policy.maxEntries,
        max_bytes: policy.maxBytes,
        max_item_bytes: policy.maxItemBytes
    };
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

function readPromptCache(promptKey = DEFAULT_PROMPT_KEY, nowMs = Date.now()) {
    const paths = promptPaths(promptKey);
    if (isEphemeralPromptKey(paths.promptKey)) {
        const policy = ephemeralPromptPolicy();
        pruneEphemeralPromptCache(nowMs, policy);
        const record = ephemeralPromptCache.get(paths.promptKey);
        if (!record || record.expiresAtMs <= nowMs) {
            removeEphemeralPrompt(paths.promptKey);
            return null;
        }
        ephemeralPromptCache.delete(paths.promptKey);
        ephemeralPromptCache.set(paths.promptKey, record);
        return record;
    }
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

function writePromptCache(promptKey, text, pcm, extra = {}, nowMs = Date.now()) {
    if (!validatePromptPcm(pcm)) {
        const error = new Error("Prompt PCM is empty, odd length, or silent");
        error.code = "VOICE_PROMPT_CACHE_INVALID_PCM";
        throw error;
    }

    const paths = promptPaths(promptKey);
    const ephemeral = isEphemeralPromptKey(paths.promptKey);
    const now = new Date(nowMs).toISOString();
    const checksum = sha256(pcm);
    const policy = ephemeralPromptPolicy();
    const expiresAtMs = ephemeral ? nowMs + policy.ttlMs : null;
    const meta = {
        prompt_key: paths.promptKey,
        text,
        locale: "zh-CN",
        audio_format: PROMPT_CACHE_FORMAT,
        sample_rate: PROMPT_CACHE_SAMPLE_RATE,
        channels: PROMPT_CACHE_CHANNELS,
        encoding: PROMPT_CACHE_ENCODING,
        file_path: ephemeral ? "" : path.relative(path.join(__dirname, "..", ".."), paths.pcmPath),
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
        tts_voice: extra.tts_voice || extra.voice_id || "",
        ephemeral,
        expires_at: expiresAtMs === null ? null : new Date(expiresAtMs).toISOString()
    };

    if (ephemeral) {
        removeEphemeralPrompt(paths.promptKey);
        const record = {
            promptKey: paths.promptKey,
            pcm,
            meta,
            expiresAtMs
        };
        if (pcm.length <= policy.maxItemBytes && pcm.length <= policy.maxBytes) {
            ephemeralPromptCache.set(paths.promptKey, record);
            ephemeralPromptCacheBytes += pcm.length;
            pruneEphemeralPromptCache(nowMs, policy);
        }
        return record;
    }

    fs.mkdirSync(paths.cacheDir, { recursive: true });
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
    const cacheControl = meta.ephemeral
        ? "private, no-store, max-age=0"
        : "public, max-age=86400";
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
            "Cache-Control": cacheControl,
            "X-Content-Type-Options": "nosniff"
        })
        .end(cacheRecord.pcm);
}

module.exports = {
    DEFAULT_PROMPT_KEY,
    PROMPT_CACHE_CONTENT_TYPE,
    PROMPT_CACHE_FORMAT,
    clearEphemeralPromptCache,
    getEphemeralPromptCacheStats,
    isEphemeralPromptKey,
    promptKeyForText,
    readPromptCache,
    safePromptKey,
    sendPromptCachePcm,
    writePromptCache
};
