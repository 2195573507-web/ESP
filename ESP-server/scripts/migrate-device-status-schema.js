#!/usr/bin/env node

const path = require("path");
const fs = require("fs");
const sqlite3 = require("sqlite3").verbose();
const {
    configureDatabase,
    createDatabase,
    createDbHelpers
} = require("../src/db/sqlite");
const {
    ensureDeviceStatusTables
} = require("../src/db/deviceStatus");
const {
    ensureDashboardSnapshotTables
} = require("../src/db/dashboardSnapshots");

const args = new Set(process.argv.slice(2));
const dryRun = args.has("--dry-run");

function configuredDatabasePath() {
    const value = String(process.env.ESP_SERVER_DB_PATH || "").trim();
    if (!value) {
        throw new Error("ESP_SERVER_DB_PATH is required; refusing to operate on db/database.db by default");
    }

    const resolved = path.resolve(value);
    const defaultDatabase = path.resolve(__dirname, "..", "db", "database.db");
    if (resolved === defaultDatabase) {
        throw new Error("refusing to operate on ESP-server/db/database.db; provide an explicit deployment database path");
    }
    return resolved;
}

async function printColumns(dbAll, tableName, label) {
    const rows = await dbAll(`PRAGMA table_info(${tableName})`);
    const names = rows.map(row => row.name);
    console.log(`[device-status-migrate] ${label} table=${tableName} columns=${names.join(",") || "(missing)"}`);
    return rows;
}

function closeDatabase(db) {
    return new Promise((resolve, reject) => {
        db.close(error => error ? reject(error) : resolve());
    });
}

async function main() {
    const dbPath = configuredDatabasePath();
    console.log(`[device-status-migrate] db_path=${dbPath} dry_run=${dryRun}`);
    if (dryRun && !fs.existsSync(dbPath)) {
        throw new Error("dry run requires an existing ESP_SERVER_DB_PATH database file");
    }
    const db = dryRun
        ? new sqlite3.Database(dbPath, sqlite3.OPEN_READONLY)
        : createDatabase(__dirname);
    const { dbRun, dbAll } = createDbHelpers(db);
    try {
        await printColumns(dbAll, "device_status", "before");
        await printColumns(dbAll, "dashboard_snapshots", "before");
        if (dryRun) {
            console.log("[device-status-migrate] dry run complete; no schema changes applied");
            return;
        }

        await configureDatabase(dbRun);
        await ensureDeviceStatusTables(dbRun, dbAll);
        await ensureDashboardSnapshotTables(dbRun, dbAll);
        await printColumns(dbAll, "device_status", "after");
        await printColumns(dbAll, "dashboard_snapshots", "after");
        console.log("[device-status-migrate] schema ensure complete; historical sensor_records device_id values were not changed");
    } finally {
        await closeDatabase(db);
    }
}

main().catch(error => {
    console.error(`[device-status-migrate] failed ${error?.message || error}`);
    process.exitCode = 1;
});
