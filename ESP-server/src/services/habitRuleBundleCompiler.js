const { createHash } = require("crypto");

const HABIT_RULE_BUNDLE_SCHEMA_VERSION = "habit-rule-bundle-v1";
const HABIT_RULE_SCOPE_TYPES = new Set(["room", "zone", "home"]);
const HABIT_RULE_TYPES = new Set([
    "PERSON_ENTER_ROOM",
    "PERSON_LEAVE_ROOM",
    "PERSON_LONG_STAY",
    "ROOM_EMPTY_TIMEOUT",
    "NIGHT_ACTIVITY",
    "LONG_OCCUPANCY"
]);

function canonicalize(value) {
    if (Array.isArray(value)) return value.map(canonicalize);
    if (value && typeof value === "object") {
        return Object.keys(value).sort().reduce((result, key) => {
            result[key] = canonicalize(value[key]);
            return result;
        }, {});
    }
    return value;
}

function canonicalJson(value) {
    return JSON.stringify(canonicalize(value));
}

function normalizeScope(rule) {
    const room = typeof rule?.config?.room === "string" ? rule.config.room.trim() : "";
    if (!room) return { error: "rule config.room is required for bundle scope" };

    if (room === "home") {
        return { value: { type: "home" } };
    }

    return { value: { type: "room", id: room } };
}

function compileRule(rule) {
    if (!rule || typeof rule !== "object" || typeof rule.id !== "string" || !rule.id.trim()) {
        return { error: "rule id is required" };
    }
    if (typeof rule.enabled !== "boolean" || typeof rule.type !== "string" || !rule.type.trim()) {
        return { error: `rule ${rule.id} has invalid enabled or type` };
    }
    if (!rule.config || typeof rule.config !== "object" || Array.isArray(rule.config)) {
        return { error: `rule ${rule.id} has invalid config` };
    }

    const scope = normalizeScope(rule);
    if (scope.error) return scope;
    const parameters = { ...rule.config };
    delete parameters.enabled;
    delete parameters.room;

    return {
        value: {
            id: rule.id.trim(),
            enabled: rule.enabled,
            type: rule.type.trim(),
            scope: scope.value,
            parameters: canonicalize(parameters)
        }
    };
}

function validateBundleRule(rule) {
    if (!rule || typeof rule !== "object" || Array.isArray(rule)) return "rule must be an object";
    if (typeof rule.id !== "string" || !rule.id.trim()) return "rule id is required";
    if (typeof rule.enabled !== "boolean") return `rule ${rule.id} enabled must be boolean`;
    if (typeof rule.type !== "string" || !rule.type.trim()) return `rule ${rule.id} type is required`;
    if (!HABIT_RULE_TYPES.has(rule.type)) return `rule ${rule.id} type is not supported by habit-rule-bundle-v1`;
    if (!rule.scope || typeof rule.scope !== "object" || Array.isArray(rule.scope) ||
        !HABIT_RULE_SCOPE_TYPES.has(rule.scope.type)) {
        return `rule ${rule.id} scope must be room, zone, or home`;
    }
    if ((rule.scope.type === "room" || rule.scope.type === "zone") || rule.scope.id !== undefined) {
        if (typeof rule.scope.id !== "string" || !rule.scope.id.trim()) {
            return `rule ${rule.id} scoped id is required`;
        }
    }
    if (!rule.parameters || typeof rule.parameters !== "object" || Array.isArray(rule.parameters)) {
        return `rule ${rule.id} parameters must be an object`;
    }
    return null;
}

function compileHabitRuleBundle(rules) {
    if (!Array.isArray(rules)) return { ok: false, error: "rules must be an array" };
    const compiledRules = [];
    const ids = new Set();
    const types = new Set();
    for (const rule of rules) {
        const compiled = compileRule(rule);
        if (compiled.error) return { ok: false, error: compiled.error };
        const validationError = validateBundleRule(compiled.value);
        if (validationError) return { ok: false, error: validationError };
        if (ids.has(compiled.value.id)) return { ok: false, error: `duplicate rule id ${compiled.value.id}` };
        if (types.has(compiled.value.type)) {
            return { ok: false, error: `duplicate rule type ${compiled.value.type} is not supported by S3 v1` };
        }
        ids.add(compiled.value.id);
        types.add(compiled.value.type);
        compiledRules.push(compiled.value);
    }
    compiledRules.sort((left, right) => left.id.localeCompare(right.id));

    const checksum = createHash("sha256")
        .update(canonicalJson({ schema_version: HABIT_RULE_BUNDLE_SCHEMA_VERSION, rules: compiledRules }))
        .digest("hex");
    const createdAt = rules.reduce((latest, rule) => {
        const candidate = typeof rule.updated_at === "string" ? rule.updated_at : rule.created_at;
        return candidate && candidate > latest ? candidate : latest;
    }, "1970-01-01 00:00:00");

    return {
        ok: true,
        bundle: {
            schema_version: HABIT_RULE_BUNDLE_SCHEMA_VERSION,
            bundle_version: `v1-${checksum.slice(0, 12)}`,
            checksum,
            created_at: createdAt,
            rules: compiledRules
        }
    };
}

module.exports = {
    HABIT_RULE_BUNDLE_SCHEMA_VERSION,
    canonicalJson,
    compileHabitRuleBundle,
    validateBundleRule
};
