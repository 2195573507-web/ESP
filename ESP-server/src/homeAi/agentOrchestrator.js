const crypto = require("crypto");
const {
    executeTool,
    listTools
} = require("./toolRegistry");
const {
    PROMPT_PROFILES,
    listPromptProfiles: listPromptProfileMetadata
} = require("./promptProfiles");

const MAX_PLAN_STEPS = 4;
const MAX_ACTIONS_PER_STEP = 4;
const AGENT_MAX_EXECUTION_MS = 60000;
const INTENT_TYPES = new Set(["control", "query", "feedback", "weather", "news", "briefing"]);
const BRIEFING_SCENES = new Set(["wake_up", "leaving_home", "user_requested", "severe_weather"]);
const BRIEFING_DEDUPE_WINDOW_MS = 30 * 60 * 1000;

const INTENT_TOOL_POLICY = Object.freeze({
    control: new Set(["get_environment", "get_device_state", "control_virtual_device", "query_history", "query_memory", "get_weather"]),
    query: new Set(["get_environment", "get_device_state", "query_history", "query_memory"]),
    feedback: new Set(["save_feedback"]),
    weather: new Set(["get_weather"]),
    news: new Set(["get_news"]),
    briefing: new Set(["get_weather", "get_news", "get_environment", "query_history"])
});

function makeDecisionId() {
    const value = typeof crypto.randomUUID === "function"
        ? crypto.randomUUID()
        : crypto.randomBytes(16).toString("hex");
    return `decision_${value}`;
}

function text(value, max = 256) {
    return typeof value === "string" ? value.trim().slice(0, max) : "";
}

function jsonText(value) {
    try {
        return JSON.stringify(value === undefined ? {} : value);
    } catch (_) {
        return "{}";
    }
}

function parseJson(value, fallback = {}) {
    if (value === undefined || value === null || value === "") return fallback;
    try {
        const parsed = typeof value === "string" ? JSON.parse(value) : value;
        return parsed === undefined ? fallback : parsed;
    } catch (_) {
        return fallback;
    }
}

function normalizeAction(action) {
    const tool = text(action?.tool, 64);
    const registered = new Set(listTools().map(item => item.name));
    if (!registered.has(tool) || !action?.args || typeof action.args !== "object" || Array.isArray(action.args)) {
        return { ok: false, code: "AGENT_ACTION_INVALID", error: "action tool or args are invalid" };
    }
    return {
        ok: true,
        action: {
            action_id: text(action.action_id, 80) || `${tool}_${crypto.randomBytes(5).toString("hex")}`,
            tool,
            args: action.args,
            required: action.required !== false
        }
    };
}

function responseTypeForIntent(type) {
    if (type === "control") return "complex_command";
    if (type === "weather") return "weather_answer";
    if (type === "news") return "news_answer";
    if (type === "briefing") return "scene_briefing";
    if (type === "feedback") return "feedback_result";
    return "query_result";
}

function actionAllowedForIntent(intentType, tool) {
    return Boolean(INTENT_TOOL_POLICY[intentType]?.has(tool));
}

function normalizeIntent(input = {}) {
    const source = input.intent && typeof input.intent === "object" ? input.intent : input;
    const type = text(source.type, 32);
    const confidence = Number(source.confidence);
    if (!INTENT_TYPES.has(type) || !Number.isFinite(confidence) || confidence < 0 || confidence > 1) {
        return { ok: false, code: "AGENT_INTENT_INVALID", error: "strict intent type and confidence are required" };
    }
    return {
        ok: true,
        intent: {
            type,
            confidence,
            room_id: text(source.room_id, 32),
            source: text(source.source, 32) || "structured_model",
            scene: text(source.scene, 32),
            raw: source
        }
    };
}

