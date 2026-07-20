const express = require("express");
const {
    insertVoiceTurn
} = require("../db/voiceTurns");
const {
    maskLogValue,
    maskUrlForLog,
    normalizeLogPreview
} = require("../utils/logging");
const {
    readVolcGatewayConfig,
    validateVoiceAsrConfig,
    validateVoiceChatConfig,
    validateVoiceTtsConfig
} = require("../voice/gatewayConfig");
const {
    readOptionalVoiceDeviceId,
    readVoiceDeviceId,
    readVoiceRequestId,
    sendVoiceError,
    sendVoiceTurnPcm,
    validateVoiceTurnRequest
} = require("../voice/http");
const {
    createVoiceStageError,
    describeVoiceError
} = require("../voice/errors");
const {
    createMockVoicePromptPcm,
    streamMockVoiceTurn
} = require("../voice/mockTurn");
const {
    DEFAULT_PROMPT_KEY,
    readPromptCache,
    safePromptKey,
    sendPromptCachePcm,
    writePromptCache
} = require("../voice/promptCache");
const {
    promptConfigMatches,
    readVoicePromptConfig,
    updateVoicePromptConfig
} = require("../voice/promptConfig");
const {
    readDeviceMetadata
} = require("../services/deviceMetadata");
const {
    refreshDeviceActivity
} = require("../services/deviceStatusService");
const {
    recordEvent
} = require("../services/eventLogService");
const {
    readVoiceTurnConfig,
    readVoiceTurnMaxBytes
} = require("../voice/turnConfig");
const {
    isSilentPcmBuffer
} = require("../voice/ttsAudio");
const {
    requestVoiceTts,
    runVoiceTurnChain
} = require("../voice/chain");

const VOICE_WAKE_PROMPT_TEXT = "我在，你说";

function formatOptionalDeviceLog(deviceId) {
    return deviceId ? ` device_id=${maskLogValue(deviceId)}` : "";
}

async function logVoiceTurnRecord(dbRun, record, logger = console) {
    try {
        await insertVoiceTurn(dbRun, record);
    } catch (error) {
        logger.error(
            `[voice-turn] log_failed request_id=${normalizeLogPreview(record.requestId, 80) || "-"} status=${record.status || "-"} code=${record.errorCode || "-"} message=${JSON.stringify(error?.message || "-")}`
        );
    }

    try {
        await recordEvent(dbRun, {
            event_type: "voice",
            event_name: "voice_event_created",
            device_id: record.deviceId || "",
            severity: record.status === "success" ? "info" : "warning",
            message: record.status || "voice turn",
            payload: {
                request_id: record.requestId || "",
                status: record.status || "",
                status_code: record.statusCode || null,
                error_code: record.errorCode || "",
                input_bytes: record.inputBytes || 0,
                response_bytes: record.responseBytes || 0,
                total_ms: record.totalMs || 0,
                mode: record.mode || ""
            },
            source: "voice_turn",
            server_recv_ms: Date.now()
        });
    } catch (_) {
        // Voice turn persistence remains authoritative; event logs are best-effort.
    }
}

function createVoiceBodyParserErrorHandler(options) {
    const dbRun = options.dbRun;
    const logger = options.logger || console;

    return async function voiceBodyParserErrorHandler(err, req, res, next) {
        if (req.path === "/api/voice/turn") {
            const status = err?.type === "entity.too.large" ? 413 : 400;
            const code = err?.type === "entity.too.large"
                ? "VOICE_BODY_TOO_LARGE"
                : "VOICE_BODY_PARSE_FAILED";
            const deviceId = readVoiceDeviceId(req);
            const requestId = readVoiceRequestId(req);
            const inputBytes = Number.isFinite(err?.received)
                ? err.received
                : (Number.isFinite(err?.length) ? err.length : 0);

            logger.warn(`[voice-turn] body parser failed device_id=${maskLogValue(deviceId)} input_bytes=${inputBytes} elapsed_ms=0 code=${code} status=${status} message=${JSON.stringify(err?.message || "-")}`);
            await logVoiceTurnRecord(dbRun, {
                requestId,
                deviceId,
                mode: "unknown",
                status: "failed",
                statusCode: status,
                errorCode: code,
                errorMessage: err?.message || "Invalid PCM request body",
                inputBytes,
                totalMs: 0,
                reason: "body_parser"
            }, logger);
            return sendVoiceError(res, status, code, err?.message || "Invalid PCM request body");
        }

        return next(err);
    };
}

