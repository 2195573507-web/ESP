const { createHash, randomUUID } = require("crypto");
const { compileHabitRuleBundle } = require("./habitRuleBundleCompiler");

const HABIT_RULE_TYPES = Object.freeze({
    PERSON_ENTER_ROOM: "PERSON_ENTER_ROOM",
    PERSON_LEAVE_ROOM: "PERSON_LEAVE_ROOM",
    PERSON_LONG_STAY: "PERSON_LONG_STAY",
    ROOM_EMPTY_TIMEOUT: "ROOM_EMPTY_TIMEOUT",
    NIGHT_ACTIVITY: "NIGHT_ACTIVITY",
    LONG_OCCUPANCY: "LONG_OCCUPANCY"
});

const DEFAULT_HABIT_RULES = Object.freeze([
    { id: "person_enter_room", name: "人员进入房间", type: HABIT_RULE_TYPES.PERSON_ENTER_ROOM, config: { enabled: true, room: "bedroom" } },
    { id: "person_leave_room", name: "人员离开房间", type: HABIT_RULE_TYPES.PERSON_LEAVE_ROOM, config: { enabled: true, room: "bedroom", duration_minutes: 0 } },
    { id: "person_long_stay", name: "长时间停留", type: HABIT_RULE_TYPES.PERSON_LONG_STAY, config: { enabled: true, room: "living_room", threshold_minutes: 60 } },
    { id: "room_empty_timeout", name: "无人超时", type: HABIT_RULE_TYPES.ROOM_EMPTY_TIMEOUT, config: { enabled: true, room: "living_room", threshold_minutes: 30 } },
    { id: "night_activity", name: "夜间活动", type: HABIT_RULE_TYPES.NIGHT_ACTIVITY, config: { enabled: true, room: "home", start_time: "22:00", end_time: "06:00" } },
    { id: "long_occupancy", name: "长期占用", type: HABIT_RULE_TYPES.LONG_OCCUPANCY, config: { enabled: true, room: "living_room", threshold_minutes: 120 } }
]);

const MAX_TEXT_LENGTH = 128;
const RULE_ID_PATTERN = /^[a-zA-Z0-9_-]{1,128}$/;
const TIME_PATTERN = /^(?:[01]\d|2[0-3]):[0-5]\d$/;

function trimText(value, field, required = true) {
    if (typeof value !== "string") {
        return { error: `${field} must be a string` };
    }

    const text = value.trim();
    if (required && !text) {
        return { error: `${field} is required` };
    }
    if (text.length > MAX_TEXT_LENGTH) {
        return { error: `${field} must be <= ${MAX_TEXT_LENGTH} characters` };
    }

    return { value: text };
}

function positiveInteger(value, field, minimum) {
    if (!Number.isInteger(value) || value < minimum || value > 10080) {
        return { error: `${field} must be an integer between ${minimum} and 10080` };
    }
    return { value };
}

function normalizeRuleId(value, fallbackId) {
    const id = value === undefined || value === null || value === "" ? fallbackId : String(value).trim();
    if (!RULE_ID_PATTERN.test(id)) {
        return { error: "id must contain only letters, numbers, underscores, or hyphens and be <= 128 characters" };
    }
    return { value: id };
}

function normalizeConfig(type, config, enabled) {
    if (!config || typeof config !== "object" || Array.isArray(config)) {
        return { error: "config must be a JSON object" };
    }

    if (config.enabled !== undefined && typeof config.enabled !== "boolean") {
        return { error: "config.enabled must be a boolean" };
    }
    if (enabled !== undefined && typeof enabled !== "boolean") {
        return { error: "enabled must be a boolean" };
    }
    if (enabled !== undefined && config.enabled !== undefined && enabled !== config.enabled) {
        return { error: "enabled must match config.enabled" };
    }

    const ruleEnabled = enabled ?? config.enabled;
    if (typeof ruleEnabled !== "boolean") {
        return { error: "enabled is required" };
    }

    const room = trimText(config.room, "config.room");
    if (room.error) {
        return room;
    }

    const normalized = { enabled: ruleEnabled, room: room.value };
    if (type === HABIT_RULE_TYPES.PERSON_LEAVE_ROOM) {
        const duration = positiveInteger(config.duration_minutes, "config.duration_minutes", 0);
        if (duration.error) return duration;
        normalized.duration_minutes = duration.value;
    } else if ([
        HABIT_RULE_TYPES.PERSON_LONG_STAY,
        HABIT_RULE_TYPES.ROOM_EMPTY_TIMEOUT,
        HABIT_RULE_TYPES.LONG_OCCUPANCY
    ].includes(type)) {
        const threshold = positiveInteger(config.threshold_minutes, "config.threshold_minutes", 1);
        if (threshold.error) return threshold;
        normalized.threshold_minutes = threshold.value;
    } else if (type === HABIT_RULE_TYPES.NIGHT_ACTIVITY) {
        if (!TIME_PATTERN.test(config.start_time || "") || !TIME_PATTERN.test(config.end_time || "")) {
            return { error: "config.start_time and config.end_time must use HH:MM" };
        }
        normalized.start_time = config.start_time;
        normalized.end_time = config.end_time;
    }

    return { ok: true, enabled: ruleEnabled, config: normalized };
}

