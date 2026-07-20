const {
    ensureTableColumns,
    ensureUniqueIndex
} = require("./migrations");

const CONVERSATION_TURN_COLUMNS = [
    { name: "id", type: "INTEGER PRIMARY KEY AUTOINCREMENT" },
    { name: "turn_id", type: "TEXT NOT NULL UNIQUE" },
    { name: "session_id", type: "TEXT" },
    { name: "device_id", type: "TEXT" },
    { name: "role", type: "TEXT NOT NULL DEFAULT 'user'" },
    { name: "input_text", type: "TEXT" },
    { name: "response_text", type: "TEXT" },
    { name: "structured_json", type: "TEXT" },
    { name: "command_ids_json", type: "TEXT" },
    { name: "memory_level", type: "TEXT NOT NULL DEFAULT 'episodic'" },
    { name: "importance", type: "INTEGER NOT NULL DEFAULT 1" },
    { name: "source", type: "TEXT" },
    { name: "created_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))", addType: "TEXT" },
    { name: "updated_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))", addType: "TEXT" }
];

const DAILY_MEMORY_COLUMNS = [
    { name: "id", type: "INTEGER PRIMARY KEY AUTOINCREMENT" },
    { name: "memory_date", type: "TEXT NOT NULL" },
    { name: "summary", type: "TEXT NOT NULL" },
    { name: "status", type: "TEXT NOT NULL DEFAULT 'candidate'" },
    { name: "source", type: "TEXT" },
    { name: "confidence", type: "REAL NOT NULL DEFAULT 0.5" },
    { name: "input_json", type: "TEXT" },
    { name: "raw_json", type: "TEXT" },
    { name: "created_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))", addType: "TEXT" },
    { name: "updated_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))", addType: "TEXT" }
];

const LONG_TERM_PROFILE_COLUMNS = [
    { name: "id", type: "INTEGER PRIMARY KEY AUTOINCREMENT" },
    { name: "profile_key", type: "TEXT NOT NULL UNIQUE" },
    { name: "profile_value", type: "TEXT NOT NULL" },
    { name: "category", type: "TEXT NOT NULL DEFAULT 'user'" },
    { name: "status", type: "TEXT NOT NULL DEFAULT 'candidate'" },
    { name: "confidence", type: "REAL NOT NULL DEFAULT 0.5" },
    { name: "evidence_json", type: "TEXT" },
    { name: "correction_count", type: "INTEGER NOT NULL DEFAULT 0" },
    { name: "source", type: "TEXT" },
    { name: "created_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))", addType: "TEXT" },
    { name: "updated_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))", addType: "TEXT" }
];

const MEMORY_CORRECTION_COLUMNS = [
    { name: "id", type: "INTEGER PRIMARY KEY AUTOINCREMENT" },
    { name: "correction_id", type: "TEXT NOT NULL UNIQUE" },
    { name: "target_type", type: "TEXT NOT NULL" },
    { name: "target_id", type: "TEXT NOT NULL" },
    { name: "correction_text", type: "TEXT NOT NULL" },
    { name: "corrected_value", type: "TEXT" },
    { name: "device_id", type: "TEXT" },
    { name: "session_id", type: "TEXT" },
    { name: "status", type: "TEXT NOT NULL DEFAULT 'applied'" },
    { name: "raw_json", type: "TEXT" },
    { name: "created_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))", addType: "TEXT" },
    { name: "updated_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))", addType: "TEXT" }
];

const MEMORY_JOB_RUN_COLUMNS = [
    { name: "id", type: "INTEGER PRIMARY KEY AUTOINCREMENT" },
    { name: "job_id", type: "TEXT NOT NULL UNIQUE" },
    { name: "job_name", type: "TEXT NOT NULL" },
    { name: "target_date", type: "TEXT" },
    { name: "status", type: "TEXT NOT NULL DEFAULT 'queued'" },
    { name: "input_json", type: "TEXT" },
    { name: "result_json", type: "TEXT" },
    { name: "error_message", type: "TEXT" },
    { name: "created_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))", addType: "TEXT" },
    { name: "updated_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))", addType: "TEXT" },
    { name: "completed_at", type: "TEXT" }
];

function columnSql(columns) {
    return columns.map(column => `${column.name} ${column.type}`).join(",\n            ");
}

function addableColumns(columns) {
    return columns.filter(column => !column.type.includes("PRIMARY KEY"));
}

async function ensureMemoryTables(dbRun) {
    await dbRun(`
        CREATE TABLE IF NOT EXISTS conversation_turns (
            ${columnSql(CONVERSATION_TURN_COLUMNS)}
        )
    `);

    await dbRun(`
        CREATE TABLE IF NOT EXISTS daily_memory (
            ${columnSql(DAILY_MEMORY_COLUMNS)}
        )
    `);

    await dbRun(`
        CREATE TABLE IF NOT EXISTS long_term_profile (
            ${columnSql(LONG_TERM_PROFILE_COLUMNS)}
        )
    `);

    await dbRun(`
        CREATE TABLE IF NOT EXISTS memory_corrections (
            ${columnSql(MEMORY_CORRECTION_COLUMNS)}
        )
    `);

    await dbRun(`
        CREATE TABLE IF NOT EXISTS memory_job_runs (
            ${columnSql(MEMORY_JOB_RUN_COLUMNS)}
        )
    `);

    if (arguments.length > 1 && typeof arguments[1] === "function") {
        const dbAll = arguments[1];
        await ensureTableColumns(dbRun, dbAll, "conversation_turns", addableColumns(CONVERSATION_TURN_COLUMNS));
        await ensureTableColumns(dbRun, dbAll, "daily_memory", addableColumns(DAILY_MEMORY_COLUMNS));
        await ensureTableColumns(dbRun, dbAll, "long_term_profile", addableColumns(LONG_TERM_PROFILE_COLUMNS));
        await ensureTableColumns(dbRun, dbAll, "memory_corrections", addableColumns(MEMORY_CORRECTION_COLUMNS));
        await ensureTableColumns(dbRun, dbAll, "memory_job_runs", addableColumns(MEMORY_JOB_RUN_COLUMNS));
        await ensureUniqueIndex(dbRun, dbAll, "conversation_turns", "idx_conversation_turns_turn_id_unique", ["turn_id"]);
        await ensureUniqueIndex(dbRun, dbAll, "long_term_profile", "idx_long_term_profile_profile_key_unique", ["profile_key"]);
        await ensureUniqueIndex(dbRun, dbAll, "memory_corrections", "idx_memory_corrections_correction_id_unique", ["correction_id"]);
        await ensureUniqueIndex(dbRun, dbAll, "memory_job_runs", "idx_memory_job_runs_job_id_unique", ["job_id"]);
    }

    await dbRun("CREATE INDEX IF NOT EXISTS idx_conversation_turns_session ON conversation_turns(session_id,id)");
    await dbRun("CREATE INDEX IF NOT EXISTS idx_conversation_turns_device ON conversation_turns(device_id,id)");
    await dbRun("CREATE INDEX IF NOT EXISTS idx_daily_memory_date ON daily_memory(memory_date,id)");
    await dbRun("CREATE INDEX IF NOT EXISTS idx_long_term_profile_status ON long_term_profile(status,category,id)");
    await dbRun("CREATE INDEX IF NOT EXISTS idx_memory_job_runs_name ON memory_job_runs(job_name,target_date,id)");
}

module.exports = {
    ensureMemoryTables
};
