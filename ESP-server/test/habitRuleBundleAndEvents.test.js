const assert = require("assert");
const fs = require("fs");
const os = require("os");
const path = require("path");
const test = require("node:test");
const sqlite3 = require("sqlite3").verbose();
const { ensureHabitEventsTables } = require("../src/db/habitEvents");
const { ensureHabitRulesTables } = require("../src/db/habitRules");
const { compileHabitRuleBundle } = require("../src/services/habitRuleBundleCompiler");
const { normalizeHabitEvent, saveHabitEvent } = require("../src/services/habitEventsService");
const { ensureDefaultHabitRules, listHabitRules } = require("../src/services/habitRulesService");
const { createDbHelpers } = require("../src/db/sqlite");

function openTestDatabase() {
    const directory = fs.mkdtempSync(path.join(os.tmpdir(), "habit-bundle-events-test-"));
    const database = new sqlite3.Database(path.join(directory, "habit.sqlite"));
    return { directory, database, ...createDbHelpers(database) };
}

function closeDatabase(database) {
    return new Promise((resolve, reject) => database.close(error => error ? reject(error) : resolve()));
}

test("habit-rule-bundle-v1 checksum is stable, changes with rules, and rejects invalid rules", async () => {
    const context = openTestDatabase();
    try {
        await ensureHabitRulesTables(context.dbRun);
        await ensureDefaultHabitRules(context.dbRun, context.dbAll);
        const rules = await listHabitRules(context.dbAll);
        const first = compileHabitRuleBundle(rules);
        const second = compileHabitRuleBundle([...rules].reverse());
        assert.equal(first.ok, true);
        assert.equal(first.bundle.schema_version, "habit-rule-bundle-v1");
        assert.equal(first.bundle.checksum, second.bundle.checksum);
        assert.deepEqual(first.bundle.rules.find(rule => rule.id === "person_leave_room").scope,
            { type: "room", id: "bedroom" });

        const changed = rules.map(rule => rule.id === "long_occupancy"
            ? { ...rule, config: { ...rule.config, threshold_minutes: 121 } }
            : rule);
        assert.notEqual(compileHabitRuleBundle(changed).bundle.checksum, first.bundle.checksum);
        assert.equal(compileHabitRuleBundle([{ id: "bad", enabled: true, type: "PERSON_ENTER_ROOM", config: {} }]).ok, false);
        assert.equal(compileHabitRuleBundle([{ id: "bad", enabled: true, type: "UNKNOWN", config: { room: "bedroom" } }]).ok, false);
    } finally {
        await closeDatabase(context.database);
        fs.rmSync(context.directory, { recursive: true, force: true });
    }
});

test("S3 habit event JSON parses, persists, and event_id de-duplicates", async () => {
    const context = openTestDatabase();
    try {
        await ensureHabitEventsTables(context.dbRun);
        const event = {
            event_id: "habit-100-1",
            rule_id: "person_enter_room",
            rule_type: "PERSON_ENTER_ROOM",
            room: "bedroom",
            source: "C52",
            timestamp: "2026-07-20T10:00:00",
            sequence: 1,
            payload: { person_count: 1, reason: "occupied_false_to_true" }
        };
        assert.equal(normalizeHabitEvent(event).ok, true);
        assert.equal((await saveHabitEvent(context.dbRun, event)).duplicate, false);
        assert.equal((await saveHabitEvent(context.dbRun, event)).duplicate, true);
        const rows = await context.dbAll("SELECT event_id, rule_type, room, payload FROM habit_events");
        assert.equal(rows.length, 1);
        assert.equal(rows[0].event_id, event.event_id);
        assert.equal(JSON.parse(rows[0].payload).source, "C52");
    } finally {
        await closeDatabase(context.database);
        fs.rmSync(context.directory, { recursive: true, force: true });
    }
});
