#!/usr/bin/env node

const assert = require("assert");
const fs = require("fs");
const os = require("os");
const path = require("path");
const {
    configureDatabase,
    createDatabase,
    createDbHelpers
} = require("../src/db/sqlite");
const {
    ensureSensorTimingColumns
} = require("../src/db/sensorRecords");
const {
    ensureDeviceStatusTables
} = require("../src/db/deviceStatus");
const {
    ensureDashboardSnapshotTables
} = require("../src/db/dashboardSnapshots");
const {
    ensureEventLogTables
} = require("../src/db/eventLogs");
const {
    ensureGatewayAuthTables
} = require("../src/db/gatewayAuth");
const {
    ensureUserDataDeletionTables
} = require("../src/db/userDataDeletion");
const {
    prepareDashboardSnapshot,
    persistDashboardSnapshot,
    readDashboardSensorLatest
} = require("../src/services/dashboardService");
const {
    readDeviceStatuses
} = require("../src/services/deviceStatusService");
const {
    clearPersistenceQueue,
    enqueuePersistenceJob
} = require("../src/services/persistenceQueue");
const {
    createPersistenceWorker
} = require("../src/services/persistenceWorker");
const {
    createDashboardRouter
} = require("../src/routes/dashboardRoutes");
const {
    createDeviceRouter
} = require("../src/routes/deviceRoutes");

const REQUIRED_STATUS_COLUMNS = [
    "status_source",
    "child_status",
    "child_last_seen_ms",
    "server_received_ms",
    "link_lost",
    "voice_busy"
];

function closeDatabase(db) {
    return new Promise((resolve, reject) => {
        db.close(error => error ? reject(error) : resolve());
    });
}

function makeTempDatabase() {
    const directory = fs.mkdtempSync(path.join(os.tmpdir(), "esp-server-device-status-"));
    const databasePath = path.join(directory, "test.sqlite");
    const previousPath = process.env.ESP_SERVER_DB_PATH;
    process.env.ESP_SERVER_DB_PATH = databasePath;
    const db = createDatabase(path.resolve(__dirname, ".."));
    return {
        db,
        databasePath,
        directory,
        previousPath,
        ...createDbHelpers(db)
    };
}

async function disposeTempDatabase(context) {
    await closeDatabase(context.db);
    if (context.previousPath === undefined) {
        delete process.env.ESP_SERVER_DB_PATH;
    } else {
        process.env.ESP_SERVER_DB_PATH = context.previousPath;
    }
    fs.rmSync(context.directory, { recursive: true, force: true });
}

async function ensureTestSchema(context) {
    await configureDatabase(context.dbRun);
    await ensureSensorTimingColumns(context.dbRun, context.dbAll);
    await ensureDeviceStatusTables(context.dbRun, context.dbAll);
    await ensureDashboardSnapshotTables(context.dbRun, context.dbAll);
    await ensureGatewayAuthTables(context.dbRun, context.dbAll);
    await ensureEventLogTables(context.dbRun, context.dbAll);
    await ensureUserDataDeletionTables(context.dbRun, context.dbAll);
}

function bmeEnvelope(deviceId, serverRecvMs) {
    return {
        schema_version: 1,
        payload_type: "sensor.bme690",
        device_id: deviceId,
        esp_time_ms: serverRecvMs,
        payload: {
            temperature_c: 24.5,
            humidity_percent: 48.2,
            pressure_hpa: 1008.1,
            gas_resistance_ohm: 12000,
            sensor_id: "bme690_01"
        }
    };
}

function dashboardEnvelope(devices, serverRecvMs) {
    return {
        schema_version: 2,
        payload_type: "gateway.dashboard_snapshot",
        gateway: {
            gateway_id: "sensair_s3_gateway_01",
            online: true,
            timestamp: serverRecvMs
        },
        devices
    };
}

async function testOldSchemaMigration() {
    const context = makeTempDatabase();
    try {
        await context.dbRun("CREATE TABLE device_status (device_id TEXT NOT NULL, online INTEGER NOT NULL DEFAULT 0)");
        await ensureDeviceStatusTables(context.dbRun, context.dbAll);
        const first = await context.dbAll("PRAGMA table_info(device_status)");
        const firstNames = new Set(first.map(column => column.name));
        for (const column of REQUIRED_STATUS_COLUMNS) {
            assert.ok(firstNames.has(column), `missing migrated column ${column}`);
        }

        await ensureDeviceStatusTables(context.dbRun, context.dbAll);
        const second = await context.dbAll("PRAGMA table_info(device_status)");
        assert.deepEqual(second.map(column => column.name), first.map(column => column.name));
    } finally {
        await disposeTempDatabase(context);
    }
}

