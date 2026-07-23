const {
    requestLlmText
} = require("../llm/textClient");
const {
    buildLlmPrompt
} = require("../services/llmPromptContextService");
const {
    runVoiceAgentConversation
} = require("./agentConversation");
const {
    maskLogValue,
    maskUrlForLog,
    normalizeLogPreview
} = require("../utils/logging");
const {
    VOICE_TURN_CONTENT_TYPE
} = require("./http");
const {
    createUpstreamVoiceStageError,
    createVoiceStageError
} = require("./errors");
const {
    readVoiceLlmTimeoutMs
} = require("./turnConfig");
const {
    decodeBase64Buffer
} = require("./payloadUtils");
const {
    buildVolcGatewayHeaders
} = require("./gatewayHeaders");
const {
    normalizeTtsPcmBuffer
} = require("./ttsAudio");
const {
    openRealtimeWebSocket
} = require("./realtimeSocket");
const {
    buildAsrSessionUpdate,
    buildTtsSessionUpdate,
    parseAsrRealtimeEvent,
    parseTtsRealtimeEvent
} = require("./realtimeEvents");

function logAsrRealtimeDebug(logger, event, finalTextSet, loopWillEnd) {
    const writer = typeof logger?.debug === "function"
        ? logger.debug.bind(logger)
        : logger?.log?.bind(logger);
    if (!writer) {
        return;
    }

    writer(
        `[voice-turn] asr_realtime_debug websocket_event_type=${JSON.stringify(event.eventName || "")} has_transcript=${event.hasTranscript ? "1" : "0"} has_text=${event.hasText ? "1" : "0"} final_text_set=${finalTextSet ? "1" : "0"} asr_loop_end=${loopWillEnd ? "1" : "0"}`
    );
}

async function requestVoiceAsr(audioBuffer, config, voiceConfig, signal, logger = console) {
    let ws = null;
    let finalText = "";

    try {
        ws = await openRealtimeWebSocket(
            config.asr.url,
            buildVolcGatewayHeaders(config, "asr"),
            signal,
            "asr"
        );
        ws.sendText(buildAsrSessionUpdate(config));

        for (let offset = 0; offset < audioBuffer.length; offset += voiceConfig.wsAudioChunkBytes) {
            const chunk = audioBuffer.subarray(offset, Math.min(offset + voiceConfig.wsAudioChunkBytes, audioBuffer.length));
            ws.sendText(JSON.stringify({
                type: "input_audio_buffer.append",
                audio: chunk.toString("base64")
            }));
        }
        ws.sendText(JSON.stringify({
            type: "input_audio_buffer.commit"
        }));

        while (!signal?.aborted) {
            const event = parseAsrRealtimeEvent(await ws.nextMessage(signal));
            if (event.isError) {
                logAsrRealtimeDebug(logger, event, false, true);
                throw createVoiceStageError("asr", "VOICE_ASR_FAILED", event.errorMessage || "ASR Realtime WebSocket returned an error", 502, {
                    endpoint: config.asr.url,
                    model: config.asr.model
                });
            }

            let finalTextSet = false;
            if (event.text) {
                if (event.isFinal) {
                    finalText = event.text;
                    finalTextSet = true;
                }
            }

            logAsrRealtimeDebug(logger, event, finalTextSet, event.isFinal);

            if (event.isFinal) {
                break;
            }
        }

        const text = finalText.trim();
        if (!text) {
            throw createVoiceStageError("asr", "VOICE_ASR_FAILED", "ASR Realtime response did not contain text", 502, {
                endpoint: config.asr.url,
                model: config.asr.model
            });
        }

        return { text };
    } catch (error) {
        if (error?.code) {
            throw error;
        }

        if (error?.name === "AbortError") {
            throw createVoiceStageError("asr", "VOICE_ASR_FAILED", "ASR Realtime request was aborted or timed out", 502, {
                endpoint: config.asr.url,
                model: config.asr.model,
                cause: error
            });
        }

        throw createVoiceStageError("asr", "VOICE_ASR_FAILED", error?.message || "ASR Realtime request failed", 502, {
            endpoint: config.asr.url,
            model: config.asr.model,
            cause: error
        });
    } finally {
        if (ws) {
            ws.sendClose();
        }
    }
}

