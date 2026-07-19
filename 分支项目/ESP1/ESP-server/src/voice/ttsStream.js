const {
    createVoiceStageError
} = require("./errors");
const {
    decodeBase64Buffer
} = require("./payloadUtils");
const {
    Pcm16Aligner,
    RawPcm16StreamGate
} = require("./pcmAligner");
const {
    BoundedPcmQueue
} = require("./pcmQueue");

function abortError(reason) {
    return createVoiceStageError("tts", "VOICE_TTS_ABORTED", reason || "TTS stream aborted", 502);
}

function normalizeStreamError(error) {
    if (error?.code) {
        return error;
    }
    if (error?.name === "AbortError") {
        return abortError("TTS stream aborted or timed out");
    }
    return createVoiceStageError("tts", "VOICE_TTS_FAILED", error?.message || "TTS stream failed", 502, {
        cause: error
    });
}

function createVoiceTtsPcmStream(options = {}) {
    const provider = options.provider || "volcengine";
    const queue = new BoundedPcmQueue(options.limits);
    const aligner = new Pcm16Aligner();
    const formatGate = new RawPcm16StreamGate();
    const events = options.events;
    const signal = options.signal;
    const maxTotalBytes = Number.isSafeInteger(options.limits?.maxTotalBytes) && options.limits.maxTotalBytes > 0
        ? options.limits.maxTotalBytes
        : 4 * 1024 * 1024;
    let abortRequested = false;
    let abortUpstreamCalled = false;

    if (!events || typeof events[Symbol.asyncIterator] !== "function") {
        throw new TypeError("events must be an AsyncIterable");
    }

    const abortUpstream = () => {
        if (abortUpstreamCalled) {
            return;
        }
        abortUpstreamCalled = true;
        try {
            options.abortUpstream?.();
        } catch (_) {
            // The local queue has already transitioned to a terminal failure state.
        }
    };

    const abort = (reason = "TTS stream aborted") => {
        if (abortRequested) {
            return false;
        }
        abortRequested = true;
        aligner.reset();
        formatGate.reset();
        queue.fail(abortError(reason));
        abortUpstream();
        return true;
    };

    const onAbort = () => abort("TTS stream aborted by client or timeout");
    if (signal) {
        signal.addEventListener("abort", onAbort, { once: true });
    }

    const completion = (async () => {
        let bytesGenerated = 0;
        let chunksGenerated = 0;

        const enqueueAlignedPcm = chunk => {
            const aligned = aligner.push(chunk);
            if (!aligned) {
                return;
            }
            if (bytesGenerated + aligned.length > maxTotalBytes) {
                throw createVoiceStageError("tts", "VOICE_TTS_MAX_BYTES", "TTS PCM stream exceeded its byte limit", 502, {
                    bytes: bytesGenerated + aligned.length
                });
            }
            queue.push(aligned);
            bytesGenerated += aligned.length;
            chunksGenerated += 1;
        };

        try {
            if (signal?.aborted) {
                abortRequested = true;
                throw abortError("TTS stream was already aborted");
            }

            for await (const event of events) {
                if (abortRequested || signal?.aborted) {
                    abortRequested = true;
                    throw abortError("TTS stream aborted while reading provider events");
                }
                if (!event) {
                    continue;
                }
                if (event.isError) {
                    throw createVoiceStageError("tts", "VOICE_TTS_FAILED", event.errorMessage || "TTS provider returned an error", 502);
                }

                if (event.isAudioDelta) {
                    const decoded = decodeBase64Buffer(event.delta);
                    if (!decoded) {
                        throw createVoiceStageError("tts", "VOICE_TTS_FAILED", "TTS audio delta was not valid base64 PCM", 502);
                    }
                    const rawPcm = formatGate.push(decoded);
                    if (rawPcm) {
                        enqueueAlignedPcm(rawPcm);
                    }
                }

                if (event.isAudioDone) {
                    const finalRawPcm = formatGate.finish();
                    if (finalRawPcm) {
                        enqueueAlignedPcm(finalRawPcm);
                    }
                    aligner.finish();
                    if (bytesGenerated === 0) {
                        throw createVoiceStageError("tts", "VOICE_TTS_EMPTY_AUDIO", "TTS stream completed without PCM audio", 502);
                    }
                    queue.finish();
                    return {
                        status: "completed",
                        provider,
                        bytesGenerated,
                        chunksGenerated,
                        failureReason: null,
                        queue: queue.stats()
                    };
                }
            }

            throw createVoiceStageError("tts", "VOICE_TTS_INCOMPLETE", "TTS provider closed before audio completion", 502);
        } catch (error) {
            const normalizedError = normalizeStreamError(error);
            aligner.reset();
            formatGate.reset();
            queue.fail(normalizedError);
            abortUpstream();
            return {
                status: abortRequested || normalizedError.code === "VOICE_TTS_ABORTED" ? "aborted" : "failed",
                provider,
                bytesGenerated,
                chunksGenerated,
                failureReason: normalizedError.code || "VOICE_TTS_FAILED",
                error: normalizedError,
                queue: queue.stats()
            };
        } finally {
            if (signal) {
                signal.removeEventListener("abort", onAbort);
            }
        }
    })();

    return {
        metadata: {
            provider,
            sampleRate: 16000,
            channels: 1,
            format: "pcm_s16le_mono_16k",
            streamingCapable: true
        },
        stream: queue,
        completion,
        abort
    };
}

module.exports = {
    createVoiceTtsPcmStream
};
