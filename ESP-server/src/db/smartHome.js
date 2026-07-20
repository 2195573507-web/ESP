const {
    ensureTableColumns,
    ensureUniqueIndex
} = require("./migrations");

const SMART_HOME_DEVICE_COLUMNS = [
    { name: "id", type: "INTEGER PRIMARY KEY AUTOINCREMENT" },
    { name: "device_id", type: "TEXT NOT NULL" },
    { name: "provider", type: "TEXT NOT NULL DEFAULT 'local'", addType: "TEXT" },
    { name: "gateway_id", type: "TEXT" },
    { name: "device_type", type: "TEXT NOT NULL DEFAULT 'unknown'", addType: "TEXT" },
    { name: "name", type: "TEXT" },
    { name: "room_id", type: "TEXT" },
    { name: "room_name", type: "TEXT" },
    { name: "online", type: "INTEGER NOT NULL DEFAULT 0", addType: "INTEGER" },
    { name: "state_json", type: "TEXT NOT NULL DEFAULT '{}'", addType: "TEXT" },
    { name: "created_at_ms", type: "INTEGER" },
    { name: "updated_at_ms", type: "INTEGER" }
];

const SMART_HOME_COMMAND_COLUMNS = [
    { name: "id", type: "INTEGER PRIMARY KEY AUTOINCREMENT" },
    { name: "command_id", type: "TEXT NOT NULL" },
    { name: "target_id", type: "TEXT NOT NULL" },
    { name: "gateway_id", type: "TEXT" },
    { name: "room_id", type: "TEXT" },
    { name: "room_name", type: "TEXT" },
    { name: "action", type: "TEXT NOT NULL" },
    { name: "params_json", type: "TEXT NOT NULL DEFAULT '{}'", addType: "TEXT" },
    { name: "source", type: "TEXT" },
    { name: "requested_by", type: "TEXT" },
    { name: "decision_id", type: "TEXT" },
    { name: "status", type: "TEXT NOT NULL DEFAULT 'queued'", addType: "TEXT" },
    { name: "result_json", type: "TEXT" },
    { name: "error_message", type: "TEXT" },
    { name: "created_at_ms", type: "INTEGER" },
    { name: "updated_at_ms", type: "INTEGER" },
    { name: "acknowledged_at_ms", type: "INTEGER" },
    { name: "executed_at_ms", type: "INTEGER" }
];

const NATURAL_LANGUAGE_COMMAND_COLUMNS = [
    { name: "id", type: "INTEGER PRIMARY KEY AUTOINCREMENT" },
    { name: "command_id", type: "TEXT NOT NULL" },
    { name: "text", type: "TEXT NOT NULL" },
    { name: "source", type: "TEXT" },
    { name: "room_id", type: "TEXT" },
    { name: "device_id", type: "TEXT" },
    { name: "parsed_intent_json", type: "TEXT" },
    { name: "status", type: "TEXT NOT NULL DEFAULT 'queued'", addType: "TEXT" },
    { name: "created_at_ms", type: "INTEGER" },
    { name: "updated_at_ms", type: "INTEGER" }
];

function columnSql(columns) {
    return columns.map(column => `${column.name} ${column.type}`).join(",\n            ");
}

function addableColumns(columns) {
    return columns.filter(column => !column.type.includes("PRIMARY KEY"));
}

async function ensureSmartHomeTables(dbRun, dbAll) {
    await dbRun(`
        CREATE TABLE IF NOT EXISTS smart_home_devices (
            ${columnSql(SMART_HOME_DEVICE_COLUMNS)}
        )
    `);
    await dbRun(`
        CREATE TABLE IF NOT EXISTS smart_home_commands (
            ${columnSql(SMART_HOME_COMMAND_COLUMNS)}
        )
    `);
    await dbRun(`
        CREATE TABLE IF NOT EXISTS natural_language_commands (
            ${columnSql(NATURAL_LANGUAGE_COMMAND_COLUMNS)}
        )
    `);

    if (typeof dbAll === "function") {
        await ensureTableColumns(dbRun, dbAll, "smart_home_devices", addableColumns(SMART_HOME_DEVICE_COLUMNS));
        await ensureTableColumns(dbRun, dbAll, "smart_home_commands", addableColumns(SMART_HOME_COMMAND_COLUMNS));
        await ensureTableColumns(dbRun, dbAll, "natural_language_commands", addableColumns(NATURAL_LANGUAGE_COMMAND_COLUMNS));
        await ensureUniqueIndex(dbRun, dbAll, "smart_home_devices", "idx_smart_home_devices_device_id_unique", ["device_id"]);
        await ensureUniqueIndex(dbRun, dbAll, "smart_home_commands", "idx_smart_home_commands_command_id_unique", ["command_id"]);
        await ensureUniqueIndex(dbRun, dbAll, "natural_language_commands", "idx_natural_language_commands_command_id_unique", ["command_id"]);
    }

    await dbRun("CREATE INDEX IF NOT EXISTS idx_smart_home_devices_updated ON smart_home_devices(updated_at_ms DESC)");
    await dbRun("CREATE INDEX IF NOT EXISTS idx_smart_home_commands_status ON smart_home_commands(status,id)");
    await dbRun("CREATE INDEX IF NOT EXISTS idx_smart_home_commands_gateway_status ON smart_home_commands(gateway_id,status,id)");
    await dbRun("CREATE INDEX IF NOT EXISTS idx_natural_language_commands_created ON natural_language_commands(id DESC)");
}

module.exports = {
    ensureSmartHomeTables
};
