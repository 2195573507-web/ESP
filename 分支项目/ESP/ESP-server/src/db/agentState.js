const {
    ensureTableColumns,
    ensureUniqueIndex
} = require("./migrations");

const ENVIRONMENT_PROFILE_COLUMNS = [
    { name: "id", type: "INTEGER PRIMARY KEY AUTOINCREMENT" },
    { name: "profile_key", type: "TEXT NOT NULL UNIQUE" },
    { name: "profile_value", type: "TEXT NOT NULL" },
    { name: "device_id", type: "TEXT" },
    { name: "status", type: "TEXT NOT NULL DEFAULT 'candidate'" },
    { name: "confidence", type: "REAL NOT NULL DEFAULT 0.5" },
    { name: "evidence_json", type: "TEXT" },
    { name: "source", type: "TEXT" },
    { name: "created_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))", addType: "TEXT" },
    { name: "updated_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))", addType: "TEXT" }
];

const EXPERIENCE_MEMORY_COLUMNS = [
    { name: "id", type: "INTEGER PRIMARY KEY AUTOINCREMENT" },
    { name: "experience_id", type: "TEXT NOT NULL UNIQUE" },
    { name: "title", type: "TEXT NOT NULL" },
    { name: "situation", type: "TEXT" },
    { name: "action", type: "TEXT" },
    { name: "outcome", type: "TEXT" },
    { name: "status", type: "TEXT NOT NULL DEFAULT 'candidate'" },
    { name: "confidence", type: "REAL NOT NULL DEFAULT 0.5" },
    { name: "evidence_json", type: "TEXT" },
    { name: "source", type: "TEXT" },
    { name: "created_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))", addType: "TEXT" },
    { name: "updated_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))", addType: "TEXT" }
];

const RELATION_MEMORY_COLUMNS = [
    { name: "id", type: "INTEGER PRIMARY KEY AUTOINCREMENT" },
    { name: "relation_id", type: "TEXT NOT NULL UNIQUE" },
    { name: "subject", type: "TEXT NOT NULL" },
    { name: "predicate", type: "TEXT NOT NULL" },
    { name: "object", type: "TEXT NOT NULL" },
    { name: "relation_type", type: "TEXT NOT NULL DEFAULT 'general'" },
    { name: "status", type: "TEXT NOT NULL DEFAULT 'candidate'" },
    { name: "confidence", type: "REAL NOT NULL DEFAULT 0.5" },
    { name: "evidence_json", type: "TEXT" },
    { name: "source", type: "TEXT" },
    { name: "created_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))", addType: "TEXT" },
    { name: "updated_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))", addType: "TEXT" }
];

const REMINDER_RULE_COLUMNS = [
    { name: "id", type: "INTEGER PRIMARY KEY AUTOINCREMENT" },
    { name: "reminder_id", type: "TEXT NOT NULL UNIQUE" },
    { name: "title", type: "TEXT NOT NULL" },
    { name: "message", type: "TEXT NOT NULL" },
    { name: "rule_json", type: "TEXT NOT NULL DEFAULT '{}'" },
    { name: "channel", type: "TEXT NOT NULL DEFAULT 'voice'" },
    { name: "status", type: "TEXT NOT NULL DEFAULT 'active'" },
    { name: "next_run_at", type: "TEXT" },
    { name: "suppress_until", type: "TEXT" },
    { name: "source", type: "TEXT" },
    { name: "created_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))", addType: "TEXT" },
    { name: "updated_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))", addType: "TEXT" }
];

const REMINDER_RECORD_COLUMNS = [
    { name: "id", type: "INTEGER PRIMARY KEY AUTOINCREMENT" },
    { name: "reminder_event_id", type: "TEXT NOT NULL UNIQUE" },
    { name: "reminder_id", type: "TEXT" },
    { name: "status", type: "TEXT NOT NULL DEFAULT 'pending'" },
    { name: "message", type: "TEXT NOT NULL" },
    { name: "due_at", type: "TEXT" },
    { name: "action_json", type: "TEXT" },
    { name: "result_json", type: "TEXT" },
    { name: "created_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))", addType: "TEXT" },
    { name: "updated_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))", addType: "TEXT" },
    { name: "completed_at", type: "TEXT" }
];

const EMERGENCY_EVENT_COLUMNS = [
    { name: "id", type: "INTEGER PRIMARY KEY AUTOINCREMENT" },
    { name: "event_id", type: "TEXT NOT NULL UNIQUE" },
    { name: "device_id", type: "TEXT" },
    { name: "event_type", type: "TEXT NOT NULL" },
    { name: "severity", type: "TEXT NOT NULL DEFAULT 'info'" },
    { name: "local_action", type: "TEXT" },
    { name: "payload_json", type: "TEXT NOT NULL DEFAULT '{}'" },
    { name: "llm_decision_json", type: "TEXT" },
    { name: "status", type: "TEXT NOT NULL DEFAULT 'received'" },
    { name: "created_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))", addType: "TEXT" },
    { name: "updated_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))", addType: "TEXT" },
    { name: "resolved_at", type: "TEXT" }
];

const CSI_BEHAVIOR_EVENT_COLUMNS = [
    { name: "id", type: "INTEGER PRIMARY KEY AUTOINCREMENT" },
    { name: "event_id", type: "TEXT NOT NULL UNIQUE" },
    { name: "device_id", type: "TEXT" },
    { name: "behavior_type", type: "TEXT NOT NULL" },
    { name: "confidence", type: "REAL NOT NULL DEFAULT 0.5" },
    { name: "features_json", type: "TEXT NOT NULL DEFAULT '{}'" },
    { name: "summary", type: "TEXT" },
    { name: "occurred_at", type: "TEXT NOT NULL" },
    { name: "created_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))", addType: "TEXT" },
    { name: "updated_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))", addType: "TEXT" }
];

