const EVENT_ID_PATTERN = /^[A-Za-z0-9_.:-]{1,128}$/;
const TEXT_LIMIT = 128;

function readText(value, field) {
    if (typeof value !== "string" || !value.trim() || value.trim().length > TEXT_LIMIT) {
        return { error: `${field} must be a non-empty string up to ${TEXT_LIMIT} characters` };
    }
    return { value: value.trim() };
}

function normalizeHabitEvent(input) {
    if (!input || typeof input !== "object" || Array.isArray(input)) {
        return { ok: false, error: "JSON object body is required" };
    }
    const eventId = readText(input.event_id, "event_id");
    const ruleId = readText(input.rule_id, "rule_id");
    const ruleType = readText(input.rule_type, "rule_type");
    const room = readText(input.room, "room");
    const source = readText(input.source, "source");
    const timestamp = readText(input.timestamp, "timestamp");
    if (eventId.error || !EVENT_ID_PATTERN.test(eventId.value || "")) {
        return { ok: false, error: eventId.error || "event_id has invalid characters" };
    }
    for (const value of [ruleId, ruleType, room, source, timestamp]) {
        if (value.error) return { ok: false, error: value.error };
    }
    if (!Number.isInteger(input.sequence) || input.sequence < 1) {
        return { ok: false, error: "sequence must be a positive integer" };
    }
    if (!input.payload || typeof input.payload !== "object" || Array.isArray(input.payload)) {
        return { ok: false, error: "payload must be an object" };
    }
    return {
        ok: true,
        event: {
            event_id: eventId.value,
            rule_id: ruleId.value,
            rule_type: ruleType.value,
            room: room.value,
            source: source.value,
            timestamp: timestamp.value,
            sequence: input.sequence,
            payload: input.payload
        }
    };
}

async function saveHabitEvent(dbRun, input) {
    const normalized = normalizeHabitEvent(input);
    if (!normalized.ok) return normalized;
    const event = normalized.event;
    const result = await dbRun(
        `INSERT INTO habit_events(event_id,rule_type,room,payload)
         VALUES(?,?,?,?)
         ON CONFLICT(event_id) DO NOTHING`,
        [event.event_id, event.rule_type, event.room, JSON.stringify(event)]
    );
    return { ok: true, duplicate: result.changes === 0, event };
}

module.exports = { normalizeHabitEvent, saveHabitEvent };
