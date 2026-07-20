const {
    ensureTableColumns,
    ensureUniqueIndex
} = require("./migrations");

const DEVICE_STATUS_COLUMNS = [
    { name: "id", type: "INTEGER PRIMARY KEY AUTOINCREMENT" },
    { name: "device_id", type: "TEXT NOT NULL" },
    { name: "device_type", type: "TEXT" },
    { name: "room_id", type: "TEXT" },
    { name: "room_name", type: "TEXT" },
    { name: "firmware_version", type: "TEXT" },
    { name: "last_seen_ms", type: "INTEGER" },
    { name: "last_seen_iso", type: "TEXT" },
    { name: "last_payload_type", type: "TEXT" },
    { name: "last_server_recv_ms", type: "INTEGER" },
    { name: "last_module_type", type: "TEXT" },
    { name: "last_esp_uptime_ms", type: "INTEGER" },
    { name: "last_esp_time_ms", type: "INTEGER" },
    { name: "time_synced", type: "INTEGER" },
    { name: "online", type: "INTEGER NOT NULL DEFAULT 0", addType: "INTEGER" },
    { name: "offline_reason", type: "TEXT" },
    { name: "status_source", type: "TEXT" },
    { name: "child_status", type: "TEXT" },
    { name: "child_last_seen_ms", type: "INTEGER" },
    { name: "server_received_ms", type: "INTEGER" },
    { name: "link_lost", type: "INTEGER" },
    { name: "voice_busy", type: "INTEGER" },
    { name: "reboot_count", type: "INTEGER NOT NULL DEFAULT 0", addType: "INTEGER" },
    { name: "latest_upload_delay_ms", type: "INTEGER" },
    { name: "avg_upload_delay_ms", type: "INTEGER" },
    { name: "delay_sample_count", type: "INTEGER NOT NULL DEFAULT 0", addType: "INTEGER" },
    { name: "created_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))", addType: "TEXT" },
    { name: "updated_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))", addType: "TEXT" }
];

const DEVICE_MODULE_STATUS_COLUMNS = [
    { name: "id", type: "INTEGER PRIMARY KEY AUTOINCREMENT" },
    { name: "device_id", type: "TEXT NOT NULL" },
    { name: "module_type", type: "TEXT NOT NULL" },
    { name: "room_id", type: "TEXT" },
    { name: "room_name", type: "TEXT" },
    { name: "last_seen_ms", type: "INTEGER" },
    { name: "last_seen_iso", type: "TEXT" },
    { name: "last_payload_type", type: "TEXT" },
    { name: "last_server_recv_ms", type: "INTEGER" },
    { name: "last_esp_uptime_ms", type: "INTEGER" },
    { name: "last_esp_time_ms", type: "INTEGER" },
    { name: "time_synced", type: "INTEGER" },
    { name: "latest_upload_delay_ms", type: "INTEGER" },
    { name: "avg_upload_delay_ms", type: "INTEGER" },
    { name: "delay_sample_count", type: "INTEGER NOT NULL DEFAULT 0", addType: "INTEGER" },
    { name: "created_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))", addType: "TEXT" },
    { name: "updated_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))", addType: "TEXT" }
];

function columnSql(columns) {
    return columns.map(column => `${column.name} ${column.type}`).join(",\n            ");
}

function addableColumns(columns) {
    return columns.filter(column => !column.type.includes("PRIMARY KEY"));
}

async function ensureDeviceStatusTables(dbRun, dbAll) {
    await dbRun(`
        CREATE TABLE IF NOT EXISTS device_status (
            ${columnSql(DEVICE_STATUS_COLUMNS)}
        )
    `);
    await dbRun(`
        CREATE TABLE IF NOT EXISTS device_module_status (
            ${columnSql(DEVICE_MODULE_STATUS_COLUMNS)}
        )
    `);

    if (typeof dbAll === "function") {
        await ensureTableColumns(dbRun, dbAll, "device_status", addableColumns(DEVICE_STATUS_COLUMNS));
        await ensureTableColumns(dbRun, dbAll, "device_module_status", addableColumns(DEVICE_MODULE_STATUS_COLUMNS));
        await ensureUniqueIndex(dbRun, dbAll, "device_status", "idx_device_status_device_id_unique", ["device_id"]);
        await ensureUniqueIndex(dbRun, dbAll, "device_module_status", "idx_device_module_status_device_module_unique", ["device_id", "module_type"]);
    }

    await dbRun("CREATE INDEX IF NOT EXISTS idx_device_status_last_seen ON device_status(last_seen_ms DESC)");
    await dbRun("CREATE INDEX IF NOT EXISTS idx_device_status_online ON device_status(online,last_seen_ms DESC)");
    await dbRun("CREATE INDEX IF NOT EXISTS idx_device_module_status_last_seen ON device_module_status(device_id,last_seen_ms DESC)");
}

module.exports = {
    ensureDeviceStatusTables
};
