const crypto = require("crypto");

const HOME_AI_SCHEMA_VERSION = 1;
const HOME_AI_MAX_ROOMS = 3;
const HOME_AI_MAX_RULES = 16;
const HOME_AI_MAX_CONDITIONS_PER_RULE = 8;
const HOME_AI_MAX_ACTIONS_PER_RULE = 4;
const HOME_AI_MAX_RULE_STEPS = 4;
const HOME_AI_MAX_RULE_PACKAGE_BYTES = 12288;
const HOME_AI_MAX_OVERRIDES = 12;
const HOME_AI_MAX_OVERRIDE_ID_LENGTH = 63;
const HOME_AI_MAX_OVERRIDE_CONDITION_LENGTH = 63;
const HOME_AI_MAX_HISTORY_EVENT_BYTES = 512;
const HOME_AI_MAX_DEVICE_ID_LENGTH = 47;
const HOME_AI_MAX_PROMPT_LENGTH = 95;
const HOME_AI_MAX_CONDITION_TEXT_LENGTH = 47;

const ROOM_ID_PATTERN = /^[a-z0-9][a-z0-9_-]{0,31}$/;
const RULE_ID_PATTERN = /^[a-z0-9][a-z0-9_.-]{0,63}$/;
const DEVICE_ID_PATTERN = new RegExp(`^[a-z0-9][a-z0-9_.-]{0,${HOME_AI_MAX_DEVICE_ID_LENGTH - 1}}$`);
const DEVICE_TYPES = new Set(["light", "air_conditioner", "fan"]);
const RULE_TYPES = new Set(["basic_automation", "habit_learning", "safety", "manual"]);
const RULE_SOURCES = new Set(["manual", "habit_learning", "system"]);
const CONDITION_FIELDS = new Set([
    "presence_state",
    "stable_target_count",
    "occupancy_mode",
    "environment_fresh",
    "radar_fresh",
    "quiet_state",
    "time_window",
    "temperature_c",
    "humidity_percent",
    "air_quality_score",
    "weather_dark"
]);
const CONDITION_OPERATORS = new Set(["eq", "neq", "gt", "gte", "lt", "lte", "in", "range"]);
const ACTIONS = new Set(["turn_on", "turn_off", "pause_automation", "resume_automation", "play_prompt"]);
const OFFLINE_POLICIES = new Set(["continue", "pause", "require_server"]);
const HOME_AI_EVENT_TYPES = new Set([
    "room_state",
    "decision",
    "suppressed_action",
    "virtual_device_state",
    "voice_session",
    "emergency",
    "rule_sync",
    "offline_buffer",
    "playback_ack",
    "feedback"
]);
const OVERRIDE_ACTIONS = new Set(["keep_on", "keep_off", "pause_automation", "mute"]);
const OVERRIDE_SOURCES = new Set(["explicit_user_command", "web_user_command"]);
const REQUIRED_SENSING_SOURCES = new Set(["s3_local", "sensair_shuttle_01", "sensair_shuttle_02"]);
const KNOWN_VOICE_TERMINALS = new Set(["sensair_shuttle_01", "sensair_shuttle_02"]);

function isPlainObject(value) {
    return value && typeof value === "object" && !Array.isArray(value);
}

function trimText(value, maxLength = 256) {
    return typeof value === "string" ? value.trim().slice(0, maxLength) : "";
}

function finiteInteger(value, fallback, min, max) {
    const numeric = Number(value);
    if (!Number.isFinite(numeric)) {
        return fallback;
    }
    return Math.min(max, Math.max(min, Math.trunc(numeric)));
}

function finiteNumber(value, fallback, min, max) {
    const numeric = Number(value);
    if (!Number.isFinite(numeric)) {
        return fallback;
    }
    return Math.min(max, Math.max(min, numeric));
}