function actionsFromIntent(intent) {
    const raw = intent.raw;
    if (Array.isArray(raw.actions)) return raw.actions;
    switch (intent.type) {
    case "weather": return [{ tool: "get_weather", args: {}, required: true }];
    case "news": return [{ tool: "get_news", args: { query: text(raw.query, 120), limit: Number(raw.limit) || 5 }, required: true }];
    case "feedback": return [{ tool: "save_feedback", args: raw.feedback || raw, required: true }];
    case "control": return [{
        tool: "control_virtual_device",
        args: {
            device_id: text(raw.device_id, 47),
            action: text(raw.action, 32),
            room_id: intent.room_id,
            gateway_id: text(raw.gateway_id, 128)
        },
        required: true
    }];
    case "query": return [{ tool: text(raw.tool, 64), args: raw.args || {}, required: true }];
    case "briefing": return [
        { tool: "get_weather", args: {}, required: false },
        { tool: "get_news", args: { query: text(raw.query, 120), limit: Number(raw.limit) || 5 }, required: false }
    ];
    default: return [];
    }
}

function buildPlan(intent) {
    const rawSteps = Array.isArray(intent.raw.steps)
        ? intent.raw.steps
        : [{ actions: actionsFromIntent(intent), continue_on_failure: false }];
    if (rawSteps.length === 0 || rawSteps.length > MAX_PLAN_STEPS) {
        return { ok: false, code: "AGENT_PLAN_STEP_LIMIT", error: `plan requires 1-${MAX_PLAN_STEPS} steps` };
    }
    const steps = [];
    for (let index = 0; index < rawSteps.length; index += 1) {
        const rawActions = Array.isArray(rawSteps[index]?.actions) ? rawSteps[index].actions : [];
        if (rawActions.length === 0 || rawActions.length > MAX_ACTIONS_PER_STEP) {
            return { ok: false, code: "AGENT_PLAN_ACTION_LIMIT", error: `each step requires 1-${MAX_ACTIONS_PER_STEP} actions` };
        }
        const actions = [];
        for (const rawAction of rawActions) {
            const normalized = normalizeAction(rawAction);
            if (!normalized.ok) return normalized;
            if (!actionAllowedForIntent(intent.type, normalized.action.tool)) {
                return {
                    ok: false,
                    code: "AGENT_TOOL_NOT_ALLOWED",
                    error: `${normalized.action.tool} is not allowed for ${intent.type} intent`
                };
            }
            actions.push(normalized.action);
        }
        const rawPrecondition = rawSteps[index]?.precondition ?? rawSteps[index]?.when ?? null;
        let precondition = null;
        if (rawPrecondition !== null && rawPrecondition !== undefined) {
            if (!rawPrecondition || typeof rawPrecondition !== "object" || Array.isArray(rawPrecondition) ||
                rawPrecondition.type !== "previous_step_succeeded" ||
                Number.parseInt(rawPrecondition.step_index, 10) !== index - 1) {
                return {
                    ok: false,
                    code: "AGENT_PRECONDITION_INVALID",
                    error: "only a previous_step_succeeded precondition is supported"
                };
            }
            precondition = {
                type: "previous_step_succeeded",
                step_index: index - 1
            };
        }
        if (index === 0 && precondition !== null) {
            return {
                ok: false,
                code: "AGENT_PRECONDITION_INVALID",
                error: "the first step cannot depend on a previous step"
            };
        }
        steps.push({
            step_index: index,
            actions,
            precondition,
            continue_on_failure: rawSteps[index]?.continue_on_failure === true
        });
    }
    return {
        ok: true,
        plan: {
            model: "bounded_sequential_v1",
            max_steps: MAX_PLAN_STEPS,
            max_actions_per_step: MAX_ACTIONS_PER_STEP,
            steps
        }
    };
}

