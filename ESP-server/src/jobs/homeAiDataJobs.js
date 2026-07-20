const crypto = require("crypto");

const RETENTION_SETTING_KEY = "retention_policy";
const HOUR_MS = 60 * 60 * 1000;
const DAY_MS = 24 * HOUR_MS;
const DEFAULT_RETENTION_POLICY = Object.freeze({
    radar_coordinate_days: 7,
    environment_raw_days: 90,
    hourly_aggregate_days: 3650,
    daily_aggregate_days: 3650,
    capacity_warning_mb: 512,
    capacity_warning_percent: 80
});

function integer(value, fallback, min, max) {
    const parsed = Number.parseInt(value, 10);
    return Number.isFinite(parsed) ? Math.min(max, Math.max(min, parsed)) : fallback;
}

function normalizeRetentionPolicy(input = {}) {
    return {
        radar_coordinate_days: integer(input.radar_coordinate_days, DEFAULT_RETENTION_POLICY.radar_coordinate_days, 1, 30),
        environment_raw_days: integer(input.environment_raw_days, DEFAULT_RETENTION_POLICY.environment_raw_days, 7, 3650),
        hourly_aggregate_days: integer(input.hourly_aggregate_days, DEFAULT_RETENTION_POLICY.hourly_aggregate_days, 30, 36500),
        daily_aggregate_days: integer(input.daily_aggregate_days, DEFAULT_RETENTION_POLICY.daily_aggregate_days, 30, 36500),
        capacity_warning_mb: integer(input.capacity_warning_mb, DEFAULT_RETENTION_POLICY.capacity_warning_mb, 64, 10240),
        capacity_warning_percent: integer(input.capacity_warning_percent, DEFAULT_RETENTION_POLICY.capacity_warning_percent, 50, 95)
    };
}

function validateRetentionPolicy(input = {}) {
    if (!input || typeof input !== "object" || Array.isArray(input)) {
        return { ok: false, code: "HOME_AI_RETENTION_INVALID", error: "retention policy must be an object" };
    }
    const ranges = {
        radar_coordinate_days: [1, 30],
        environment_raw_days: [7, 3650],
        hourly_aggregate_days: [30, 36500],
        daily_aggregate_days: [30, 36500],
        capacity_warning_mb: [64, 10240],
        capacity_warning_percent: [50, 95]
    };
    for (const [field, [min, max]] of Object.entries(ranges)) {
        if (input[field] === undefined) continue;
        const value = Number(input[field]);
        if (!Number.isInteger(value) || value < min || value > max) {
            return { ok: false, code: "HOME_AI_RETENTION_INVALID", error: `${field} is outside the supported range` };
        }
    }
    return { ok: true, policy: normalizeRetentionPolicy(input) };
}

function jsonText(value) {
    return JSON.stringify(value === undefined ? {} : value);
}

function makeId(prefix) {
    const suffix = typeof crypto.randomUUID === "function"
        ? crypto.randomUUID()
        : crypto.randomBytes(16).toString("hex");
    return `${prefix}_${suffix}`;
}

function aggregateId(period, bucketStartMs, sourceKind, sourceId, roomId) {
    return crypto.createHash("sha256")
        .update(`${period}|${bucketStartMs}|${sourceKind}|${sourceId}|${roomId}`)
        .digest("hex");
}

async function ensureHomeAiDataTables(dbRun) {
    await dbRun(`
        CREATE TABLE IF NOT EXISTS home_ai_data_aggregates (
            aggregate_id TEXT PRIMARY KEY,
            period TEXT NOT NULL,
            bucket_start_ms INTEGER NOT NULL,
            source_kind TEXT NOT NULL,
            source_id TEXT NOT NULL DEFAULT '',
            room_id TEXT NOT NULL DEFAULT '',
            sample_count INTEGER NOT NULL,
            summary_json TEXT NOT NULL,
            created_at_ms INTEGER NOT NULL,
            updated_at_ms INTEGER NOT NULL,
            UNIQUE(period,bucket_start_ms,source_kind,source_id,room_id)
        )
    `);
    await dbRun(`
        CREATE TABLE IF NOT EXISTS home_ai_maintenance_runs (
            run_id TEXT PRIMARY KEY,
            job_name TEXT NOT NULL,
            status TEXT NOT NULL,
            result_json TEXT NOT NULL,
            error_message TEXT NOT NULL DEFAULT '',
            started_at_ms INTEGER NOT NULL,
            finished_at_ms INTEGER
        )
    `);
    await dbRun("CREATE INDEX IF NOT EXISTS idx_home_ai_aggregates_period_bucket ON home_ai_data_aggregates(period,bucket_start_ms DESC)");
    await dbRun("CREATE INDEX IF NOT EXISTS idx_home_ai_maintenance_runs_job_time ON home_ai_maintenance_runs(job_name,started_at_ms DESC)");
}

