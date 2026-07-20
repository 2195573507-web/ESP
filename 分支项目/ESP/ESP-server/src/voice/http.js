const crypto = require("crypto");

const VOICE_TURN_CONTENT_TYPE = "audio/L16; rate=16000; channels=1";
const VOICE_TURN_AUDIO_FORMAT = "pcm_s16le_mono_16k";
const VOICE_TURN_SAMPLE_RATE = 16000;

function parseContentType(value) {
    const parts = String(value || "")
        .split(";")
        .map(part => part.trim())
        .filter(Boolean);
    const mediaType = (parts.shift() || "").toLowerCase();
    const params = {};

    for (const part of parts) {
        const separator = part.indexOf("=");
        if (separator <= 0) {
            continue;
        }

        const key = part.slice(0, separator).trim().toLowerCase();
        const rawValue = part.slice(separator + 1).trim();
        params[key] = rawValue.replace(/^"|"$/g, "");
    }

    return {
        mediaType,
        params
    };
}

function isVoiceTurnContentType(value) {
    const parsed = parseContentType(value);

    return parsed.mediaType === "audio/l16" &&
        parsed.params.rate === "16000" &&
        parsed.params.channels === "1";
}

function isVoiceTurnAudioFormat(value) {
    return String(value || "").trim().toLowerCase() === VOICE_TURN_AUDIO_FORMAT;
}

function readOptionalVoiceDeviceId(req) {
    const value = req.get("device_id") ||
        req.get("Device-Id") ||
        req.get("X-Device-Id") ||
        req.get("X-ESP-Device-Id") ||
        req.get("X-Client-Id") ||
        req.query.device_id;

    return String(value || "").trim();
}

function readVoiceDeviceId(req) {
    const value = readOptionalVoiceDeviceId(req) ||
        req.ip ||
        "unknown";

    return String(value).trim() || "unknown";
}

function readVoiceRequestId(req) {
    const value = req.get("X-Voice-Turn-Id") ||
        req.get("X-Request-Id") ||
        req.query.request_id;
    const requestId = String(value || "").trim();

    if (requestId) {
        return requestId;
    }

    if (typeof crypto.randomUUID === "function") {
        return crypto.randomUUID();
    }

    return crypto.randomBytes(16).toString("hex");
}

function validateVoiceTurnRequest(req) {
    if (!isVoiceTurnContentType(req.get("Content-Type"))) {
        return {
            status: 415,
            code: "VOICE_UNSUPPORTED_CONTENT_TYPE",
            message: `Content-Type must be ${VOICE_TURN_CONTENT_TYPE}`
        };
    }

    if (!isVoiceTurnAudioFormat(req.get("X-Audio-Format"))) {
        return {
            status: 415,
            code: "VOICE_UNSUPPORTED_AUDIO_FORMAT",
            message: `X-Audio-Format must be ${VOICE_TURN_AUDIO_FORMAT}`
        };
    }

    if (!Buffer.isBuffer(req.body)) {
        return {
            status: 400,
            code: "VOICE_BODY_REQUIRED",
            message: "PCM request body is required"
        };
    }

    if (req.body.length === 0) {
        return {
            status: 400,
            code: "VOICE_BODY_EMPTY",
            message: "PCM request body must not be empty"
        };
    }

    if (req.body.length % 2 !== 0) {
        return {
            status: 400,
            code: "VOICE_PCM_ALIGNMENT_INVALID",
            message: "PCM s16le body length must be an even number of bytes"
        };
    }

    return null;
}

function sendVoiceError(res, status, code, message, details = {}) {
    if (res.headersSent) {
        res.end();
        return;
    }

    const headers = {
        "X-Error-Code": code
    };
    const payload = {
        ok: false,
        code,
        error: message
    };

    if (typeof details.upstreamStatus === "number") {
        headers["X-Upstream-Status"] = String(details.upstreamStatus);
        payload.upstream_status = details.upstreamStatus;
    }

    res
        .status(status)
        .set(headers)
        .json(payload);
}

