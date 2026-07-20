async function ensureHabitEventsTables(dbRun) {
    await dbRun(`
        CREATE TABLE IF NOT EXISTS habit_events (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            event_id TEXT NOT NULL,
            rule_type TEXT NOT NULL,
            room TEXT NOT NULL,
            payload TEXT NOT NULL,
            created_at TEXT NOT NULL DEFAULT (datetime('now'))
        )
    `);
    await dbRun("CREATE UNIQUE INDEX IF NOT EXISTS idx_habit_events_event_id ON habit_events(event_id)");
}

module.exports = { ensureHabitEventsTables };
