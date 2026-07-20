const HOME_LOCATION_COLUMNS = [
    { name: "id", type: "INTEGER PRIMARY KEY CHECK (id = 1)" },
    { name: "country", type: "TEXT NOT NULL DEFAULT ''" },
    { name: "province", type: "TEXT NOT NULL DEFAULT ''" },
    { name: "city", type: "TEXT NOT NULL DEFAULT ''" },
    { name: "district", type: "TEXT NOT NULL DEFAULT ''" },
    { name: "latitude", type: "REAL" },
    { name: "longitude", type: "REAL" },
    { name: "timezone", type: "TEXT NOT NULL DEFAULT ''" },
    { name: "created_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))", addType: "TEXT" },
    { name: "updated_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))", addType: "TEXT" }
];

function columnSql(columns) {
    return columns.map(column => `${column.name} ${column.type}`).join(",\n            ");
}

async function ensureHomeLocationTables(dbRun) {
    await dbRun(`
        CREATE TABLE IF NOT EXISTS home_location (
            ${columnSql(HOME_LOCATION_COLUMNS)}
        )
    `);
}

module.exports = {
    ensureHomeLocationTables
};
