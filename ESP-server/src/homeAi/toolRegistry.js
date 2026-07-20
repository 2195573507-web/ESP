const crypto = require("crypto");
const {
    createSmartHomeCommand
} = require("../services/smartHomeService");
const {
    recordFeedback
} = require("../services/homeAiService");

const TOOL_MAX_CONCURRENCY = 3;
const WEATHER_MAX_AGE_MS = 30 * 60 * 1000;
const NEWS_MAX_AGE_MS = 6 * 60 * 60 * 1000;

class HomeAiToolError extends Error {
    constructor(code, message, status = 400) {
        super(message);
        this.code = code;
        this.status = status;
    }
}

function makeId(prefix) {
    const value = typeof crypto.randomUUID === "function"
        ? crypto.randomUUID()
        : crypto.randomBytes(16).toString("hex");
    return `${prefix}_${value}`;
}

function text(value, max = 256) {
    return typeof value === "string" ? value.trim().slice(0, max) : "";
}

function limit(value, fallback = 50, maximum = 200) {
    const parsed = Number.parseInt(value, 10);
    return Number.isFinite(parsed) && parsed > 0 ? Math.min(parsed, maximum) : fallback;
}

function json(value, fallback = {}) {
    if (!value) return fallback;
    try {
        return typeof value === "string" ? JSON.parse(value) : value;
    } catch (_) {
        return fallback;
    }
}

function jsonText(value) {
    try {
        return JSON.stringify(value === undefined ? {} : value);
    } catch (_) {
        return "{}";
    }
}

function timezoneIsValid(value) {
    const timezone = text(value, 64);
    if (!timezone) return false;
    try {
        new Intl.DateTimeFormat("en-US", { timeZone: timezone }).format(new Date(0));
        return true;
    } catch (_) {
        return false;
    }
}

function normalizeHomeLocation(value) {
    const city = text(value?.city, 80);
    const timezone = text(value?.timezone, 64);
    const latitude = Number(value?.latitude);
    const longitude = Number(value?.longitude);
    if (!city || !timezoneIsValid(timezone) || !Number.isFinite(latitude) || latitude < -90 || latitude > 90 ||
        !Number.isFinite(longitude) || longitude < -180 || longitude > 180) {
        return { ok: false, code: "HOME_LOCATION_INVALID", error: "city, valid coordinates, and an IANA timezone are required" };
    }
    return { ok: true, location: { city, latitude, longitude, timezone } };
}

function weatherIsDark(current) {
    const isDay = Number(current?.is_day);
    const weatherCode = Number(current?.weather_code);
    return isDay === 0 || (Number.isFinite(weatherCode) && weatherCode >= 45);
}

async function writeWeatherContext(context, value) {
    if (typeof context.dbRun !== "function") return;
    await writeSetting(context.dbRun, "weather_context", value);
}

async function markWeatherUnavailable(context, reason) {
    await writeWeatherContext(context, {
        available: false,
        dark: false,
        reason: text(reason, 64) || "WEATHER_UNAVAILABLE",
        observed_at_ms: null,
        expires_at_ms: Date.now()
    });
}

async function readSetting(dbAll, key, fallback = null) {
    const rows = await dbAll("SELECT value_json FROM home_ai_tool_settings WHERE setting_key=?", [key]);
    return rows[0] ? json(rows[0].value_json, fallback) : fallback;
}

async function writeSetting(dbRun, key, value) {
    const nowMs = Date.now();
    await dbRun(
        `INSERT INTO home_ai_tool_settings(setting_key,value_json,updated_at_ms)
         VALUES(?,?,?)
         ON CONFLICT(setting_key) DO UPDATE SET value_json=excluded.value_json,updated_at_ms=excluded.updated_at_ms`,
        [key, jsonText(value), nowMs]
    );
    return { key, value, updated_at_ms: nowMs };
}