function createVoiceRouter(options) {
    const router = express.Router();
    const dbRun = options.dbRun;
    const dbAll = options.dbAll;
    const logger = options.logger || console;
    const activeVoiceDevices = new Set();
    let activeVoiceTurns = 0;
    const voiceTurnRawParser = express.raw({
        type: () => true,
        limit: readVoiceTurnMaxBytes(),
        inflate: false
    });

    function acquireVoiceTurn(deviceId, config) {
        if (activeVoiceDevices.has(deviceId)) {
            return {
                status: 409,
                code: "VOICE_DEVICE_BUSY",
                message: "Device already has an active voice turn"
            };
        }

        if (activeVoiceTurns >= config.maxConcurrent) {
            return {
                status: 429,
                code: "VOICE_SERVER_BUSY",
                message: "Voice turn concurrency limit reached"
            };
        }

        activeVoiceDevices.add(deviceId);
        activeVoiceTurns += 1;
        return null;
    }

    function releaseVoiceTurn(deviceId) {
        if (activeVoiceDevices.delete(deviceId)) {
            activeVoiceTurns = Math.max(0, activeVoiceTurns - 1);
        }
    }

    function promptCacheExtra(promptConfig, ttsProvider, ttsVoice) {
        return {
            provider: promptConfig.provider || ttsProvider,
            voice_id: promptConfig.voice_id || ttsVoice,
            speaker_id: promptConfig.speaker_id || "",
            speed: promptConfig.speed,
            pitch: promptConfig.pitch,
            volume: promptConfig.volume,
            sample_rate: promptConfig.sample_rate,
            format: promptConfig.format,
            channels: promptConfig.channels,
            prompt_version: promptConfig.prompt_version,
            voice_config_hash: promptConfig.voice_config_hash,
            tts_provider: ttsProvider,
            tts_voice: ttsVoice
        };
    }

    function buildPromptGatewayConfig(gatewayConfig, promptConfig) {
        return {
            ...gatewayConfig,
            tts: {
                ...gatewayConfig.tts,
                voice: promptConfig.voice_id || gatewayConfig.tts.voice,
                sampleRate: promptConfig.sample_rate || gatewayConfig.tts.sampleRate,
                format: promptConfig.format === "s16le" ?
                    gatewayConfig.tts.format :
                    (promptConfig.format || gatewayConfig.tts.format),
                speed: promptConfig.speed,
                pitch: promptConfig.pitch,
                volume: promptConfig.volume
            }
        };
    }

    function sendPromptConfig(res, config) {
        return res.status(200).json({
            ok: true,
            config
        });
    }

    async function handleVoicePromptConfigGet(req, res) {
        void req;
        return sendPromptConfig(res, readVoicePromptConfig());
    }

    async function handleVoicePromptConfigPut(req, res) {
        const patch = req.body && typeof req.body === "object" ? req.body : {};
        const config = updateVoicePromptConfig(patch);
        logger.log(
            `[voice-prompt-config] updated wake_prompt_text=${JSON.stringify(config.wake_prompt_text)} voice_id=${JSON.stringify(config.voice_id)} hash=${config.voice_config_hash} version=${config.prompt_version}`
        );
        return sendPromptConfig(res, config);
    }

    async function handleVoicePrompt(req, res) {
        const startedAt = Date.now();
        const deviceId = readOptionalVoiceDeviceId(req);
        const upstreamDeviceId = readVoiceDeviceId(req);
        const metadata = readDeviceMetadata({
            headers: req.headers,
            query: req.query,
            body: {},
            deviceId: upstreamDeviceId,
            payloadType: "voice.prompt",
            serverRecvMs: startedAt
        });
        if (metadata.device_id && typeof dbRun === "function" && typeof dbAll === "function") {
            try {
                await refreshDeviceActivity(dbRun, dbAll, metadata, "voice.prompt");
            } catch (error) {
                logger.warn(`[voice-prompt] status refresh failed device_id=${maskLogValue(metadata.device_id)} message=${JSON.stringify(error?.message || "-")}`);
            }
        }

        const promptKey = safePromptKey(req.query.prompt_key || DEFAULT_PROMPT_KEY);
        const forceRefresh = req.query.refresh === "1" || req.query.force_refresh === "1";
        const promptConfig = readVoicePromptConfig();
        const hit = readPromptCache(promptKey);
        if (hit && !forceRefresh && promptConfigMatches(hit.meta, promptConfig)) {
            const elapsedMs = Date.now() - startedAt;
            sendPromptCachePcm(res, hit, "hit");
            logger.log(
                `[voice-prompt] cache_hit${formatOptionalDeviceLog(deviceId)} prompt_key=${promptKey} hash=${promptConfig.voice_config_hash} version=${promptConfig.prompt_version} bytes=${hit.pcm.length} elapsed_ms=${elapsedMs}`
            );
            return;
        }

        const config = readVoiceTurnConfig();
        if (config.mockEnabled) {
            const pcm = createMockVoicePromptPcm();
            const elapsedMs = Date.now() - startedAt;
            const cached = writePromptCache(promptKey,
                                            promptConfig.wake_prompt_text,
                                            pcm,
                                            promptCacheExtra(promptConfig, "mock", "mock"));
            sendPromptCachePcm(res, cached, "miss");
            logger.log(
                `[voice-prompt] cache_miss${formatOptionalDeviceLog(deviceId)} mode=mock prompt_key=${promptKey} prompt_text=${JSON.stringify(promptConfig.wake_prompt_text)} hash=${promptConfig.voice_config_hash} version=${promptConfig.prompt_version} tts_pcm_bytes=${pcm.length} elapsed_ms=${elapsedMs}`
            );
            return;
        }

        const gatewayConfig = buildPromptGatewayConfig(readVolcGatewayConfig(), promptConfig);
        const ttsConfigError = validateVoiceTtsConfig(gatewayConfig);
        let ttsPcmBytes = 0;

        if (ttsConfigError) {
            const elapsedMs = Date.now() - startedAt;
            const stale = readPromptCache(promptKey);
            if (stale && promptConfigMatches(stale.meta, promptConfig)) {
                sendPromptCachePcm(res, stale, "stale");
                logger.warn(
                    `[voice-prompt] cache_stale${formatOptionalDeviceLog(deviceId)} prompt_key=${promptKey} reason=tts_config code=${ttsConfigError.code} elapsed_ms=${elapsedMs}`
                );
                return;
            }

            logger.warn(
                `[voice-prompt] rejected${formatOptionalDeviceLog(deviceId)} prompt_text=${JSON.stringify(promptConfig.wake_prompt_text)} tts_pcm_bytes=${ttsPcmBytes} elapsed_ms=${elapsedMs} code=${ttsConfigError.code} status=${ttsConfigError.status} message=${JSON.stringify(ttsConfigError.message)} key_${gatewayConfig.keySummary}`
            );
            return sendVoiceError(res, 503, "VOICE_TTS_NOT_CONFIGURED", ttsConfigError.message);
        }

        const controller = new AbortController();
        let timedOut = false;
        const timeout = setTimeout(() => {
            timedOut = true;
            controller.abort();
        }, config.timeoutMs);

        const abortOnClientClose = () => {
            if (!res.writableEnded) {
                controller.abort();
            }
        };
        req.on("aborted", abortOnClientClose);
        res.on("close", abortOnClientClose);

        try {
            const ttsResult = await requestVoiceTts(
                promptConfig.wake_prompt_text,
                gatewayConfig,
                upstreamDeviceId,
                controller.signal
            );
            ttsPcmBytes = ttsResult.pcm.length;

            if (isSilentPcmBuffer(ttsResult.pcm)) {
                throw createVoiceStageError("tts", "VOICE_TTS_FAILED", "TTS prompt PCM must not be silent", 502);
            }

            const cached = writePromptCache(promptKey,
                                            promptConfig.wake_prompt_text,
                                            ttsResult.pcm,
                                            promptCacheExtra(promptConfig,
                                                             promptConfig.provider || "volc",
                                                             gatewayConfig.tts.voice || promptConfig.voice_id || "server_prompt_v1"));
            sendPromptCachePcm(res, cached, "miss");

            const elapsedMs = Date.now() - startedAt;
            logger.log(
                `[voice-prompt] cache_miss${formatOptionalDeviceLog(deviceId)} prompt_key=${promptKey} prompt_text=${JSON.stringify(promptConfig.wake_prompt_text)} hash=${promptConfig.voice_config_hash} version=${promptConfig.prompt_version} tts_pcm_bytes=${ttsPcmBytes} elapsed_ms=${elapsedMs}`
            );
        } catch (error) {
            const elapsedMs = Date.now() - startedAt;
            const message = timedOut
                ? "TTS prompt request timed out"
                : (error?.message || "TTS prompt request failed");
            const stale = readPromptCache(promptKey);
            if (stale && promptConfigMatches(stale.meta, promptConfig)) {
                sendPromptCachePcm(res, stale, "stale");
                logger.warn(
                    `[voice-prompt] cache_stale${formatOptionalDeviceLog(deviceId)} prompt_key=${promptKey} tts_pcm_bytes=${stale.pcm.length} elapsed_ms=${elapsedMs} code=VOICE_TTS_FAILED message=${JSON.stringify(message)}`
                );
                return;
            }

            logger.error(
                `[voice-prompt] failed${formatOptionalDeviceLog(deviceId)} prompt_text=${JSON.stringify(promptConfig.wake_prompt_text)} tts_pcm_bytes=${ttsPcmBytes} elapsed_ms=${elapsedMs} code=VOICE_TTS_FAILED status=502 message=${JSON.stringify(message)} ${describeVoiceError(error)}`
            );

            sendVoiceError(res, 502, "VOICE_TTS_FAILED", message, {
                upstreamStatus: error?.upstreamStatus
            });
        } finally {
            clearTimeout(timeout);
            req.off("aborted", abortOnClientClose);
            res.off("close", abortOnClientClose);
        }
    }

    async function handleVoiceTurn(req, res) {
        const startedAt = Date.now();
        const deviceId = readVoiceDeviceId(req);
        const requestId = readVoiceRequestId(req);
        const requestBytes = Buffer.isBuffer(req.body) ? req.body.length : 0;
        const metadata = readDeviceMetadata({
            headers: req.headers,
            query: req.query,
            body: {},
            deviceId,
            payloadType: "voice.turn",
            serverRecvMs: startedAt
        });
        if (metadata.device_id && typeof dbRun === "function" && typeof dbAll === "function") {
            try {
                await refreshDeviceActivity(dbRun, dbAll, metadata, "voice.turn");
            } catch (error) {
                logger.warn(`[voice-turn] status refresh failed device_id=${maskLogValue(metadata.device_id)} message=${JSON.stringify(error?.message || "-")}`);
            }
        }

        const validationError = validateVoiceTurnRequest(req);
        if (validationError) {
            const elapsedMs = Date.now() - startedAt;
            logger.warn(
                `[voice-turn] rejected device_id=${maskLogValue(deviceId)} input_bytes=${requestBytes} elapsed_ms=${elapsedMs} code=${validationError.code} status=${validationError.status} message=${JSON.stringify(validationError.message)}`
            );
            await logVoiceTurnRecord(dbRun, {
                requestId,
                deviceId,
                mode: "unknown",
                status: "rejected",
                statusCode: validationError.status,
                errorCode: validationError.code,
                errorMessage: validationError.message,
                inputBytes: requestBytes,
                totalMs: elapsedMs,
                reason: "request_validation"
            }, logger);

            return sendVoiceError(
                res,
                validationError.status,
                validationError.code,
                validationError.message
            );
        }

        const config = readVoiceTurnConfig();
        const gatewayConfig = readVolcGatewayConfig();
        const asrConfigError = config.mockEnabled ? null : validateVoiceAsrConfig(gatewayConfig);
        const chatConfigError = config.mockEnabled ? null : validateVoiceChatConfig(gatewayConfig);
        const ttsConfigError = config.mockEnabled ? null : validateVoiceTtsConfig(gatewayConfig);
        const configError = asrConfigError || chatConfigError || ttsConfigError;

        if (configError) {
            const elapsedMs = Date.now() - startedAt;
            logger.warn(
                `[voice-turn] rejected device_id=${maskLogValue(deviceId)} input_bytes=${requestBytes} elapsed_ms=${elapsedMs} code=${configError.code} status=${configError.status} message=${JSON.stringify(configError.message)} mode=chain key_${gatewayConfig.keySummary}`
            );
            await logVoiceTurnRecord(dbRun, {
                requestId,
                deviceId,
                mode: "chain",
                status: "rejected",
                statusCode: configError.status,
                errorCode: configError.code,
                errorMessage: configError.message,
                inputBytes: requestBytes,
                totalMs: elapsedMs,
                timeoutMs: config.timeoutMs,
                activeLimit: config.maxConcurrent,
                reason: "config_validation"
            }, logger);
            return sendVoiceError(res, configError.status, configError.code, configError.message);
        }

        const acquireError = acquireVoiceTurn(deviceId, config);
        if (acquireError) {
            const elapsedMs = Date.now() - startedAt;
            logger.warn(
                `[voice-turn] rejected device_id=${maskLogValue(deviceId)} input_bytes=${requestBytes} elapsed_ms=${elapsedMs} code=${acquireError.code} status=${acquireError.status} message=${JSON.stringify(acquireError.message)} active=${activeVoiceTurns}/${config.maxConcurrent}`
            );
            await logVoiceTurnRecord(dbRun, {
                requestId,
                deviceId,
                mode: config.mockEnabled ? "mock" : "chain",
                status: "rejected",
                statusCode: acquireError.status,
                errorCode: acquireError.code,
                errorMessage: acquireError.message,
                inputBytes: requestBytes,
                totalMs: elapsedMs,
                timeoutMs: config.timeoutMs,
                activeLimit: config.maxConcurrent,
                reason: "concurrency"
            }, logger);

            return sendVoiceError(res, acquireError.status, acquireError.code, acquireError.message);
        }

        const metrics = {
            asrMs: null,
            llmMs: null,
            ttsMs: null,
            asrTextLength: 0,
            asrTextPreview: "",
            llmReplyLength: 0,
            ttsPcmBytes: 0
        };
        const controller = new AbortController();
        let timedOut = false;
        const timeout = setTimeout(() => {
            timedOut = true;
            controller.abort();
        }, config.timeoutMs);

        const abortOnClientClose = () => {
            if (!res.writableEnded) {
                controller.abort();
            }
        };
        req.on("aborted", abortOnClientClose);
        res.on("close", abortOnClientClose);

        logger.log(
            `[voice-turn] start device_id=${maskLogValue(deviceId)} input_bytes=${requestBytes} active=${activeVoiceTurns}/${config.maxConcurrent} mode=${config.mockEnabled ? "mock" : "chain"} asr_ws_url=${maskUrlForLog(gatewayConfig.asr.url)} timeout_ms=${config.timeoutMs} key_${gatewayConfig.keySummary}`
        );

        try {
            let result;
            if (config.mockEnabled) {
                result = await streamMockVoiceTurn(req.body, res);
            } else {
                result = await runVoiceTurnChain(
                    req.body,
                    deviceId,
                    config,
                    gatewayConfig,
                    controller.signal,
                    metrics,
                    logger,
                    { dbAll }
                );
            }

            const elapsedMs = Date.now() - startedAt;
            await logVoiceTurnRecord(dbRun, {
                requestId,
                deviceId,
                mode: result.mode,
                status: "success",
                statusCode: 200,
                inputBytes: requestBytes,
                responseBytes: result.bytes,
                asrMs: result.asrMs,
                llmMs: result.llmMs,
                ttsMs: result.ttsMs,
                totalMs: elapsedMs,
                asrTextLength: result.asrTextLength,
                asrTextPreview: result.asrTextPreview || "",
                llmReplyLength: result.llmReplyLength,
                ttsPcmBytes: result.ttsPcmBytes,
                timeoutMs: config.timeoutMs,
                activeLimit: config.maxConcurrent
            }, logger);
            sendVoiceTurnPcm(res, result.pcm);

            logger.log(
                `[voice-turn] success device_id=${maskLogValue(deviceId)} mode=${result.mode} input_bytes=${requestBytes} asr_ws_url=${maskUrlForLog(gatewayConfig.asr.url)} asr_text_length=${result.asrTextLength} asr_text=${JSON.stringify(result.asrTextPreview || "")} llm_reply_length=${result.llmReplyLength} tts_pcm_bytes=${result.ttsPcmBytes} response_bytes=${result.bytes} elapsed_ms=${elapsedMs}`
            );
        } catch (error) {
            const elapsedMs = Date.now() - startedAt;
            const normalizedError = timedOut
                ? createVoiceStageError(error?.stage || "voice_turn", "VOICE_TURN_TIMEOUT", "Voice turn timed out", 504, {
                    upstreamStatus: error?.upstreamStatus,
                    cause: error
                })
                : error;
            const status = normalizedError.status || 500;
            const code = normalizedError.code || "VOICE_TURN_FAILED";
            const message = normalizedError.message || "Voice turn failed";

            logger.error(
                `[voice-turn] failed device_id=${maskLogValue(deviceId)} input_bytes=${requestBytes} asr_ws_url=${maskUrlForLog(gatewayConfig.asr.url)} asr_text_length=${metrics.asrTextLength} asr_text=${JSON.stringify(metrics.asrTextPreview)} llm_reply_length=${metrics.llmReplyLength} tts_pcm_bytes=${metrics.ttsPcmBytes} elapsed_ms=${elapsedMs} code=${code} status=${status} message=${JSON.stringify(message)} ${describeVoiceError(normalizedError)}`
            );
            await logVoiceTurnRecord(dbRun, {
                requestId,
                deviceId,
                mode: config.mockEnabled ? "mock" : "chain",
                status: "failed",
                statusCode: status,
                errorCode: code,
                errorMessage: message,
                inputBytes: requestBytes,
                responseBytes: 0,
                asrMs: metrics.asrMs,
                llmMs: metrics.llmMs,
                ttsMs: metrics.ttsMs,
                totalMs: elapsedMs,
                asrTextLength: metrics.asrTextLength,
                asrTextPreview: metrics.asrTextPreview,
                llmReplyLength: metrics.llmReplyLength,
                ttsPcmBytes: metrics.ttsPcmBytes,
                timeoutMs: config.timeoutMs,
                activeLimit: config.maxConcurrent,
                stage: normalizedError.stage,
                upstreamStatus: normalizedError.upstreamStatus
            }, logger);

            sendVoiceError(res, status, code, message, {
                upstreamStatus: normalizedError.upstreamStatus
            });
        } finally {
            clearTimeout(timeout);
            req.off("aborted", abortOnClientClose);
            res.off("close", abortOnClientClose);
            releaseVoiceTurn(deviceId);
        }
    }

    router.post("/api/voice/turn", voiceTurnRawParser, handleVoiceTurn);
    router.get("/api/voice/prompt/config", handleVoicePromptConfigGet);
    router.put("/api/voice/prompt/config", express.json({ limit: "8kb" }), handleVoicePromptConfigPut);
    router.get("/api/voice/prompt", handleVoicePrompt);
    router.get("/api/voice/prompt-cache", handleVoicePrompt);

    return router;
}

module.exports = {
    VOICE_WAKE_PROMPT_TEXT,
    createVoiceBodyParserErrorHandler,
    createVoiceRouter,
    logVoiceTurnRecord
};