function speechFor(plan, execution, options = {}) {
    const hasSlowTool = plan.steps.some(step => step.actions.some(action => ["get_weather", "get_news"].includes(action.tool)));
    if (!execution) {
        return {
            start: hasSlowTool ? "正在处理。" : "",
            progress_allowed: hasSlowTool,
            final: "",
            based_on_actual_results: true,
            channel: options.channel || "voice",
            response_type: options.responseType || "complex_command"
        };
    }
    const results = execution.steps.flatMap(step => step.actions);
    const succeeded = results.filter(item => item.status === "succeeded");
    const failed = results.filter(item => item.status === "failed");
    const skipped = results.filter(item => item.status === "skipped");
    const awaitingAck = succeeded.some(item => item.result?.result?.verified === false);
    let final = "操作未完成。";
    const errors = [...failed, ...skipped].map(item => item.error?.code).filter(Boolean);
    const weather = succeeded.find(item => item.tool === "get_weather")?.result?.result;
    const news = succeeded.find(item => item.tool === "get_news")?.result?.result;
    const feedback = succeeded.find(item => item.tool === "save_feedback");
    const weatherSummary = weather?.fresh === true ? (() => {
        const temperature = Number(weather.current?.temperature_2m);
        const humidity = Number(weather.current?.relative_humidity_2m);
        const parts = [];
        if (Number.isFinite(temperature)) parts.push(`${temperature} 摄氏度`);
        if (Number.isFinite(humidity)) parts.push(`湿度 ${humidity}%`);
        return `${text(weather.location?.city, 40) || "家庭所在地"}当前${parts.join("，") || "天气数据已更新"}。`;
    })() : "";
    const newsTitles = (Array.isArray(news?.articles) ? news.articles : [])
        .map(article => text(article?.title, 80))
        .filter(Boolean)
        .slice(0, 3);
    const newsSummary = news?.fresh === true ?
        (newsTitles.length > 0 ? `最新消息：${newsTitles.join("；")}。` : "最新新闻已获取。") : "";
    if (awaitingAck && failed.length === 0 && skipped.length === 0) final = "命令已提交，等待设备确认。";
    else if (failed.length === 0 && skipped.length === 0) {
        if (weatherSummary || newsSummary) final = `${weatherSummary}${newsSummary}`;
        else if (feedback) final = "反馈已记录。";
        else final = "已完成。";
    }
    else if (succeeded.length > 0) {
        const unavailable = [];
        if (errors.some(code => code.startsWith("WEATHER_"))) unavailable.push("天气未获取");
        if (errors.some(code => code.startsWith("NEWS_"))) unavailable.push("新闻未获取");
        final = `${weatherSummary}${newsSummary}${unavailable.length ? `${unavailable.join("，")}。` : "部分操作未完成。"}`;
    }
    if (skipped.length > 0 && failed.length === 0) {
        final = succeeded.length > 0 ? "前面的操作已完成，后续步骤因前置条件未满足而未执行。" :
            "前置条件未满足，操作未执行。";
    } else if (failed.length > 0 && succeeded.length === 0) {
        const first = errors[0] || "TOOL_EXECUTION_FAILED";
        const labels = {
            WEATHER_TIMEOUT: "天气服务超时。",
            WEATHER_STALE: "天气数据已过期，未使用旧数据。",
            WEATHER_LOCATION_NOT_CONFIGURED: "尚未配置家庭位置。",
            NEWS_TIMEOUT: "新闻服务超时。",
            NEWS_STALE: "没有可用的新鲜新闻。",
            NEWS_NOT_CONFIGURED: "尚未配置新闻服务。"
        };
        final = labels[first] || "请求未完成，请稍后重试。";
    }
    return {
        start: hasSlowTool ? "正在处理。" : "",
        progress_allowed: hasSlowTool,
        final,
        based_on_actual_results: true,
        succeeded_count: succeeded.length,
        failed_count: failed.length,
        skipped_count: skipped.length,
        awaiting_ack: awaitingAck,
        errors,
        channel: options.channel || "voice",
        response_type: options.responseType || "complex_command"
    };
}