function stableJson(value) {
    if (Array.isArray(value)) {
        return `[${value.map(stableJson).join(",")}]`;
    }
    if (isPlainObject(value)) {
        return `{${Object.keys(value).sort().map(key => `${JSON.stringify(key)}:${stableJson(value[key])}`).join(",")}}`;
    }
    return JSON.stringify(value);
}

function checksumFor(value) {
    return crypto.createHash("sha256").update(stableJson(value)).digest("hex");
}

function clone(value) {
    return JSON.parse(JSON.stringify(value));
}

function defaultRooms() {
    return [
        {
            room_id: "living_room",
            room_name: "客厅",
            sensing_sources: ["s3_local"],
            voice_terminal_device_id: "",
            presence_confirm_ms: 1500,
            vacant_confirm_ms: 120000,
            multiple_confirm_ms: 3000,
            single_confirm_ms: 10000,
            quiet_start: "23:00",
            quiet_end: "07:00"
        },
        {
            room_id: "bedroom_01",
            room_name: "卧室一",
            sensing_sources: ["sensair_shuttle_01"],
            voice_terminal_device_id: "sensair_shuttle_01",
            presence_confirm_ms: 1500,
            vacant_confirm_ms: 300000,
            multiple_confirm_ms: 3000,
            single_confirm_ms: 10000,
            quiet_start: "23:00",
            quiet_end: "07:00"
        },
        {
            room_id: "bedroom_02",
            room_name: "卧室二",
            sensing_sources: ["sensair_shuttle_02"],
            voice_terminal_device_id: "sensair_shuttle_02",
            presence_confirm_ms: 1500,
            vacant_confirm_ms: 300000,
            multiple_confirm_ms: 3000,
            single_confirm_ms: 10000,
            quiet_start: "23:00",
            quiet_end: "07:00"
        }
    ];
}

function normalizeTime(value, fallback) {
    const text = trimText(value, 5);
    return /^([01]\d|2[0-3]):[0-5]\d$/.test(text) ? text : fallback;
}

function normalizeVoiceTerminalDeviceId(value) {
    if (value === undefined || value === null || value === "") {
        return { ok: true, value: "" };
    }
    if (typeof value !== "string") {
        return { ok: false, code: "ROOM_VOICE_TERMINAL_INVALID", error: "voice_terminal_device_id must be a string" };
    }
    const terminalId = value.trim();
    if (terminalId.length === 0) {
        return { ok: true, value: "" };
    }
    if (!DEVICE_ID_PATTERN.test(terminalId)) {
        return { ok: false, code: "ROOM_VOICE_TERMINAL_INVALID", error: "voice_terminal_device_id has an invalid format or length" };
    }
    if (!KNOWN_VOICE_TERMINALS.has(terminalId)) {
        return { ok: false, code: "ROOM_VOICE_TERMINAL_UNSUPPORTED", error: "voice terminal is not registered on this gateway" };
    }
    return { ok: true, value: terminalId };
}

function normalizeRoom(input, fallback = {}) {
    const roomId = trimText(input?.room_id, 32);
    if (!ROOM_ID_PATTERN.test(roomId)) {
        return { ok: false, code: "ROOM_ID_INVALID", error: "room_id must be a lowercase stable identifier" };
    }
    const sources = Array.isArray(input?.sensing_sources)
        ? input.sensing_sources.map(value => trimText(value, 96)).filter(Boolean).slice(0, 3)
        : [];
    if (sources.length === 0) {
        return { ok: false, code: "ROOM_SOURCES_REQUIRED", error: "at least one sensing source is required" };
    }
    const terminal = normalizeVoiceTerminalDeviceId(input?.voice_terminal_device_id);
    if (!terminal.ok) {
        return terminal;
    }

    return {
        ok: true,
        room: {
            room_id: roomId,
            room_name: trimText(input?.room_name, 64) || roomId,
            sensing_sources: sources,
            voice_terminal_device_id: terminal.value,
            presence_confirm_ms: finiteInteger(input?.presence_confirm_ms, fallback.presence_confirm_ms || 1500, 500, 30000),
            vacant_confirm_ms: finiteInteger(input?.vacant_confirm_ms, fallback.vacant_confirm_ms || 60000, 10000, 900000),
            multiple_confirm_ms: finiteInteger(input?.multiple_confirm_ms, fallback.multiple_confirm_ms || 3000, 500, 60000),
            single_confirm_ms: finiteInteger(input?.single_confirm_ms, fallback.single_confirm_ms || 10000, 500, 120000),
            quiet_start: normalizeTime(input?.quiet_start, fallback.quiet_start || "23:00"),
            quiet_end: normalizeTime(input?.quiet_end, fallback.quiet_end || "07:00")
        }
    };
}

