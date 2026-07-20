const {
    runAgentConversation
} = require("../agent/agentRunner");
const {
    createDefaultToolRegistry
} = require("../agent/defaultToolRegistry");
const {
    readWeatherConfig
} = require("../agent/weatherQuery");
const {
    buildContextText,
    buildLlmPrompt
} = require("../services/llmPromptContextService");
const {
    maskLogValue,
    normalizeLogPreview
} = require("../utils/logging");
const {
    createVoiceStageError
} = require("./errors");
const {
    readVoiceLlmTimeoutMs,
    readVoiceToolTimeoutMs
} = require("./turnConfig");

const VOICE_TOOL_FAILURE_TEXT = Object.freeze({
    weather_query: "抱歉，天气查询失败，暂时无法提供可靠的天气信息。",
    home_state_query: "抱歉，家庭状态查询失败，暂时无法提供可靠的状态信息。",
    sensor_query: "抱歉，传感器数据查询失败，暂时无法提供可靠的环境信息。",
    device_status_query: "抱歉，设备状态查询失败，暂时无法提供可靠的设备信息。"
});

function normalizeVoiceText(value) {
    return typeof value === "string" ? value.trim() : "";
}

function hasVoiceQueryShape(text) {
    return /[?？吗呢]|(怎么样|如何|多少|是否|能否|告诉我|帮我查|查询|现在|今天|明天|后天)/.test(text);
}

function routeVoiceIntent(text) {
    const normalized = normalizeVoiceText(text).toLowerCase();
    if (!normalized || !hasVoiceQueryShape(normalized)) {
        return null;
    }

    // Route only when a question has both a domain target and a matching state/data request.
    // Ambiguous phrases such as "温度多少" intentionally remain on the normal Agent path.
    const sensorTarget = /(室内|屋内|房间|客厅|卧室|厨房|卫生间|传感器|bme|空气质量|甲醛|挥发性有机物|voc|co2|二氧化碳|气体电阻|气压|家里.{0,6}(温度|湿度|空气))/.test(normalized);
    const deviceTarget = /(设备|网关|模块|s3|c5[125]?|c51|c52|在线|离线|联网|连接|固件|版本)/.test(normalized);
    const homeTarget = /(家里|家庭|房屋|有人吗|是否有人|在家吗|是否在家|哪个房间|房间.{0,8}(状态|情况|有人|无人)|客厅.{0,8}(有人|无人)|卧室.{0,8}(有人|无人))/.test(normalized);
    if (sensorTarget) {
        return { name: "sensor_query", intent: "sensor" };
    }

    if (deviceTarget) {
        return { name: "device_status_query", intent: "device_status" };
    }

    if (homeTarget) {
        return { name: "home_state_query", intent: "home_state" };
    }

    return null;
}

function buildVoiceAgentConfig(gatewayConfig) {
    return {
        apiKey: gatewayConfig.apiKey,
        endpoint: gatewayConfig.chat.endpoint,
        baseUrl: gatewayConfig.chat.baseUrl,
        chatPath: gatewayConfig.chat.path,
        model: gatewayConfig.chat.model,
        timeoutMs: readVoiceLlmTimeoutMs()
    };
}

async function buildVoiceDeviceContext(dbAll, asrText, deviceId) {
    if (typeof dbAll !== "function") {
        return "设备上下文：当前设备上下文不可用。涉及环境、空气状态、在线状态或模块状态时必须说明无法确认实时数据。";
    }

    const promptResult = await buildLlmPrompt(dbAll, asrText, {
        deviceId,
        mode: "voice"
    });
    if (promptResult.context) {
        return [
            buildContextText(promptResult.context),
            "涉及环境或空气状态时必须说明数据新鲜度和非 AQI 属性。"
        ].join("\n");
    }

    return "设备上下文：当前设备上下文不可用。涉及环境、空气状态、在线状态或模块状态时必须说明无法确认实时数据。";
}

function voiceToolFailureText(name) {
    return VOICE_TOOL_FAILURE_TEXT[name] || "抱歉，当前查询失败，暂时无法提供可靠结果。";
}

