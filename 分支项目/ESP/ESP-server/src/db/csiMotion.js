const {
    ensureTableColumns
} = require("./migrations");

const CSI_MOTION_COLUMNS = [
    { name: "id", type: "INTEGER PRIMARY KEY AUTOINCREMENT" },
    { name: "device_id", type: "TEXT NOT NULL" },
    { name: "link_id", type: "TEXT NOT NULL" },
    { name: "state", type: "TEXT NOT NULL" },
    { name: "frame_energy", type: "REAL" },
    { name: "variance", type: "REAL" },
    { name: "rssi", type: "INTEGER" },
    { name: "motion_score", type: "REAL" },
    { name: "confidence", type: "REAL" },
    { name: "timestamp", type: "INTEGER NOT NULL" },
    { name: "gateway_id", type: "TEXT" },
    { name: "server_recv_ms", type: "INTEGER NOT NULL" },
    { name: "server_time_iso", type: "TEXT NOT NULL" },
    { name: "raw_json", type: "TEXT" },
    { name: "created_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))", addType: "TEXT" },
    { name: "updated_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))", addType: "TEXT" }
];

function columnSql(columns) {
    return columns.map(column => `${column.name} ${column.type}`).join(",\n            ");
}

function addableColumns(columns) {
    return columns.filter(column => !column.type.includes("PRIMARY KEY"));
}

async function ensureCsiMotionTables(dbRun, dbAll) {
    await dbRun(`
        CREATE TABLE IF NOT EXISTS csi_motion_events (
            ${columnSql(CSI_MOTION_COLUMNS)}
        )
    `);

    if (typeof dbAll === "function") {
        await ensureTableColumns(dbRun, dbAll, "csi_motion_events", addableColumns(CSI_MOTION_COLUMNS));
    }

    await dbRun("CREATE INDEX IF NOT EXISTS idx_csi_motion_latest ON csi_motion_events(timestamp DESC, id DESC)");
    await dbRun("CREATE INDEX IF NOT EXISTS idx_csi_motion_device_latest ON csi_motion_events(device_id, timestamp DESC, id DESC)");
    await dbRun("CREATE INDEX IF NOT EXISTS idx_csi_motion_link_latest ON csi_motion_events(link_id, timestamp DESC, id DESC)");
}

async function insertCsiMotionEvent(dbRun, record) {
    const result = await dbRun(
        `INSERT INTO csi_motion_events
        (device_id,link_id,state,frame_energy,variance,rssi,motion_score,confidence,timestamp,gateway_id,server_recv_ms,server_time_iso,raw_json,created_at,updated_at)
        VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)`,
        [
            record.device_id,
            record.link_id,
            record.state,
            record.frame_energy,
            record.variance,
            record.rssi,
            record.motion_score,
            record.confidence,
            record.timestamp,
            record.gateway_id,
            record.server_recv_ms,
            record.server_time_iso,
            JSON.stringify(record.raw_json || null),
            record.server_time_iso,
            record.server_time_iso
        ]
    );
    return result?.lastID || null;
}

async function readLatestCsiMotionEvents(dbAll, { device_id = "", limit = 50 } = {}) {
    const cappedLimit = Math.max(1, Math.min(Number.parseInt(limit, 10) || 50, 500));
    const params = [];
    let where = "";
    if (device_id) {
        where = "WHERE device_id=?";
        params.push(device_id);
    }
    params.push(cappedLimit);
    return dbAll(
        `SELECT * FROM (
            SELECT * FROM csi_motion_events
            ${where}
            ORDER BY timestamp DESC, id DESC
            LIMIT ?
        ) ORDER BY timestamp ASC, id ASC`,
        params
    );
}

module.exports = {
    ensureCsiMotionTables,
    insertCsiMotionEvent,
    readLatestCsiMotionEvents
};
