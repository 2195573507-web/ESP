const fs = require("fs");
const path = require("path");
const {
    buildAgentContext
} = require("./contextBuilder");
const {
    requestLlmChat
} = require("../llm/textClient");

const MAX_TOOL_ROUNDS = 3;
let cachedSystemPrompt = null;

function loadSystemPrompt() {
    if (cachedSystemPrompt !== null) {
        return cachedSystemPrompt;
    }
    const promptPath = path.join(__dirname, "..", "prompts", "esp-home-agent-system-prompt.txt");
    cachedSystemPrompt = fs.readFileSync(promptPath, "utf8").trim();
    if (!cachedSystemPrompt) {
        throw new Error("ESP Home Agent system prompt is empty");
    }
    return cachedSystemPrompt;
}

function toolMessage(toolCall, result) {
    return {
        role: "tool",
        tool_call_id: toolCall.id || toolCall.tool_call_id || toolCall.function?.name || "tool",
        content: JSON.stringify(result)
    };
}

function agentToolFailure(name, error, model, context, toolRounds) {
    return {
        text: "",
        model,
        context,
        tool_rounds: toolRounds,
        tool_failure: {
            name: name || "unknown",
            error: error || "tool execution failed"
        }
    };
}

function isForcedVoiceTool(options) {
    return options.mode === "voice" &&
        options.toolPolicy === "forced" &&
        typeof options.requiredToolName === "string" &&
        options.requiredToolName.length > 0;
}

function invokeWithTimeout(invoke, timeoutMs, signal) {
    if (!Number.isFinite(timeoutMs) || timeoutMs <= 0) {
        return invoke();
    }

    return new Promise((resolve, reject) => {
        let settled = false;
        const finish = callback => value => {
            if (settled) {
                return;
            }
            settled = true;
            clearTimeout(timer);
            signal?.removeEventListener("abort", onAbort);
            callback(value);
        };
        const onResolve = finish(resolve);
        const onReject = finish(reject);
        const onAbort = () => {
            const error = new Error("Agent tool invocation aborted");
            error.name = "AbortError";
            onReject(error);
        };
        const timer = setTimeout(() => {
            onResolve({ success: false, error: "tool execution timed out" });
        }, timeoutMs);

        if (signal?.aborted) {
            onAbort();
            return;
        }
        signal?.addEventListener("abort", onAbort, { once: true });
        Promise.resolve()
            .then(invoke)
            .then(onResolve, onReject);
    });
}

function resolveWeatherConfig(weatherConfig) {
    return typeof weatherConfig === "function" ? weatherConfig() : weatherConfig;
}

async function runAgentConversation(options) {
    const systemPrompt = options.systemPrompt || loadSystemPrompt();
    const allowWeatherTool = options.allowWeatherTool !== false;
    const activeTools = options.toolRegistry.openAiTools().filter(tool =>
        allowWeatherTool || tool?.function?.name !== "weather_query"
    );
    const contextRegistry = {
        list: () => options.toolRegistry.list().filter(tool =>
            allowWeatherTool || tool?.name !== "weather_query"
        )
    };
    const context = await buildAgentContext(options.dbAll, contextRegistry);
    const messages = [
        { role: "system", content: systemPrompt },
        { role: "system", content: `Dynamic context (not real-time sensor data):\n${JSON.stringify(context)}` }
    ];
    if (options.additionalSystemPrompt) {
        messages.push({ role: "system", content: options.additionalSystemPrompt });
    }
    messages.push({ role: "user", content: options.userText });

    const forcedVoiceTool = isForcedVoiceTool(options);
    let lastModel = options.config.model;
    for (let round = 0; round <= MAX_TOOL_ROUNDS; round += 1) {
        const response = await requestLlmChat(
            messages,
            activeTools,
            options.config,
            options.signal,
            forcedVoiceTool && round === 0
                ? {
                    toolChoice: {
                        type: "function",
                        function: { name: options.requiredToolName }
                    }
                }
                : {}
        );
        lastModel = response.model || lastModel;
        const toolCalls = Array.isArray(response.toolCalls) ? response.toolCalls : [];
        if (forcedVoiceTool && round === 0 &&
            (toolCalls.length !== 1 || toolCalls[0]?.function?.name !== options.requiredToolName)) {
            return agentToolFailure(
                options.requiredToolName,
                "required tool was not invoked",
                lastModel,
                context,
                round
            );
        }
        if (toolCalls.length === 0) {
            return { text: response.text, model: lastModel, context, tool_rounds: round };
        }

        messages.push({
            role: "assistant",
            content: response.text || null,
            tool_calls: toolCalls
        });
        for (const toolCall of toolCalls) {
            const toolName = toolCall?.function?.name || "";
            if (!allowWeatherTool && toolName === "weather_query") {
                return agentToolFailure(toolName, "weather tool is not enabled for this request", lastModel, context, round);
            }
            options.onToolCall?.({
                name: toolName,
                round,
                forced: forcedVoiceTool && round === 0
            });
            const result = await invokeWithTimeout(
                () => options.toolRegistry.invoke(toolName, toolCall?.function?.arguments, {
                    dbAll: options.dbAll,
                    deviceId: options.deviceId || "",
                    logger: options.logger,
                    weatherConfig: toolName === "weather_query"
                        ? resolveWeatherConfig(options.weatherConfig)
                        : undefined,
                    fetcher: options.fetcher
                }),
                options.toolTimeoutMs,
                options.signal
            );
            options.onToolResult?.({
                name: toolName,
                round,
                forced: forcedVoiceTool && round === 0,
                result
            });
            if (options.failClosedOnToolFailure && result?.success !== true) {
                return agentToolFailure(
                    toolName,
                    result?.error,
                    lastModel,
                    context,
                    round
                );
            }
            messages.push(toolMessage(toolCall, result));
        }
    }

    return {
        text: "实时查询未在允许的工具调用次数内完成，无法提供可靠结果。",
        model: lastModel,
        context,
        tool_rounds: MAX_TOOL_ROUNDS
    };
}

module.exports = {
    MAX_TOOL_ROUNDS,
    loadSystemPrompt,
    runAgentConversation
};