async function readRetentionPolicy(dbAll) {
    const rows = await dbAll(
        "SELECT value_json FROM home_ai_tool_settings WHERE setting_key=?",
        [RETENTION_SETTING_KEY]
    );
    if (!rows[0]) return { ...DEFAULT_RETENTION_POLICY };
    try {
        return normalizeRetentionPolicy(JSON.parse(rows[0].value_json));
    } catch (_) {
        return { ...DEFAULT_RETENTION_POLICY };
    }
}

async function writeRetentionPolicy(dbRun, input = {}) {
    const policy = normalizeRetentionPolicy(input);
    const nowMs = Date.now();
    await dbRun(
        `INSERT INTO home_ai_tool_settings(setting_key,value_json,updated_at_ms)
         VALUES(?,?,?)
         ON CONFLICT(setting_key) DO UPDATE SET value_json=excluded.value_json,updated_at_ms=excluded.updated_at_ms`,
        [RETENTION_SETTING_KEY, jsonText(policy), nowMs]
    );
    return { key: RETENTION_SETTING_KEY, policy, updated_at_ms: nowMs };
}

async function tableExists(dbAll, tableName) {
    const rows = await dbAll(
        "SELECT name FROM sqlite_master WHERE type='table' AND name=?",
        [tableName]
    );
    return rows.length > 0;
}

async function upsertAggregate(dbRun, row, nowMs) {
    const id = aggregateId(row.period, row.bucket_start_ms, row.source_kind, row.source_id, row.room_id);
    await dbRun(
        `INSERT INTO home_ai_data_aggregates
         (aggregate_id,period,bucket_start_ms,source_kind,source_id,room_id,sample_count,summary_json,created_at_ms,updated_at_ms)
         VALUES(?,?,?,?,?,?,?,?,?,?)
         ON CONFLICT(period,bucket_start_ms,source_kind,source_id,room_id) DO UPDATE SET
            sample_count=excluded.sample_count,
            summary_json=excluded.summary_json,
            updated_at_ms=excluded.updated_at_ms`,
        [
            id,
            row.period,
            row.bucket_start_ms,
            row.source_kind,
            row.source_id,
            row.room_id,
            row.sample_count,
            jsonText(row.summary),
            nowMs,
            nowMs
        ]
    );
}

async function aggregateSince(dbAll, period, sourceKind, fallbackMs) {
    const rows = await dbAll(
        `SELECT MAX(bucket_start_ms) AS latest_bucket_ms
         FROM home_ai_data_aggregates
         WHERE period=? AND source_kind=?`,
        [period, sourceKind]
    );
    const latest = Number(rows[0]?.latest_bucket_ms);
    return Number.isFinite(latest) && latest > 0
        ? Math.max(fallbackMs, latest - 4 * DAY_MS)
        : fallbackMs;
}

