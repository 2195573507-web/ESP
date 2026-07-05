const {
    normalizeLogPreview
} = require("../utils/logging");
const {
    createVoiceStageError
} = require("./errors");
const {
    findStringField
} = require("./payloadUtils");

function parseRealtimeJsonMessage(message, stage) {
    try {
        return JSON.parse(message);
    } catch (error) {
        throw createVoiceStageError(stage, `VOICE_${stage.toUpperCase()}_FAILED`, "Realtime WebSocket response was not valid JSON", 502, {
            bodyLength: message.length,
            bodyPreview: normalizeLogPreview(message),
            cause: error
        });
    }
}

function readRealtimeEventName(payload) {
    return typeof payload?.type === "string" && payload.type
        ? payload.type
        : (typeof payload?.event === "string" ? payload.event : "");
}

function extractRealtimeErrorMessage(payload) {
    const errorValue = payload?.error;
    if (typeof errorValue === "string" && errorValue.trim()) {
        return errorValue.trim();
    }

    if (errorValue && typeof errorValue === "object") {
        return findStringField(errorValue, ["message", "code", "error"]);
    }

    return findStringField(payload, ["message", "code"]);
}

function parseAsrRealtimeEvent(message) {
    const payload = parseRealtimeJsonMessage(message, "asr");
    const eventName = readRealtimeEventName(payload);
    const lowerName = eventName.toLowerCase();
    const text = findStringField(payload, [
        "text",
        "asr_text",
        "transcript",
        "utterance",
        "result_text",
        "final_text",
        "content",
        "delta"
    ]);

    return {
        eventName,
        text,
        isError: lowerName.includes("error") || Boolean(payload?.error),
        errorMessage: extractRealtimeErrorMessage(payload),
        isFinal: lowerName.includes("final") ||
            lowerName.includes("completed") ||
            lowerName.includes("conversation.item.input_audio_transcription.completed") ||
            lowerName.includes("transcription.done") ||
            payload?.final === true,
        isPartial: lowerName.includes("partial") ||
            lowerName.includes("conversation.item.input_audio_transcription.result") ||
            lowerName.includes("delta") ||
            lowerName.includes("transcription.delta")
    };
}

function parseTtsRealtimeEvent(message) {
    const payload = parseRealtimeJsonMessage(message, "tts");
    const eventName = readRealtimeEventName(payload);
    const lowerName = eventName.toLowerCase();
    const delta = typeof payload?.delta === "string" ? payload.delta : findStringField(payload, [
        "audio_base64",
        "pcm_base64",
        "audio",
        "data"
    ]);

    return {
        eventName,
        delta,
        isError: lowerName.includes("error") || Boolean(payload?.error),
        errorMessage: extractRealtimeErrorMessage(payload),
        isSessionUpdated: lowerName === "tts_session.updated",
        isAudioDelta: lowerName === "response.audio.delta" || lowerName.includes("audio.delta"),
        isAudioDone: lowerName === "response.audio.done" ||
            lowerName.includes("audio.done") ||
            lowerName.includes("completed")
    };
}

function buildAsrSessionUpdate(config) {
    return JSON.stringify({
        type: "transcription_session.update",
        session: {
            input_audio_format: config.asr.format,
            input_audio_codec: config.asr.codec,
            input_audio_sample_rate: config.asr.sampleRate,
            input_audio_bits: config.asr.bits,
            input_audio_channel: config.asr.channels,
            input_audio_transcription: {
                model: config.asr.model
            }
        }
    });
}

function buildTtsSessionUpdate(config) {
    return JSON.stringify({
        type: "tts_session.update",
        session: {
            voice: config.tts.voice,
            output_audio_format: "pcm",
            output_audio_sample_rate: config.tts.sampleRate,
            speed: config.tts.speed,
            pitch: config.tts.pitch,
            volume: config.tts.volume,
            text_to_speech: {
                model: config.tts.model
            }
        }
    });
}

module.exports = {
    buildAsrSessionUpdate,
    buildTtsSessionUpdate,
    parseAsrRealtimeEvent,
    parseTtsRealtimeEvent
};