async function persistDecision(context, decision) {
    if (typeof context.dbRun !== "function") return null;
    const nowMs = Date.now();
    await context.dbRun(
        `INSERT INTO home_ai_agent_decisions
         (decision_id,response_type,intent_json,plan_json,action_json,speech_json,status,created_at_ms,updated_at_ms)
         VALUES(?,?,?,?,?,?,?,?,?)
         ON CONFLICT(decision_id) DO UPDATE SET
           action_json=excluded.action_json,speech_json=excluded.speech_json,
           status=excluded.status,updated_at_ms=excluded.updated_at_ms`,
        [decision.decision_id, decision.response_type, jsonText(decision.intent), jsonText(decision.control),
            jsonText(decision.execution || {}), jsonText(decision.speech), decision.status,
            decision.created_at_ms, nowMs]
    );
    for (const step of decision.execution?.steps || []) {
        await context.dbRun(
            `INSERT INTO home_ai_decision_steps
             (decision_id,step_index,status,step_json,result_json,updated_at_ms)
             VALUES(?,?,?,?,?,?)
             ON CONFLICT(decision_id,step_index) DO UPDATE SET
               status=excluded.status,result_json=excluded.result_json,updated_at_ms=excluded.updated_at_ms`,
            [decision.decision_id, step.step_index, step.status,
                jsonText(decision.control.steps[step.step_index]), jsonText(step), nowMs]
        );
    }
    return reconcilePendingAcks(context, decision.decision_id);
}

async function savePendingAck(context, command) {
    if (typeof context.dbRun !== "function" || !command?.command_id || !command?.decision_id) return;
    await context.dbRun(
        `INSERT INTO home_ai_agent_acks
         (command_id,decision_id,status,result_json,error_message,received_at_ms)
         VALUES(?,?,?,?,?,?)
         ON CONFLICT(command_id) DO UPDATE SET
           decision_id=excluded.decision_id,status=excluded.status,
           result_json=excluded.result_json,error_message=excluded.error_message,
           received_at_ms=excluded.received_at_ms`,
        [command.command_id, command.decision_id, command.status || "failed",
            jsonText(command.result || {}), text(command.error_message, 500), Date.now()]
    );
}

function applyAckToExecution(plan, execution, command) {
    let matched = false;
    for (const step of execution.steps || []) {
        for (const action of step.actions || []) {
            const commandId = action.result?.result?.command_id;
            if (commandId !== command.command_id) continue;
            matched = true;
            const previousResult = action.result?.result || {};
            const acknowledgedResult = {
                ...previousResult,
                command_id: command.command_id,
                status: command.status,
                verified: command.status === "succeeded",
                ack: command.result || null,
                error_message: command.error_message || ""
            };
            action.result = { ...(action.result || {}), result: acknowledgedResult };
            if (command.status === "succeeded") {
                action.status = "succeeded";
                delete action.error;
            } else {
                action.status = "failed";
                action.error = {
                    code: `S3_${String(command.status || "failed").toUpperCase()}`,
                    message: command.error_message || "S3 command failed"
                };
            }
        }
        const actions = step.actions || [];
        const failed = actions.filter(action => action.status === "failed").length;
        const succeeded = actions.filter(action => action.status === "succeeded").length;
        const pending = actions.some(action => action.result?.result?.verified === false);
        step.status = pending ? "waiting_ack" : failed > 0 && succeeded > 0 ? "partial" : failed > 0 ? "failed" : "succeeded";
    }
    if (!matched) return false;
    execution.completed_at_ms = Date.now();
    execution.partial = (execution.steps || []).some(step => step.status === "partial" || step.status === "failed");
    return true;
}