async function aggregateHomeAiData(dbRun, dbAll, policy, nowMs = Date.now()) {
    const completedHour = Math.floor(nowMs / HOUR_MS) * HOUR_MS;
    const lookbackMs = Math.max(policy.environment_raw_days, 365) * DAY_MS;
    const sinceMs = Math.max(0, nowMs - lookbackMs);
    let aggregateCount = 0;
    const periods = [
        { name: "hour", size: HOUR_MS },
        { name: "day", size: DAY_MS }
    ];

    if (await tableExists(dbAll, "sensor_records")) {
        for (const period of periods) {
            const sourceSinceMs = await aggregateSince(dbAll, period.name, "environment", sinceMs);
            const rows = await dbAll(
                `SELECT
                    CAST(COALESCE(server_recv_ms,timestamp) / ? AS INTEGER) * ? AS bucket_start_ms,
                    COALESCE(device_id,'') AS source_id,
                    COUNT(*) AS sample_count,
                    AVG(temperature) AS avg_temperature,
                    AVG(humidity) AS avg_humidity,
                    AVG(pressure) AS avg_pressure,
                    AVG(gas_resistance) AS avg_gas_resistance,
                    AVG(air_quality_score) AS avg_air_quality_score
                 FROM sensor_records
                 WHERE (payload_type='sensor.bme690' OR payload_type IS NULL OR payload_type='')
                   AND COALESCE(server_recv_ms,timestamp) >= ?
                   AND COALESCE(server_recv_ms,timestamp) < ?
                 GROUP BY bucket_start_ms,source_id`,
                [period.size, period.size, sourceSinceMs, completedHour]
            );
            for (const row of rows) {
                await upsertAggregate(dbRun, {
                    period: period.name,
                    bucket_start_ms: Number(row.bucket_start_ms),
                    source_kind: "environment",
                    source_id: row.source_id || "",
                    room_id: "",
                    sample_count: Number(row.sample_count) || 0,
                    summary: {
                        avg_temperature: row.avg_temperature === null ? null : Number(row.avg_temperature),
                        avg_humidity: row.avg_humidity === null ? null : Number(row.avg_humidity),
                        avg_pressure: row.avg_pressure === null ? null : Number(row.avg_pressure),
                        avg_gas_resistance: row.avg_gas_resistance === null ? null : Number(row.avg_gas_resistance),
                        avg_air_quality_score: row.avg_air_quality_score === null ? null : Number(row.avg_air_quality_score)
                    }
                }, nowMs);
                aggregateCount += 1;
            }
        }
    }

    if (await tableExists(dbAll, "home_ai_events")) {
        for (const period of periods) {
            const sourceSinceMs = await aggregateSince(dbAll, period.name, "home_ai_event", sinceMs);
            const rows = await dbAll(
                `SELECT
                    CAST(occurred_at_ms / ? AS INTEGER) * ? AS bucket_start_ms,
                    event_type,
                    COALESCE(room_id,'') AS room_id,
                    COUNT(*) AS sample_count,
                    MIN(occurred_at_ms) AS first_occurred_at_ms,
                    MAX(occurred_at_ms) AS last_occurred_at_ms
                 FROM home_ai_events
                 WHERE occurred_at_ms >= ? AND occurred_at_ms < ?
                 GROUP BY bucket_start_ms,event_type,room_id`,
                [period.size, period.size, sourceSinceMs, completedHour]
            );
            for (const row of rows) {
                await upsertAggregate(dbRun, {
                    period: period.name,
                    bucket_start_ms: Number(row.bucket_start_ms),
                    source_kind: "home_ai_event",
                    source_id: row.event_type || "",
                    room_id: row.room_id || "",
                    sample_count: Number(row.sample_count) || 0,
                    summary: {
                        event_type: row.event_type || "",
                        first_occurred_at_ms: Number(row.first_occurred_at_ms) || null,
                        last_occurred_at_ms: Number(row.last_occurred_at_ms) || null
                    }
                }, nowMs);
                aggregateCount += 1;
            }
        }
    }
    return { aggregate_count: aggregateCount, since_ms: sinceMs, completed_before_ms: completedHour };
}

async function pruneRawData(dbRun, dbAll, policy, nowMs = Date.now()) {
    const deleted = {};
    const environmentCutoffMs = nowMs - policy.environment_raw_days * DAY_MS;
    if (await tableExists(dbAll, "sensor_records")) {
        const result = await dbRun(
            `DELETE FROM sensor_records
             WHERE (payload_type='sensor.bme690' OR payload_type IS NULL OR payload_type='')
               AND COALESCE(server_recv_ms,timestamp) IS NOT NULL
               AND COALESCE(server_recv_ms,timestamp) < ?`,
            [environmentCutoffMs]
        );
        deleted.sensor_records = Number(result?.changes) || 0;
    } else {
        deleted.sensor_records = 0;
    }

    if (await tableExists(dbAll, "home_ai_data_aggregates")) {
        const hourlyResult = await dbRun(
            "DELETE FROM home_ai_data_aggregates WHERE period='hour' AND bucket_start_ms < ?",
            [nowMs - policy.hourly_aggregate_days * DAY_MS]
        );
        const dailyResult = await dbRun(
            "DELETE FROM home_ai_data_aggregates WHERE period='day' AND bucket_start_ms < ?",
            [nowMs - policy.daily_aggregate_days * DAY_MS]
        );
        deleted.aggregates = (Number(hourlyResult?.changes) || 0) + (Number(dailyResult?.changes) || 0);
    } else {
        deleted.aggregates = 0;
    }
    return {
        deleted,
        radar_coordinate_rows_deleted: 0,
        radar_coordinate_note: "no raw radar-coordinate table is persisted by the current gateway contract"
    };
}

async function readDatabaseCapacity(dbAll, policy) {
    const rows = await dbAll("PRAGMA page_count");
    const pageSizeRows = await dbAll("PRAGMA page_size");
    const freelistRows = await dbAll("PRAGMA freelist_count");
    const pageCount = Number(rows[0]?.page_count) || 0;
    const pageSize = Number(pageSizeRows[0]?.page_size) || 0;
    const freelist = Number(freelistRows[0]?.freelist_count) || 0;
    const usedBytes = Math.max(0, pageCount - freelist) * pageSize;
    const capacityBytes = policy.capacity_warning_mb * 1024 * 1024;
    const percent = capacityBytes > 0 ? (usedBytes / capacityBytes) * 100 : 0;
    return {
        used_bytes: usedBytes,
        capacity_bytes: capacityBytes,
        capacity_percent: Math.round(percent * 100) / 100,
        warning: percent >= policy.capacity_warning_percent,
        page_count: pageCount,
        page_size: pageSize,
        freelist_count: freelist
    };
}

