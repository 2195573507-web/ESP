const {
    readDeviceStatus,
    readModuleStatuses,
    MODULE_ONLINE_THRESHOLD_MS
} = require("./deviceStatusService");
const {
    trimText
} = require("./deviceMetadata");
const {
    resolveDeviceId
} = require("./deviceIdResolver");

const ENVIRONMENT_FRESH_MS = 30000;

function rowFirst(rows) {
    return Array.isArray(rows) && rows.length > 0 ? rows[0] : null;
}

function numberOrNull(value) {
    return Number.isFinite(Number(value)) ? Number(value) : null;
}

function integerOrNull(value) {
    const numeric = numberOrNull(value);
    return numeric === null ? null : Math.trunc(numeric);
}

function parseJsonObject(value, fallback = null) {
    if (!value) {
        return fallback;
    }

    try {
        const parsed = typeof value === "string" ? JSON.parse(value) : value;
        return parsed && typeof parsed === "object" && !Array.isArray(parsed) ? parsed : fallback;
    } catch (_) {
        return fallback;
    }
}

function mapAirQuality(row) {
    const raw = parseJsonObject(row?.air_quality_json, {});
    const score = integerOrNull(row?.air_quality_score ?? raw.air_quality_score);
    const level = trimText(row?.air_quality_level || raw.air_quality_level);
    const confidence = trimText(row?.air_quality_confidence || raw.air_quality_confidence);
    const source = trimText(row?.air_quality_source || raw.air_quality_source);
    const available = score !== null && level && level !== "unknown" && confidence && confidence !== "none";

    return {
        available,
        score,
        level: level || "unknown",
        confidence: confidence || "none",
        source,
        algo_version: trimText(row?.air_quality_algo_version || raw.air_quality_algo_version),
        gas_baseline_ohm: numberOrNull(row?.gas_baseline_ohm ?? raw.gas_baseline_ohm),
        gas_ratio: numberOrNull(row?.gas_ratio ?? raw.gas_ratio),
        gas_score: integerOrNull(row?.gas_score ?? raw.gas_score),
        humidity_score: integerOrNull(row?.humidity_score ?? raw.humidity_score),
        note: "ESP local BME690 relative estimate, not national AQI, PM2.5, PM10, or CO2."
    };
}

function mapEnvironment(row, nowMs = Date.now()) {
    if (!row) {
        return {
            available: false,
            fresh: false,
            age_ms: null
        };
    }

    const updatedMs = integerOrNull(row.server_recv_ms || row.timestamp);
    const ageMs = updatedMs === null ? null : Math.max(0, nowMs - updatedMs);
    return {
        available: true,
        fresh: ageMs !== null && ageMs <= ENVIRONMENT_FRESH_MS,
        age_ms: ageMs,
        server_recv_ms: updatedMs,
        server_time_iso: row.server_time_iso || "",
        temperature_c: numberOrNull(row.temperature),
        humidity_percent: numberOrNull(row.humidity),
        pressure_hpa: numberOrNull(row.pressure),
        gas_resistance_ohm: numberOrNull(row.gas_resistance),
        sensor_id: row.sensor_id || "",
        payload_type: row.payload_type || "sensor.bme690"
    };
}

function moduleMapFromRows(rows) {
    const modules = {};
    for (const row of rows) {
        modules[row.module_type] = row;
    }

    for (const moduleType of ["sensor.bme690", "voice.turn", "voice.prompt", "command.capabilities", "command.poll", "command.ack", "time.ping", "csi.motion", "lcd.status"]) {
        if (!modules[moduleType]) {
            modules[moduleType] = {
                module_type: moduleType,
                available: false,
                online: false,
                module_online: false,
                last_seen_age_ms: null
            };
        } else {
            modules[moduleType].available = true;
        }
    }

    return modules;
}

async function readLatestBmeRow(dbAll, deviceId) {
    const resolvedDeviceId = resolveDeviceId(deviceId);
    const params = [];
    let where = "WHERE deleted_at IS NULL AND (payload_type='sensor.bme690' OR payload_type IS NULL OR payload_type='')";
    if (resolvedDeviceId) {
        where += " AND device_id=?";
        params.push(resolvedDeviceId);
    }

    const rows = await dbAll(
        `SELECT * FROM sensor_records
        ${where}
        ORDER BY COALESCE(server_recv_ms, timestamp, id) DESC, id DESC
        LIMIT 1`,
        params
    );
    return rowFirst(rows);
}

async function getDeviceContext(dbAll, deviceId = "", options = {}) {
    const nowMs = Number.isFinite(options.nowMs) ? options.nowMs : Date.now();
    const safeDeviceId = resolveDeviceId(deviceId);
    const device = await readDeviceStatus(dbAll, safeDeviceId, nowMs);
    const inferredDeviceId = safeDeviceId || device?.device_id || "";
    const moduleRows = await readModuleStatuses(dbAll, inferredDeviceId, nowMs);
    const latestBme = await readLatestBmeRow(dbAll, inferredDeviceId);
    const environment = mapEnvironment(latestBme, nowMs);
    const airQuality = mapAirQuality(latestBme);

    return {
        device: device || {
            device_id: inferredDeviceId,
            online: null,
            device_online: null,
            status: "unknown",
            status_source: "not_observed",
            observed: false,
            offline_reason: null,
            last_seen_age_ms: null,
            latest_upload_delay_ms: null,
            avg_upload_delay_ms: null,
            delay_sample_count: 0
        },
        modules: moduleMapFromRows(moduleRows),
        environment,
        air_quality: airQuality,
        thresholds: {
            environment_fresh_ms: ENVIRONMENT_FRESH_MS,
            module_online_ms: MODULE_ONLINE_THRESHOLD_MS
        }
    };
}

module.exports = {
    ENVIRONMENT_FRESH_MS,
    getDeviceContext,
    mapAirQuality,
    mapEnvironment
};