function normalizeRoomConfig(input) {
    const sourceRooms = Array.isArray(input?.rooms) ? input.rooms : defaultRooms();
    if (sourceRooms.length === 0 || sourceRooms.length > HOME_AI_MAX_ROOMS) {
        return { ok: false, code: "ROOM_COUNT_INVALID", error: `rooms must contain 1-${HOME_AI_MAX_ROOMS} entries` };
    }
    const roomIds = new Set();
    const sourceIds = new Set();
    const terminalIds = new Set();
    const rooms = [];
    for (const entry of sourceRooms) {
        const normalized = normalizeRoom(entry);
        if (!normalized.ok) {
            return normalized;
        }
        if (roomIds.has(normalized.room.room_id)) {
            return { ok: false, code: "ROOM_ID_DUPLICATE", error: "room_id values must be unique" };
        }
        for (const sourceId of normalized.room.sensing_sources) {
            if (!REQUIRED_SENSING_SOURCES.has(sourceId)) {
                return { ok: false, code: "ROOM_SOURCE_UNSUPPORTED", error: "sensing source is not part of this gateway" };
            }
            if (sourceIds.has(sourceId)) {
                return { ok: false, code: "ROOM_SOURCE_DUPLICATE", error: "a sensing source may only belong to one room" };
            }
            sourceIds.add(sourceId);
        }
        const terminalId = normalized.room.voice_terminal_device_id;
        if (terminalId && terminalIds.has(terminalId)) {
            return { ok: false, code: "ROOM_VOICE_TERMINAL_DUPLICATE", error: "a voice terminal may only belong to one room" };
        }
        if (terminalId) {
            terminalIds.add(terminalId);
        }
        roomIds.add(normalized.room.room_id);
        rooms.push(normalized.room);
    }
    if (sourceIds.size !== REQUIRED_SENSING_SOURCES.size ||
        [...REQUIRED_SENSING_SOURCES].some(sourceId => !sourceIds.has(sourceId))) {
        return { ok: false, code: "ROOM_SOURCE_SET_INCOMPLETE", error: "all three gateway sensing sources must be assigned" };
    }
    return { ok: true, rooms };
}