async function persistAckedDecision(context, row, command) {
    const plan = parseJson(row.plan_json, { steps: [] });
    const execution = parseJson(row.action_json, { steps: [] });
    if (!applyAckToExecution(plan, execution, command)) return false;
    const actionResults = (execution.steps || []).flatMap(step => step.actions || []);
    const pending = actionResults.some(action => action.result?.result?.verified === false);
    const failedCount = actionResults.filter(action => action.status === "failed").length;
    const succeededCount = actionResults.filter(action => action.status === "succeeded").length;
    const speech = speechFor(plan, execution, {
        responseType: row.response_type || "complex_command"
    });
    const status = pending ? "WAITING_ACK" :
        (failedCount > 0 && succeededCount === 0 ? "FAILED" :
            (execution.partial ? "PARTIAL" : "COMPLETED"));
    const nowMs = Date.now();
    await context.dbRun(
        `UPDATE home_ai_agent_decisions
         SET action_json=?, speech_json=?, status=?, updated_at_ms=?
         WHERE decision_id=?`,
        [jsonText(execution), jsonText(speech), status, nowMs, row.decision_id]
    );
    for (const step of execution.steps || []) {
        await context.dbRun(
            `UPDATE home_ai_decision_steps SET status=?, result_json=?, updated_at_ms=?
             WHERE decision_id=? AND step_index=?`,
            [step.status, jsonText(step), nowMs, row.decision_id, step.step_index]
        );
    }
    return { decision_id: row.decision_id, status, speech, execution };
}

async function reconcilePendingAcks(context, decisionId) {
    if (typeof context.dbAll !== "function" || typeof context.dbRun !== "function") return null;
    const rows = await context.dbAll(
        "SELECT * FROM home_ai_agent_acks WHERE decision_id=? ORDER BY received_at_ms ASC",
        [decisionId]
    );
    if (!rows.length) return null;
    const decisions = await context.dbAll(
        "SELECT * FROM home_ai_agent_decisions WHERE decision_id=? LIMIT 1",
        [decisionId]
    );
    if (!decisions[0]) return null;
    let latest = null;
    for (const row of rows) {
        const applied = await persistAckedDecision(context, decisions[0], {
            decision_id: row.decision_id,
            command_id: row.command_id,
            status: row.status,
            result: parseJson(row.result_json, {}),
            error_message: row.error_message || ""
        });
        if (applied) {
            await context.dbRun("DELETE FROM home_ai_agent_acks WHERE command_id=?", [row.command_id]);
            decisions[0].action_json = JSON.stringify(applied.execution);
            decisions[0].speech_json = JSON.stringify(applied.speech);
            decisions[0].status = applied.status;
            latest = applied;
        }
    }
    return latest;
}

async function executePlan(plan, context) {
    const execution = { steps: [], started_at_ms: Date.now(), completed_at_ms: null };
    let halted = false;
    let timedOut = false;
    const deadlineMs = Number.isFinite(Number(context.deadline_ms))
        ? Number(context.deadline_ms)
        : execution.started_at_ms + AGENT_MAX_EXECUTION_MS;
    for (const step of plan.steps) {
        if (halted) break;
        if (step.precondition?.type === "previous_step_succeeded") {
            const previous = execution.steps.find(item => item.step_index === step.precondition.step_index);
            if (!previous || previous.status !== "succeeded") {
                execution.steps.push({
                    step_index: step.step_index,
                    status: "skipped",
                    actions: step.actions.map(action => ({
                        action_id: action.action_id,
                        tool: action.tool,
                        status: "skipped",
                        error: {
                            code: "AGENT_PRECONDITION_NOT_MET",
                            message: "previous step did not succeed"
                        }
                    }))
                });
                halted = !step.continue_on_failure;
                continue;
            }
        }
        if (Date.now() >= deadlineMs) {
            timedOut = true;
            execution.steps.push({
                step_index: step.step_index,
                status: "failed",
                actions: step.actions.map(action => ({
                    action_id: action.action_id,
                    tool: action.tool,
                    status: "failed",
                    error: { code: "AGENT_TIMEOUT", message: "agent execution deadline exceeded" }
                }))
            });
            halted = true;
            break;
        }
        const settled = [];
        /* Keep the four-action plan bound while respecting the Tool Registry's
         * fixed three-request concurrency budget. */
        for (let offset = 0; offset < step.actions.length; offset += 3) {
            if (Date.now() >= deadlineMs) {
                for (let index = offset; index < step.actions.length; index += 1) {
                    settled.push({
                        status: "rejected",
                        reason: { code: "AGENT_TIMEOUT", message: "agent execution deadline exceeded" }
                    });
                }
                timedOut = true;
                break;
            }
            const batch = step.actions.slice(offset, offset + 3);
            const results = await Promise.allSettled(batch.map(action => executeTool(
                action.tool,
                action.args,
                { ...context, deadline_ms: deadlineMs }
            )));
            settled.push(...results);
        }
        const actions = settled.map((result, index) => {
            const action = step.actions[index];
            if (result.status === "fulfilled") {
                return { action_id: action.action_id, tool: action.tool, status: "succeeded", result: result.value };
            }
            return {
                action_id: action.action_id,
                tool: action.tool,
                status: "failed",
                error: {
                    code: result.reason?.code || "TOOL_EXECUTION_FAILED",
                    message: result.reason?.message || "tool execution failed"
                }
            };
        });
        const requiredFailed = actions.some((action, index) => action.status === "failed" && step.actions[index].required);
        const waitingAck = actions.some(action => action.result?.result?.verified === false);
        execution.steps.push({
            step_index: step.step_index,
            status: waitingAck ? "waiting_ack" : requiredFailed ? "failed" :
                (actions.some(action => action.status === "failed") ? "partial" : "succeeded"),
            actions
        });
        halted = requiredFailed && !step.continue_on_failure;
    }
    execution.completed_at_ms = Date.now();
    execution.partial = execution.steps.some(step => ["partial", "failed", "skipped"].includes(step.status));
    execution.halted = halted;
    execution.timed_out = timedOut;
    return execution;
}