function writeVoiceTurnHeaders(res) {
    res
        .status(200)
        .set({
            "Content-Type": VOICE_TURN_CONTENT_TYPE,
            "X-Audio-Format": VOICE_TURN_AUDIO_FORMAT,
            "Cache-Control": "no-store",
            "X-Content-Type-Options": "nosniff"
        });
}

function sendVoiceTurnPcm(res, pcmBuffer) {
    writeVoiceTurnHeaders(res);
    res.set("Content-Length", String(pcmBuffer.length));
    res.end(pcmBuffer);
}

function createResponseAbortError(message, cause) {
    const error = new Error(message);
    error.name = "AbortError";
    error.cause = cause;
    return error;
}

function attachVoiceStreamWriteState(error, started, bytesWritten, chunksWritten) {
    const state = {
        started,
        bytesWritten,
        chunksWritten
    };
    if (error && typeof error === "object") {
        error.voiceStreamWrite = state;
        return error;
    }

    const wrapped = new Error(String(error || "Voice stream write failed"));
    wrapped.cause = error;
    wrapped.voiceStreamWrite = state;
    return wrapped;
}

function waitForResponseDrain(res, signal) {
    return new Promise((resolve, reject) => {
        let settled = false;
        const cleanup = () => {
            res.off("drain", onDrain);
            res.off("close", onClose);
            res.off("error", onError);
            signal?.removeEventListener("abort", onAbort);
        };
        const finish = error => {
            if (settled) {
                return;
            }
            settled = true;
            cleanup();
            if (error) {
                reject(error);
                return;
            }
            resolve();
        };
        const onDrain = () => finish();
        const onClose = () => finish(createResponseAbortError("Voice response closed before drain"));
        const onError = error => finish(error);
        const onAbort = () => finish(createResponseAbortError("Voice response aborted before drain"));

        if (res.destroyed || res.writableEnded) {
            finish(createResponseAbortError("Voice response closed before drain"));
            return;
        }
        if (signal?.aborted) {
            finish(createResponseAbortError("Voice response aborted before drain"));
            return;
        }

        res.once("drain", onDrain);
        res.once("close", onClose);
        res.once("error", onError);
        signal?.addEventListener("abort", onAbort, { once: true });
    });
}

async function sendVoiceTurnPcmStream(res, stream, signal) {
    let bytesWritten = 0;
    let chunksWritten = 0;
    let started = false;

    try {
        for await (const chunk of stream) {
            if (!Buffer.isBuffer(chunk) || chunk.length === 0 || chunk.length % 2 !== 0) {
                const error = new Error("Voice stream yielded invalid PCM16 chunk");
                error.code = "VOICE_TTS_FAILED";
                error.status = 502;
                throw error;
            }
            if (signal?.aborted || res.destroyed || res.writableEnded) {
                throw createResponseAbortError("Voice response aborted before write");
            }

            if (!started) {
                writeVoiceTurnHeaders(res);
                started = true;
            }

            const accepted = res.write(chunk);
            bytesWritten += chunk.length;
            chunksWritten += 1;
            if (!accepted) {
                await waitForResponseDrain(res, signal);
            }
        }

        if (!started) {
            const error = new Error("TTS stream completed without PCM audio");
            error.code = "VOICE_TTS_EMPTY_AUDIO";
            error.status = 502;
            throw error;
        }

        if (!res.writableEnded && !res.destroyed) {
            res.end();
        }
        return {
            bytesWritten,
            chunksWritten,
            started
        };
    } catch (error) {
        throw attachVoiceStreamWriteState(error, started, bytesWritten, chunksWritten);
    }
}

module.exports = {
    VOICE_TURN_AUDIO_FORMAT,
    VOICE_TURN_CONTENT_TYPE,
    VOICE_TURN_SAMPLE_RATE,
    readOptionalVoiceDeviceId,
    readVoiceDeviceId,
    readVoiceRequestId,
    sendVoiceError,
    sendVoiceTurnPcm,
    sendVoiceTurnPcmStream,
    writeVoiceTurnHeaders,
    validateVoiceTurnRequest
};