async function requestVoiceTurnLlm(asrText, config, signal, options = {}) {
    try {
        const promptResult = typeof options.dbAll === "function"
            ? await buildLlmPrompt(options.dbAll, asrText, {
                deviceId: options.deviceId,
                mode: "voice"
            })
            : { prompt: asrText };
        return await requestLlmText(promptResult.prompt, {
            apiKey: config.apiKey,
            endpoint: config.chat.endpoint,
            baseUrl: config.chat.baseUrl,
            chatPath: config.chat.path,
            model: config.chat.model,
            timeoutMs: readVoiceLlmTimeoutMs()
        }, signal);
    } catch (error) {
        throw createVoiceStageError("llm", "VOICE_LLM_FAILED", error?.message || "LLM request failed", 502, {
            upstreamStatus: error?.status,
            bodyLength: error?.bodyLength,
            bodyPreview: error?.bodyPreview,
            endpoint: error?.endpoint || config.chat.endpoint,
            model: error?.model || config.chat.model,
            cause: error
        });
    }
}

async function requestRealtimeVoiceTts(text, config, signal) {
    let ws = null;
    const audioChunks = [];

    try {
        ws = await openRealtimeWebSocket(
            config.tts.url,
            buildVolcGatewayHeaders(config, "tts"),
            signal,
            "tts"
        );
        ws.sendText(buildTtsSessionUpdate(config));
        while (!signal?.aborted) {
            const event = parseTtsRealtimeEvent(await ws.nextMessage(signal));
            if (event.isError) {
                throw createVoiceStageError("tts", "VOICE_TTS_FAILED", event.errorMessage || "TTS Realtime WebSocket returned an error", 502, {
                    endpoint: config.tts.url,
                    model: config.tts.model
                });
            }

            if (event.isSessionUpdated) {
                break;
            }
        }

        ws.sendText(JSON.stringify({
            type: "input_text.append",
            delta: text
        }));
        ws.sendText(JSON.stringify({
            type: "input_text.done"
        }));

        while (!signal?.aborted) {
            const event = parseTtsRealtimeEvent(await ws.nextMessage(signal));
            if (event.isError) {
                throw createVoiceStageError("tts", "VOICE_TTS_FAILED", event.errorMessage || "TTS Realtime WebSocket returned an error", 502, {
                    endpoint: config.tts.url,
                    model: config.tts.model
                });
            }

            if (event.isAudioDelta) {
                const decoded = decodeBase64Buffer(event.delta);
                if (!decoded) {
                    throw createVoiceStageError("tts", "VOICE_TTS_FAILED", "TTS Realtime audio delta was not valid base64 PCM", 502, {
                        endpoint: config.tts.url,
                        model: config.tts.model
                    });
                }
                audioChunks.push(decoded);
            }

            if (event.isAudioDone) {
                break;
            }
        }

        const audioBuffer = Buffer.concat(audioChunks);
        return {
            pcm: normalizeTtsPcmBuffer(audioBuffer)
        };
    } catch (error) {
        if (error?.code) {
            throw error;
        }

        if (error?.name === "AbortError") {
            throw createVoiceStageError("tts", "VOICE_TTS_FAILED", "TTS Realtime request was aborted or timed out", 502, {
                endpoint: config.tts.url,
                model: config.tts.model,
                cause: error
            });
        }

        throw createVoiceStageError("tts", "VOICE_TTS_FAILED", error?.message || "TTS Realtime request failed", 502, {
            endpoint: config.tts.url,
            model: config.tts.model,
            cause: error
        });
    } finally {
        if (ws) {
            ws.sendClose();
        }
    }
}

async function requestHttpVoiceTts(text, config, deviceId, signal) {
    if (typeof fetch !== "function") {
        throw createVoiceStageError("tts", "VOICE_TTS_FAILED", "fetch is unavailable", 502);
    }

    try {
        const upstreamResponse = await fetch(config.tts.url, {
            method: "POST",
            headers: {
                "Content-Type": "application/json",
                Accept: `${VOICE_TURN_CONTENT_TYPE}, audio/wav, audio/x-wav, application/octet-stream, application/json`,
                "X-Device-Id": deviceId,
                ...buildVolcGatewayHeaders(config, "tts")
            },
            body: JSON.stringify({
                model: config.tts.model,
                text,
                input: text,
                voice: config.tts.voice,
                voice_type: config.tts.voice,
                format: "pcm",
                response_format: "pcm",
                output_audio_format: "pcm",
                sample_rate: config.tts.sampleRate,
                output_audio_sample_rate: config.tts.sampleRate,
                speed: config.tts.speed,
                pitch: config.tts.pitch,
                volume: config.tts.volume
            }),
            signal
        });
        const responseBuffer = Buffer.from(await upstreamResponse.arrayBuffer());

        if (!upstreamResponse.ok) {
            const responseBody = responseBuffer.toString("utf8");
            throw createUpstreamVoiceStageError(
                "tts",
                "VOICE_TTS_FAILED",
                upstreamResponse,
                responseBody,
                "TTS upstream request failed"
            );
        }

        const contentType = upstreamResponse.headers.get("content-type") || "";
        const pcmBuffer = normalizeTtsPcmBuffer(responseBuffer, contentType);

        return {
            pcm: pcmBuffer
        };
    } catch (error) {
        if (error?.code) {
            throw error;
        }

        if (error?.name === "AbortError") {
            throw createVoiceStageError("tts", "VOICE_TTS_FAILED", "TTS request was aborted or timed out", 502, {
                endpoint: config.tts.url,
                model: config.tts.model,
                cause: error
            });
        }

        throw createVoiceStageError("tts", "VOICE_TTS_FAILED", error?.message || "TTS request failed", 502, {
            endpoint: config.tts.url,
            model: config.tts.model,
            cause: error
        });
    }
}

