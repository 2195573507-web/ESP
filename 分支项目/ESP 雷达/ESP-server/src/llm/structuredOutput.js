const STRUCTURED_LLM_VERSION = "agent-command-v1";

function buildStructuredPrompt(userText) {
    return [
        "Return only valid JSON for this schema:",
        "{",
        "  \"chat\": { \"text\": \"natural language reply for TTS\" },",
        "  \"commands\": [",
        "    { \"name\": \"device.noop\", \"payload\": {}, \"reason\": \"short reason\" }",
        "  ]",
        "}",
        "Use commands only when the user asks for a concrete device action.",
        "Allowed command names are: device.noop, voice.set_volume, sensor.set_upload_interval, display.show_text, alert.play_tone.",
        "Do not choose a target device in JSON; the server will attach the target device from the request.",
        "If no command is needed, return an empty commands array.",
        "",
        `User text: ${userText}`
    ].join("\n");
}

function extractJsonCandidate(text) {
    const trimmed = String(text || "").trim();
    if (!trimmed) {
        return "";
    }

    if (trimmed.startsWith("{") && trimmed.endsWith("}")) {
        return trimmed;
    }

    const fenced = trimmed.match(/```(?:json)?\s*([\s\S]*?)```/i);
    if (fenced) {
        const candidate = fenced[1].trim();
        if (candidate.startsWith("{") && candidate.endsWith("}")) {
            return candidate;
        }
    }

    const first = trimmed.indexOf("{");
    const last = trimmed.lastIndexOf("}");
    if (first >= 0 && last > first) {
        return trimmed.slice(first, last + 1);
    }

    return "";
}

function readChatText(payload) {
    if (typeof payload?.chat === "string") {
        return payload.chat.trim();
    }

    if (payload?.chat && typeof payload.chat.text === "string") {
        return payload.chat.text.trim();
    }

    if (typeof payload?.text === "string") {
        return payload.text.trim();
    }

    return "";
}

function normalizeCommandList(payload) {
    if (Array.isArray(payload?.commands)) {
        return payload.commands;
    }

    if (payload?.command && typeof payload.command === "object") {
        return [payload.command];
    }

    return [];
}

function parseStructuredLlmOutput(text) {
    const jsonCandidate = extractJsonCandidate(text);
    if (!jsonCandidate) {
        return {
            ok: true,
            parsed: false,
            version: STRUCTURED_LLM_VERSION,
            chat_text: String(text || "").trim(),
            commands: [],
            raw_json: null,
            error: "LLM output did not contain JSON; fell back to pure chat text"
        };
    }

    let payload;
    try {
        payload = JSON.parse(jsonCandidate);
    } catch (error) {
        return {
            ok: true,
            parsed: false,
            version: STRUCTURED_LLM_VERSION,
            chat_text: String(text || "").trim(),
            commands: [],
            raw_json: null,
            error: `LLM JSON parse failed: ${error.message}`
        };
    }

    if (!payload || typeof payload !== "object" || Array.isArray(payload)) {
        return {
            ok: true,
            parsed: false,
            version: STRUCTURED_LLM_VERSION,
            chat_text: String(text || "").trim(),
            commands: [],
            raw_json: null,
            error: "LLM JSON root must be an object"
        };
    }

    return {
        ok: true,
        parsed: true,
        version: STRUCTURED_LLM_VERSION,
        chat_text: readChatText(payload),
        commands: normalizeCommandList(payload),
        raw_json: payload,
        error: ""
    };
}

module.exports = {
    STRUCTURED_LLM_VERSION,
    buildStructuredPrompt,
    parseStructuredLlmOutput
};
