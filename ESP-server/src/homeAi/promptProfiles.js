const PROFILE_SCHEMAS = Object.freeze({
    intent: {
        route: "home_ai",
        intent: {
            type: "control|query|feedback|weather|news|briefing",
            confidence: "number 0..1",
            room_id: "optional room id",
            device_id: "optional virtual device id",
            action: "optional turn_on|turn_off",
            query: "optional short query",
            scene: "optional wake_up|leaving_home|user_requested|severe_weather"
        }
    },
    tool_calling: {
        route: "home_ai",
        intent: { type: "query|weather|news|briefing", confidence: "number 0..1" },
        actions: [{ tool: "registered tool name", args: {}, required: "boolean" }]
    },
    plan: {
        route: "home_ai",
        intent: { type: "control", confidence: "number 0..1", room_id: "room id" },
        steps: [{
            actions: [{ tool: "control_virtual_device", args: {}, required: true }],
            precondition: { type: "previous_step_succeeded", step_index: "integer" },
            continue_on_failure: false
        }]
    }
});

const PROMPT_PROFILES = Object.freeze({
    conversation: {
        output: "text",
        purpose: "ordinary conversation only",
        strict_json: false,
        system_prompt: "你是家庭语音助手的普通对话层。只回答用户可理解的自然语言，不声称已经控制设备，不输出 JSON，不猜测不存在的实时数据。涉及设备控制时，明确请用户用清晰的设备命令重试。"
    },
    intent: {
        output: "strict_json",
        purpose: "classify intent without executing actions",
        strict_json: true,
        schema: PROFILE_SCHEMAS.intent,
        system_prompt: "你是 Home AI 的意图层。只返回一个 JSON 对象，不要 Markdown、解释或代码围栏。仅把明确的家庭控制、查询、反馈、天气、新闻或场景简报归入 home_ai；不得执行动作，不得声称动作成功。confidence 必须是 0 到 1 的数字。"
    },
    tool_calling: {
        output: "strict_json",
        purpose: "select registered tools and arguments",
        strict_json: true,
        schema: PROFILE_SCHEMAS.tool_calling,
        system_prompt: "你是 Home AI 的 Tool Calling 层。只返回严格 JSON。只能选择上下文列出的已注册工具，参数必须是 JSON 对象；不得输出设备控制以外的自然语言，不得猜测工具结果。"
    },
    plan: {
        output: "strict_json",
        purpose: "produce bounded sequential steps with parallel actions",
        strict_json: true,
        schema: PROFILE_SCHEMAS.plan,
        system_prompt: "你是 Home AI 的复杂命令计划层。只返回严格 JSON。最多 4 个顺序步骤，每步最多 4 个动作，步骤内动作可并行；只使用已注册工具。只支持 previous_step_succeeded 前置条件，不生成循环、DAG 或补偿动作。控制动作必须等待 S3 ACK，不能声称已完成。"
    },
    rule_generation: {
        output: "strict_json",
        purpose: "produce a Home AI rule candidate",
        strict_json: true,
        system_prompt: "你是 Home AI 规则候选生成层。只输出严格 JSON 规则候选；只使用明确证据，不把沉默当作同意，不创建安全规则，不越过 Server 门禁。"
    },
    rule_safety_review: {
        output: "strict_json",
        purpose: "review safety and conflict gates",
        strict_json: true,
        system_prompt: "你是 Home AI 规则安全审查层。只输出严格 JSON 审查结果。检查冲突、影响范围、传感器新鲜度、资源上限和安全边界；不能自行发布规则。"
    },
    habit: {
        output: "strict_json",
        purpose: "summarize explicit habit evidence only",
        strict_json: true,
        system_prompt: "你是 Home AI 习惯归纳层。只依据明确反馈和已记录证据输出严格 JSON；没有明确证据时返回空候选，不把自动动作或沉默当作接受。"
    },
    memory_candidate: {
        output: "strict_json",
        purpose: "propose a non-authoritative memory candidate",
        strict_json: true,
        system_prompt: "你是 Home AI 记忆候选层。只输出严格 JSON 候选。记忆不是事实权威，必须可修改、可拒绝、可删除，不得直接改变设备控制。"
    },
    weather: {
        output: "strict_json",
        purpose: "answer only from fresh weather tool output",
        strict_json: true,
        system_prompt: "你是 Home AI 天气回答层。只依据本次新鲜天气工具结果输出严格 JSON；工具失败或数据过期时明确 unavailable，不猜测。"
    },
    news: {
        output: "strict_json",
        purpose: "answer only from fresh news tool output",
        strict_json: true,
        system_prompt: "你是 Home AI 新闻回答层。只依据本次新鲜新闻工具结果输出严格 JSON；过滤未来和过期条目，不把新闻用于设备控制。"
    },
    final_speech: {
        output: "text",
        purpose: "summarize actual action results without control JSON",
        strict_json: false,
        system_prompt: "你是 Home AI 最终语音层。只能把提供的实际工具结果改写成简短自然语言。不得输出 JSON、命令字段或不存在的成功结论；等待 ACK 时必须明确说正在等待确认，失败和部分失败必须说明。"
    },
    web_explanation: {
        output: "strict_json",
        purpose: "explain decisions for the Web",
        strict_json: true,
        system_prompt: "你是 Home AI Web 解释层。只输出严格 JSON，解释已落库的意图、步骤、实际结果和抑制原因，不添加未观察到的事实。"
    }
});

function getPromptProfile(name) {
    const key = typeof name === "string" ? name.trim() : "";
    return PROMPT_PROFILES[key] || null;
}

function buildProfilePrompt(name, userText, contextText = "", details = {}) {
    const profile = getPromptProfile(name);
    if (!profile) throw new Error(`unknown prompt profile: ${name}`);
    const sections = [];
    if (contextText) sections.push(String(contextText).slice(0, 12000));
    sections.push(`Home AI prompt profile: ${name}`);
    if (profile.schema) sections.push(`Required JSON shape:\n${JSON.stringify(profile.schema)}`);
    if (details && Object.keys(details).length > 0) {
        sections.push(`Trusted server context:\n${JSON.stringify(details).slice(0, 12000)}`);
    }
    sections.push(`User text:\n${String(userText || "").trim().slice(0, 4000)}`);
    return sections.join("\n\n");
}

function parseStrictJson(value) {
    if (typeof value !== "string") {
        return { ok: false, code: "AGENT_JSON_REQUIRED", error: "model output must be a JSON object" };
    }
    const trimmed = value.trim();
    if (!trimmed || !trimmed.startsWith("{") || !trimmed.endsWith("}")) {
        return { ok: false, code: "AGENT_JSON_INVALID", error: "model output must contain only one JSON object" };
    }
    try {
        const parsed = JSON.parse(trimmed);
        if (!parsed || typeof parsed !== "object" || Array.isArray(parsed)) {
            return { ok: false, code: "AGENT_JSON_INVALID", error: "model output root must be an object" };
        }
        return { ok: true, value: parsed };
    } catch (_) {
        return { ok: false, code: "AGENT_JSON_INVALID", error: "model output JSON could not be parsed" };
    }
}

function listPromptProfiles() {
    return Object.fromEntries(Object.entries(PROMPT_PROFILES).map(([name, profile]) => [name, {
        output: profile.output,
        purpose: profile.purpose,
        strict_json: profile.strict_json
    }]));
}

module.exports = {
    PROMPT_PROFILES,
    buildProfilePrompt,
    getPromptProfile,
    listPromptProfiles,
    parseStrictJson
};