async function queueSnapshot(worker, context, body, serverRecvMs) {
    const prepared = prepareDashboardSnapshot(body, {
        trustedGatewayId: "sensair_s3_gateway_01",
        serverRecvMs
    });
    assert.equal(prepared.ok, true, prepared.error);
    enqueuePersistenceJob({
        type: "gateway.dashboard_snapshot",
        priority: "high",
        snapshot_id: prepared.snapshotId,
        run: () => persistDashboardSnapshot(context.dbRun, context.dbAll, prepared)
    });
    await worker.waitForIdle();
    return prepared;
}

function invokeRouter(router, request) {
    return new Promise((resolve, reject) => {
        const response = {
            statusCode: 200,
            status(code) {
                this.statusCode = code;
                return this;
            },
            json(body) {
                this.body = body;
                resolve(this);
                return this;
            }
        };
        router.handle(request, response, error => error ? reject(error) : resolve(response));
    });
}

async function testSnapshotBmeAndStatusSemantics() {
    const context = makeTempDatabase();
    clearPersistenceQueue();
    try {
        await ensureTestSchema(context);
        const workerLogs = [];
        const worker = createPersistenceWorker({
            dbRun: context.dbRun,
            logger: {
                info: message => workerLogs.push(message),
                warn: message => workerLogs.push(message),
                error: message => workerLogs.push(message)
            }
        });
        const now = Date.now();
        const snapshotRouter = createDashboardRouter({
            dbRun: context.dbRun,
            dbAll: context.dbAll,
            persistenceWorker: worker,
            runtimeCache: {
                updateDashboardSnapshot() {}
            },
            logger: console
        });
        const routeResponse = await invokeRouter(snapshotRouter, {
            method: "POST",
            url: "/snapshot",
            query: {},
            headers: {
                "x-gateway-id": "sensair_s3_gateway_01"
            },
            body: dashboardEnvelope([
            {
                device_id: "C51",
                online: true,
                status: "online",
                child_last_seen_ms: now - 10,
                link_lost: false,
                voice_busy: false
            },
            {
                device_id: "C52",
                online: false,
                status: "voice_busy",
                child_last_seen_ms: now - 20,
                link_lost: false,
                voice_busy: true
            }
            ], now)
        });
        assert.equal(routeResponse.statusCode, 202);
        assert.equal(routeResponse.body.ok, true);
        await worker.waitForIdle();

        const snapshots = await context.dbAll("SELECT COUNT(*) AS count FROM dashboard_snapshots");
        assert.equal(snapshots[0].count, 1);
        const statuses = await context.dbAll(
            `SELECT device_id,online,child_status,child_last_seen_ms,link_lost,voice_busy
             FROM device_status
             WHERE device_id IN ('sensair_shuttle_01', 'sensair_shuttle_02')
             ORDER BY device_id`
        );
        assert.deepEqual(statuses.map(row => row.device_id), ["sensair_shuttle_01", "sensair_shuttle_02"]);
        assert.deepEqual(statuses.map(row => Number(row.online)), [1, 1]);
        assert.deepEqual(statuses.map(row => row.child_status), ["online", "voice_busy"]);
        assert.deepEqual(statuses.map(row => Number(row.voice_busy)), [0, 1]);
        assert.ok(statuses.every(row => row.child_last_seen_ms !== null));
        assert.ok(statuses.every(row => Number(row.link_lost) === 0));
        assert.ok(workerLogs.some(message => message.includes("[persistence_worker] persisted=")));

        const deviceRouter = createDeviceRouter({
            dbRun: context.dbRun,
            dbAll: context.dbAll,
            persistenceWorker: worker,
            runtimeCache: {
                updateBmeSensor() {}
            },
            logger: console
        });
        const c51 = await invokeRouter(deviceRouter, {
            method: "POST",
            url: "/api/device/v1/ingest",
            query: {},
            headers: { "x-gateway-id": "sensair_s3_gateway_01" },
            body: bmeEnvelope("sensair_shuttle_01", now + 1)
        });
        const c52 = await invokeRouter(deviceRouter, {
            method: "POST",
            url: "/api/device/v1/ingest",
            query: {},
            headers: { "x-gateway-id": "sensair_s3_gateway_01" },
            body: bmeEnvelope("sensair_shuttle_02", now + 2)
        });
        assert.equal(c51.statusCode, 201);
        assert.equal(c52.statusCode, 201);
        assert.equal(c51.body.data.device_id, "sensair_shuttle_01");
        assert.equal(c52.body.data.device_id, "sensair_shuttle_02");
        await worker.waitForIdle();
        const sensors = await context.dbAll("SELECT device_id FROM sensor_records ORDER BY id");
        assert.deepEqual(sensors.map(row => row.device_id), ["sensair_shuttle_01", "sensair_shuttle_02"]);

        const beforeInvalid = sensors.length;
        const missing = await invokeRouter(deviceRouter, {
            method: "POST",
            url: "/api/device/v1/ingest",
            query: {},
            headers: { "x-gateway-id": "sensair_s3_gateway_01" },
            body: { ...bmeEnvelope("", now + 3), device_id: "" }
        });
        const invalid = await invokeRouter(deviceRouter, {
            method: "POST",
            url: "/api/device/v1/ingest",
            query: {},
            headers: { "x-gateway-id": "sensair_s3_gateway_01" },
            body: bmeEnvelope("unknown", now + 4)
        });
        assert.equal(missing.statusCode, 400);
        assert.equal(invalid.statusCode, 400);
        assert.equal(invalid.body.error.code, "DEVICE_ID_NOT_ALLOWED");
        const afterInvalid = await context.dbAll("SELECT COUNT(*) AS count FROM sensor_records");
        assert.equal(afterInvalid[0].count, beforeInvalid);

        const beforeFailure = afterInvalid[0].count;
        const faultDbRun = (sql, params) => {
            if (/^UPDATE device_status/i.test(String(sql).trim())) {
                return Promise.reject(new Error("simulated device_status failure"));
            }
            return context.dbRun(sql, params);
        };
        const faultRouter = createDeviceRouter({
            dbRun: faultDbRun,
            dbAll: context.dbAll,
            runtimeCache: { updateBmeSensor() {} },
            logger: { log() {}, error() {}, warn() {} }
        });
        const failure = await invokeRouter(faultRouter, {
            method: "POST",
            url: "/api/device/v1/ingest",
            query: {},
            headers: { "x-gateway-id": "sensair_s3_gateway_01" },
            body: bmeEnvelope("sensair_shuttle_01", now + 5)
        });
        assert.equal(failure.statusCode, 500);
        assert.equal(failure.body.ok, false);
        const afterFailure = await context.dbAll("SELECT COUNT(*) AS count FROM sensor_records");
        assert.equal(afterFailure[0].count, beforeFailure);

        const unknown = await readDeviceStatuses(context.dbAll, {
            device_id: "sensair_shuttle_02_missing"
        }, now + 6);
        assert.equal(unknown[0].online, null);
        assert.equal(unknown[0].status_source, "not_observed");

        await context.dbRun(
            "INSERT INTO sensor_records (timestamp,device_id,payload_type) VALUES(?,?,?)",
            [now + 6, "not_observed_c5", "sensor.bme690"]
        );
        const sensorWithoutStatus = await readDashboardSensorLatest(context.dbAll, {
            device_id: "not_observed_c5"
        });
        assert.equal(sensorWithoutStatus.online, null);
        assert.equal(sensorWithoutStatus.status_source, "not_observed");

        await queueSnapshot(worker, context, dashboardEnvelope([{
            device_id: "C52",
            online: false,
            status: "offline",
            offline_reason: "link_lost",
            child_last_seen_ms: now + 7,
            link_lost: true,
            voice_busy: false
        }], now + 7), now + 7);
        const offline = await readDeviceStatuses(context.dbAll, {
            device_id: "sensair_shuttle_02"
        }, now + 8);
        assert.equal(offline[0].online, false);
        assert.equal(offline[0].status, "offline");
        assert.equal(offline[0].offline_reason, "link_lost");

        clearPersistenceQueue();
        const failureLogs = [];
        const failingWorker = createPersistenceWorker({
            dbRun: context.dbRun,
            logger: {
                info: message => failureLogs.push(message),
                warn: message => failureLogs.push(message),
                error: message => failureLogs.push(message)
            }
        });
        enqueuePersistenceJob({
            type: "gateway.dashboard_snapshot",
            priority: "high",
            snapshot_id: "failure_test",
            run: () => Promise.reject(new Error("simulated snapshot persistence failure"))
        });
        await assert.rejects(
            () => failingWorker.waitForIdle(),
            /simulated snapshot persistence failure/
        );
        assert.ok(failureLogs.some(message => message.includes("PERSISTENCE_JOB_FAILED")));
        assert.equal(failingWorker.getLastFailure().jobs[0].snapshot_id, "failure_test");
    } finally {
        clearPersistenceQueue();
        await disposeTempDatabase(context);
    }
}

async function main() {
    await testOldSchemaMigration();
    await testSnapshotBmeAndStatusSemantics();
    console.log("device-status-chain tests passed (temporary SQLite only)");
}

main().catch(error => {
    console.error(error?.stack || error);
    process.exitCode = 1;
});
