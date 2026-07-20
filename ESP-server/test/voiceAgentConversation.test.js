const assert = require("assert");
const http = require("http");
const test = require("node:test");
const {
    runAgentConversation
} = require("../src/agent/agentRunner");
const {
    routeVoiceIntent,
    runVoiceAgentConversation,
    VOICE_TOOL_FAILURE_TEXT
} = require("../src/voice/agentConversation");

function createLogger() {
    const entries = [];
    return {
        entries,
        log: message => entries.push(message),
        warn: message => entries.push(message),
        error: message => entries.push(message)
    };
}

function createGatewayConfig(endpoint) {
    return {
        apiKey: "test-key",
        chat: {
            endpoint,
            baseUrl: endpoint,
            path: "/v1/chat/completions",
            model: "voice-test-model"
        }
    };
}

async function emptyDbAll() {
    return [];
}

function createRegistry(invoke) {
    return {
        openAiTools: () => [{
            type: "function",
            function: {
                name: "weather_query",
                description: "test weather tool",
                parameters: { type: "object", properties: {} }
            }
        }],
        list: () => [{ name: "weather_query", description: "test weather tool" }],
        invoke
    };
}

async function startChatServer(respond) {
    const requests = [];
    const server = http.createServer((req, res) => {
        const chunks = [];
        req.on("data", chunk => chunks.push(chunk));
        req.on("end", () => {
            const payload = JSON.parse(Buffer.concat(chunks).toString("utf8"));
            requests.push(payload);
            const body = respond(payload, requests.length);
            res.writeHead(200, { "Content-Type": "application/json" });
            res.end(JSON.stringify(body));
        });
    });
    await new Promise(resolve => server.listen(0, "127.0.0.1", resolve));
    const address = server.address();
    return {
        endpoint: `http://127.0.0.1:${address.port}/v1/chat/completions`,
        requests,
        close: () => new Promise(resolve => server.close(resolve))
    };
}

test("voice intent router only forces clear domain requests", () => {
    assert.equal(routeVoiceIntent("上海天气怎么样"), null);
    assert.deepEqual(routeVoiceIntent("家里现在是什么状态"), {
        name: "home_state_query",
        intent: "home_state"
    });
    assert.deepEqual(routeVoiceIntent("客厅温度和空气质量怎么样"), {
        name: "sensor_query",
        intent: "sensor"
    });
    assert.deepEqual(routeVoiceIntent("S3 网关在线吗"), {
        name: "device_status_query",
        intent: "device_status"
    });
    assert.equal(routeVoiceIntent("你好，讲个笑话"), null);
    assert.equal(routeVoiceIntent("温度多少"), null);
});

test("ordinary voice chat keeps normal Agent policy and does not emit a tool call", async () => {
    const chat = await startChatServer(payload => {
        assert.equal(Object.hasOwn(payload, "tool_choice"), false);
        return {
            model: "voice-test-model",
            choices: [{ message: { content: "这是一个简短的笑话。" } }]
        };
    });
    const logger = createLogger();
    try {
        const result = await runVoiceAgentConversation("你好，讲个笑话", createGatewayConfig(chat.endpoint), undefined, {
            dbAll: emptyDbAll,
            deviceId: "c5-test",
            logger
        });

        assert.equal(result.text, "这是一个简短的笑话。");
        assert.equal(chat.requests.length, 1);
        assert.ok(logger.entries.some(entry => entry.includes("VOICE_AGENT_START")));
        assert.ok(logger.entries.some(entry => entry.includes("VOICE_AGENT_FINAL") && entry.includes("status=success")));
        assert.equal(logger.entries.some(entry => entry.includes("VOICE_TOOL_CALL")), false);
    } finally {
        await chat.close();
    }
});

test("voice state queries pass only their corresponding forced tool to the Agent", async () => {
    const cases = [
        ["家里现在是什么状态", "home_state_query"],
        ["客厅空气质量怎么样", "sensor_query"],
        ["网关在线吗", "device_status_query"]
    ];

    for (const [text, requiredToolName] of cases) {
        let agentOptions = null;
        await runVoiceAgentConversation(text, createGatewayConfig("http://unused"), undefined, {
            dbAll: emptyDbAll,
            logger: createLogger(),
            agentRunner: async options => {
                agentOptions = options;
                return { text: "已处理。", model: "voice-test-model", tool_rounds: 1 };
            }
        });
        assert.equal(agentOptions.mode, "voice");
        assert.equal(agentOptions.toolPolicy, "forced");
        assert.equal(agentOptions.requiredToolName, requiredToolName);
    }
});

test("ordinary voice weather does not expose weather_query as a real-time tool", async () => {
    let agentOptions = null;
    await runVoiceAgentConversation("天气怎么样", createGatewayConfig("http://unused"), undefined, {
        dbAll: emptyDbAll,
        logger: createLogger(),
        agentRunner: async options => {
            agentOptions = options;
            return { text: "当前没有可确认的新鲜天气数据。", model: "voice-test-model", tool_rounds: 0 };
        }
    });
    assert.equal(agentOptions.toolPolicy, "default");
    assert.equal(agentOptions.requiredToolName, "");
    assert.equal(agentOptions.allowWeatherTool, false);
});

