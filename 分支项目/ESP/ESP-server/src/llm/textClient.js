const LEGACY_LLM_BASE_URL = "https://fai-gateway.vei.volces.com";
const DEFAULT_LLM_BASE_URL = "https://ai-gateway.vei.volces.com";
const DEFAULT_LLM_CHAT_PATH = "/v1/chat/completions";
const DEFAULT_LLM_MODEL = "Doubao-Seed-1.6-flash";
const DEFAULT_LLM_TIMEOUT_MS = 30000;
const LLM_TEXT_MAX_CHARS = 4000;
const LLM_METADATA_MAX_CHARS = 128;

let legacyLlmBaseUrlWarned = false;

const {
    readGatewayPathEnv,
    readPositiveInteger,
    readTrimmedEnv
} = require("../utils/env");
const {
    extractErrorMessageFromBody,
    normalizeLogPreview,
    summarizeSecret
} = require("../utils/logging");

function normalizeLlmBaseUrl(baseUrl, logger = console) {
    if (baseUrl === LEGACY_LLM_BASE_URL) {
        if (!legacyLlmBaseUrlWarned) {
            logger.warn(`[llm-text] LLM_BASE_URL uses legacy ${LEGACY_LLM_BASE_URL}; using ${DEFAULT_LLM_BASE_URL}`);
            legacyLlmBaseUrlWarned = true;
        }

        return DEFAULT_LLM_BASE_URL;
    }

    return baseUrl;
}

function readLlmConfig(logger = console) {
    const apiKey = readTrimmedEnv("VOLC_GATEWAY_API_KEY", readTrimmedEnv("LLM_API_KEY"));
    const baseUrl = readTrimmedEnv(
        "VOLC_GATEWAY_HTTP_BASE_URL",
        readTrimmedEnv("LLM_BASE_URL", DEFAULT_LLM_BASE_URL)
    ).replace(/\/+$/, "");
    const chatPath = readGatewayPathEnv(
        "VOLC_GATEWAY_CHAT_PATH",
        readTrimmedEnv("LLM_CHAT_PATH", DEFAULT_LLM_CHAT_PATH)
    );
    const model = readTrimmedEnv(
        "VOLC_GATEWAY_CHAT_MODEL",
        readTrimmedEnv("LLM_MODEL", DEFAULT_LLM_MODEL)
    );
    const normalizedBaseUrl = normalizeLlmBaseUrl(baseUrl, logger);

    return {
        apiKey,
        keySummary: summarizeSecret(apiKey),
        baseUrl: normalizedBaseUrl,
        chatPath,
        endpoint: `${normalizedBaseUrl}${chatPath}`,
        model,
        timeoutMs: readPositiveInteger(process.env.LLM_TIMEOUT_MS, DEFAULT_LLM_TIMEOUT_MS)
    };
}

function readLlmTextRequest(body) {
    if (!body || typeof body !== "object" || Array.isArray(body)) {
        return {
            error: "JSON object body is required"
        };
    }

    if (typeof body.text !== "string") {
        return {
            error: "text is required"
        };
    }

    const text = body.text.trim();
    if (!text) {
        return {
            error: "text is required"
        };
    }

    if (text.length > LLM_TEXT_MAX_CHARS) {
        return {
            error: `text exceeds ${LLM_TEXT_MAX_CHARS} characters`
        };
    }

    return {
        text,
        deviceId: readLlmMetadataField(body.device_id),
        sessionId: readLlmMetadataField(body.session_id)
    };
}

function readLlmMetadataField(value) {
    if (typeof value !== "string") {
        return "";
    }

    return value.trim().slice(0, LLM_METADATA_MAX_CHARS);
}

function normalizeLlmContent(content) {
    if (typeof content === "string") {
        return content.trim();
    }

    if (Array.isArray(content)) {
        return content
            .map(part => {
                if (typeof part === "string") {
                    return part;
                }

                if (part && typeof part.text === "string") {
                    return part.text;
                }

                return "";
            })
            .join("")
            .trim();
    }

    return "";
}

