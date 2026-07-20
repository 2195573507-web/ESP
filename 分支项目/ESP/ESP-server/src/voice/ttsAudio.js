const {
    normalizeLogPreview
} = require("../utils/logging");
const {
    VOICE_TURN_SAMPLE_RATE
} = require("./http");
const {
    createVoiceStageError
} = require("./errors");
const {
    decodeBase64Buffer,
    findStringField
} = require("./payloadUtils");

function contentTypeIndicatesUnsupportedAudio(contentType) {
    const normalized = String(contentType || "").toLowerCase();
    return normalized.includes("audio/mpeg") ||
        normalized.includes("audio/mp3") ||
        normalized.includes("audio/ogg") ||
        normalized.includes("audio/flac") ||
        normalized.includes("audio/aac") ||
        normalized.includes("audio/mp4") ||
        normalized.includes("audio/webm") ||
        normalized.includes("application/ogg") ||
        normalized.includes("video/mp4") ||
        normalized.includes("video/webm");
}

function contentTypeIndicatesJson(contentType) {
    const normalized = String(contentType || "").toLowerCase();
    return normalized.includes("application/json") || normalized.includes("+json");
}

function bufferLooksLikeJson(buffer) {
    if (!Buffer.isBuffer(buffer) || buffer.length === 0) {
        return false;
    }

    const trimmedStart = buffer.toString("utf8", 0, Math.min(buffer.length, 32)).trimStart();
    return trimmedStart.startsWith("{") || trimmedStart.startsWith("[");
}

function bufferLooksLikeUnsupportedAudio(buffer) {
    if (!Buffer.isBuffer(buffer) || buffer.length < 4) {
        return false;
    }

    return buffer.toString("ascii", 0, 3) === "ID3" ||
        buffer.toString("ascii", 0, 4) === "OggS" ||
        buffer.toString("ascii", 0, 4) === "fLaC" ||
        (buffer[0] === 0x1a && buffer[1] === 0x45 && buffer[2] === 0xdf && buffer[3] === 0xa3) ||
        buffer.toString("ascii", 4, 8) === "ftyp" ||
        (buffer[0] === 0xff && (buffer[1] & 0xe0) === 0xe0);
}

function extractPcmFromWav(buffer) {
    let fmt = null;
    let dataStart = -1;
    let dataEnd = -1;
    let offset = 12;

    while (offset + 8 <= buffer.length) {
        const chunkId = buffer.toString("ascii", offset, offset + 4);
        const chunkSize = buffer.readUInt32LE(offset + 4);
        const chunkStart = offset + 8;
        const chunkEnd = chunkStart + chunkSize;

        if (chunkEnd > buffer.length) {
            throw createVoiceStageError("tts", "VOICE_TTS_FAILED", "TTS WAV response is truncated", 502);
        }

        if (chunkId === "fmt ") {
            if (chunkSize < 16) {
                throw createVoiceStageError("tts", "VOICE_TTS_FAILED", "TTS WAV fmt chunk is invalid", 502);
            }

            fmt = {
                audioFormat: buffer.readUInt16LE(chunkStart),
                channels: buffer.readUInt16LE(chunkStart + 2),
                sampleRate: buffer.readUInt32LE(chunkStart + 4),
                bitsPerSample: buffer.readUInt16LE(chunkStart + 14)
            };
        } else if (chunkId === "data") {
            dataStart = chunkStart;
            dataEnd = chunkEnd;
        }

        offset = chunkEnd + (chunkSize % 2);
    }

    if (!fmt || dataStart < 0) {
        throw createVoiceStageError("tts", "VOICE_TTS_FAILED", "TTS WAV response is missing fmt or data chunk", 502);
    }

    if (fmt.audioFormat !== 1 ||
        fmt.channels !== 1 ||
        fmt.sampleRate !== VOICE_TURN_SAMPLE_RATE ||
        fmt.bitsPerSample !== 16) {
        throw createVoiceStageError(
            "tts",
            "VOICE_TTS_FAILED",
            "TTS WAV response must be PCM s16le mono 16kHz",
            502
        );
    }

    return buffer.subarray(dataStart, dataEnd);
}

function normalizeTtsPcmBuffer(buffer, contentType = "") {
    if (!Buffer.isBuffer(buffer) || buffer.length === 0) {
        throw createVoiceStageError("tts", "VOICE_TTS_FAILED", "TTS response did not contain audio", 502);
    }

    if (contentTypeIndicatesJson(contentType)) {
        return extractTtsPcmFromJson(buffer.toString("utf8"));
    }

    if (bufferLooksLikeJson(buffer)) {
        const pcmFromJson = maybeExtractTtsPcmFromJson(buffer);
        if (pcmFromJson) {
            return pcmFromJson;
        }
    }

    if (contentTypeIndicatesUnsupportedAudio(contentType) || bufferLooksLikeUnsupportedAudio(buffer)) {
        throw createVoiceStageError("tts", "VOICE_TTS_UNSUPPORTED_FORMAT", "TTS response must be raw PCM or WAV PCM s16le mono 16kHz", 502, {
            bodyLength: buffer.length
        });
    }

    let pcmBuffer = buffer;
    if (buffer.length >= 12 &&
        buffer.toString("ascii", 0, 4) === "RIFF" &&
        buffer.toString("ascii", 8, 12) === "WAVE") {
        pcmBuffer = extractPcmFromWav(buffer);
    }

    if (pcmBuffer.length === 0 || pcmBuffer.length % 2 !== 0) {
        throw createVoiceStageError("tts", "VOICE_TTS_FAILED", "TTS PCM response must be non-empty s16le data", 502);
    }

    return pcmBuffer;
}

function isSilentPcmBuffer(pcmBuffer) {
    if (!Buffer.isBuffer(pcmBuffer) || pcmBuffer.length === 0) {
        return true;
    }

    for (let offset = 0; offset < pcmBuffer.length; offset += 1) {
        if (pcmBuffer[offset] !== 0) {
            return false;
        }
    }

    return true;
}

function extractTtsPcmFromJson(responseBody) {
    let payload;
    try {
        payload = JSON.parse(responseBody);
    } catch (error) {
        throw createVoiceStageError("tts", "VOICE_TTS_FAILED", "TTS JSON response could not be parsed", 502, {
            bodyLength: responseBody.length,
            bodyPreview: normalizeLogPreview(responseBody),
            cause: error
        });
    }

    const encodedAudio = findStringField(payload, [
        "pcm_base64",
        "audio_base64",
        "pcm",
        "audio",
        "data"
    ]);
    const decoded = decodeBase64Buffer(encodedAudio);
    if (!decoded) {
        throw createVoiceStageError("tts", "VOICE_TTS_FAILED", "TTS JSON response did not contain base64 PCM audio", 502, {
            bodyLength: responseBody.length,
            bodyPreview: normalizeLogPreview(responseBody)
        });
    }

    return normalizeTtsPcmBuffer(decoded);
}

function maybeExtractTtsPcmFromJson(responseBuffer) {
    const responseBody = responseBuffer.toString("utf8");
    const preview = responseBody.trimStart();
    const trimmed = preview.trimEnd();
    const looksLikeJson = (trimmed.startsWith("{") && trimmed.endsWith("}")) ||
        (trimmed.startsWith("[") && trimmed.endsWith("]"));
    if (!looksLikeJson) {
        return null;
    }

    return extractTtsPcmFromJson(responseBody);
}

module.exports = {
    extractTtsPcmFromJson,
    isSilentPcmBuffer,
    maybeExtractTtsPcmFromJson,
    normalizeTtsPcmBuffer
};