function validateSchema(schema, input) {
    if (!schema || schema.type !== "object" || !input || typeof input !== "object" || Array.isArray(input)) {
        throw new HomeAiToolError("TOOL_ARGUMENTS_INVALID", "tool arguments must be an object");
    }
    for (const field of schema.required || []) {
        if (input[field] === undefined || input[field] === null || input[field] === "") {
            throw new HomeAiToolError("TOOL_ARGUMENT_REQUIRED", `${field} is required`);
        }
    }
    for (const [field, rule] of Object.entries(schema.properties || {})) {
        if (input[field] === undefined) continue;
        const actual = Array.isArray(input[field]) ? "array" : typeof input[field];
        if (rule.type && actual !== rule.type) {
            throw new HomeAiToolError("TOOL_ARGUMENT_TYPE_INVALID", `${field} must be ${rule.type}`);
        }
        if (rule.enum && !rule.enum.includes(input[field])) {
            throw new HomeAiToolError("TOOL_ARGUMENT_VALUE_INVALID", `${field} is unsupported`);
        }
    }
}

async function fetchJson(url, options, context, timeoutCode) {
    const fetchImpl = context.fetchImpl || globalThis.fetch;
    if (typeof fetchImpl !== "function") {
        throw new HomeAiToolError("TOOL_FETCH_UNAVAILABLE", "HTTP fetch is unavailable", 503);
    }
    let response;
    try {
        response = await fetchImpl(url, {
            ...options,
            cache: "no-store",
            signal: context.signal
        });
    } catch (error) {
        if (context.signal?.aborted) {
            throw new HomeAiToolError(timeoutCode, "tool request timed out", 504);
        }
        throw new HomeAiToolError("TOOL_UPSTREAM_UNAVAILABLE", "tool upstream request failed", 502);
    }
    if (!response?.ok) {
        throw new HomeAiToolError("TOOL_UPSTREAM_REJECTED", `tool upstream returned ${response?.status || 0}`, 502);
    }
    try {
        return await response.json();
    } catch (_) {
        throw new HomeAiToolError("TOOL_UPSTREAM_INVALID", "tool upstream returned invalid JSON", 502);
    }
}