function normalizeCondition(input) {
    const field = trimText(input?.field, 48);
    const operator = trimText(input?.operator, 16);
    if (!CONDITION_FIELDS.has(field) || !CONDITION_OPERATORS.has(operator)) {
        return { ok: false, code: "RULE_CONDITION_UNSUPPORTED", error: "condition field or operator is unsupported" };
    }
    if (input?.value === undefined) {
        return { ok: false, code: "RULE_CONDITION_VALUE_REQUIRED", error: "condition value is required" };
    }
    const textFields = new Set(["presence_state", "occupancy_mode", "quiet_state", "time_window"]);
    const numberFields = new Set(["stable_target_count", "temperature_c", "humidity_percent", "air_quality_score"]);
    const booleanFields = new Set(["environment_fresh", "radar_fresh", "weather_dark"]);
    const comparableOperators = new Set(["eq", "neq", "in"]);
    if ((textFields.has(field) || booleanFields.has(field)) && !comparableOperators.has(operator)) {
        return { ok: false, code: "RULE_CONDITION_OPERATOR_INVALID", error: "operator is invalid for condition value type" };
    }

    const rawValues = operator === "in" || operator === "range" ? input.value : [input.value];
    if (!Array.isArray(rawValues) || rawValues.length === 0 || rawValues.length > 4 ||
        (operator === "range" && rawValues.length !== 2)) {
        return { ok: false, code: "RULE_CONDITION_VALUE_INVALID", error: "condition value count is invalid" };
    }
    let value;
    if (textFields.has(field)) {
        if (operator === "range" || rawValues.some(item => typeof item !== "string" ||
            !item.trim() || item.trim().length > HOME_AI_MAX_CONDITION_TEXT_LENGTH)) {
            return { ok: false, code: "RULE_CONDITION_VALUE_INVALID", error: "text condition value is invalid" };
        }
        const normalizedValues = rawValues.map(item => item.trim());
        value = operator === "in" ? normalizedValues : normalizedValues[0];
    } else if (numberFields.has(field)) {
        if (rawValues.some(item => typeof item !== "number" || !Number.isFinite(item) ||
            item < -1000000 || item > 1000000) ||
            (operator === "range" && rawValues[0] > rawValues[1])) {
            return { ok: false, code: "RULE_CONDITION_VALUE_INVALID", error: "numeric condition value is invalid" };
        }
        value = operator === "in" || operator === "range" ? [...rawValues] : rawValues[0];
    } else if (booleanFields.has(field)) {
        if (operator === "range" || rawValues.some(item => typeof item !== "boolean")) {
            return { ok: false, code: "RULE_CONDITION_VALUE_INVALID", error: "boolean condition value is invalid" };
        }
        value = operator === "in" ? [...rawValues] : rawValues[0];
    } else {
        return { ok: false, code: "RULE_CONDITION_UNSUPPORTED", error: "condition field is unsupported" };
    }
    return {
        ok: true,
        condition: {
            field,
            operator,
            value,
            duration_ms: finiteInteger(input?.duration_ms, 0, 0, 3600000)
        }
    };
}

function normalizeAction(input) {
    const deviceId = trimText(input?.device_id, 96);
    const action = trimText(input?.action, 32);
    if (!ACTIONS.has(action)) {
        return { ok: false, code: "RULE_ACTION_UNSUPPORTED", error: "action is unsupported" };
    }
    const deviceType = trimText(input?.device_type, 32);
    const prompt = trimText(input?.prompt, HOME_AI_MAX_PROMPT_LENGTH);
    if (["turn_on", "turn_off"].includes(action) &&
        (!DEVICE_ID_PATTERN.test(deviceId) || !DEVICE_TYPES.has(deviceType))) {
        return { ok: false, code: "RULE_ACTION_DEVICE_INVALID", error: "device_id is required for virtual device actions" };
    }
    if (action === "play_prompt" && !prompt) {
        return { ok: false, code: "RULE_ACTION_PROMPT_REQUIRED", error: "play_prompt requires a prompt" };
    }
    return {
        ok: true,
        action: {
            device_id: deviceId,
            device_type: DEVICE_TYPES.has(deviceType) ? deviceType : "",
            action,
            prompt,
            minimum_active_seconds: finiteInteger(input?.minimum_active_seconds, 0, 0, 86400)
        }
    };
}