async function recordMaintenanceRun(dbRun, run) {
    await dbRun(
        `INSERT INTO home_ai_maintenance_runs
         (run_id,job_name,status,result_json,error_message,started_at_ms,finished_at_ms)
         VALUES(?,?,?,?,?,?,?)`,
        [run.run_id, run.job_name, run.status, jsonText(run.result || {}), run.error_message || "", run.started_at_ms, run.finished_at_ms || null]
    );
}

async function runHomeAiMaintenance(options = {}) {
    const { dbRun, dbAll, logger = console } = options;
    const nowMs = Number(options.now_ms) || Date.now();
    const run = {
        run_id: makeId("home_ai_maintenance"),
        job_name: "home_ai_retention_aggregation",
        status: "RUNNING",
        started_at_ms: nowMs,
        result: {}
    };
    try {
        const policy = options.policy || await readRetentionPolicy(dbAll);
        const aggregate = options.aggregateImpl
            ? await options.aggregateImpl(policy, nowMs)
            : await aggregateHomeAiData(dbRun, dbAll, policy, nowMs);
        const retention = options.retentionImpl
            ? await options.retentionImpl(policy, nowMs)
            : await pruneRawData(dbRun, dbAll, policy, nowMs);
        const capacity = options.capacityImpl
            ? await options.capacityImpl(policy, nowMs)
            : await readDatabaseCapacity(dbAll, policy);
        run.status = "SUCCEEDED";
        run.result = { policy, aggregate, retention, capacity };
        run.finished_at_ms = Date.now();
        await recordMaintenanceRun(dbRun, run);
        if (capacity.warning && typeof logger.warn === "function") {
            logger.warn(`[home-ai] database capacity warning percent=${capacity.capacity_percent}`);
        }
        return { ok: true, ...run };
    } catch (error) {
        run.status = "FAILED";
        run.error_message = String(error?.message || error);
        run.result = { phase: "aggregate_before_retention" };
        run.finished_at_ms = Date.now();
        try {
            await recordMaintenanceRun(dbRun, run);
        } catch (recordError) {
            if (typeof logger.error === "function") logger.error(`[home-ai] maintenance failure log failed ${recordError?.message || recordError}`);
        }
        if (typeof logger.error === "function") logger.error(`[home-ai] maintenance failed ${run.error_message}`);
        return { ok: false, ...run };
    }
}

async function listMaintenanceRuns(dbAll, options = {}) {
    const limit = integer(options.limit, 20, 1, 100);
    const rows = await dbAll(
        `SELECT * FROM home_ai_maintenance_runs
         ORDER BY started_at_ms DESC LIMIT ?`,
        [limit]
    );
    return rows.map(row => ({
        run_id: row.run_id,
        job_name: row.job_name,
        status: row.status,
        result: (() => {
            try { return JSON.parse(row.result_json || "{}"); } catch (_) { return {}; }
        })(),
        error_message: row.error_message || "",
        started_at_ms: Number(row.started_at_ms) || null,
        finished_at_ms: Number(row.finished_at_ms) || null
    }));
}

function createHomeAiDataScheduler(options = {}) {
    const intervalMs = Math.max(60 * 1000, Number(options.interval_ms) || 15 * 60 * 1000);
    const logger = options.logger || console;
    let timer = null;
    let running = false;
    let inFlight = null;

    async function tick() {
        if (inFlight) return inFlight;
        inFlight = (async () => {
            try {
                return await runHomeAiMaintenance({ ...options, logger });
            } finally {
                inFlight = null;
            }
        })();
        return inFlight;
    }

    return {
        start() {
            if (timer) return;
            running = true;
            timer = setInterval(() => { void tick(); }, intervalMs);
            if (typeof timer.unref === "function") timer.unref();
            if (options.run_immediately) void tick();
        },
        async stop() {
            if (timer) clearInterval(timer);
            timer = null;
            running = false;
            if (inFlight) await inFlight;
        },
        tick,
        isRunning() { return running; }
    };
}

module.exports = {
    DEFAULT_RETENTION_POLICY,
    RETENTION_SETTING_KEY,
    aggregateHomeAiData,
    createHomeAiDataScheduler,
    ensureHomeAiDataTables,
    listMaintenanceRuns,
    normalizeRetentionPolicy,
    validateRetentionPolicy,
    pruneRawData,
    readDatabaseCapacity,
    readRetentionPolicy,
    runHomeAiMaintenance,
    writeRetentionPolicy
};
