const {
    getDeviceContext
} = require("./deviceContextService");
const fs = require("fs");
const path = require("path");

const SYSTEM_PROMPT_PATH = path.resolve(__dirname, "../../prompts/esp_home_agent_system.md");
let cachedSystemPrompt = null;

function getEspHomeAgentSystemPrompt() {
    if (cachedSystemPrompt === null) {
        cachedSystemPrompt = fs.readFileSync(SYSTEM_PROMPT_PATH, "utf8").trim();
    }

    return cachedSystemPrompt;
}

function formatAge(ageMs) {
    if (!Number.isFinite(ageMs)) {
        return "未知";
    }
    if (ageMs < 1000) {
        return `${Math.round(ageMs)} ms`;
    }
    return `${Math.round(ageMs / 1000)} 秒`;
}

function formatNullable(value, suffix = "") {
    return value === null || value === undefined || value === "" ? "未知" : `${value}${suffix}`;
}

function buildContextText(context) {
    const device = context.device || {};
    const env = context.environment || {};
    const air = context.air_quality || {};
    const modules = context.modules || {};
    const lines = [
        "设备上下文：",
        ...(context.context_unavailable
            ? ["- 当前设备上下文不可用。涉及环境、空气状态、在线状态或模块状态时必须说明无法确认实时数据。"]
            : []),
        `- 设备 ${device.device_id || "unknown"} 当前${device.online ? "在线" : "离线或未知"}，最近通信约 ${formatAge(device.last_seen_age_ms)} 前。`,
        `- 平均上传延迟 ${formatNullable(device.avg_upload_delay_ms, " ms")}，最近一次有效延迟 ${formatNullable(device.latest_upload_delay_ms, " ms")}，有效样本 ${device.delay_sample_count || 0}。`,
        `- 时间同步状态：${device.time_synced === true ? "已同步" : device.time_synced === false ? "未同步" : "未知"}。`
    ];

    if (!env.available) {
        lines.push("环境传感器：当前没有可用的实时 BME690 环境数据。回答环境问题时必须说明无法确认实时环境。");
    } else if (!env.fresh) {
        lines.push(`环境传感器：最近 BME690 数据已过期，约 ${formatAge(env.age_ms)} 前更新，只能作为历史参考。`);
    } else {
        lines.push(`环境传感器：BME690 数据约 ${formatAge(env.age_ms)} 前更新，温度 ${formatNullable(env.temperature_c, " C")}，湿度 ${formatNullable(env.humidity_percent, "%")}，气压 ${formatNullable(env.pressure_hpa, " hPa")}，气体电阻 ${formatNullable(env.gas_resistance_ohm, " ohm")}。`);
    }

    if (air.available) {
        lines.push(`空气状态：ESP 本地 BME690 相对空气状态估算为 ${air.level}，得分 ${air.score}/100，置信度 ${air.confidence}，来源 ${air.source || "unknown"}。注意：这不是国标 AQI，也不代表 PM2.5、PM10 或 CO2。`);
    } else {
        lines.push("空气状态：当前没有可靠的 ESP 本地空气状态估算；可以说明原始 BME690 数据，但不要给空气质量结论。");
    }

    lines.push(
        "模块状态：",
        `- sensor.bme690：${modules["sensor.bme690"]?.online ? "online" : "offline/unavailable"}`,
        `- voice.turn：${modules["voice.turn"]?.online ? "recent" : "not recent/unavailable"}`,
        `- voice.prompt：${modules["voice.prompt"]?.online ? "recent" : "not recent/unavailable"}`,
        `- csi.motion：${modules["csi.motion"]?.available ? "available" : "unavailable"}`,
        `- lcd.status：${modules["lcd.status"]?.available ? "available" : "unavailable"}`
    );

    return lines.join("\n");
}

function buildPromptWithContext(userText, context, mode = "text") {
    const contextText = buildContextText(context);
    return [
        "[System Prompt]",
        getEspHomeAgentSystemPrompt(),
        "",
        "[Runtime Context]",
        contextText,
        "",
        "[User Message]",
        String(userText || "").trim(),
        ...(mode === "structured"
            ? ["", "结构化命令解析仍必须尊重上述设备状态；设备离线或模块不可用时，不得声称已获得实时环境。"]
            : [])
    ].join("\n");
}

async function buildLlmPrompt(dbAll, userText, options = {}) {
    try {
        const context = await getDeviceContext(dbAll, options.deviceId || "");
        return {
            ok: true,
            context,
            prompt: buildPromptWithContext(userText, context, options.mode || "text")
        };
    } catch (error) {
        return {
            ok: false,
            context: null,
            prompt: buildPromptWithContext(userText, {
                context_unavailable: true,
                environment: {
                    available: false
                },
                air_quality: {
                    available: false
                }
            }, options.mode || "text"),
            error
        };
    }
}

module.exports = {
    buildContextText,
    buildLlmPrompt,
    buildPromptWithContext,
    getEspHomeAgentSystemPrompt
};