function normalizeRuleBinding(input, roomId, roomIds, enabled) {
    if (input === undefined || input === null) {
        return roomIds.has(roomId)
            ? { ok: true, binding: null }
            : { ok: false, code: "RULE_ID_OR_ROOM_INVALID", error: "rule_id or room_id is invalid" };
    }
    if (!isPlainObject(input) || !["MIGRATED", "PENDING_REBIND"].includes(input.state) ||
        input.reason !== "room_config_migration") {
        return { ok: false, code: "RULE_BINDING_INVALID", error: "rule binding state is invalid" };
    }
    const sourceIds = Array.isArray(input.source_ids)
        ? input.source_ids.map(value => trimText(value, 32)).filter(Boolean)
        : [];
    if (sourceIds.length === 0 || sourceIds.length > REQUIRED_SENSING_SOURCES.size ||
        new Set(sourceIds).size !== sourceIds.length ||
        sourceIds.some(sourceId => !REQUIRED_SENSING_SOURCES.has(sourceId))) {
        return { ok: false, code: "RULE_BINDING_SOURCES_INVALID", error: "rule binding sources are invalid" };
    }
    const fromRoomId = trimText(input.from_room_id, 32);
    const toRoomId = trimText(input.to_room_id, 32);
    if (!ROOM_ID_PATTERN.test(fromRoomId) ||
        (input.state === "PENDING_REBIND" && (enabled || toRoomId)) ||
        (input.state === "MIGRATED" && (!roomIds.has(roomId) || toRoomId !== roomId))) {
        return { ok: false, code: "RULE_BINDING_INVALID", error: "rule binding does not match the rule state" };
    }
    return {
        ok: true,
        binding: {
            state: input.state,
            reason: "room_config_migration",
            from_room_id: fromRoomId,
            to_room_id: toRoomId,
            source_ids: sourceIds,
            config_version: finiteInteger(input.config_version, 1, 1, 2147483647),
            was_enabled: input.was_enabled !== false
        }
    };
}

function normalizeRule(input, roomIds) {
    const ruleId = trimText(input?.rule_id, 64);
    const roomId = trimText(input?.room_id, 32);
    const enabled = input?.enabled !== false;
    if (!RULE_ID_PATTERN.test(ruleId) || !ROOM_ID_PATTERN.test(roomId)) {
        return { ok: false, code: "RULE_ID_OR_ROOM_INVALID", error: "rule_id or room_id is invalid" };
    }
    const binding = normalizeRuleBinding(input?.binding, roomId, roomIds, enabled);
    if (!binding.ok) {
        return binding;
    }
    const type = trimText(input?.rule_type, 32) || "basic_automation";
    const source = trimText(input?.source, 32) || "manual";
    if (!RULE_TYPES.has(type) || !RULE_SOURCES.has(source)) {
        return { ok: false, code: "RULE_TYPE_OR_SOURCE_INVALID", error: "rule_type or source is invalid" };
    }
    const rawConditions = Array.isArray(input?.conditions) ? input.conditions : [];
    const rawActions = Array.isArray(input?.actions) ? input.actions : [];
    if (rawConditions.length === 0 || rawConditions.length > HOME_AI_MAX_CONDITIONS_PER_RULE ||
        rawActions.length === 0 || rawActions.length > HOME_AI_MAX_ACTIONS_PER_RULE) {
        return { ok: false, code: "RULE_RESOURCE_LIMIT", error: "conditions or actions exceed fixed limits" };
    }
    const conditions = [];
    for (const condition of rawConditions) {
        const normalized = normalizeCondition(condition);
        if (!normalized.ok) {
            return normalized;
        }
        conditions.push(normalized.condition);
    }
    const actions = [];
    for (const action of rawActions) {
        const normalized = normalizeAction(action);
        if (!normalized.ok) {
            return normalized;
        }
        actions.push(normalized.action);
    }
    const rawSteps = Array.isArray(input?.steps) ? input.steps : [];
    if (rawSteps.length > HOME_AI_MAX_RULE_STEPS) {
        return { ok: false, code: "RULE_STEP_LIMIT", error: "steps exceed fixed limit" };
    }
    return {
        ok: true,
        rule: {
            rule_id: ruleId,
            version: finiteInteger(input?.version, 1, 1, 2147483647),
            rule_type: type,
            source,
            room_id: roomId,
            enabled,
            priority: finiteInteger(input?.priority, 500, 0, 1000),
            conditions,
            actions,
            cooldown_seconds: finiteInteger(input?.cooldown_seconds, 120, 0, 86400),
            minimum_active_seconds: finiteInteger(input?.minimum_active_seconds, 0, 0, 86400),
            offline_policy: OFFLINE_POLICIES.has(trimText(input?.offline_policy, 32)) ? trimText(input?.offline_policy, 32) : "continue",
            expires_at_ms: input?.expires_at_ms === null || input?.expires_at_ms === undefined
                ? null
                : finiteInteger(input?.expires_at_ms, 0, 1, Number.MAX_SAFE_INTEGER),
            probation: {
                enabled: Boolean(input?.probation?.enabled),
                until_ms: input?.probation?.until_ms ? finiteInteger(input.probation.until_ms, 0, 1, Number.MAX_SAFE_INTEGER) : null
            },
            ...(binding.binding ? { binding: binding.binding } : {})
        }
    };
}

