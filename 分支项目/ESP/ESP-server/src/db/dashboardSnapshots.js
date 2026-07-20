const crypto = require("crypto");
const {
    ensureTableColumns
} = require("./migrations");

const DASHBOARD_SNAPSHOT_COLUMNS = [
    { name: "id", type: "INTEGER PRIMARY KEY AUTOINCREMENT" },
    { name: "snapshot_id", type: "TEXT NOT NULL UNIQUE" },
    { name: "gateway_id", type: "TEXT NOT NULL" },
    { name: "server_recv_ms", type: "INTEGER NOT NULL" },
    { name: "payload_json", type: "TEXT NOT NULL" },
    { name: "schema_version", type: "INTEGER" },
    { name: "created_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))", addType: "TEXT" },
    { name: "updated_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))", addType: "TEXT" }
];

function columnSql(columns) {
    return columns.map(column => `${column.name} ${column.type}`).join(",\n            ");
}

function addableColumns(columns) {
    return columns.filter(column => !column.type.includes("PRIMARY KEY"));
}

function makeSnapshotId(gatewayId, serverRecvMs) {
    const safeGateway = String(gatewayId || "gateway").replace(/[^a-z0-9_-]/ig, "_").slice(0, 48) || "gateway";
    const suffix = typeof crypto.randomUUID === "function"
        ? crypto.randomUUID()
        : crypto.randomBytes(12).toString("hex");
    return `${safeGateway}_${Math.trunc(serverRecvMs || Date.now())}_${suffix}`;
}

async function ensureDashboardSnapshotTables(dbRun, dbAll) {
    await dbRun(`
        CREATE TABLE IF NOT EXISTS dashboard_snapshots (
            ${columnSql(DASHBOARD_SNAPSHOT_COLUMNS)}
        )
    `);

    if (typeof dbAll === "function") {
        await ensureTableColumns(dbRun, dbAll, "dashboard_snapshots", addableColumns(DASHBOARD_SNAPSHOT_COLUMNS));
    }

    await dbRun("CREATE UNIQUE INDEX IF NOT EXISTS idx_dashboard_snapshots_snapshot_id ON dashboard_snapshots(snapshot_id)");
    await dbRun("CREATE INDEX IF NOT EXISTS idx_dashboard_snapshots_latest ON dashboard_snapshots(server_recv_ms DESC, id DESC)");
    await dbRun("CREATE INDEX IF NOT EXISTS idx_dashboard_snapshots_gateway_latest ON dashboard_snapshots(gateway_id, server_recv_ms DESC)");
}

module.exports = {
    ensureDashboardSnapshotTables,
    makeSnapshotId
};