test("text Agent keeps its default tool policy and does not send tool_choice", async () => {
    const chat = await startChatServer(payload => {
        assert.equal(Object.hasOwn(payload, "tool_choice"), false);
        return {
            model: "text-test-model",
            choices: [{ message: { content: "普通文本回答。" } }]
        };
    });
    try {
        const result = await runAgentConversation({
            dbAll: emptyDbAll,
            toolRegistry: createRegistry(async () => ({ success: true })),
            userText: "你好",
            config: { apiKey: "test-key", endpoint: chat.endpoint, model: "text-test-model", timeoutMs: 1000 }
        });
        assert.equal(result.text, "普通文本回答。");
        assert.equal(chat.requests.length, 1);
    } finally {
        await chat.close();
    }
});

test("forced voice weather tool returns its result to the final LLM call", async () => {
    const chat = await startChatServer((payload, callNumber) => {
        if (callNumber === 1) {
            assert.deepEqual(payload.tool_choice, {
                type: "function",
                function: { name: "weather_query" }
            });
            return {
                model: "voice-test-model",
                choices: [{
                    message: {
                        content: null,
                        tool_calls: [{
                            id: "weather-1",
                            type: "function",
                            function: { name: "weather_query", arguments: "{}" }
                        }]
                    }
                }]
            };
        }

        assert.ok(payload.messages.some(message => message.role === "tool" && message.tool_call_id === "weather-1"));
        return {
            model: "voice-test-model",
            choices: [{ message: { content: "上海当前晴朗，26 摄氏度。" } }]
        };
    });
    try {
        const registry = createRegistry(async name => {
            assert.equal(name, "weather_query");
            return { success: true, weather: "clear", temperature: 26 };
        });
        const result = await runAgentConversation({
            dbAll: emptyDbAll,
            toolRegistry: registry,
            userText: "上海天气怎么样",
            deviceId: "c5-test",
            config: { apiKey: "test-key", endpoint: chat.endpoint, model: "voice-test-model", timeoutMs: 1000 },
            mode: "voice",
            toolPolicy: "forced",
            requiredToolName: "weather_query",
            failClosedOnToolFailure: true
        });

        assert.equal(result.text, "上海当前晴朗，26 摄氏度。");
        assert.equal(result.tool_failure, undefined);
        assert.equal(chat.requests.length, 2);
    } finally {
        await chat.close();
    }
});

test("voice tool failure stops before a second LLM answer and reports fixed speech", async () => {
    const chat = await startChatServer(() => ({
        model: "voice-test-model",
        choices: [{
            message: {
                content: null,
                tool_calls: [{
                    id: "weather-1",
                    type: "function",
                    function: { name: "weather_query", arguments: "{}" }
                }]
            }
        }]
    }));
    try {
        const logger = createLogger();
        const registry = createRegistry(async () => ({ success: false, error: "OpenWeather is not configured" }));
        const result = await runVoiceAgentConversation("天气怎么样", createGatewayConfig(chat.endpoint), undefined, {
            dbAll: emptyDbAll,
            deviceId: "c5-test",
            logger,
            toolRegistry: registry
        });

        assert.equal(result.text, VOICE_TOOL_FAILURE_TEXT.weather_query);
        assert.equal(result.tool_failure.name, "weather_query");
        assert.equal(chat.requests.length, 1);
        assert.ok(logger.entries.some(entry => entry.includes("VOICE_AGENT_START")));
        assert.equal(logger.entries.some(entry => entry.includes("VOICE_TOOL_CALL name=weather_query")), false);
        assert.equal(logger.entries.some(entry => entry.includes("VOICE_TOOL_RESULT name=weather_query")), false);
        assert.ok(logger.entries.some(entry => entry.includes("VOICE_AGENT_FINAL") && entry.includes("status=tool_failed")));
    } finally {
        await chat.close();
    }
});

test("voice tool timeout does not wait for the full voice turn budget", async () => {
    const chat = await startChatServer(() => ({
        model: "voice-test-model",
        choices: [{
            message: {
                content: null,
                tool_calls: [{
                    id: "weather-1",
                    type: "function",
                    function: { name: "weather_query", arguments: "{}" }
                }]
            }
        }]
    }));
    try {
        const startedAt = Date.now();
        const result = await runAgentConversation({
            dbAll: emptyDbAll,
            toolRegistry: createRegistry(async () => new Promise(() => {})),
            userText: "天气怎么样",
            config: { apiKey: "test-key", endpoint: chat.endpoint, model: "voice-test-model", timeoutMs: 1000 },
            mode: "voice",
            toolPolicy: "forced",
            requiredToolName: "weather_query",
            toolTimeoutMs: 20,
            failClosedOnToolFailure: true
        });

        assert.equal(result.tool_failure.name, "weather_query");
        assert.equal(result.tool_failure.error, "tool execution timed out");
        assert.ok(Date.now() - startedAt < 500);
        assert.equal(chat.requests.length, 1);
    } finally {
        await chat.close();
    }
});
