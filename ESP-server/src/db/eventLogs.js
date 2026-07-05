const crypto = require("crypto");
const {
    ensureTableColumns
} = require("./migrations");

const EVENT_LOG_COLUMNS = [
    { name: "id", type: "INTEGER PRIMARY KEY AUTOINCREMENT" },
    { name: "event_id", type: "TEXT NOT NULL UNIQUE" },
    { name: "event_type", type: "TEXT NOT NULL" },
    { name: "event_name", type: "TEXT NOT NULL" },
    { name: "gateway_id", type: "TEXT" },
    { name: "device_id", type: "TEXT" },
    { name: "severity", type: "TEXT NOT NULL DEFAULT 'info'", addType: "TEXT" },
    { name: "message", type: "TEXT" },
    { name: "payload_json", type: "TEXT NOT NULL DEFAULT '{}'", addType: "TEXT" },
    { name: "source", type: "TEXT" },
    { name: "server_recv_ms", type: "INTEGER" },
    { name: "created_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))", addType: "TEXT" },
    { name: "updated_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))", addType: "TEXT" }
];

function columnSql(columns) {
    return columns.map(column => `${column.name} ${column.type}`).join(",\n            ");
}

function addableColumns(columns) {
    return columns.filter(column => !column.type.includes("PRIMARY KEY"));
}

function makeEventId(prefix = "evt") {
    const safePrefix = String(prefix || "evt").replace(/[^a-z0-9_:-]/ig, "_").slice(0, 24) || "evt";
    if (typeof crypto.randomUUID === "function") {
        return `${safePrefix}_${crypto.randomUUID()}`;
    }

    return `${safePrefix}_${crypto.randomBytes(16).toString("hex")}`;
}

async function ensureEventLogTables(dbRun, dbAll) {
    await dbRun(`
        CREATE TABLE IF NOT EXISTS event_logs (
            ${columnSql(EVENT_LOG_COLUMNS)}
        )
    `);

    if (typeof dbAll === "function") {
        await ensureTableColumns(dbRun, dbAll, "event_logs", addableColumns(EVENT_LOG_COLUMNS));
    }

    await dbRun("CREATE UNIQUE INDEX IF NOT EXISTS idx_event_logs_event_id ON event_logs(event_id)");
    await dbRun("CREATE INDEX IF NOT EXISTS idx_event_logs_type_created ON event_logs(event_type, id DESC)");
    await dbRun("CREATE INDEX IF NOT EXISTS idx_event_logs_gateway_created ON event_logs(gateway_id, id DESC)");
    await dbRun("CREATE INDEX IF NOT EXISTS idx_event_logs_device_created ON event_logs(device_id, id DESC)");
    await dbRun("CREATE INDEX IF NOT EXISTS idx_event_logs_recv_ms ON event_logs(server_recv_ms DESC)");
}

module.exports = {
    ensureEventLogTables,
    makeEventId
};
