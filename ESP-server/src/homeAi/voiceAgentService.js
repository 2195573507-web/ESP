const {
    requestLlmText
} = require("../llm/textClient");
const {
    buildLlmPrompt
} = require("../services/llmPromptContextService");
const {
    readVoiceLlmTimeoutMs
} = require("../voice/turnConfig");
const {
    orchestrate
} = require("./agentOrchestrator");
const {
    buildProfilePrompt,
    getPromptProfile,
    parseStrictJson
} = require("./promptProfiles");
const {
    listTools
} = require("./toolRegistry");
const {
    defaultRooms
} = require("./schema");

const VOICE_AGENT_LLM_TIMEOUT_MS = 20000;
const VOICE_AGENT_MAX_ROOMS = 3;
const VOICE_AGENT_MAX_DEVICES = 9;
const VOICE_AGENT_MIN_CONFIDENCE = 0.6;
const DEVICE_ID_PATTERN = /^[a-z0-9][a-z0-9_.-]{0,46}$/;
const ROOM_ID_PATTERN = /^[a-z0-9][a-z0-9_-]{0,31}$/;

const PROFILE_PATTERNS = Object.freeze([
    {
        name: "plan",
        pattern: /(?:打开|开启|关掉|关闭|开灯|关灯|保持打开|保持关闭|恢复自动|暂停自动|灯|空调|风扇|turn\s+(?:on|off)|light|fan|air\s*conditioner)/iu
    },
    {
        name: "intent",
        pattern: /(?:停止|取消|静音|恢复播报|撤销|不要这样|反馈|纠正|天气|新闻|简报|出门|起床|weather|news|briefing|cancel|mute|feedback)/iu
    },
    {
        name: "tool_calling",
        pattern: /(?:温度|湿度|环境|空气|有人|设备状态|历史|记忆|查询|多少度|temperature|humidity|environment|device\s+state|history|memory)/iu
    }
]);

function text(value, max = 256) {
    return typeof value === "string" ? value.trim().slice(0, max) : "";
}

function json(value, fallback = {}) {
    if (value === undefined || value === null || value === "") return fallback;
    try {
        const parsed = typeof value === "string" ? JSON.parse(value) : value;
        return parsed && typeof parsed === "object" ? parsed : fallback;
    } catch (_) {
        return fallback;
    }
}

function selectVoicePromptProfile(userText) {
    const value = text(userText, 4000);
    return PROFILE_PATTERNS.find(item => item.pattern.test(value))?.name || "conversation";
}

async function readVoiceAgentContext(dbAll, deviceId) {
    const context = {
        device_id: text(deviceId, 128),
        current_room_id: "",
        rooms: [],
        devices: []
    };
    if (typeof dbAll !== "function") return context;
    try {
        const rows = await dbAll("SELECT config_json FROM home_ai_rooms ORDER BY room_id ASC LIMIT ?", [VOICE_AGENT_MAX_ROOMS]);
        const configured = rows.map(row => json(row.config_json, {})).filter(room => ROOM_ID_PATTERN.test(text(room.room_id, 32)));
        context.rooms = configured.length > 0 ? configured : defaultRooms();
        const assigned = context.rooms.find(room => text(room.voice_terminal_device_id, 47) === context.device_id);
        context.current_room_id = text(assigned?.room_id, 32);
    } catch (_) {
        context.rooms = [];
    }
    try {
        const rows = await dbAll(
            "SELECT device_id,room_id,device_type FROM home_ai_virtual_devices ORDER BY room_id,device_id LIMIT ?",
            [VOICE_AGENT_MAX_DEVICES]
        );
        context.devices = rows.map(row => ({
            device_id: text(row.device_id, 47),
            room_id: text(row.room_id, 32),
            device_type: text(row.device_type, 32)
        })).filter(item => DEVICE_ID_PATTERN.test(item.device_id) && ROOM_ID_PATTERN.test(item.room_id));
    } catch (_) {
        context.devices = [];
    }
    const known = new Set(context.devices.map(device => device.device_id));
    for (const room of context.rooms) {
        for (const deviceType of ["light", "air_conditioner", "fan"]) {
            const deviceIdValue = `${room.room_id}_${deviceType}`;
            if (!DEVICE_ID_PATTERN.test(deviceIdValue) || known.has(deviceIdValue) || context.devices.length >= VOICE_AGENT_MAX_DEVICES) continue;
            context.devices.push({
                device_id: deviceIdValue,
                room_id: room.room_id,
                device_type: deviceType
            });
            known.add(deviceIdValue);
        }
    }
    return context;
}