const definitions = [
    {
        name: "get_environment",
        description: "Read the latest persisted room state and environment freshness.",
        input_schema: { type: "object", properties: { room_id: { type: "string" } }, required: ["room_id"] },
        timeout_ms: 3000,
        permission: "read_home",
        read_only: true,
        requires_confirmation: false,
        cache_policy: "no_cache",
        error_codes: ["ENVIRONMENT_NOT_FOUND", "TOOL_TIMEOUT"],
        async handler(args, context) {
            const rows = await context.dbAll(
                `SELECT * FROM home_ai_events
                 WHERE room_id=? AND event_type='room_state'
                 ORDER BY occurred_at_ms DESC LIMIT 1`,
                [text(args.room_id, 32)]
            );
            if (!rows[0]) throw new HomeAiToolError("ENVIRONMENT_NOT_FOUND", "room environment was not found", 404);
            return {
                room_id: rows[0].room_id,
                occurred_at_ms: Number(rows[0].occurred_at_ms),
                state: json(rows[0].payload_json, {})
            };
        }
    },
    {
        name: "get_device_state",
        description: "Read a virtual device state.",
        input_schema: { type: "object", properties: { device_id: { type: "string" } }, required: ["device_id"] },
        timeout_ms: 3000,
        permission: "read_home",
        read_only: true,
        requires_confirmation: false,
        cache_policy: "no_cache",
        error_codes: ["DEVICE_NOT_FOUND", "TOOL_TIMEOUT"],
        async handler(args, context) {
            const rows = await context.dbAll("SELECT * FROM home_ai_virtual_devices WHERE device_id=?", [text(args.device_id, 47)]);
            if (!rows[0]) throw new HomeAiToolError("DEVICE_NOT_FOUND", "virtual device was not found", 404);
            return {
                device_id: rows[0].device_id,
                room_id: rows[0].room_id,
                device_type: rows[0].device_type,
                state: json(rows[0].state_json, {}),
                updated_at_ms: Number(rows[0].updated_at_ms)
            };
        }
    },
    {
        name: "control_virtual_device",
        description: "Queue a virtual-device command for S3 and wait for its later ACK.",
        input_schema: {
            type: "object",
            properties: {
                device_id: { type: "string" },
                action: { type: "string", enum: ["turn_on", "turn_off"] },
                room_id: { type: "string" },
                gateway_id: { type: "string" }
            },
            required: ["device_id", "action"]
        },
        timeout_ms: 3000,
        permission: "control_home",
        read_only: false,
        requires_confirmation: false,
        cache_policy: "no_cache",
        error_codes: ["SMART_HOME_COMMAND_WRITE_FAILED", "TOOL_TIMEOUT"],
        async handler(args, context) {
            const result = await createSmartHomeCommand(context.dbRun, {
                target_id: text(args.device_id, 47),
                action: args.action,
                room_id: text(args.room_id, 32),
                gateway_id: text(args.gateway_id, 128),
                source: "home_ai_agent",
                requested_by: text(context.requestedBy, 128) || "home_ai_agent",
                decision_id: text(context.decisionId, 160)
            });
            if (!result.ok) throw new HomeAiToolError(result.code, result.error);
            return {
                command_id: result.command.command_id,
                status: result.command.status,
                verified: false,
                message: "queued; actual success requires S3 ACK"
            };
        }
    },
    {
        name: "query_history",
        description: "Read bounded Home AI event history.",
        input_schema: { type: "object", properties: { room_id: { type: "string" }, event_type: { type: "string" }, limit: { type: "number" } } },
        timeout_ms: 3000,
        permission: "read_home",
        read_only: true,
        requires_confirmation: false,
        cache_policy: "no_cache",
        error_codes: ["TOOL_TIMEOUT"],
        async handler(args, context) {
            const params = [];
            const where = [];
            if (text(args.room_id, 32)) { where.push("room_id=?"); params.push(text(args.room_id, 32)); }
            if (text(args.event_type, 48)) { where.push("event_type=?"); params.push(text(args.event_type, 48)); }
            params.push(limit(args.limit, 20, 100));
            const rows = await context.dbAll(
                `SELECT * FROM home_ai_events ${where.length ? `WHERE ${where.join(" AND ")}` : ""}
                 ORDER BY occurred_at_ms DESC LIMIT ?`,
                params
            );
            return rows.map(row => ({
                event_id: row.event_id,
                room_id: row.room_id || "",
                event_type: row.event_type,
                occurred_at_ms: Number(row.occurred_at_ms),
                payload: json(row.payload_json, {})
            }));
        }
    },
    {
        name: "query_memory",
        description: "Read confirmed memories and visible candidates.",
        input_schema: { type: "object", properties: { room_id: { type: "string" }, limit: { type: "number" } } },
        timeout_ms: 3000,
        permission: "read_memory",
        read_only: true,
        requires_confirmation: false,
        cache_policy: "no_cache",
        error_codes: ["TOOL_TIMEOUT"],
        async handler(args, context) {
            const roomId = text(args.room_id, 32);
            const rowLimit = limit(args.limit, 20, 100);
            const params = roomId ? [roomId, rowLimit] : [rowLimit];
            const rows = await context.dbAll(
                `SELECT memory_id,room_id,category,content,automation_allowed,updated_at_ms
                 FROM home_ai_confirmed_memories ${roomId ? "WHERE room_id=?" : ""}
                 ORDER BY updated_at_ms DESC LIMIT ?`,
                params
            );
            return rows.map(row => ({ ...row, automation_allowed: Number(row.automation_allowed) !== 0 }));
        }
    },
    {
        name: "get_weather",
        description: "Fetch current weather for the fixed home location.",
        input_schema: { type: "object", properties: {} },
        timeout_ms: 10000,
        timeout_error_code: "WEATHER_TIMEOUT",
        permission: "internet_read",
        read_only: true,
        requires_confirmation: false,
        cache_policy: "fresh_only_30m",
        error_codes: ["WEATHER_LOCATION_NOT_CONFIGURED", "WEATHER_TIMEOUT", "WEATHER_STALE", "TOOL_UPSTREAM_UNAVAILABLE"],
        async handler(args, context) {
            void args;
            const normalizedLocation = normalizeHomeLocation(await readSetting(context.dbAll, "home_location", null));
            if (!normalizedLocation.ok) {
                await markWeatherUnavailable(context, "WEATHER_LOCATION_NOT_CONFIGURED");
                throw new HomeAiToolError("WEATHER_LOCATION_NOT_CONFIGURED", "home location is not configured", 409);
            }
            const location = normalizedLocation.location;
            const url = new URL("https://api.open-meteo.com/v1/forecast");
            url.searchParams.set("latitude", String(location.latitude));
            url.searchParams.set("longitude", String(location.longitude));
            url.searchParams.set("current", "temperature_2m,relative_humidity_2m,weather_code,is_day");
            url.searchParams.set("timezone", "UTC");
            url.searchParams.set("timeformat", "unixtime");
            let payload;
            try {
                payload = await fetchJson(url, {}, context, "WEATHER_TIMEOUT");
            } catch (error) {
                await markWeatherUnavailable(context, error?.code || "WEATHER_UPSTREAM_UNAVAILABLE");
                throw error;
            }
            const rawObservedAt = Number(payload?.current?.time);
            const observedAtMs = Number.isFinite(rawObservedAt)
                ? rawObservedAt * 1000
                : Date.parse(payload?.current?.time || "");
            const ageMs = Date.now() - observedAtMs;
            if (!Number.isFinite(observedAtMs) || ageMs < 0 || ageMs > WEATHER_MAX_AGE_MS) {
                await markWeatherUnavailable(context, "WEATHER_STALE");
                throw new HomeAiToolError("WEATHER_STALE", "weather data is missing or stale", 502);
            }
            const result = {
                location,
                observed_at_ms: observedAtMs,
                expires_at_ms: observedAtMs + WEATHER_MAX_AGE_MS,
                current: payload.current,
                dark: weatherIsDark(payload.current),
                fresh: true
            };
            await writeWeatherContext(context, {
                available: true,
                dark: result.dark,
                reason: "fresh_weather_tool_result",
                observed_at_ms: result.observed_at_ms,
                expires_at_ms: result.expires_at_ms
            });
            return result;
        }
    },
    {
        name: "get_news",
        description: "Fetch fresh news for user requests or scene briefings only.",
        input_schema: { type: "object", properties: { query: { type: "string" }, limit: { type: "number" } } },
        timeout_ms: 12000,
        timeout_error_code: "NEWS_TIMEOUT",
        permission: "internet_read",
        read_only: true,
        requires_confirmation: false,
        cache_policy: "fresh_only_6h",
        error_codes: ["NEWS_NOT_CONFIGURED", "NEWS_TIMEOUT", "NEWS_STALE", "TOOL_UPSTREAM_UNAVAILABLE"],
        async handler(args, context) {
            const config = await readSetting(context.dbAll, "news_provider", null);
            const endpoint = text(config?.endpoint || process.env.HOME_AI_NEWS_API_URL, 500);
            if (!endpoint) throw new HomeAiToolError("NEWS_NOT_CONFIGURED", "news provider is not configured", 409);
            let url;
            try {
                url = new URL(endpoint);
            } catch (_) {
                throw new HomeAiToolError("NEWS_NOT_CONFIGURED", "news provider URL is invalid", 409);
            }
            if (url.protocol !== "https:") throw new HomeAiToolError("NEWS_NOT_CONFIGURED", "news provider must use HTTPS", 409);
            if (text(args.query, 120)) url.searchParams.set("q", text(args.query, 120));
            url.searchParams.set("pageSize", String(limit(args.limit, 5, 20)));
            const headers = {};
            const apiKey = text(config?.api_key || process.env.HOME_AI_NEWS_API_KEY, 300);
            if (apiKey) headers.Authorization = `Bearer ${apiKey}`;
            const payload = await fetchJson(url, { headers }, context, "NEWS_TIMEOUT");
            const articles = Array.isArray(payload?.articles) ? payload.articles.slice(0, limit(args.limit, 5, 20)) : [];
            const fresh = articles.filter(article => {
                const publishedAtMs = Date.parse(article?.publishedAt || article?.published_at || "");
                const ageMs = Date.now() - publishedAtMs;
                return Number.isFinite(publishedAtMs) && ageMs >= 0 && ageMs <= NEWS_MAX_AGE_MS;
            });
            if (fresh.length === 0) throw new HomeAiToolError("NEWS_STALE", "news data is missing or stale", 502);
            return { articles: fresh, fresh: true, fetched_at_ms: Date.now() };
        }
    },
    {
        name: "save_feedback",
        description: "Persist explicit user feedback; silence is never recorded as acceptance.",
        input_schema: { type: "object", properties: { feedback_type: { type: "string", enum: ["accepted", "rejected", "modified", "cancelled", "reverted", "manual_override"] }, rule_id: { type: "string" }, room_id: { type: "string" } }, required: ["feedback_type"] },
        timeout_ms: 3000,
        permission: "write_feedback",
        read_only: false,
        requires_confirmation: false,
        cache_policy: "no_cache",
        error_codes: ["HOME_AI_FEEDBACK_INVALID", "TOOL_TIMEOUT"],
        async handler(args, context) {
            const result = await recordFeedback(context.dbRun, args, context.dbAll);
            if (!result.ok) throw new HomeAiToolError(result.code, result.error);
            return result.feedback;
        }
    }
];