function normalizeVirtualDeviceState(input) {
    const source = isPlainObject(input?.state) ? { ...input, ...input.state } : input;
    const deviceId = trimText(source?.device_id || source?.id, 96);
    const roomId = trimText(source?.room_id, 32);
    const deviceType = trimText(source?.device_type || source?.type, 32);
    const power = trimText(source?.power, 8).toLowerCase();
    const action = trimText(source?.last_action, 32);
    if (!DEVICE_ID_PATTERN.test(deviceId) || !ROOM_ID_PATTERN.test(roomId) || !DEVICE_TYPES.has(deviceType)) {
        return { ok: false, code: "VIRTUAL_DEVICE_STATE_INVALID", error: "device_id, room_id, and device_type are invalid" };
    }
    if (!["on", "off"].includes(power)) {
        return { ok: false, code: "VIRTUAL_DEVICE_POWER_INVALID", error: "virtual device power must be on or off" };
    }
    if (!["turn_on", "turn_off"].includes(action)) {
        return { ok: false, code: "VIRTUAL_DEVICE_ACTION_INVALID", error: "last_action must be turn_on or turn_off" };
    }
    if (source?.execution_mode !== undefined && source.execution_mode !== "virtual") {
        return { ok: false, code: "VIRTUAL_DEVICE_MODE_INVALID", error: "only virtual execution is supported" };
    }
    return {
        ok: true,
        device: {
            device_id: deviceId,
            room_id: roomId,
            device_type: deviceType,
            power,
            execution_mode: "virtual",
            last_action: action,
            action_source: trimText(source?.action_source, 64),
            decision_id: trimText(source?.decision_id, 160),
            decision_reason: trimText(source?.decision_reason, 160),
            verified: source?.verified === true,
            updated_at_ms: finiteInteger(source?.updated_at_ms, Date.now(), 1, Number.MAX_SAFE_INTEGER)
        }
    };
}

function normalizeUserOverride(input) {
    const scope = isPlainObject(input?.scope) ? input.scope : {};
    const roomId = trimText(scope.room_id || input?.room_id, 32);
    const deviceId = trimText(scope.device_id || input?.device_id, 96);
    const action = trimText(input?.action, 32);
    const source = trimText(input?.source, 48) || "explicit_user_command";
    const createdAtMs = finiteInteger(input?.created_at_ms, Date.now(), 1, Number.MAX_SAFE_INTEGER);
    const expiresAtMs = input?.expires_at_ms === null || input?.expires_at_ms === undefined
        ? null
        : finiteInteger(input.expires_at_ms, 0, createdAtMs + 1, Number.MAX_SAFE_INTEGER);
    if ((!ROOM_ID_PATTERN.test(roomId) && !DEVICE_ID_PATTERN.test(deviceId)) ||
        !OVERRIDE_ACTIONS.has(action) || !OVERRIDE_SOURCES.has(source)) {
        return { ok: false, code: "USER_OVERRIDE_INVALID", error: "scope, action, or source is invalid" };
    }
    if (["keep_on", "keep_off"].includes(action) && !DEVICE_ID_PATTERN.test(deviceId)) {
        return { ok: false, code: "USER_OVERRIDE_DEVICE_REQUIRED", error: "device override requires device_id" };
    }
    return {
        ok: true,
        override: {
            override_id: trimText(input?.override_id, HOME_AI_MAX_OVERRIDE_ID_LENGTH),
            scope: {
                room_id: ROOM_ID_PATTERN.test(roomId) ? roomId : "",
                device_id: DEVICE_ID_PATTERN.test(deviceId) ? deviceId : null
            },
            action,
            source,
            created_at_ms: createdAtMs,
            expires_at_ms: expiresAtMs,
            until_condition: trimText(input?.until_condition, HOME_AI_MAX_OVERRIDE_CONDITION_LENGTH),
            priority: finiteInteger(input?.priority, 900, 800, 999),
            allow_safety_override: input?.allow_safety_override !== false
        }
    };
}