function llmConfig(gatewayConfig, profile) {
    return {
        apiKey: gatewayConfig.apiKey,
        endpoint: gatewayConfig.chat.endpoint,
        baseUrl: gatewayConfig.chat.baseUrl,
        chatPath: gatewayConfig.chat.path,
        model: gatewayConfig.chat.model,
        timeoutMs: Math.min(readVoiceLlmTimeoutMs(), VOICE_AGENT_LLM_TIMEOUT_MS),
        systemPrompt: profile.system_prompt,
        jsonMode: profile.strict_json === true
    };
}

function safeFailureText(code) {
    if (code === "AGENT_CONFIDENCE_LOW") return "我没有充分把握识别这个家庭命令，请说得更明确一些。";
    if (code === "AGENT_DEVICE_NOT_ALLOWED") return "我无法确认要控制的设备，请说出房间和灯、空调或风扇。";
    if (code === "AGENT_ROOM_NOT_ALLOWED") return "我无法确认这个房间，请重新说明。";
    return "我没能安全解析这个家庭命令，请再说一次。";
}

function normalizeVoiceIntent(payload, profileName, homeContext) {
    if (payload.route === "conversation") {
        return { ok: true, conversation: true };
    }
    if (payload.route !== "home_ai" || !payload.intent || typeof payload.intent !== "object" || Array.isArray(payload.intent)) {
        return { ok: false, code: "AGENT_INTENT_INVALID" };
    }
    const intent = { ...payload.intent };
    intent.confidence = Number(intent.confidence);
    if (!Number.isFinite(intent.confidence) || intent.confidence < VOICE_AGENT_MIN_CONFIDENCE || intent.confidence > 1) {
        return { ok: false, code: "AGENT_CONFIDENCE_LOW" };
    }
    const knownRooms = new Set(homeContext.rooms.map(room => room.room_id));
    intent.room_id = text(intent.room_id, 32) || homeContext.current_room_id;
    if (intent.room_id && !knownRooms.has(intent.room_id)) {
        return { ok: false, code: "AGENT_ROOM_NOT_ALLOWED" };
    }
    if (Array.isArray(payload.steps)) intent.steps = payload.steps;
    if (Array.isArray(payload.actions)) intent.actions = payload.actions;
    if (profileName === "plan" && intent.type !== "control") {
        return { ok: false, code: "AGENT_INTENT_INVALID" };
    }
    if (profileName === "tool_calling" && !["query", "weather", "news", "briefing"].includes(intent.type)) {
        return { ok: false, code: "AGENT_INTENT_INVALID" };
    }
    if (intent.type === "control" && !Array.isArray(intent.steps) && !Array.isArray(intent.actions)) {
        const deviceType = text(intent.device_type, 32);
        if (!text(intent.device_id, 47) && intent.room_id && ["light", "air_conditioner", "fan"].includes(deviceType)) {
            intent.device_id = `${intent.room_id}_${deviceType}`;
        }
    }
    const allowedDevices = new Set(homeContext.devices.map(device => device.device_id));
    const actions = Array.isArray(intent.steps)
        ? intent.steps.flatMap(step => Array.isArray(step?.actions) ? step.actions : [])
        : (Array.isArray(intent.actions) ? intent.actions : []);
    if (intent.type === "control" && text(intent.device_id, 47) && !allowedDevices.has(text(intent.device_id, 47))) {
        return { ok: false, code: "AGENT_DEVICE_NOT_ALLOWED" };
    }
    for (const action of actions) {
        if (action?.tool !== "control_virtual_device") continue;
        const deviceId = text(action?.args?.device_id, 47);
        if (!allowedDevices.has(deviceId)) return { ok: false, code: "AGENT_DEVICE_NOT_ALLOWED" };
        const device = homeContext.devices.find(item => item.device_id === deviceId);
        action.args = {
            ...action.args,
            room_id: text(action.args.room_id, 32) || device?.room_id || intent.room_id
        };
    }
    if (intent.type === "query" && Array.isArray(intent.actions)) {
        intent.actions = intent.actions.map(action => ({
            ...action,
            args: action?.args && typeof action.args === "object" ? {
                ...action.args,
                ...(action.tool === "get_environment" && !text(action.args.room_id, 32) ? { room_id: intent.room_id } : {})
            } : {}
        }));
    }
    intent.source = "voice_profile_model";
    return { ok: true, intent };
}