async function requestVoiceTts(text, config, deviceId, signal) {
    const protocol = new URL(config.tts.url).protocol;
    if (protocol === "ws:" || protocol === "wss:") {
        return requestRealtimeVoiceTts(text, config, signal);
    }

    return requestHttpVoiceTts(text, config, deviceId, signal);
}

async function runVoiceTurnChain(audioBuffer, deviceId, voiceConfig, gatewayConfig, signal, metrics, logger = console, options = {}) {
    let stageStartedAt = Date.now();
    logger.log(
        `[voice-turn] ASR_STREAM_START device_id=${maskLogValue(deviceId)} input_bytes=${audioBuffer.length}`
    );
    const asrResult = await requestVoiceAsr(audioBuffer, gatewayConfig, voiceConfig, signal, logger);
    metrics.asrMs = Date.now() - stageStartedAt;
    metrics.asrTextLength = asrResult.text.length;
    metrics.asrTextPreview = normalizeLogPreview(asrResult.text, 60);
    logger.log(
        `[voice-turn] ASR_RESULT device_id=${maskLogValue(deviceId)} asr_text_length=${metrics.asrTextLength} asr_text=${JSON.stringify(metrics.asrTextPreview)}`
    );
    logger.log(
        `[voice-turn] asr_success device_id=${maskLogValue(deviceId)} input_bytes=${audioBuffer.length} asr_ws_url=${maskUrlForLog(gatewayConfig.asr.url)} asr_text_length=${metrics.asrTextLength} asr_text=${JSON.stringify(metrics.asrTextPreview)} elapsed_ms=${metrics.asrMs}`
    );

    stageStartedAt = Date.now();
    logger.log(
        `[voice-turn] LLM_REQUEST device_id=${maskLogValue(deviceId)} asr_text_length=${metrics.asrTextLength}`
    );
    const llmResult = await runVoiceAgentConversation(asrResult.text, gatewayConfig, signal, {
        dbAll: options.dbAll,
        deviceId,
        logger
    });
    metrics.llmMs = Date.now() - stageStartedAt;
    metrics.llmReplyLength = llmResult.text.length;
    logger.log(
        `[voice-turn] llm_success device_id=${maskLogValue(deviceId)} asr_text_length=${metrics.asrTextLength} llm_reply_length=${metrics.llmReplyLength} elapsed_ms=${metrics.llmMs}`
    );

    stageStartedAt = Date.now();
    logger.log(
        `[voice-turn] TTS_START device_id=${maskLogValue(deviceId)} llm_reply_length=${metrics.llmReplyLength}`
    );
    const ttsResult = await requestVoiceTts(llmResult.text, gatewayConfig, deviceId, signal);
    metrics.ttsMs = Date.now() - stageStartedAt;
    metrics.ttsPcmBytes = ttsResult.pcm.length;
    logger.log(
        `[voice-turn] tts_success device_id=${maskLogValue(deviceId)} llm_reply_length=${metrics.llmReplyLength} tts_pcm_bytes=${metrics.ttsPcmBytes} elapsed_ms=${metrics.ttsMs}`
    );

    return {
        bytes: ttsResult.pcm.length,
        mode: "chain",
        asrMs: metrics.asrMs,
        llmMs: metrics.llmMs,
        ttsMs: metrics.ttsMs,
        asrTextLength: metrics.asrTextLength,
        asrTextPreview: metrics.asrTextPreview,
        llmReplyLength: metrics.llmReplyLength,
        ttsPcmBytes: metrics.ttsPcmBytes,
        pcm: ttsResult.pcm
    };
}

module.exports = {
    requestVoiceAsr,
    requestVoiceTurnLlm,
    requestVoiceTts,
    runVoiceTurnChain
};
