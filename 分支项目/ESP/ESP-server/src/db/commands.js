const {
    ensureTableColumns,
    ensureUniqueIndex
} = require("./migrations");

const DEVICE_CAPABILITIES_COLUMNS = [
    { name: "id", type: "INTEGER PRIMARY KEY AUTOINCREMENT" },
    { name: "device_id", type: "TEXT NOT NULL UNIQUE" },
    { name: "protocol_version", type: "TEXT" },
    { name: "capabilities_json", type: "TEXT NOT NULL DEFAULT '{}'" },
    { name: "last_seen_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))", addType: "TEXT" },
    { name: "created_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))", addType: "TEXT" },
    { name: "updated_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))", addType: "TEXT" }
];

const COMMAND_QUEUE_COLUMNS = [
    { name: "id", type: "INTEGER PRIMARY KEY AUTOINCREMENT" },
    { name: "command_id", type: "TEXT NOT NULL UNIQUE" },
    { name: "device_id", type: "TEXT NOT NULL" },
    { name: "name", type: "TEXT NOT NULL" },
    { name: "payload_json", type: "TEXT NOT NULL DEFAULT '{}'" },
    { name: "status", type: "TEXT NOT NULL DEFAULT 'queued'" },
    { name: "gateway_id", type: "TEXT" },
    { name: "source", type: "TEXT" },
    { name: "requested_by", type: "TEXT" },
    { name: "related_llm_record_id", type: "INTEGER" },
    { name: "error_code", type: "TEXT" },
    { name: "error_message", type: "TEXT" },
    { name: "result_json", type: "TEXT" },
    { name: "raw_json", type: "TEXT" },
    { name: "created_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))", addType: "TEXT" },
    { name: "updated_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))", addType: "TEXT" },
    { name: "dispatched_at", type: "TEXT" },
    { name: "dispatch_count", type: "INTEGER NOT NULL DEFAULT 0", addType: "INTEGER" },
    { name: "acknowledged_at", type: "TEXT" },
    { name: "completed_at", type: "TEXT" },
    { name: "expires_at", type: "TEXT" },
    { name: "reject_reason", type: "TEXT" }
];

function columnSql(columns) {
    return columns.map(column => `${column.name} ${column.type}`).join(",\n            ");
}

function addableColumns(columns) {
    return columns.filter(column => !column.type.includes("PRIMARY KEY"));
}

async function ensureCommandTables(dbRun, dbAll) {
    await dbRun(`
        CREATE TABLE IF NOT EXISTS device_capabilities (
            ${columnSql(DEVICE_CAPABILITIES_COLUMNS)}
        )
    `);

    await dbRun(`
        CREATE TABLE IF NOT EXISTS command_queue (
            ${columnSql(COMMAND_QUEUE_COLUMNS)}
        )
    `);

    if (typeof dbAll === "function") {
        await ensureTableColumns(dbRun, dbAll, "device_capabilities", addableColumns(DEVICE_CAPABILITIES_COLUMNS));
        await ensureTableColumns(dbRun, dbAll, "command_queue", addableColumns(COMMAND_QUEUE_COLUMNS));
        await ensureUniqueIndex(dbRun, dbAll, "device_capabilities", "idx_device_capabilities_device_id_unique", ["device_id"]);
        await ensureUniqueIndex(dbRun, dbAll, "command_queue", "idx_command_queue_command_id_unique", ["command_id"]);
    }

    await dbRun("CREATE INDEX IF NOT EXISTS idx_command_queue_device_status ON command_queue(device_id,status,id)");
    await dbRun("CREATE INDEX IF NOT EXISTS idx_command_queue_gateway_device_status ON command_queue(gateway_id,device_id,status,id)");
    await dbRun("CREATE INDEX IF NOT EXISTS idx_command_queue_command_id ON command_queue(command_id)");
}

module.exports = {
    ensureCommandTables
};