function normalizeHomeAiEvent(input) {
    const eventId = trimText(input?.event_id, 160);
    const eventType = trimText(input?.event_type, 48);
    const roomId = trimText(input?.room_id, 32);
    const payload = isPlainObject(input?.payload) ? input.payload : null;
    if (!eventId || !HOME_AI_EVENT_TYPES.has(eventType) || payload === null ||
        (roomId && !ROOM_ID_PATTERN.test(roomId))) {
        return { ok: false, code: "HOME_AI_EVENT_INVALID", error: "event id, type, room, or payload is invalid" };
    }
    if (Buffer.byteLength(JSON.stringify(payload), "utf8") > HOME_AI_MAX_HISTORY_EVENT_BYTES) {
        return { ok: false, code: "HOME_AI_EVENT_TOO_LARGE", error: "event payload exceeds fixed S3 history budget" };
    }
    const schemaVersion = input?.schema_version === undefined
        ? HOME_AI_SCHEMA_VERSION
        : Number(input.schema_version);
    if (schemaVersion !== HOME_AI_SCHEMA_VERSION) {
        return { ok: false, code: "HOME_AI_EVENT_SCHEMA_INVALID", error: "event schema_version is unsupported" };
    }
    return {
        ok: true,
        event: {
            event_id: eventId,
            event_type: eventType,
            room_id: roomId,
            priority: finiteInteger(input?.priority, 0, 0, 1000),
            occurred_at_ms: finiteInteger(input?.occurred_at_ms, Date.now(), 1, Number.MAX_SAFE_INTEGER),
            request_id: trimText(input?.request_id, 160),
            trace_id: trimText(input?.trace_id, 160),
            source_device_id: trimText(input?.source_device_id, 96),
            sequence_no: finiteInteger(input?.sequence_no, 0, 0, Number.MAX_SAFE_INTEGER),
            schema_version: schemaVersion,
            payload
        }
    };
}

function normalizeHomeAiEvents(input, maxEvents = 64) {
    const rawEvents = Array.isArray(input?.events) ? input.events.slice(0, maxEvents) : [];
    if (rawEvents.length === 0) {
        return { ok: false, code: "HOME_AI_EVENTS_INVALID", error: "at least one event is required" };
    }
    const events = [];
    const rejected = [];
    for (const raw of rawEvents) {
        const result = normalizeHomeAiEvent(raw);
        if (result.ok) {
            events.push(result.event);
        } else {
            rejected.push({ event_id: trimText(raw?.event_id, 160), code: result.code });
        }
    }
    return { ok: events.length > 0, events, rejected, code: "HOME_AI_EVENTS_INVALID" };
}

