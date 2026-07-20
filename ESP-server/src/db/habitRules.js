const HABIT_RULES_COLUMNS = [
    { name: "id", type: "TEXT PRIMARY KEY" },
    { name: "name", type: "TEXT NOT NULL" },
    { name: "type", type: "TEXT NOT NULL" },
    { name: "enabled", type: "INTEGER NOT NULL DEFAULT 1 CHECK (enabled IN (0, 1))" },
    { name: "config_json", type: "TEXT NOT NULL" },
    { name: "created_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))" },
    { name: "updated_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))" }
];

function columnSql(columns) {
    return columns.map(column => `${column.name} ${column.type}`).join(",\n            ");
}

async function ensureHabitRulesTables(dbRun) {
    await dbRun(`
        CREATE TABLE IF NOT EXISTS habitRules (
            ${columnSql(HABIT_RULES_COLUMNS)}
        )
    `);
    await dbRun("CREATE INDEX IF NOT EXISTS idx_habit_rules_type_enabled ON habitRules(type,enabled)");
}

module.exports = {
    ensureHabitRulesTables
};