const registry = new Map(definitions.map(definition => [definition.name, Object.freeze(definition)]));
let activeTools = 0;

function listTools() {
    return definitions.map(({ handler, ...definition }) => ({ ...definition }));
}

async function audit(context, record) {
    if (typeof context.dbRun !== "function") return;
    try {
        await context.dbRun(
            `INSERT INTO home_ai_tool_audit
             (audit_id,tool_name,status,request_json,result_json,error_code,elapsed_ms,created_at_ms)
             VALUES(?,?,?,?,?,?,?,?)`,
            [makeId("tool_audit"), record.tool_name, record.status, jsonText({
                args: record.request,
                decision_id: text(context.decisionId, 160),
                requested_by: text(context.requestedBy, 128),
                trace_id: text(context.traceId, 160)
            }),
                jsonText(record.result), record.error_code || "", record.elapsed_ms, Date.now()]
        );
    } catch (_) {
        // Tool outcome is authoritative; audit persistence is best-effort.
    }
}

async function executeTool(name, args = {}, context = {}) {
    const definition = registry.get(name);
    if (!definition) throw new HomeAiToolError("TOOL_NOT_FOUND", "tool is not registered", 404);
    validateSchema(definition.input_schema, args);
    if (activeTools >= TOOL_MAX_CONCURRENCY) {
        throw new HomeAiToolError("TOOL_CONCURRENCY_LIMIT", "tool concurrency limit reached", 429);
    }
    activeTools += 1;
    const startedAt = Date.now();
    const remainingMs = Number.isFinite(Number(context.deadline_ms))
        ? Math.max(0, Number(context.deadline_ms) - startedAt)
        : definition.timeout_ms;
    if (remainingMs === 0) {
        activeTools = Math.max(0, activeTools - 1);
        throw new HomeAiToolError("TOOL_TIMEOUT", `${name} exceeded the agent deadline`, 504);
    }
    const timeoutMs = Math.min(definition.timeout_ms, remainingMs);
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), timeoutMs);
    try {
        const result = await Promise.race([
            definition.handler(args, { ...context, signal: controller.signal }),
            new Promise((_, reject) => {
                controller.signal.addEventListener("abort", () => reject(
                    new HomeAiToolError(definition.timeout_error_code || "TOOL_TIMEOUT", `${name} timed out`, 504)
                ), { once: true });
            })
        ]);
        await audit(context, {
            tool_name: name,
            status: "succeeded",
            request: args,
            result,
            elapsed_ms: Date.now() - startedAt
        });
        return { ok: true, tool: name, result, elapsed_ms: Date.now() - startedAt };
    } catch (error) {
        const toolError = error instanceof HomeAiToolError
            ? error
            : new HomeAiToolError("TOOL_EXECUTION_FAILED", "tool execution failed", 500);
        await audit(context, {
            tool_name: name,
            status: "failed",
            request: args,
            result: {},
            error_code: toolError.code,
            elapsed_ms: Date.now() - startedAt
        });
        throw toolError;
    } finally {
        clearTimeout(timeout);
        activeTools = Math.max(0, activeTools - 1);
    }
}

module.exports = {
    HomeAiToolError,
    NEWS_MAX_AGE_MS,
    TOOL_MAX_CONCURRENCY,
    WEATHER_MAX_AGE_MS,
    executeTool,
    listTools,
    normalizeHomeLocation,
    readSetting,
    timezoneIsValid,
    writeSetting
};