async function requestHomeAiVoiceDecision(userText, gatewayConfig, signal, options = {}) {
    const profileName = selectVoicePromptProfile(userText);
    if (profileName === "conversation") {
        return { handled: false, profile: profileName };
    }
    const profile = getPromptProfile(profileName);
    const homeContext = await readVoiceAgentContext(options.dbAll, options.deviceId);
    const promptContext = typeof options.dbAll === "function"
        ? await buildLlmPrompt(options.dbAll, userText, { deviceId: options.deviceId, mode: "structured" })
        : { prompt: userText };
    const prompt = buildProfilePrompt(profileName, userText, promptContext.prompt, {
        current_room_id: homeContext.current_room_id,
        rooms: homeContext.rooms.map(room => ({
            room_id: room.room_id,
            room_name: text(room.room_name, 64)
        })),
        virtual_devices: homeContext.devices,
        tools: listTools().map(tool => ({ name: tool.name, input_schema: tool.input_schema }))
    });
    const requestModel = options.requestModel || requestLlmText;
    let modelResult;
    try {
        modelResult = await requestModel(prompt, llmConfig(gatewayConfig, profile), signal);
    } catch (_) {
        return {
            handled: true,
            profile: profileName,
            text: safeFailureText("AGENT_MODEL_FAILED"),
            code: "AGENT_MODEL_FAILED"
        };
    }
    const parsed = parseStrictJson(modelResult.text);
    if (!parsed.ok) {
        return {
            handled: true,
            profile: profileName,
            text: safeFailureText(parsed.code),
            code: parsed.code,
            model: modelResult.model || ""
        };
    }
    const normalized = normalizeVoiceIntent(parsed.value, profileName, homeContext);
    if (!normalized.ok) {
        return {
            handled: true,
            profile: profileName,
            text: safeFailureText(normalized.code),
            code: normalized.code,
            model: modelResult.model || ""
        };
    }
    if (normalized.conversation) {
        return { handled: false, profile: "conversation" };
    }
    let result;
    try {
        result = await orchestrate({ intent: normalized.intent }, {
            dbRun: options.dbRun,
            dbAll: options.dbAll,
            requestedBy: text(options.deviceId, 128) || "voice",
            traceId: text(options.traceId, 160)
        });
    } catch (_) {
        return {
            handled: true,
            profile: profileName,
            text: safeFailureText("AGENT_ORCHESTRATION_FAILED"),
            code: "AGENT_ORCHESTRATION_FAILED",
            model: modelResult.model || ""
        };
    }
    if (!result.ok) {
        return {
            handled: true,
            profile: profileName,
            text: safeFailureText(result.code),
            code: result.code,
            model: modelResult.model || ""
        };
    }
    const speech = text(result.decision?.speech?.final, 500) || "请求未完成，请稍后重试。";
    return {
        handled: true,
        profile: profileName,
        text: speech,
        model: modelResult.model || "",
        decision_id: result.decision.decision_id,
        decision_status: result.decision.status
    };
}

module.exports = {
    PROFILE_PATTERNS,
    VOICE_AGENT_MAX_DEVICES,
    VOICE_AGENT_MAX_ROOMS,
    normalizeVoiceIntent,
    readVoiceAgentContext,
    requestHomeAiVoiceDecision,
    selectVoicePromptProfile
};
