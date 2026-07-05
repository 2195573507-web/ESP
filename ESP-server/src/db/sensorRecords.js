const {
    ensureTableColumns
} = require("./migrations");

const SENSOR_RECORD_COLUMNS = [
    { name: "id", type: "INTEGER PRIMARY KEY AUTOINCREMENT" },
    { name: "timestamp", type: "INTEGER" },
    { name: "temperature", type: "REAL" },
    { name: "humidity", type: "REAL" },
    { name: "pressure", type: "REAL" },
    { name: "gas_resistance", type: "REAL" },
    { name: "device_id", type: "TEXT" },
    { name: "esp_time_ms", type: "INTEGER" },
    { name: "esp_uptime_ms", type: "INTEGER" },
    { name: "server_recv_ms", type: "INTEGER" },
    { name: "server_time_iso", type: "TEXT" },
    { name: "upload_delay_ms", type: "INTEGER" },
    { name: "schema_version", type: "INTEGER" },
    { name: "device_type", type: "TEXT" },
    { name: "firmware_version", type: "TEXT" },
    { name: "request_seq", type: "INTEGER" },
    { name: "time_synced", type: "INTEGER" },
    { name: "payload_type", type: "TEXT" },
    { name: "raw_payload", type: "TEXT" },
    { name: "payload_json", type: "TEXT" },
    { name: "sensor_id", type: "TEXT" },
    { name: "metadata_json", type: "TEXT" },
    { name: "raw_json", type: "TEXT" },
    { name: "air_quality_json", type: "TEXT" },
    { name: "air_quality_score", type: "INTEGER" },
    { name: "air_quality_level", type: "TEXT" },
    { name: "air_quality_confidence", type: "TEXT" },
    { name: "air_quality_algo_version", type: "TEXT" },
    { name: "air_quality_source", type: "TEXT" },
    { name: "gas_baseline_ohm", type: "REAL" },
    { name: "gas_ratio", type: "REAL" },
    { name: "gas_score", type: "INTEGER" },
    { name: "humidity_score", type: "INTEGER" }
];

function columnSql(columns) {
    return columns.map(column => `${column.name} ${column.type}`).join(",\n            ");
}

function addableColumns(columns) {
    return columns.filter(column => !column.type.includes("PRIMARY KEY"));
}

async function ensureSensorTimingColumns(dbRun, dbAll, logger = console) {
    await dbRun(`
        CREATE TABLE IF NOT EXISTS sensor_records (
            ${columnSql(SENSOR_RECORD_COLUMNS)}
        )
    `);

    if (typeof dbAll !== "function") {
        return;
    }

    const columns = await dbAll("PRAGMA table_info(sensor_records)");
    const existingNames = new Set(columns.map(column => column.name));
    await ensureTableColumns(dbRun, dbAll, "sensor_records", addableColumns(SENSOR_RECORD_COLUMNS));

    for (const column of SENSOR_RECORD_COLUMNS) {
        if (!column.type.includes("PRIMARY KEY") && !existingNames.has(column.name)) {
            logger.log(`[db] sensor_records added column ${column.name}`);
        }
    }

    await dbRun(
        `CREATE INDEX IF NOT EXISTS idx_sensor_records_device_recv_ms
        ON sensor_records(device_id, server_recv_ms DESC)`
    );
    await dbRun(
        `CREATE INDEX IF NOT EXISTS idx_sensor_records_recv_ms
        ON sensor_records(server_recv_ms DESC)`
    );
    await dbRun(
        `CREATE INDEX IF NOT EXISTS idx_sensor_records_payload_type_recv_ms
        ON sensor_records(payload_type, server_recv_ms DESC)`
    );
}

module.exports = {
    ensureSensorTimingColumns
};
