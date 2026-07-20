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

const ASR_IGNORED_EVENT_NAMES = new Set([
    "session.updated",
    "input_audio_buffer.committed",
    "input_audio_buffer.cleared",
    "response.created",
    "response.done"
]);

function isAsrTranscriptionCompletedEvent(lowerName) {
    return lowerName === "conversation.item.input_audio_transcription.completed" ||
        lowerName === "input_audio_transcription.completed" ||
        lowerName === "transcription.completed" ||
        lowerName === "transcription.done" ||
        (lowerName.includes("transcription") && (
            lowerName.endsWith(".completed") ||
            lowerName.endsWith(".done")
        ));
}

function isAckLikeEvent(lowerName) {
    return lowerName.includes("ack") ||
        lowerName.endsWith(".committed") ||
        lowerName.endsWith(".cleared");
}

function isIgnoredAsrRealtimeEvent(lowerName, hasTranscriptOrText) {
    if (ASR_IGNORED_EVENT_NAMES.has(lowerName)) {
        return true;
    }

    return !hasTranscriptOrText && (
        lowerName.includes("completed") ||
        lowerName.endsWith(".done") ||
        isAckLikeEvent(lowerName)
    );
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
    const transcript = findStringField(payload, ["transcript"]);
    const directText = findStringField(payload, ["text"]);
    const transcriptOrText = transcript || directText;
    const ignored = isIgnoredAsrRealtimeEvent(lowerName, Boolean(transcriptOrText));
    const text = ignored
        ? ""
        : (transcriptOrText || findStringField(payload, [
            "asr_text",
            "utterance",
            "result_text",
            "final_text",
            "content",
            "delta"
        ]));
    const isTranscriptionCompleted = isAsrTranscriptionCompletedEvent(lowerName);

    return {
        eventName,
        text,
        hasTranscript: Boolean(transcript),
        hasText: Boolean(directText),
        isError: lowerName.includes("error") || Boolean(payload?.error),
        errorMessage: extractRealtimeErrorMessage(payload),
        isFinal: !ignored && isTranscriptionCompleted && Boolean(transcriptOrText),
        isPartial: !ignored && (
            lowerName.includes("partial") ||
            lowerName.includes("conversation.item.input_audio_transcription.result") ||
            lowerName.includes("delta") ||
            lowerName.includes("transcription.delta")
        )
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

    const isAudioDone = lowerName === "response.audio.done" ||
        lowerName === "audio.done" ||
        lowerName.endsWith(".audio.done") ||
        lowerName === "response.completed" ||
        lowerName === "response.done" ||
        (lowerName.includes("audio") && lowerName.includes("completed")) ||
        (lowerName.startsWith("response.") && lowerName.endsWith(".completed"));

    return {
        eventName,
        delta,
        isError: lowerName.includes("error") || Boolean(payload?.error),
        errorMessage: extractRealtimeErrorMessage(payload),
        isSessionUpdated: lowerName === "tts_session.updated",
        isAudioDelta: lowerName === "response.audio.delta" || lowerName.includes("audio.delta"),
        isAudioDone
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