function extractLlmReply(payload) {
    const choice = payload && Array.isArray(payload.choices) ? payload.choices[0] : null;
    const replyText = normalizeLlmContent(
        choice?.message?.content ??
        choice?.delta?.content ??
        choice?.text
    );

    return {
        text: replyText,
        model: typeof payload?.model === "string" && payload.model.trim() ? payload.model.trim() : ""
    };
}

function createLlmError(code) {
    const error = new Error(code);
    error.code = code;
    return error;
}

function describeLlmError(error) {
    const parts = [
        `name=${error?.name || "Error"}`
    ];

    if (error?.code) {
        parts.push(`code=${error.code}`);
    }

    if (typeof error?.status === "number") {
        parts.push(`upstream_status=${error.status}`);
    }

    if (typeof error?.bodyLength === "number") {
        parts.push(`body_len=${error.bodyLength}`);
    }

    if (error?.endpoint) {
        parts.push(`endpoint=${error.endpoint}`);
    }

    if (error?.model) {
        parts.push(`model=${error.model}`);
    }

    if (error?.bodyPreview) {
        parts.push(`body=${JSON.stringify(error.bodyPreview)}`);
    }

    return parts.join(" ");
}

function getLlmResponseStatus(error) {
    if (error?.code === "LLM_API_KEY_MISSING") {
        return 503;
    }

    if (error?.code === "LLM_TIMEOUT") {
        return 504;
    }

    if (error?.code === "LLM_UPSTREAM_STATUS" ||
        error?.code === "LLM_JSON_PARSE_FAILED" ||
        error?.code === "LLM_REPLY_EMPTY") {
        return 502;
    }

    return 500;
}

async function requestLlmText(text, config, externalSignal) {
    if (!config.apiKey) {
        throw createLlmError("LLM_API_KEY_MISSING");
    }

    if (typeof fetch !== "function") {
        throw createLlmError("FETCH_UNAVAILABLE");
    }

    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), config.timeoutMs);
    const endpoint = config.endpoint || `${config.baseUrl}${config.chatPath}`;
    const abortFromExternalSignal = () => controller.abort();
    if (externalSignal?.aborted) {
        controller.abort();
    } else if (externalSignal) {
        externalSignal.addEventListener("abort", abortFromExternalSignal, { once: true });
    }

    try {
        const upstreamResponse = await fetch(endpoint, {
            method: "POST",
            headers: {
                "Content-Type": "application/json",
                Authorization: `Bearer ${config.apiKey}`
            },
            body: JSON.stringify({
                model: config.model,
                messages: [
                    {
                        role: "user",
                        content: text
                    }
                ],
                stream: false
            }),
            signal: controller.signal
        });
        const responseBody = await upstreamResponse.text();

        if (!upstreamResponse.ok) {
            const error = createLlmError("LLM_UPSTREAM_STATUS");
            error.status = upstreamResponse.status;
            error.bodyLength = responseBody.length;
            error.endpoint = endpoint;
            error.model = config.model;
            error.bodyPreview = normalizeLogPreview(responseBody);
            error.message = extractErrorMessageFromBody(responseBody) || error.message;
            throw error;
        }

        let payload;
        try {
            payload = responseBody ? JSON.parse(responseBody) : null;
        } catch (parseError) {
            const error = createLlmError("LLM_JSON_PARSE_FAILED");
            error.bodyLength = responseBody.length;
            error.cause = parseError;
            throw error;
        }

        const reply = extractLlmReply(payload);
        if (!reply.text) {
            const error = createLlmError("LLM_REPLY_EMPTY");
            error.endpoint = endpoint;
            error.model = config.model;
            throw error;
        }

        return {
            text: reply.text,
            model: reply.model || config.model
        };
    } catch (error) {
        if (error?.name === "AbortError") {
            const timeoutError = createLlmError("LLM_TIMEOUT");
            timeoutError.endpoint = endpoint;
            timeoutError.model = config.model;
            timeoutError.cause = error;
            throw timeoutError;
        }

        throw error;
    } finally {
        clearTimeout(timer);
        if (externalSignal) {
            externalSignal.removeEventListener("abort", abortFromExternalSignal);
        }
    }
}

module.exports = {
    describeLlmError,
    getLlmResponseStatus,
    LLM_METADATA_MAX_CHARS,
    readLlmConfig,
    readLlmTextRequest,
    requestLlmText
};
