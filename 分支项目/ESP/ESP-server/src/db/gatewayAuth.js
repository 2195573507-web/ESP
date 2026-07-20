const {
    ensureTableColumns,
    ensureUniqueIndex
} = require("./migrations");

const GATEWAY_AUTH_COLUMNS = [
    { name: "id", type: "INTEGER PRIMARY KEY AUTOINCREMENT" },
    { name: "gateway_id", type: "TEXT NOT NULL UNIQUE" },
    { name: "token_hash", type: "TEXT" },
    { name: "label", type: "TEXT" },
    { name: "enabled", type: "INTEGER NOT NULL DEFAULT 1", addType: "INTEGER" },
    { name: "last_seen_ms", type: "INTEGER" },
    { name: "last_seen_at", type: "TEXT" },
    { name: "created_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))", addType: "TEXT" },
    { name: "updated_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))", addType: "TEXT" }
];

const GATEWAY_DEVICE_BINDING_COLUMNS = [
    { name: "id", type: "INTEGER PRIMARY KEY AUTOINCREMENT" },
    { name: "gateway_id", type: "TEXT NOT NULL" },
    { name: "device_id", type: "TEXT NOT NULL" },
    { name: "source", type: "TEXT" },
    { name: "last_seen_ms", type: "INTEGER" },
    { name: "last_seen_at", type: "TEXT" },
    { name: "created_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))", addType: "TEXT" },
    { name: "updated_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))", addType: "TEXT" }
];

function columnSql(columns) {
    return columns.map(column => `${column.name} ${column.type}`).join(",\n            ");
}

function addableColumns(columns) {
    return columns.filter(column => !column.type.includes("PRIMARY KEY"));
}

async function ensureGatewayAuthTables(dbRun, dbAll) {
    await dbRun(`
        CREATE TABLE IF NOT EXISTS gateway_auth (
            ${columnSql(GATEWAY_AUTH_COLUMNS)}
        )
    `);
    await dbRun(`
        CREATE TABLE IF NOT EXISTS gateway_device_bindings (
            ${columnSql(GATEWAY_DEVICE_BINDING_COLUMNS)}
        )
    `);

    if (typeof dbAll === "function") {
        await ensureTableColumns(dbRun, dbAll, "gateway_auth", addableColumns(GATEWAY_AUTH_COLUMNS));
        await ensureTableColumns(dbRun, dbAll, "gateway_device_bindings", addableColumns(GATEWAY_DEVICE_BINDING_COLUMNS));
        await ensureUniqueIndex(dbRun, dbAll, "gateway_auth", "idx_gateway_auth_gateway_id_unique", ["gateway_id"]);
        await ensureUniqueIndex(dbRun, dbAll, "gateway_device_bindings", "idx_gateway_device_bindings_gateway_device_unique", ["gateway_id", "device_id"]);
    }

    await dbRun("CREATE INDEX IF NOT EXISTS idx_gateway_device_bindings_device ON gateway_device_bindings(device_id)");
    await dbRun("CREATE INDEX IF NOT EXISTS idx_gateway_device_bindings_gateway_seen ON gateway_device_bindings(gateway_id,last_seen_ms DESC)");
}

module.exports = {
    ensureGatewayAuthTables
};