function validateRulePackage(input, options = {}) {
    if (!isPlainObject(input)) {
        return { ok: false, code: "RULE_PACKAGE_INVALID", error: "rule package must be an object" };
    }
    if (Number(input.schema_version) !== HOME_AI_SCHEMA_VERSION) {
        return { ok: false, code: "RULE_PACKAGE_SCHEMA_INVALID", error: `schema_version must be ${HOME_AI_SCHEMA_VERSION}` };
    }
    const roomResult = normalizeRoomConfig({ rooms: input.rooms });
    if (!roomResult.ok) {
        return roomResult;
    }
    const rawRules = Array.isArray(input.rules) ? input.rules : [];
    if (rawRules.length > HOME_AI_MAX_RULES) {
        return { ok: false, code: "RULE_PACKAGE_LIMIT", error: `rules exceed fixed limit ${HOME_AI_MAX_RULES}` };
    }
    const roomIds = new Set(roomResult.rooms.map(room => room.room_id));
    const ruleIds = new Set();
    const rules = [];
    for (const rawRule of rawRules) {
        const normalized = normalizeRule(rawRule, roomIds);
        if (!normalized.ok) {
            return normalized;
        }
        if (ruleIds.has(normalized.rule.rule_id)) {
            return { ok: false, code: "RULE_ID_DUPLICATE", error: "rule_id values must be unique" };
        }
        ruleIds.add(normalized.rule.rule_id);
        rules.push(normalized.rule);
    }
    const packageVersion = finiteInteger(input.version, 1, 1, 2147483647);
    const normalized = {
        schema_version: HOME_AI_SCHEMA_VERSION,
        version: packageVersion,
        generated_at_ms: finiteInteger(input.generated_at_ms, Date.now(), 1, Number.MAX_SAFE_INTEGER),
        rooms: roomResult.rooms,
        rules
    };
    const calculatedChecksum = checksumFor(normalized);
    const suppliedChecksum = trimText(input.checksum, 128);
    if (options.requireChecksum && suppliedChecksum !== calculatedChecksum) {
        return { ok: false, code: "RULE_PACKAGE_CHECKSUM_INVALID", error: "checksum does not match package" };
    }
    const packageWithChecksum = {
        ...normalized,
        checksum: calculatedChecksum
    };
    const bytes = Buffer.byteLength(JSON.stringify(packageWithChecksum), "utf8");
    if (bytes > HOME_AI_MAX_RULE_PACKAGE_BYTES) {
        return { ok: false, code: "RULE_PACKAGE_TOO_LARGE", error: "rule package exceeds S3 fixed package budget" };
    }
    return {
        ok: true,
        package: packageWithChecksum,
        resource: {
            max_rules: HOME_AI_MAX_RULES,
            max_conditions_per_rule: HOME_AI_MAX_CONDITIONS_PER_RULE,
            max_actions_per_rule: HOME_AI_MAX_ACTIONS_PER_RULE,
            max_rule_steps: HOME_AI_MAX_RULE_STEPS,
            max_user_overrides: HOME_AI_MAX_OVERRIDES,
            max_history_event_bytes: HOME_AI_MAX_HISTORY_EVENT_BYTES,
            package_bytes: bytes,
            package_budget_bytes: HOME_AI_MAX_RULE_PACKAGE_BYTES
        }
    };
}

module.exports = {
    ACTIONS,
    HOME_AI_MAX_ACTIONS_PER_RULE,
    HOME_AI_MAX_CONDITIONS_PER_RULE,
    HOME_AI_MAX_CONDITION_TEXT_LENGTH,
    HOME_AI_MAX_HISTORY_EVENT_BYTES,
    HOME_AI_MAX_DEVICE_ID_LENGTH,
    HOME_AI_MAX_OVERRIDES,
    HOME_AI_MAX_OVERRIDE_ID_LENGTH,
    HOME_AI_MAX_OVERRIDE_CONDITION_LENGTH,
    HOME_AI_MAX_PROMPT_LENGTH,
    HOME_AI_MAX_ROOMS,
    HOME_AI_MAX_RULE_PACKAGE_BYTES,
    HOME_AI_MAX_RULES,
    HOME_AI_MAX_RULE_STEPS,
    HOME_AI_SCHEMA_VERSION,
    HOME_AI_EVENT_TYPES,
    OVERRIDE_ACTIONS,
    REQUIRED_SENSING_SOURCES,
    checksumFor,
    defaultRooms,
    normalizeHomeAiEvent,
    normalizeHomeAiEvents,
    normalizeRoomConfig,
    normalizeUserOverride,
    normalizeVirtualDeviceState,
    stableJson,
    validateRulePackage
};