function voiceAgentError(error, gatewayConfig) {
    if (error?.code && error?.stage) {
        return error;
    }

    return createVoiceStageError("llm", "VOICE_LLM_FAILED", error?.message || "Voice Agent request failed", 502, {
        upstreamStatus: error?.status,
        bodyLength: error?.bodyLength,
        bodyPreview: error?.bodyPreview,
        endpoint: error?.endpoint || gatewayConfig.chat.endpoint,
        model: error?.model || gatewayConfig.chat.model,
        cause: error
    });
}

async function runVoiceAgentConversation(asrText, gatewayConfig, signal, options = {}) {
    const userText = normalizeVoiceText(asrText);
    const logger = options.logger || console;
    const deviceId = normalizeVoiceText(options.deviceId);
    const routedIntent = routeVoiceIntent(userText);
    const toolPolicy = routedIntent ? "forced" : "default";
    const toolTimeoutMs = Number.isFinite(options.toolTimeoutMs)
        ? options.toolTimeoutMs
        : readVoiceToolTimeoutMs();
    const toolStartedAt = new Map();
    const startedAt = Date.now();

    logger.log(
        `[voice-agent] VOICE_AGENT_START device_id=${maskLogValue(deviceId)} asr_text_length=${userText.length} asr_text=${JSON.stringify(normalizeLogPreview(userText, 60))} mode=voice tool_policy=${toolPolicy} required_tool=${routedIntent?.name || "-"} tool_timeout_ms=${toolTimeoutMs}`
    );

    try {
        const additionalSystemPrompt = await buildVoiceDeviceContext(options.dbAll, userText, deviceId);
        const agent = options.agentRunner || runAgentConversation;
        const result = await agent({
            dbAll: options.dbAll,
            toolRegistry: options.toolRegistry || createDefaultToolRegistry(),
            userText,
            deviceId,
            config: buildVoiceAgentConfig(gatewayConfig),
            logger,
            mode: "voice",
            toolPolicy,
            requiredToolName: routedIntent?.name || "",
            signal,
            toolTimeoutMs,
            failClosedOnToolFailure: true,
            weatherConfig: () => ({
                ...readWeatherConfig(logger),
                timeoutMs: toolTimeoutMs
            }),
            allowWeatherTool: false,
            additionalSystemPrompt,
            onToolCall: event => {
                const key = `${event.round}:${event.name}`;
                toolStartedAt.set(key, Date.now());
                logger.log(
                    `[voice-agent] VOICE_TOOL_CALL name=${event.name || "unknown"} device_id=${maskLogValue(deviceId)} round=${event.round} forced=${event.forced ? "1" : "0"}`
                );
            },
            onToolResult: event => {
                const key = `${event.round}:${event.name}`;
                const elapsedMs = Date.now() - (toolStartedAt.get(key) || Date.now());
                logger.log(
                    `[voice-agent] VOICE_TOOL_RESULT name=${event.name || "unknown"} device_id=${maskLogValue(deviceId)} round=${event.round} success=${event.result?.success === true ? "1" : "0"} elapsed_ms=${elapsedMs}`
                );
            }
        });
        const toolFailure = result?.tool_failure || null;
        const text = toolFailure ? voiceToolFailureText(toolFailure.name) : normalizeVoiceText(result?.text);
        if (!text) {
            throw new Error("Voice Agent did not return text");
        }

        logger.log(
            `[voice-agent] VOICE_AGENT_FINAL device_id=${maskLogValue(deviceId)} status=${toolFailure ? "tool_failed" : "success"} tool_rounds=${result?.tool_rounds ?? 0} tool_failure=${toolFailure?.name || "-"} reply_length=${text.length} elapsed_ms=${Date.now() - startedAt}`
        );
        return {
            text,
            model: result?.model || gatewayConfig.chat.model,
            tool_rounds: result?.tool_rounds ?? 0,
            tool_failure: toolFailure
        };
    } catch (error) {
        const normalizedError = voiceAgentError(error, gatewayConfig);
        logger.error(
            `[voice-agent] VOICE_AGENT_FINAL device_id=${maskLogValue(deviceId)} status=failed reply_length=0 elapsed_ms=${Date.now() - startedAt} code=${normalizedError.code || "VOICE_LLM_FAILED"} message=${JSON.stringify(normalizedError.message || "Voice Agent request failed")}`
        );
        throw normalizedError;
    }
}

module.exports = {
    VOICE_TOOL_FAILURE_TEXT,
    buildVoiceAgentConfig,
    routeVoiceIntent,
    runVoiceAgentConversation,
    voiceToolFailureText
};