function normalizeRuleInput(input, options = {}) {
    if (!input || typeof input !== "object" || Array.isArray(input)) {
        return { ok: false, error: "JSON object body is required" };
    }

    const id = normalizeRuleId(input.id, options.fallbackId || randomUUID());
    if (id.error) return { ok: false, error: id.error };
    const name = trimText(input.name, "name");
    if (name.error) return { ok: false, error: name.error };
    const type = typeof input.type === "string" ? input.type.trim() : "";
    if (!Object.values(HABIT_RULE_TYPES).includes(type)) {
        return { ok: false, error: "type is not a supported habit rule type" };
    }

    const config = normalizeConfig(type, input.config, input.enabled);
    if (config.error) return { ok: false, error: config.error };
    return {
        ok: true,
        rule: { id: id.value, name: name.value, type, enabled: config.enabled, config: config.config }
    };
}

function parseRuleRow(row) {
    try {
        const config = JSON.parse(row.config_json);
        if (!config || typeof config !== "object" || Array.isArray(config) || typeof config.enabled !== "boolean") {
            return null;
        }
        return {
            id: row.id,
            name: row.name,
            type: row.type,
            enabled: Boolean(row.enabled),
            config,
            created_at: row.created_at,
            updated_at: row.updated_at
        };
    } catch (_) {
        return null;
    }
}

async function getHabitRule(dbAll, id) {
    const rows = await dbAll("SELECT * FROM habitRules WHERE id=? LIMIT 1", [id]);
    return rows[0] ? parseRuleRow(rows[0]) : null;
}

async function listHabitRules(dbAll) {
    const rows = await dbAll("SELECT * FROM habitRules ORDER BY created_at ASC, id ASC");
    return rows.map(parseRuleRow).filter(Boolean);
}

async function listEnabledHabitRules(dbAll) {
    return (await listHabitRules(dbAll)).filter(rule => rule.enabled);
}

async function getHabitRulesSnapshotMetadata(dbAll) {
    const rules = await listHabitRules(dbAll);
    const canonicalRules = rules
        .map(rule => ({
            id: rule.id,
            name: rule.name,
            type: rule.type,
            enabled: rule.enabled,
            config: rule.config
        }))
        .sort((left, right) => left.id.localeCompare(right.id));
    const checksum = createHash("sha256")
        .update(JSON.stringify({ rules: canonicalRules }))
        .digest("hex");
    const updatedAt = rules.reduce((latest, rule) => latest > rule.updated_at ? latest : rule.updated_at, "");
    return {
        version: `habit-rules-v1-${checksum.slice(0, 12)}`,
        checksum,
        updated_at: updatedAt
    };
}

async function createHabitRule(dbRun, dbAll, input) {
    const normalized = normalizeRuleInput(input);
    if (!normalized.ok) return normalized;
    const rule = normalized.rule;
    try {
        await dbRun(`
            INSERT INTO habitRules(id,name,type,enabled,config_json,created_at,updated_at)
            VALUES(?,?,?,?,?,datetime('now'),datetime('now'))`,
        [rule.id, rule.name, rule.type, Number(rule.enabled), JSON.stringify(rule.config)]);
    } catch (error) {
        if (String(error?.message || "").includes("UNIQUE constraint failed")) {
            return { ok: false, code: "HABIT_RULE_EXISTS", error: "habit rule id already exists" };
        }
        throw error;
    }
    return { ok: true, rule: await getHabitRule(dbAll, rule.id) };
}

async function updateHabitRule(dbRun, dbAll, id, input) {
    const existing = await getHabitRule(dbAll, id);
    if (!existing) return { ok: false, code: "HABIT_RULE_NOT_FOUND", error: "habit rule not found" };
    const normalized = normalizeRuleInput({ ...input, id }, { fallbackId: id });
    if (!normalized.ok) return normalized;
    const rule = normalized.rule;
    await dbRun(`
        UPDATE habitRules
        SET name=?,type=?,enabled=?,config_json=?,updated_at=datetime('now')
        WHERE id=?`,
    [rule.name, rule.type, Number(rule.enabled), JSON.stringify(rule.config), id]);
    return { ok: true, rule: await getHabitRule(dbAll, id) };
}

async function deleteHabitRule(dbRun, dbAll, id) {
    const existing = await getHabitRule(dbAll, id);
    if (!existing) return { ok: false, code: "HABIT_RULE_NOT_FOUND", error: "habit rule not found" };
    await dbRun("DELETE FROM habitRules WHERE id=?", [id]);
    return { ok: true, rule: existing };
}

async function ensureDefaultHabitRules(dbRun, dbAll) {
    for (const rule of DEFAULT_HABIT_RULES) {
        const existing = await getHabitRule(dbAll, rule.id);
        if (!existing) {
            const result = await createHabitRule(dbRun, dbAll, rule);
            if (!result.ok) throw new Error(result.error);
        }
    }
}

module.exports = {
    DEFAULT_HABIT_RULES,
    HABIT_RULE_TYPES,
    createHabitRule,
    compileHabitRuleBundle,
    deleteHabitRule,
    ensureDefaultHabitRules,
    getHabitRule,
    getHabitRulesSnapshotMetadata,
    listEnabledHabitRules,
    listHabitRules,
    normalizeRuleInput,
    updateHabitRule
};
