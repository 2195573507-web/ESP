const path = require("path");
const fs = require("fs");
const sqlite3 = require("sqlite3").verbose();

function createDatabase(baseDir) {
    const configuredPath = process.env.ESP_SERVER_DB_PATH;
    const dbPath = typeof configuredPath === "string" && configuredPath.trim()
        ? path.resolve(configuredPath.trim())
        : path.join(baseDir, "db", "database.db");

    fs.mkdirSync(path.dirname(dbPath), { recursive: true });
    return new sqlite3.Database(dbPath);
}

function createDbHelpers(db) {
    function dbRun(sql, params = []) {
        return new Promise((resolve, reject) => {
            db.run(sql, params, function (err) {
                if (err) {
                    reject(err);
                    return;
                }

                resolve(this);
            });
        });
    }

    function dbAll(sql, params = []) {
        return new Promise((resolve, reject) => {
            db.all(sql, params, (err, rows) => {
                if (err) {
                    reject(err);
                    return;
                }

                resolve(rows);
            });
        });
    }

    return {
        dbRun,
        dbAll
    };
}

async function configureDatabase(dbRun) {
    if (typeof dbRun !== "function") {
        return;
    }

    await dbRun("PRAGMA journal_mode=WAL");
    await dbRun("PRAGMA synchronous=NORMAL");
    await dbRun("PRAGMA busy_timeout=5000");
}

module.exports = {
    configureDatabase,
    createDatabase,
    createDbHelpers
};