const LCD_STATUS_COLUMNS = [
    { name: "id", type: "INTEGER PRIMARY KEY AUTOINCREMENT" },
    { name: "device_id", type: "TEXT NOT NULL UNIQUE" },
    { name: "page", type: "TEXT NOT NULL DEFAULT 'idle'" },
    { name: "state_json", type: "TEXT NOT NULL DEFAULT '{}'" },
    { name: "last_command_id", type: "TEXT" },
    { name: "updated_at", type: "TEXT NOT NULL" },
    { name: "created_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))", addType: "TEXT" }
];

function columnSql(columns) {
    return columns.map(column => `${column.name} ${column.type}`).join(",\n            ");
}

function addableColumns(columns) {
    return columns.filter(column => !column.type.includes("PRIMARY KEY"));
}

async function ensureAgentStateTables(dbRun, dbAll) {
    await dbRun(`
        CREATE TABLE IF NOT EXISTS environment_profile (
            ${columnSql(ENVIRONMENT_PROFILE_COLUMNS)}
        )
    `);

    await dbRun(`
        CREATE TABLE IF NOT EXISTS experience_memory (
            ${columnSql(EXPERIENCE_MEMORY_COLUMNS)}
        )
    `);

    await dbRun(`
        CREATE TABLE IF NOT EXISTS relation_memory (
            ${columnSql(RELATION_MEMORY_COLUMNS)}
        )
    `);

    await dbRun(`
        CREATE TABLE IF NOT EXISTS reminder_rules (
            ${columnSql(REMINDER_RULE_COLUMNS)}
        )
    `);

    await dbRun(`
        CREATE TABLE IF NOT EXISTS reminder_records (
            ${columnSql(REMINDER_RECORD_COLUMNS)}
        )
    `);

    await dbRun(`
        CREATE TABLE IF NOT EXISTS emergency_events (
            ${columnSql(EMERGENCY_EVENT_COLUMNS)}
        )
    `);

    await dbRun(`
        CREATE TABLE IF NOT EXISTS csi_behavior_events (
            ${columnSql(CSI_BEHAVIOR_EVENT_COLUMNS)}
        )
    `);

    await dbRun(`
        CREATE TABLE IF NOT EXISTS lcd_status (
            ${columnSql(LCD_STATUS_COLUMNS)}
        )
    `);

    if (typeof dbAll === "function") {
        await ensureTableColumns(dbRun, dbAll, "environment_profile", addableColumns(ENVIRONMENT_PROFILE_COLUMNS));
        await ensureTableColumns(dbRun, dbAll, "experience_memory", addableColumns(EXPERIENCE_MEMORY_COLUMNS));
        await ensureTableColumns(dbRun, dbAll, "relation_memory", addableColumns(RELATION_MEMORY_COLUMNS));
        await ensureTableColumns(dbRun, dbAll, "reminder_rules", addableColumns(REMINDER_RULE_COLUMNS));
        await ensureTableColumns(dbRun, dbAll, "reminder_records", addableColumns(REMINDER_RECORD_COLUMNS));
        await ensureTableColumns(dbRun, dbAll, "emergency_events", addableColumns(EMERGENCY_EVENT_COLUMNS));
        await ensureTableColumns(dbRun, dbAll, "csi_behavior_events", addableColumns(CSI_BEHAVIOR_EVENT_COLUMNS));
        await ensureTableColumns(dbRun, dbAll, "lcd_status", addableColumns(LCD_STATUS_COLUMNS));
        await ensureUniqueIndex(dbRun, dbAll, "environment_profile", "idx_environment_profile_profile_key_unique", ["profile_key"]);
        await ensureUniqueIndex(dbRun, dbAll, "experience_memory", "idx_experience_memory_experience_id_unique", ["experience_id"]);
        await ensureUniqueIndex(dbRun, dbAll, "relation_memory", "idx_relation_memory_relation_id_unique", ["relation_id"]);
        await ensureUniqueIndex(dbRun, dbAll, "reminder_rules", "idx_reminder_rules_reminder_id_unique", ["reminder_id"]);
        await ensureUniqueIndex(dbRun, dbAll, "reminder_records", "idx_reminder_records_event_id_unique", ["reminder_event_id"]);
        await ensureUniqueIndex(dbRun, dbAll, "emergency_events", "idx_emergency_events_event_id_unique", ["event_id"]);
        await ensureUniqueIndex(dbRun, dbAll, "csi_behavior_events", "idx_csi_behavior_events_event_id_unique", ["event_id"]);
        await ensureUniqueIndex(dbRun, dbAll, "lcd_status", "idx_lcd_status_device_id_unique", ["device_id"]);
    }

    await dbRun("CREATE INDEX IF NOT EXISTS idx_environment_profile_status ON environment_profile(status,device_id,id)");
    await dbRun("CREATE INDEX IF NOT EXISTS idx_experience_memory_status ON experience_memory(status,id)");
    await dbRun("CREATE INDEX IF NOT EXISTS idx_relation_memory_status ON relation_memory(status,relation_type,id)");
    await dbRun("CREATE INDEX IF NOT EXISTS idx_reminder_rules_status ON reminder_rules(status,next_run_at,id)");
    await dbRun("CREATE INDEX IF NOT EXISTS idx_reminder_records_status ON reminder_records(status,due_at,id)");
    await dbRun("CREATE INDEX IF NOT EXISTS idx_emergency_events_device ON emergency_events(device_id,status,id)");
    await dbRun("CREATE INDEX IF NOT EXISTS idx_csi_behavior_events_device ON csi_behavior_events(device_id,occurred_at,id)");
}

module.exports = {
    ensureAgentStateTables
};
