const WEATHER_CONTEXT_COLUMNS = [
    { name: "scope_key", type: "TEXT PRIMARY KEY" },
    { name: "location_updated_at", type: "TEXT" },
    { name: "provider", type: "TEXT NOT NULL DEFAULT 'openweather'" },
    { name: "available", type: "INTEGER NOT NULL DEFAULT 0" },
    { name: "observed_at_ms", type: "INTEGER" },
    { name: "fetched_at_ms", type: "INTEGER NOT NULL DEFAULT 0" },
    { name: "expires_at_ms", type: "INTEGER NOT NULL DEFAULT 0" },
    { name: "condition_code", type: "INTEGER" },
    { name: "condition_key", type: "TEXT" },
    { name: "temperature_c", type: "REAL" },
    { name: "feels_like_c", type: "REAL" },
    { name: "humidity_percent", type: "INTEGER" },
    { name: "wind_speed_mps", type: "REAL" },
    { name: "precipitation_probability", type: "REAL" },
    { name: "sunrise_at_ms", type: "INTEGER" },
    { name: "sunset_at_ms", type: "INTEGER" },
    { name: "forecast_json", type: "TEXT NOT NULL DEFAULT '[]'" },
    { name: "last_error_code", type: "TEXT" },
    { name: "last_error_at_ms", type: "INTEGER" },
    { name: "updated_at_ms", type: "INTEGER NOT NULL DEFAULT 0" }
];

function columnSql(columns) {
    return columns.map(column => `${column.name} ${column.type}`).join(",\n            ");
}

async function ensureWeatherContextTables(dbRun) {
    await dbRun(`
        CREATE TABLE IF NOT EXISTS weather_context (
            ${columnSql(WEATHER_CONTEXT_COLUMNS)}
        )
    `);
    await dbRun(`
        CREATE TABLE IF NOT EXISTS weather_refresh_requests (
            request_id TEXT PRIMARY KEY,
            gateway_id TEXT NOT NULL,
            reason TEXT NOT NULL,
            received_at_ms INTEGER NOT NULL,
            outcome TEXT NOT NULL,
            context_updated_at_ms INTEGER,
            error_code TEXT
        )
    `);
    await dbRun(`
        CREATE INDEX IF NOT EXISTS idx_weather_refresh_requests_received
        ON weather_refresh_requests(received_at_ms)
    `);
}

module.exports = {
    ensureWeatherContextTables
};