function briefingEligibility(intent) {
    const raw = intent.raw || {};
    const scene = text(raw.scene || intent.scene, 32);
    if (!BRIEFING_SCENES.has(scene)) {
        return { ok: false, code: "BRIEFING_SCENE_INVALID", reason: "scene is not supported" };
    }
    const presence = text(raw.presence_state || raw.room_presence, 24);
    const confidence = Number(raw.presence_confidence ?? raw.room_presence_confidence ?? intent.confidence);
    const dataValid = raw.data_valid === true;
    const terminalAvailable = raw.voice_terminal_available !== false;
    const c5Online = raw.c5_online === true || raw.voice_terminal_online === true;
    const voiceIdle = raw.voice_idle === true;
    const muted = raw.muted === true;
    const duplicate = raw.duplicate === true;
    if (presence !== "occupied" || !Number.isFinite(confidence) || confidence < 0.85 || !dataValid) {
        return { ok: false, code: "BRIEFING_ELIGIBILITY_LOW", reason: "room presence or source data is not sufficiently fresh" };
    }
    if (duplicate) return { ok: false, code: "BRIEFING_DUPLICATE", reason: "briefing was recently delivered" };
    if (terminalAvailable && (!c5Online || !voiceIdle || muted)) {
        return { ok: false, code: "BRIEFING_VOICE_UNAVAILABLE", reason: "voice terminal is offline, busy, or muted" };
    }
    return {
        ok: true,
        scene,
        room_id: text(intent.room_id || raw.room_id, 32),
        channel: terminalAvailable ? "voice" : "web_only"
    };
}

function briefingHash(intent) {
    const raw = intent.raw || {};
    return crypto.createHash("sha256")
        .update(JSON.stringify({
            scene: text(raw.scene || intent.scene, 32),
            room_id: text(intent.room_id || raw.room_id, 32),
            query: text(raw.query, 120)
        }))
        .digest("hex");
}

async function briefingWasRecentlyDelivered(context, eligibility, hash, nowMs) {
    if (typeof context.dbAll !== "function" || !eligibility.room_id) return false;
    const rows = await context.dbAll(
        `SELECT 1 FROM home_ai_briefing_runs
         WHERE scene=? AND room_id=? AND content_hash=? AND created_at_ms>=?
         LIMIT 1`,
        [eligibility.scene, eligibility.room_id, hash, nowMs - BRIEFING_DEDUPE_WINDOW_MS]
    );
    return rows.length > 0;
}

