const assert = require("assert");
const fs = require("fs");
const os = require("os");
const path = require("path");
const test = require("node:test");
const sqlite3 = require("sqlite3").verbose();
const { ensureHabitRulesTables } = require("../src/db/habitRules");
const {
    createHabitRule,
    deleteHabitRule,
    ensureDefaultHabitRules,
    getHabitRule,
    getHabitRulesSnapshotMetadata,
    listEnabledHabitRules,
    listHabitRules,
    normalizeRuleInput,
    updateHabitRule
} = require("../src/services/habitRulesService");
const { createDbHelpers } = require("../src/db/sqlite");

function openTestDatabase() {
    const directory = fs.mkdtempSync(path.join(os.tmpdir(), "habit-rules-test-"));
    const database = new sqlite3.Database(path.join(directory, "rules.sqlite"));
    return { directory, database, ...createDbHelpers(database) };
}

function closeDatabase(database) {
    return new Promise((resolve, reject) => database.close(error => error ? reject(error) : resolve()));
}

test("habit rules initialize default rules once and expose enabled automation rules", async () => {
    const context = openTestDatabase();
    try {
        await ensureHabitRulesTables(context.dbRun);
        await ensureDefaultHabitRules(context.dbRun, context.dbAll);
        await ensureDefaultHabitRules(context.dbRun, context.dbAll);

        const rules = await listHabitRules(context.dbAll);
        assert.equal(rules.length, 6);
        assert.deepEqual((await getHabitRule(context.dbAll, "person_leave_room")).config, {
            enabled: true,
            room: "bedroom",
            duration_minutes: 0
        });
        assert.equal((await listEnabledHabitRules(context.dbAll)).length, 6);
    } finally {
        await closeDatabase(context.database);
        fs.rmSync(context.directory, { recursive: true, force: true });
    }
});

test("habit rules support CRUD and keep enabled mirrored in config_json", async () => {
    const context = openTestDatabase();
    try {
        await ensureHabitRulesTables(context.dbRun);
        await ensureDefaultHabitRules(context.dbRun, context.dbAll);
        const before = await getHabitRulesSnapshotMetadata(context.dbAll);
        const created = await createHabitRule(context.dbRun, context.dbAll, {
            id: "leave-office",
            name: "离开书房",
            type: "PERSON_LEAVE_ROOM",
            enabled: true,
            config: { enabled: true, room: "office", duration_minutes: 0 }
        });
        assert.equal(created.ok, true);
        assert.equal(created.rule.config.room, "office");

        const updated = await updateHabitRule(context.dbRun, context.dbAll, "leave-office", {
            name: "离开书房后提醒",
            type: "PERSON_LEAVE_ROOM",
            enabled: false,
            config: { enabled: false, room: "office", duration_minutes: 15 }
        });
        assert.equal(updated.ok, true);
        assert.equal(updated.rule.enabled, false);
        assert.deepEqual(updated.rule.config, { enabled: false, room: "office", duration_minutes: 15 });
        assert.ok(updated.rule.updated_at);
        const after = await getHabitRulesSnapshotMetadata(context.dbAll);
        assert.notEqual(after.checksum, before.checksum);
        assert.notEqual(after.version, before.version);
        assert.ok(after.updated_at);

        const deleted = await deleteHabitRule(context.dbRun, context.dbAll, "leave-office");
        assert.equal(deleted.ok, true);
        assert.equal(await getHabitRule(context.dbAll, "leave-office"), null);
    } finally {
        await closeDatabase(context.database);
        fs.rmSync(context.directory, { recursive: true, force: true });
    }
});

test("habit rule schema rejects invalid JSON shape, config fields, and incompatible enabled values", () => {
    assert.equal(normalizeRuleInput(null).ok, false);
    assert.equal(normalizeRuleInput({
        id: "bad-json",
        name: "坏规则",
        type: "PERSON_LEAVE_ROOM",
        enabled: true,
        config: "not-an-object"
    }).ok, false);
    assert.equal(normalizeRuleInput({
        id: "bad-duration",
        name: "坏规则",
        type: "PERSON_LEAVE_ROOM",
        enabled: true,
        config: { enabled: true, room: "bedroom", duration_minutes: -1 }
    }).ok, false);
    assert.equal(normalizeRuleInput({
        id: "mismatch",
        name: "坏规则",
        type: "LONG_OCCUPANCY",
        enabled: false,
        config: { enabled: true, room: "living_room", threshold_minutes: 120 }
    }).ok, false);
});
