const assert = require("assert");
const sqlite3 = require("sqlite3").verbose();
const { ensureHomeAiTables } = require("../src/db/homeAi");
const {
    createHomeAiDataScheduler,
    ensureHomeAiDataTables,
    listMaintenanceRuns,
    readRetentionPolicy,
    runHomeAiMaintenance,
    writeRetentionPolicy
} = require("../src/jobs/homeAiDataJobs");

function helpers(db) {
    return {
        dbRun(sql, params = []) {
            return new Promise((resolve, reject) => db.run(sql, params, function (error) {
                if (error) reject(error);
                else resolve(this);
            }));
        },
        dbAll(sql, params = []) {
            return new Promise((resolve, reject) => db.all(sql, params, (error, rows) => {
                if (error) reject(error);
                else resolve(rows);
            }));
        }
    };
}

function closeDatabase(db) {
    return new Promise((resolve, reject) => db.close(error => error ? reject(error) : resolve()));
}

(async () => {
    const db = new sqlite3.Database(":memory:");
    const { dbRun, dbAll } = helpers(db);
    await ensureHomeAiTables(dbRun, dbAll);
    await ensureHomeAiDataTables(dbRun);
    await dbRun(`
        CREATE TABLE sensor_records (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp INTEGER,
            server_recv_ms INTEGER,
            device_id TEXT,
            temperature REAL,
            humidity REAL,
            pressure REAL,
            gas_resistance REAL,
            air_quality_score REAL,
            payload_type TEXT
        )
    `);

    const nowMs = Date.now();
    const oldTimestamp = nowMs - 100 * 24 * 60 * 60 * 1000;
    const recentTimestamp = nowMs - 2 * 60 * 60 * 1000;
    await dbRun(
        `INSERT INTO sensor_records
         (timestamp,server_recv_ms,device_id,temperature,humidity,pressure,gas_resistance,air_quality_score,payload_type)
         VALUES(?,?,?,?,?,?,?,?,?)`,
        [oldTimestamp, oldTimestamp, "sensair_shuttle_01", 21, 50, 1000, 12000, 80, "sensor.bme690"]
    );
    await dbRun(
        `INSERT INTO sensor_records
         (timestamp,server_recv_ms,device_id,temperature,humidity,pressure,gas_resistance,air_quality_score,payload_type)
         VALUES(?,?,?,?,?,?,?,?,?)`,
        [recentTimestamp, recentTimestamp, "sensair_shuttle_01", 24, 60, 1002, 14000, 90, "sensor.bme690"]
    );
    await dbRun(
        `INSERT INTO sensor_records
         (timestamp,server_recv_ms,device_id,payload_type)
         VALUES(?,?,?,?)`,
        [oldTimestamp, oldTimestamp, "sensair_s3_gateway_01", "csi.motion"]
    );
    await dbRun(
        `INSERT INTO home_ai_events
         (event_id,gateway_id,room_id,event_type,priority,occurred_at_ms,received_at_ms,payload_json)
         VALUES(?,?,?,?,?,?,?,?)`,
        ["retention_room_state", "gateway_01", "bedroom_01", "room_state", 400, oldTimestamp, oldTimestamp, "{}"]
    );

    await writeRetentionPolicy(dbRun, {
        environment_raw_days: 90,
        radar_coordinate_days: 7,
        capacity_warning_mb: 64,
        capacity_warning_percent: 80
    });
    const policy = await readRetentionPolicy(dbAll);
    assert.equal(policy.environment_raw_days, 90);
    assert.equal(policy.radar_coordinate_days, 7);

    const logger = { warn() {}, error() {} };
    const result = await runHomeAiMaintenance({ dbRun, dbAll, logger, now_ms: nowMs });
    assert.equal(result.ok, true);
    assert(result.result.aggregate.aggregate_count >= 4);
    assert.equal(result.result.retention.deleted.sensor_records, 1);
    assert.equal(result.result.retention.radar_coordinate_rows_deleted, 0);
    assert.equal((await dbAll("SELECT COUNT(*) AS count FROM sensor_records"))[0].count, 2);
    assert.equal((await dbAll("SELECT COUNT(*) AS count FROM sensor_records WHERE payload_type='csi.motion'"))[0].count, 1);
    assert.equal((await dbAll("SELECT COUNT(*) AS count FROM home_ai_events WHERE event_id='retention_room_state'"))[0].count, 1);
    const oldAggregate = await dbAll(
        `SELECT * FROM home_ai_data_aggregates
         WHERE source_kind='environment' AND period='day' AND bucket_start_ms<=?`,
        [oldTimestamp]
    );
    assert.equal(oldAggregate.length, 1);
    assert.equal(oldAggregate[0].sample_count, 1);
    assert.equal((await listMaintenanceRuns(dbAll, { limit: 5 }))[0].status, "SUCCEEDED");

    let retentionCalled = false;
    const failed = await runHomeAiMaintenance({
        dbRun,
        dbAll,
        logger,
        now_ms: nowMs + 1,
        aggregateImpl: async () => { throw new Error("forced aggregation failure"); },
        retentionImpl: async () => {
            retentionCalled = true;
            return {};
        }
    });
    assert.equal(failed.ok, false);
    assert.equal(failed.status, "FAILED");
    assert.equal(retentionCalled, false);
    assert.equal((await listMaintenanceRuns(dbAll, { limit: 5 }))[0].status, "FAILED");

    const scheduler = createHomeAiDataScheduler({ dbRun, dbAll, logger, interval_ms: 60000 });
    scheduler.start();
    assert.equal(scheduler.isRunning(), true);
    await scheduler.stop();
    assert.equal(scheduler.isRunning(), false);

    await closeDatabase(db);
    process.stdout.write("home ai data jobs tests: PASS\n");
})().catch(error => {
    console.error(error);
    process.exitCode = 1;
});