async function recordBriefingRun(context, decision, eligibility, hash, status) {
    if (typeof context.dbRun !== "function" || !eligibility?.scene) return;
    await context.dbRun(
        `INSERT INTO home_ai_briefing_runs
         (briefing_id,scene,room_id,decision_id,delivery_channel,status,content_hash,created_at_ms)
         VALUES(?,?,?,?,?,?,?,?)`,
        [`briefing_${crypto.randomBytes(8).toString("hex")}`, eligibility.scene,
            eligibility.room_id || "", decision.decision_id, eligibility.channel, status, hash, Date.now()]
    );
}

function suppressedDecision(intent, code, reason, channel = "web") {
    const createdAtMs = Date.now();
    const decisionId = makeDecisionId();
    const speech = {
        start: "",
        progress_allowed: false,
        final: reason,
        based_on_actual_results: false,
        channel,
        response_type: "scene_briefing",
        suppression_code: code
    };
    return {
        response_type: "scene_briefing",
        decision_id: decisionId,
        intent,
        control: { model: "bounded_sequential_v1", max_steps: MAX_PLAN_STEPS, max_actions_per_step: MAX_ACTIONS_PER_STEP, steps: [] },
        steps: [],
        speech,
        speech_policy: speech,
        execution: null,
        status: "SUPPRESSED",
        suppression_code: code,
        created_at_ms: createdAtMs
    };
}

async function orchestrate(input = {}, context = {}) {
    const normalized = normalizeIntent(input);
    if (!normalized.ok) return normalized;
    let briefing = null;
    let briefingHashValue = "";
    if (normalized.intent.type === "briefing") {
        briefing = briefingEligibility(normalized.intent);
        if (!briefing.ok) {
            const decision = suppressedDecision(normalized.intent, briefing.code, briefing.reason);
            await persistDecision(context, decision);
            return { ok: true, decision };
        }
        briefingHashValue = briefingHash(normalized.intent);
        if (await briefingWasRecentlyDelivered(context, briefing, briefingHashValue, Date.now())) {
            const decision = suppressedDecision(normalized.intent, "BRIEFING_DUPLICATE", "简报最近已发送。", briefing.channel);
            await persistDecision(context, decision);
            return { ok: true, decision };
        }
    }
    const planned = buildPlan(normalized.intent);
    if (!planned.ok) return planned;
    const createdAtMs = Date.now();
    const responseType = responseTypeForIntent(normalized.intent.type);
    const channel = briefing?.channel || "voice";
    const initialSpeech = speechFor(planned.plan, null, { channel, responseType });
    const decision = {
        response_type: responseType,
        decision_id: makeDecisionId(),
        intent: normalized.intent,
        control: planned.plan,
        speech: initialSpeech,
        speech_policy: initialSpeech,
        steps: planned.plan.steps,
        execution: null,
        status: input.execute === false ? "PLANNED" : "EXECUTING",
        created_at_ms: createdAtMs
    };
    const reconciled = await persistDecision(context, decision);
    if (reconciled) {
        decision.execution = reconciled.execution;
        decision.speech = reconciled.speech;
        decision.speech_policy = reconciled.speech;
        decision.status = reconciled.status;
    }
    if (input.execute === false) return { ok: true, decision };
    decision.execution = await executePlan(planned.plan, {
        ...context,
        decisionId: decision.decision_id,
        intentType: normalized.intent.type,
        deadline_ms: createdAtMs + AGENT_MAX_EXECUTION_MS
    });
    decision.speech = speechFor(planned.plan, decision.execution, { channel, responseType });
    decision.speech_policy = decision.speech;
    decision.steps = planned.plan.steps;
    const awaitingAck = decision.speech.awaiting_ack;
    const actionResults = decision.execution.steps.flatMap(step => step.actions);
    const failedCount = actionResults.filter(action => action.status === "failed").length;
    const succeededCount = actionResults.filter(action => action.status === "succeeded").length;
    decision.status = awaitingAck ? "WAITING_ACK" :
        (failedCount > 0 && succeededCount === 0 ? "FAILED" :
            (decision.execution.partial ? "PARTIAL" : "COMPLETED"));
    const finalReconciled = await persistDecision(context, decision);
    if (finalReconciled) {
        decision.execution = finalReconciled.execution;
        decision.speech = finalReconciled.speech;
        decision.speech_policy = finalReconciled.speech;
        decision.status = finalReconciled.status;
    }
    if (briefing) await recordBriefingRun(context, decision, briefing, briefingHashValue, decision.status);
    return { ok: true, decision };
}

async function handleSmartHomeAck(context, command) {
    if (!command?.decision_id || typeof context?.dbAll !== "function" || typeof context?.dbRun !== "function") {
        return { ok: false, code: "AGENT_DECISION_NOT_LINKED" };
    }
    const rows = await context.dbAll(
        "SELECT * FROM home_ai_agent_decisions WHERE decision_id=? LIMIT 1",
        [command.decision_id]
    );
    if (!rows[0]) {
        await savePendingAck(context, command);
        return { ok: true, pending: true, decision_id: command.decision_id };
    }
    const applied = await persistAckedDecision(context, rows[0], command);
    if (!applied) {
        await savePendingAck(context, command);
        return { ok: true, pending: true, decision_id: command.decision_id };
    }
    await context.dbRun("DELETE FROM home_ai_agent_acks WHERE command_id=?", [command.command_id]);
    return { ok: true, ...applied };
}

function mapDecisionRow(row) {
    const control = parseJson(row.plan_json, {});
    return {
        decision_id: row.decision_id,
        response_type: row.response_type,
        intent: parseJson(row.intent_json, {}),
        control,
        steps: Array.isArray(control.steps) ? control.steps : [],
        execution: parseJson(row.action_json, {}),
        speech: parseJson(row.speech_json, {}),
        speech_policy: parseJson(row.speech_json, {}),
        status: row.status,
        created_at_ms: Number(row.created_at_ms) || null,
        updated_at_ms: Number(row.updated_at_ms) || null
    };
}

async function listAgentDecisions(dbAll, options = {}) {
    if (typeof dbAll !== "function") return [];
    const limitValue = Number.parseInt(options.limit, 10);
    const limit = Number.isFinite(limitValue) && limitValue > 0 ? Math.min(limitValue, 100) : 30;
    const params = [];
    const where = [];
    const decisionId = text(options.decision_id, 160);
    const status = text(options.status, 32).toUpperCase();
    if (decisionId) {
        where.push("decision_id=?");
        params.push(decisionId);
    }
    if (status) {
        where.push("status=?");
        params.push(status);
    }
    params.push(limit);
    const rows = await dbAll(
        `SELECT * FROM home_ai_agent_decisions ${where.length ? `WHERE ${where.join(" AND ")}` : ""}
         ORDER BY updated_at_ms DESC LIMIT ?`,
        params
    );
    return rows.map(mapDecisionRow);
}

async function readAgentDecision(dbAll, decisionId) {
    const id = text(decisionId, 160);
    if (typeof dbAll !== "function" || !id) return null;
    const rows = await dbAll("SELECT * FROM home_ai_agent_decisions WHERE decision_id=? LIMIT 1", [id]);
    return rows[0] ? mapDecisionRow(rows[0]) : null;
}

function listPromptProfiles() {
    return listPromptProfileMetadata();
}

module.exports = {
    INTENT_TYPES,
    AGENT_MAX_EXECUTION_MS,
    MAX_ACTIONS_PER_STEP,
    MAX_PLAN_STEPS,
    PROMPT_PROFILES,
    buildPlan,
    briefingEligibility,
    listPromptProfiles,
    listAgentDecisions,
    readAgentDecision,
    handleSmartHomeAck,
    normalizeIntent,
    orchestrate,
    speechFor
};
