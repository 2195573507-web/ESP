const {
    getTimeSyncStatus
} = require("../../server-time-sync/timeSync");
const {
    readDeviceStatuses,
    readDeviceStatus,
    readModuleStatuses,
    refreshDeviceActivity,
    updateChildStatusFromGatewaySnapshot
} = require("./deviceStatusService");
const {
    readDeviceMetadata,
    toIntegerOrNull,
    trimText
} = require("./deviceMetadata");
const {
    resolveDeviceId
} = require("./deviceIdResolver");
const {
    makeSnapshotId
} = require("../db/dashboardSnapshots");
const {
    readLatestCsiMotionEvents
} = require("../db/csiMotion");
const {
    listEvents,
    recordEvent
} = require("./eventLogService");

const DASHBOARD_HISTORY_DEFAULT_LIMIT = 50;
const DASHBOARD_HISTORY_MAX_LIMIT = 500;
const DASHBOARD_SENSOR_HISTORY_DEFAULT_TARGET_POINTS = 300;
const DASHBOARD_SENSOR_HISTORY_MIN_TARGET_POINTS = 100;
const DASHBOARD_SENSOR_HISTORY_DEFAULT_RANGE = "7d";
const DASHBOARD_SENSOR_HISTORY_RANGE_MS = Object.freeze({
    "5m": 5 * 60 * 1000,
    "1h": 60 * 60 * 1000,
    "24h": 24 * 60 * 60 * 1000,
    "7d": 7 * 24 * 60 * 60 * 1000
});
const DASHBOARD_SENSOR_HISTORY_MAX_SPAN_MS = DASHBOARD_SENSOR_HISTORY_RANGE_MS["7d"];
const SENSOR_HISTORY_TIME_SQL = "COALESCE(NULLIF(server_recv_ms, 0), NULLIF(timestamp, 0))";
const DASHBOARD_SNAPSHOT_PAYLOAD_TYPE = "gateway.dashboard_snapshot";
const CSI_MOTION_PAYLOAD_TYPE = "csi.motion";
const CSI_STATES = new Set(["IDLE", "MOTION", "HOLD"]);
const MIN_PLAUSIBLE_UNIX_MS = Date.UTC(2000, 0, 1);
let latestDashboardSnapshot = null;
const latestCsiMotionByDevice = new Map();

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

function normalizeDashboardDeviceId(value) {
    return resolveDeviceId(value);
}

function readDashboardLimit(value) {
    if (value === undefined || value === null || value === "") {
        return {
            ok: true,
            limit: DASHBOARD_HISTORY_DEFAULT_LIMIT
        };
    }

    const text = String(value).trim();
    if (!/^\d+$/.test(text)) {
        return {
            ok: false,
            code: "DASHBOARD_BAD_LIMIT",
            message: "limit must be a positive integer"
        };
    }

    const numeric = Number.parseInt(text, 10);
    if (!Number.isFinite(numeric) || numeric <= 0) {
        return {
            ok: false,
            code: "DASHBOARD_BAD_LIMIT",
            message: "limit must be a positive integer"
        };
    }

    return {
        ok: true,
        limit: Math.min(numeric, DASHBOARD_HISTORY_MAX_LIMIT)
    };
}

function queryText(value) {
    const raw = Array.isArray(value) ? value[0] : value;
    if (raw === undefined || raw === null) {
        return "";
    }

    return String(raw).trim();
}

function hasQueryValue(query = {}, key) {
    return queryText(query[key]) !== "";
}

function parseEpochMsParam(query = {}, key) {
    const text = queryText(query[key]);
    if (!/^\d+$/.test(text)) {
        return {
            ok: false,
            code: "DASHBOARD_BAD_TIME_RANGE",
            message: `${key} must be unix epoch milliseconds`
        };
    }

    const value = Number.parseInt(text, 10);
    if (!Number.isSafeInteger(value) || value < 0) {
        return {
            ok: false,
            code: "DASHBOARD_BAD_TIME_RANGE",
            message: `${key} must be unix epoch milliseconds`
        };
    }

    return {
        ok: true,
        value
    };
}

function sensorHistoryTargetPoints(limitValue) {
    const limitResult = readDashboardLimit(limitValue);
    if (!limitResult.ok) {
        return limitResult;
    }

    if (queryText(limitValue) === "") {
        return {
            ok: true,
            limit: DASHBOARD_SENSOR_HISTORY_DEFAULT_TARGET_POINTS
        };
    }

    return {
        ok: true,
        limit: Math.max(DASHBOARD_SENSOR_HISTORY_MIN_TARGET_POINTS, limitResult.limit)
    };
}

function readDashboardSensorHistoryQuery(query = {}, nowMs = Date.now()) {
    const targetResult = sensorHistoryTargetPoints(query.limit);
    if (!targetResult.ok) {
        return targetResult;
    }

    const hasFrom = hasQueryValue(query, "from_ms");
    const hasTo = hasQueryValue(query, "to_ms");
    let range = queryText(query.range) || DASHBOARD_SENSOR_HISTORY_DEFAULT_RANGE;
    let fromMs;
    let toMs;

    if (hasFrom || hasTo) {
        if (!hasFrom || !hasTo) {
            return {
                ok: false,
                code: "DASHBOARD_BAD_TIME_RANGE",
                message: "from_ms and to_ms must be provided together"
            };
        }

        const parsedFrom = parseEpochMsParam(query, "from_ms");
        if (!parsedFrom.ok) {
            return parsedFrom;
        }
        const parsedTo = parseEpochMsParam(query, "to_ms");
        if (!parsedTo.ok) {
            return parsedTo;
        }

        fromMs = parsedFrom.value;
        toMs = parsedTo.value;
        range = null;
    } else {
        if (!Object.prototype.hasOwnProperty.call(DASHBOARD_SENSOR_HISTORY_RANGE_MS, range)) {
            return {
                ok: false,
                code: "DASHBOARD_BAD_RANGE",
                message: "range must be one of 5m, 1h, 24h, 7d"
            };
        }

        toMs = nowMs;
        fromMs = toMs - DASHBOARD_SENSOR_HISTORY_RANGE_MS[range];
    }

    if (fromMs > toMs) {
        return {
            ok: false,
            code: "DASHBOARD_BAD_TIME_RANGE",
            message: "from_ms must be less than or equal to to_ms"
        };
    }

    const spanMs = toMs - fromMs;
    if (spanMs <= 0 || spanMs > DASHBOARD_SENSOR_HISTORY_MAX_SPAN_MS) {
        return {
            ok: false,
            code: "DASHBOARD_BAD_TIME_RANGE",
            message: "time span must be greater than 0 and no more than 7d"
        };
    }

    return {
        ok: true,
        range,
        from_ms: fromMs,
        to_ms: toMs,
        span_ms: spanMs,
        target_points: targetResult.limit
    };
}

function rowFirst(rows) {
    return Array.isArray(rows) && rows.length > 0 ? rows[0] : null;
}

function numberOrNull(value) {
    if (value === undefined || value === null || value === "") {
        return null;
    }

    const numeric = Number(value);
    return Number.isFinite(numeric) ? numeric : null;
}

function integerOrNull(value) {
    const numeric = numberOrNull(value);
    return numeric === null ? null : Math.trunc(numeric);
}

function textOrNull(value) {
    if (value === undefined || value === null || value === "") {
        return null;
    }

    return String(value);
}

function numberValueOrNull(value) {
    const numeric = numberOrNull(value);
    return numeric === null ? null : numeric;
}

function booleanValue(value, fallback = false) {
    if (typeof value === "boolean") {
        return value;
    }
    if (typeof value === "number") {
        return value !== 0;
    }
    if (typeof value === "string") {
        const text = value.trim().toLowerCase();
        if (["true", "1", "yes", "y"].includes(text)) {
            return true;
        }
        if (["false", "0", "no", "n"].includes(text)) {
            return false;
        }
    }
    return fallback;
}

function isPlainObject(value) {
    return value && typeof value === "object" && !Array.isArray(value);
}

function mockAppliances() {
    return {
        air_conditioner: {
            power: false,
            mode: "cool",
            target_temperature: 26,
            source: "mock",
            mock: true
        },
        fan: {
            power: false,
            speed: 0,
            source: "mock",
            mock: true
        },
        light: {
            power: true,
            brightness: 60,
            source: "mock",
            mock: true
        },
        tv: {
            power: false,
            source: "mock",
            mock: true
        },
        curtain: {
            open_percent: 70,
            source: "mock",
            mock: true
        }
    };
}

function normalizeAppliances(input) {
    const fallback = mockAppliances();
    const source = isPlainObject(input) ? input : {};
    for (const key of Object.keys(fallback)) {
        const appliance = isPlainObject(source[key]) ? source[key] : {};
        fallback[key] = {
            ...fallback[key],
            ...appliance,
            source: appliance.source || "mock",
            mock: appliance.mock === undefined ? true : Boolean(appliance.mock)
        };
    }
    return fallback;
}

function normalizeSnapshotGateway(gateway, serverRecvMs) {
    const source = isPlainObject(gateway) ? gateway : {};
    return {
        gateway_id: resolveDeviceId(source.gateway_id || "sensair_s3_gateway_01"),
        online: booleanValue(source.online, true),
        softap_ready: booleanValue(source.softap_ready, false),
        softap_enabled: booleanValue(source.softap_enabled ?? source.softap_ready, false),
        sta_connected: booleanValue(source.sta_connected, false),
        server_available: booleanValue(source.server_available, false),
        server_connected: booleanValue(source.server_connected ?? source.server_available, false),
        voice_busy: booleanValue(source.voice_busy, false),
        free_heap: integerOrNull(source.free_heap),
        psram_heap: integerOrNull(source.psram_heap),
        last_error: trimText(source.last_error, 128),
        timestamp: integerOrNull(source.timestamp) || serverRecvMs
    };
}

function projectChildLastSeenMs(childLastSeenMs, gatewayTimestampMs, serverRecvMs) {
    const childTimestamp = integerOrNull(childLastSeenMs);
    if (childTimestamp === null) {
        return null;
    }
    if (childTimestamp >= MIN_PLAUSIBLE_UNIX_MS) {
        return childTimestamp;
    }

    const gatewayTimestamp = integerOrNull(gatewayTimestampMs);
    if (gatewayTimestamp === null || gatewayTimestamp >= MIN_PLAUSIBLE_UNIX_MS) {
        return serverRecvMs;
    }

    const ageMs = Math.max(0, gatewayTimestamp - childTimestamp);
    return Math.max(0, serverRecvMs - ageMs);
}

function applyTrustedGatewayId(snapshot, trustedGatewayId) {
    const gatewayId = resolveDeviceId(trustedGatewayId);
    if (!gatewayId || !snapshot?.gateway) {
        return snapshot;
    }

    snapshot.gateway.gateway_id = gatewayId;
    return snapshot;
}

function normalizeSnapshotSensors(sensors) {
    if (!isPlainObject(sensors)) {
        return null;
    }

    const suppliedAirQuality = isPlainObject(sensors.air_quality)
        ? cloneJson(sensors.air_quality)
        : null;
    const bmeDiag = isPlainObject(sensors.bme_diag)
        ? cloneJson(sensors.bme_diag)
        : null;
    const baselineState = isPlainObject(sensors.baseline_state)
        ? cloneJson(sensors.baseline_state)
        : null;
    const score = integerOrNull(sensors.air_quality_score ?? suppliedAirQuality?.air_quality_score ?? suppliedAirQuality?.score);
    const level = trimText(sensors.air_quality_level ?? suppliedAirQuality?.air_quality_level ?? suppliedAirQuality?.level, 40) || "unknown";
    const confidence = trimText(sensors.air_quality_confidence ?? suppliedAirQuality?.air_quality_confidence ?? suppliedAirQuality?.confidence, 40);
    const source = trimText(sensors.air_quality_source ?? suppliedAirQuality?.air_quality_source ?? suppliedAirQuality?.source, 40) || "s3_mapped";

    return {
        temperature: numberValueOrNull(sensors.temperature ?? sensors.temperature_c),
        humidity: numberValueOrNull(sensors.humidity ?? sensors.humidity_percent),
        pressure: numberValueOrNull(sensors.pressure ?? sensors.pressure_hpa),
        gas_resistance: numberValueOrNull(sensors.gas_resistance ?? sensors.gas_resistance_ohm),
        air_quality_score: score,
        air_quality_level: level,
        air_quality_confidence: confidence,
        air_quality_source: source,
        ...(suppliedAirQuality ? {
            air_quality: suppliedAirQuality
        } : {}),
        ...(bmeDiag ? {
            bme_diag: bmeDiag
        } : {}),
        ...(baselineState ? {
            baseline_state: baselineState
        } : {})
    };
}

function normalizeCsiState(value) {
    const state = trimText(value, 16).toUpperCase();
    return CSI_STATES.has(state) ? state : "IDLE";
}

function clampMotionScore(value) {
    const numeric = numberValueOrNull(value);
    if (numeric === null) {
        return null;
    }

    return Math.min(Math.max(numeric, 0), 1);
}

function normalizeSnapshotCsi(csi, serverRecvMs, options = {}) {
    const source = isPlainObject(csi) ? csi : {};
    const hasSource = isPlainObject(csi);
    const available = booleanValue(source.available, options.availableDefault ?? hasSource);
    const state = normalizeCsiState(source.state);

    if (!available) {
        return {
            device_id: resolveDeviceId(source.device_id),
            link_id: trimText(source.link_id || "fused", 64),
            state: "IDLE",
            available: false,
            frame_energy: null,
            variance: null,
            rssi: null,
            motion_score: null,
            timestamp: null
        };
    }

    return {
        device_id: resolveDeviceId(source.device_id),
        link_id: trimText(source.link_id || "fused", 64),
        state,
        available: true,
        frame_energy: numberValueOrNull(source.frame_energy),
        variance: numberValueOrNull(source.variance),
        rssi: integerOrNull(source.rssi),
        motion_score: clampMotionScore(source.motion_score),
        timestamp: integerOrNull(source.timestamp ?? source.updated_at) || serverRecvMs
    };
}

function normalizeSnapshotDevice(device, serverRecvMs, gatewayTimestampMs) {
    if (!isPlainObject(device)) {
        return null;
    }

    const deviceId = resolveDeviceId(device.device_id);
    if (!deviceId) {
        return null;
    }
    if (isPlainObject(device.occupancy)) {
        return null;
    }

    const voiceBusy = booleanValue(device.voice_busy, false);
    // voice_busy is an active S3 child-registry state. It must not be exposed
    // as offline merely because the separate online bit is momentarily stale.
    const online = booleanValue(device.online, false) || voiceBusy;
    const childLastSeenMs = integerOrNull(device.child_last_seen_ms ?? device.last_seen_ms);
    const lastSeenMs = projectChildLastSeenMs(childLastSeenMs, gatewayTimestampMs, serverRecvMs);

    return {
        device_id: deviceId,
        local_id: integerOrNull(device.local_id),
        device_type: trimText(device.device_type, 40) || "C5",
        name: trimText(device.name || device.alias, 128),
        room_id: trimText(device.room_id, 128),
        room_name: trimText(device.room_name || device.room_id || "unassigned", 128),
        // C5 online state is decided by ESPS3 child_registry. Preserve it verbatim;
        // the server must not infer it from receipt, network, or upload health.
        online,
        status: trimText(device.status, 40) || (online ? "online" : "offline"),
        offline_reason: trimText(device.offline_reason, 128) || null,
        link_lost: booleanValue(device.link_lost, false),
        voice_busy: voiceBusy,
        child_last_seen_ms: childLastSeenMs,
        server_received_ms: serverRecvMs,
        // Public status timestamps stay in Server epoch time. child_last_seen_ms
        // retains the raw S3 monotonic clock for diagnostics.
        last_seen_ms: lastSeenMs,
        wifi_rssi: integerOrNull(device.wifi_rssi),
        timestamp: integerOrNull(device.timestamp) || serverRecvMs,
        sensors: normalizeSnapshotSensors(device.sensors),
        csi: normalizeSnapshotCsi(device.csi, serverRecvMs, {
            availableDefault: false
        }),
        appliances: normalizeAppliances(device.appliances)
    };
}

function normalizeSnapshotHistoryItem(item, serverRecvMs) {
    if (!isPlainObject(item)) {
        return null;
    }

    const deviceId = resolveDeviceId(item.device_id);
    if (!deviceId) {
        return null;
    }

    return {
        device_id: deviceId,
        sensor_type: trimText(item.sensor_type || "bme690", 40),
        timestamp: integerOrNull(item.timestamp) || serverRecvMs,
        temperature: numberValueOrNull(item.temperature ?? item.temperature_c),
        humidity: numberValueOrNull(item.humidity ?? item.humidity_percent),
        pressure: numberValueOrNull(item.pressure ?? item.pressure_hpa),
        gas_resistance: numberValueOrNull(item.gas_resistance ?? item.gas_resistance_ohm),
        air_quality_score: integerOrNull(item.air_quality_score),
        air_quality_level: trimText(item.air_quality_level, 40) || "unknown"
    };
}

function normalizeVoiceEvent(item, serverRecvMs) {
    if (!isPlainObject(item)) {
        return null;
    }
    const deviceId = resolveDeviceId(item.device_id);
    if (!deviceId) {
        return null;
    }

    return {
        device_id: deviceId,
        event: trimText(item.event || "voice_turn_completed", 80),
        timestamp: integerOrNull(item.timestamp) || serverRecvMs,
        duration_ms: integerOrNull(item.duration_ms),
        source: trimText(item.source || "s3_gateway", 40)
    };
}

function normalizeCommandEvent(item, serverRecvMs) {
    if (!isPlainObject(item)) {
        return null;
    }
    const commandId = trimText(item.command_id, 128);
    const deviceId = resolveDeviceId(item.device_id);
    if (!commandId || !deviceId) {
        return null;
    }

    return {
        command_id: commandId,
        device_id: deviceId,
        command_code: integerOrNull(item.command_code),
        status: trimText(item.status || "completed", 40),
        timestamp: integerOrNull(item.timestamp) || serverRecvMs,
        source: trimText(item.source || "s3_gateway", 40)
    };
}

function computeHomeSummary(devices) {
    const summary = {
        online_device_count: 0,
        offline_device_count: 0,
        unknown_device_count: 0,
        avg_temperature: null,
        avg_humidity: null,
        avg_air_quality: null
    };
    let tempSum = 0;
    let humiditySum = 0;
    let airSum = 0;
    let count = 0;

    for (const device of devices) {
        if (device.online === true) {
            summary.online_device_count += 1;
        } else if (device.online === false) {
            summary.offline_device_count += 1;
        } else {
            summary.unknown_device_count += 1;
        }
        if (device.online && device.sensors) {
            if (Number.isFinite(device.sensors.temperature)) {
                tempSum += device.sensors.temperature;
            }
            if (Number.isFinite(device.sensors.humidity)) {
                humiditySum += device.sensors.humidity;
            }
            if (Number.isFinite(device.sensors.air_quality_score)) {
                airSum += device.sensors.air_quality_score;
            }
            count += 1;
        }
    }

    if (count > 0) {
        summary.avg_temperature = Number((tempSum / count).toFixed(2));
        summary.avg_humidity = Number((humiditySum / count).toFixed(2));
        summary.avg_air_quality = Number((airSum / count).toFixed(2));
    }

    return summary;
}

function normalizeHomeSummary(input, devices) {
    const computed = computeHomeSummary(devices);
    if (!isPlainObject(input)) {
        return computed;
    }

    return {
        online_device_count: integerOrNull(input.online_device_count) ?? computed.online_device_count,
        offline_device_count: integerOrNull(input.offline_device_count) ?? computed.offline_device_count,
        avg_temperature: numberValueOrNull(input.avg_temperature) ?? computed.avg_temperature,
        avg_humidity: numberValueOrNull(input.avg_humidity) ?? computed.avg_humidity,
        avg_air_quality: numberValueOrNull(input.avg_air_quality) ?? computed.avg_air_quality
    };
}

function normalizeGatewaySnapshot(body, serverRecvMs = Date.now()) {
    if (!isPlainObject(body)) {
        return {
            ok: false,
            code: "INVALID_DASHBOARD_SNAPSHOT",
            error: "JSON object snapshot is required"
        };
    }
    if (trimText(body.payload_type, 80) !== DASHBOARD_SNAPSHOT_PAYLOAD_TYPE) {
        return {
            ok: false,
            code: "UNSUPPORTED_PAYLOAD_TYPE",
            error: "payload_type must be gateway.dashboard_snapshot"
        };
    }
    if (Number(body.schema_version) !== 2) {
        return {
            ok: false,
            code: "INVALID_SCHEMA_VERSION",
            error: "schema_version must be 2"
        };
    }
    if ((Array.isArray(body.devices) ? body.devices : [])
        .some(device => isPlainObject(device) && isPlainObject(device.occupancy))) {
        return {
            ok: false,
            code: "LEGACY_CSI_MODEL_NOT_ACCEPTED",
            error: "dashboard snapshot devices must not include legacy occupancy CSI fields"
        };
    }
    if (isPlainObject(body.csi) && body.csi.available !== false) {
        return {
            ok: false,
            code: "CANONICAL_CSI_EVENT_REQUIRED",
            error: "dashboard snapshot must not carry CSI state; use /kernel/csi_event"
        };
    }

    const gateway = normalizeSnapshotGateway(body.gateway, serverRecvMs);
    const devices = (Array.isArray(body.devices) ? body.devices : [])
        .map(device => normalizeSnapshotDevice(device, serverRecvMs, gateway.timestamp))
        .filter(Boolean);
    const history = (Array.isArray(body.history) ? body.history : [])
        .map(item => normalizeSnapshotHistoryItem(item, serverRecvMs))
        .filter(Boolean);
    const recentVoiceEvents = (Array.isArray(body.recent_voice_events) ? body.recent_voice_events : [])
        .map(item => normalizeVoiceEvent(item, serverRecvMs))
        .filter(Boolean);
    const recentCommands = (Array.isArray(body.recent_commands) ? body.recent_commands : [])
        .map(item => normalizeCommandEvent(item, serverRecvMs))
        .filter(Boolean);

    return {
        ok: true,
        snapshot: {
            gateway,
            devices,
            home_summary: normalizeHomeSummary(body.home_summary, devices),
            csi: normalizeSnapshotCsi(body.csi, serverRecvMs, {
                availableDefault: false
            }),
            history,
            recent_voice_events: recentVoiceEvents,
            recent_commands: recentCommands,
            received_at_ms: serverRecvMs,
            source: trimText(body.source || "s3_gateway", 40)
        }
    };
}

function cloneJson(value) {
    return JSON.parse(JSON.stringify(value));
}

function canonicalizeSnapshotDeviceIds(snapshot) {
    if (!isPlainObject(snapshot)) {
        return null;
    }

    const normalized = cloneJson(snapshot);
    const canonicalizeRecord = record => {
        if (!isPlainObject(record)) {
            return record;
        }

        if (record.device_id) {
            record.device_id = resolveDeviceId(record.device_id);
        }
        if (isPlainObject(record.csi) && record.csi.device_id) {
            record.csi.device_id = resolveDeviceId(record.csi.device_id);
        }
        return record;
    };

    if (isPlainObject(normalized.gateway)) {
        normalized.gateway.gateway_id = resolveDeviceId(normalized.gateway.gateway_id || "sensair_s3_gateway_01");
    }
    normalized.devices = (Array.isArray(normalized.devices) ? normalized.devices : []).map(canonicalizeRecord);
    normalized.history = (Array.isArray(normalized.history) ? normalized.history : []).map(canonicalizeRecord);
    normalized.recent_voice_events = (Array.isArray(normalized.recent_voice_events) ? normalized.recent_voice_events : []).map(canonicalizeRecord);
    normalized.recent_commands = (Array.isArray(normalized.recent_commands) ? normalized.recent_commands : []).map(canonicalizeRecord);
    if (isPlainObject(normalized.csi) && normalized.csi.device_id) {
        normalized.csi.device_id = resolveDeviceId(normalized.csi.device_id);
    }
    return normalized;
}

function stripMockAppliancesForStorage(snapshot) {
    const stored = cloneJson(snapshot);
    for (const device of stored.devices || []) {
        if (!isPlainObject(device.appliances)) {
            continue;
        }

        const realAppliances = {};
        for (const [name, appliance] of Object.entries(device.appliances)) {
            if (!isPlainObject(appliance)) {
                continue;
            }

            const source = trimText(appliance.source, 40).toLowerCase();
            if (appliance.mock === true || source === "mock") {
                continue;
            }

            realAppliances[name] = appliance;
        }
        device.appliances = realAppliances;
    }

    stored.mock_persistence = "stripped";
    return stored;
}

function mergeCsiMotionIntoSnapshot(snapshot) {
    if (!snapshot || !Array.isArray(snapshot.devices)) {
        return snapshot;
    }

    const fusedRecord = [...latestCsiMotionByDevice.values()]
        .filter(record => record.csi?.link_id === "fused")
        .sort((a, b) => (integerOrNull(a.csi?.timestamp) || 0) - (integerOrNull(b.csi?.timestamp) || 0))
        .pop();
    if (fusedRecord) {
        snapshot.csi = cloneJson(fusedRecord.csi);
    }

    snapshot.home_summary = computeHomeSummary(snapshot.devices);
    return snapshot;
}

function filterSnapshotForQuery(snapshot, query = {}, options = {}) {
    const deviceId = normalizeDashboardDeviceId(query.device_id);
    const cloned = cloneJson(snapshot);
    if (options.mergeCsi !== false) {
        mergeCsiMotionIntoSnapshot(cloned, deviceId);
    }
    if (!deviceId) {
        return cloned;
    }

    cloned.devices = cloned.devices.filter(device => device.device_id === deviceId);
    cloned.history = cloned.history.filter(item => item.device_id === deviceId);
    cloned.recent_voice_events = cloned.recent_voice_events.filter(item => item.device_id === deviceId);
    cloned.recent_commands = cloned.recent_commands.filter(item => item.device_id === deviceId);
    cloned.home_summary = computeHomeSummary(cloned.devices);
    return cloned;
}

function snapshotRowToPayload(row) {
    if (!row) {
        return null;
    }

    return parseJsonObject(row.payload_json, null);
}

async function restoreLatestDashboardSnapshot(dbAll) {
    if (typeof dbAll !== "function") {
        return null;
    }

    const rows = await dbAll(
        `SELECT * FROM dashboard_snapshots
        ORDER BY server_recv_ms DESC, id DESC
        LIMIT 1`
    );
    const payload = canonicalizeSnapshotDeviceIds(snapshotRowToPayload(rows[0]));
    if (payload) {
        latestDashboardSnapshot = payload;
    }

    return latestDashboardSnapshot ? cloneJson(latestDashboardSnapshot) : null;
}

async function readLatestDashboardSnapshot(dbAll) {
    if (latestDashboardSnapshot) {
        return cloneJson(latestDashboardSnapshot);
    }

    return restoreLatestDashboardSnapshot(dbAll);
}

async function readDashboardSnapshotHistory(dbAll, query = {}) {
    const limitResult = readDashboardLimit(query.limit);
    if (!limitResult.ok) {
        return limitResult;
    }

    const rows = await dbAll(
        `SELECT snapshot_id,gateway_id,server_recv_ms,payload_json,schema_version,created_at
        FROM dashboard_snapshots
        ORDER BY server_recv_ms DESC, id DESC
        LIMIT ?`,
        [limitResult.limit]
    );

    return rows.map(row => ({
        snapshot_id: row.snapshot_id,
        gateway_id: resolveDeviceId(row.gateway_id),
        server_recv_ms: integerOrNull(row.server_recv_ms),
        schema_version: integerOrNull(row.schema_version),
        payload: canonicalizeSnapshotDeviceIds(snapshotRowToPayload(row)),
        created_at: row.created_at || ""
    }));
}

function recordCsiMotion(record, options = {}) {
    const serverRecvMs = Number.isFinite(options.serverRecvMs) ? options.serverRecvMs : Date.now();
    const deviceId = normalizeDashboardDeviceId(record?.device_id);
    if (!deviceId) {
        return null;
    }

    const csi = normalizeSnapshotCsi(record, serverRecvMs, {
        availableDefault: true
    });
    csi.available = true;

    const normalized = {
        device_id: deviceId,
        local_id: integerOrNull(record?.local_id),
        name: trimText(record?.name || record?.alias, 128),
        room_name: trimText(record?.room_name || record?.room_id || "unassigned", 128),
        csi,
        received_at_ms: serverRecvMs,
        payload_type: CSI_MOTION_PAYLOAD_TYPE
    };

    latestCsiMotionByDevice.set(deviceId, normalized);
    return cloneJson(normalized);
}

function prepareDashboardSnapshot(body, options = {}) {
    const serverRecvMs = Number.isFinite(options.serverRecvMs) ? options.serverRecvMs : Date.now();
    const validation = normalizeGatewaySnapshot(body, serverRecvMs);
    if (!validation.ok) {
        return {
            ok: false,
            status: 400,
            code: validation.code,
            error: validation.error
        };
    }

    latestDashboardSnapshot = canonicalizeSnapshotDeviceIds(
        applyTrustedGatewayId(validation.snapshot, options.trustedGatewayId)
    );
    const gatewayId = latestDashboardSnapshot.gateway.gateway_id;
    const snapshotId = makeSnapshotId(gatewayId, serverRecvMs);

    console.info(`[dashboard_snapshot] gateway_status_source=server gateway_id=${gatewayId}`);
    console.info(`[dashboard_snapshot] child_status_source=s3 child_count=${latestDashboardSnapshot.devices.length}`);

    return {
        ok: true,
        status: 202,
        body,
        headers: options.headers,
        snapshot: latestDashboardSnapshot,
        snapshotId,
        gatewayId,
        serverRecvMs,
        data: {
            snapshot_id: snapshotId,
            payload_type: DASHBOARD_SNAPSHOT_PAYLOAD_TYPE,
            gateway_id: latestDashboardSnapshot.gateway.gateway_id,
            device_count: latestDashboardSnapshot.devices.length,
            history_count: latestDashboardSnapshot.history.length,
            bound_device_ids: latestDashboardSnapshot.devices.map(device => device.device_id).filter(Boolean),
            received_at_ms: latestDashboardSnapshot.received_at_ms
        }
    };
}

async function persistDashboardSnapshot(dbRun, dbAll, prepared) {
    if (!prepared?.ok) {
        return null;
    }

    const body = prepared.body;
    const snapshot = prepared.snapshot;
    const serverRecvMs = prepared.serverRecvMs;
    const gatewayId = prepared.gatewayId;
    const snapshotId = prepared.snapshotId;
    const nowIso = new Date(serverRecvMs).toISOString();

    await dbRun(
        `INSERT INTO dashboard_snapshots
        (snapshot_id,gateway_id,server_recv_ms,payload_json,schema_version,created_at,updated_at)
        VALUES(?,?,?,?,?,?,?)`,
        [
            snapshotId,
            gatewayId,
            serverRecvMs,
            JSON.stringify(stripMockAppliancesForStorage(snapshot)),
            Number(body.schema_version) || 2,
            nowIso,
            nowIso
        ]
    );

    const gatewayMetadata = readDeviceMetadata({
        body: {
            device_id: gatewayId,
            device_type: "S3",
            room_id: body.gateway?.room_id || body.room_id,
            room_name: body.gateway?.room_name || body.room_name,
            payload_type: DASHBOARD_SNAPSHOT_PAYLOAD_TYPE,
            esp_time_ms: body.gateway?.timestamp || body.timestamp,
            esp_uptime_ms: body.gateway?.esp_uptime_ms
        },
        headers: prepared.headers,
        payloadType: DASHBOARD_SNAPSHOT_PAYLOAD_TYPE,
        serverRecvMs
    });
    await refreshDeviceActivity(dbRun, dbAll, gatewayMetadata, DASHBOARD_SNAPSHOT_PAYLOAD_TYPE);

    for (const device of snapshot.devices) {
        // C5 status is stored exactly as S3 reported it; never infer it from
        // this server receiving a dashboard snapshot.
        await updateChildStatusFromGatewaySnapshot(dbRun, dbAll, device, {
            payloadType: DASHBOARD_SNAPSHOT_PAYLOAD_TYPE,
            serverReceivedMs: serverRecvMs
        });
    }

    await recordEvent(dbRun, {
        event_type: "system",
        event_name: "dashboard_snapshot_updated",
        device_id: gatewayId,
        severity: "info",
        message: "dashboard snapshot updated",
        payload: {
            snapshot_id: snapshotId,
            gateway_id: gatewayId,
            device_count: snapshot.devices.length,
            history_count: snapshot.history.length
        },
        source: "dashboard_snapshot",
        server_recv_ms: serverRecvMs
    });

    for (const voiceEvent of snapshot.recent_voice_events || []) {
        await recordEvent(dbRun, {
            event_type: "voice",
            event_name: "voice_event_created",
            device_id: voiceEvent.device_id,
            severity: "info",
            message: voiceEvent.event || "voice event",
            payload: voiceEvent,
            source: "dashboard_snapshot",
            server_recv_ms: serverRecvMs
        });
    }

    for (const commandEvent of snapshot.recent_commands || []) {
        await recordEvent(dbRun, {
            event_type: "command",
            event_name: "command_created",
            device_id: commandEvent.device_id,
            severity: "info",
            message: commandEvent.status || "command event",
            payload: commandEvent,
            source: "dashboard_snapshot",
            server_recv_ms: serverRecvMs
        });
    }

    return {
        ok: true,
        status: 202,
        data: prepared.data
    };
}

async function ingestDashboardSnapshot(body, options = {}) {
    const prepared = prepareDashboardSnapshot(body, options);
    if (!prepared.ok) {
        return prepared;
    }

    if (typeof options.dbRun === "function") {
        await persistDashboardSnapshot(options.dbRun, options.dbAll, prepared);
    }

    return {
        ok: true,
        status: 202,
        data: prepared.data
    };
}

function readAirQuality(row) {
    const parsed = parseJsonObject(row?.air_quality_json, null);
    const airQuality = isPlainObject(parsed) ? cloneJson(parsed) : null;
    const rawPayload = parseJsonObject(row?.raw_json, null)?.payload;
    const bmeDiag = isPlainObject(rawPayload?.bme_diag)
        ? cloneJson(rawPayload.bme_diag)
        : null;
    const baselineState = isPlainObject(rawPayload?.baseline_state)
        ? cloneJson(rawPayload.baseline_state)
        : null;
    const score = row?.air_quality_score ?? airQuality?.air_quality_score ?? airQuality?.score ?? null;
    const level = row?.air_quality_level || airQuality?.air_quality_level || airQuality?.level || null;
    const confidence = row?.air_quality_confidence || airQuality?.air_quality_confidence || airQuality?.confidence || null;
    const source = row?.air_quality_source || airQuality?.air_quality_source || airQuality?.source || null;
    const algorithm = textOrNull(airQuality?.algorithm || row?.air_quality_algo_version);
    const gasRatio = numberValueOrNull(row?.gas_ratio ?? airQuality?.gas_ratio);
    const stabilityScore = numberValueOrNull(airQuality?.stability_score);
    const sensorState = textOrNull(airQuality?.sensor_state);
    const baselineReady = airQuality?.baseline_ready === undefined
        ? null
        : booleanValue(airQuality.baseline_ready, null);

    return {
        air_quality: airQuality,
        bme_diag: bmeDiag,
        baseline_state: baselineState,
        air_quality_score: score,
        air_quality_level: level,
        air_quality_confidence: confidence,
        air_quality_source: source,
        score,
        level,
        confidence,
        source,
        algorithm,
        gas_ratio: gasRatio,
        stability_score: stabilityScore,
        sensor_state: sensorState,
        baseline_ready: baselineReady
    };
}

function mapDashboardDeviceStatus(status, fallbackDeviceId = "") {
    const observed = Boolean(status);
    return {
        device_id: resolveDeviceId(status?.device_id || fallbackDeviceId),
        online: observed ? Boolean(status.online) : null,
        device_online: observed ? Boolean(status.device_online) : null,
        status: textOrNull(status?.status) || "unknown",
        status_source: textOrNull(status?.status_source) || "not_observed",
        observed,
        offline_reason: textOrNull(status?.offline_reason),
        link_lost: observed ? Boolean(status.link_lost) : null,
        voice_busy: observed ? Boolean(status.voice_busy) : null,
        child_last_seen_ms: status?.child_last_seen_ms ?? null,
        server_received_ms: status?.server_received_ms ?? null,
        last_seen_ms: status?.last_seen_ms ?? null,
        last_seen_iso: textOrNull(status?.last_seen_iso),
        last_seen_age_ms: status?.last_seen_age_ms ?? null,
        time_synced: status?.time_synced ?? null,
        latest_upload_delay_ms: status?.latest_upload_delay_ms ?? null,
        avg_upload_delay_ms: status?.avg_upload_delay_ms ?? null,
        delay_sample_count: status?.delay_sample_count ?? 0
    };
}

function mapDashboardModuleStatus(moduleStatus) {
    return {
        device_id: resolveDeviceId(moduleStatus?.device_id),
        module_type: moduleStatus?.module_type || null,
        online: Boolean(moduleStatus?.online),
        module_online: Boolean(moduleStatus?.module_online),
        last_seen_ms: moduleStatus?.last_seen_ms ?? null,
        last_seen_iso: textOrNull(moduleStatus?.last_seen_iso),
        last_seen_age_ms: moduleStatus?.last_seen_age_ms ?? null,
        latest_upload_delay_ms: moduleStatus?.latest_upload_delay_ms ?? null,
        avg_upload_delay_ms: moduleStatus?.avg_upload_delay_ms ?? null,
        delay_sample_count: moduleStatus?.delay_sample_count ?? 0
    };
}

function lastVoiceEventMs(snapshot) {
    const events = Array.isArray(snapshot?.recent_voice_events) ? snapshot.recent_voice_events : [];
    let latest = null;
    for (const event of events) {
        const timestamp = integerOrNull(event.timestamp);
        if (timestamp !== null && (latest === null || timestamp > latest)) {
            latest = timestamp;
        }
    }
    return latest;
}

function latestHistoryMs(snapshot) {
    const history = Array.isArray(snapshot?.history) ? snapshot.history : [];
    let latest = null;
    for (const item of history) {
        const timestamp = integerOrNull(item.timestamp);
        if (timestamp !== null && (latest === null || timestamp > latest)) {
            latest = timestamp;
        }
    }
    return latest;
}

function moduleOnline(modules, moduleType) {
    return modules.some(module => module.module_type === moduleType && module.online);
}

function moduleStatusText(online) {
    return online ? "online" : "offline";
}

function buildModuleSummary(snapshot, modules) {
    const gateway = snapshot.gateway || {};
    const wifiOnline = Boolean(gateway.sta_connected || gateway.softap_enabled || gateway.softap_ready);
    const voiceOnline = moduleOnline(modules, "voice.turn") || moduleOnline(modules, "voice.prompt") || Boolean(lastVoiceEventMs(snapshot));
    const bmeOnline = moduleOnline(modules, "sensor.bme690") ||
        (Array.isArray(snapshot.devices) && snapshot.devices.some(device => Boolean(device.sensors)));
    const csiOnline = moduleOnline(modules, "csi.motion") ||
        Boolean(snapshot.csi?.available);
    const serverOnline = gateway.server_connected !== undefined
        ? Boolean(gateway.server_connected)
        : Boolean(gateway.server_available ?? true);

    return {
        wifi: {
            online: wifiOnline,
            status: moduleStatusText(wifiOnline)
        },
        voice: {
            online: voiceOnline,
            busy: Boolean(gateway.voice_busy),
            last_event_ms: lastVoiceEventMs(snapshot)
        },
        bme: {
            online: bmeOnline
        },
        csi: {
            online: csiOnline,
            last_result_ms: latestHistoryMs(snapshot)
        },
        server: {
            online: serverOnline
        }
    };
}

function adaptGatewayForOverview(snapshot, statuses) {
    const gateway = snapshot.gateway || {};
    const gatewayStatus = statuses.find(status => status.device_id === gateway.gateway_id);
    const lastSeen = gatewayStatus?.last_seen_ms ?? integerOrNull(gateway.timestamp) ?? snapshot.received_at_ms ?? null;
    return {
        ...gateway,
        gateway_id: resolveDeviceId(gateway.gateway_id || "sensair_s3_gateway_01"),
        online: Boolean(gatewayStatus?.online ?? gateway.online),
        status_source: "server",
        lastSeen,
        sta_connected: Boolean(gateway.sta_connected),
        softap_enabled: Boolean(gateway.softap_enabled ?? gateway.softap_ready),
        server_connected: Boolean(gateway.server_connected ?? gateway.server_available),
        free_heap: integerOrNull(gateway.free_heap),
        psram_heap: integerOrNull(gateway.psram_heap),
        last_error: gateway.last_error || ""
    };
}

function adaptDeviceForOverview(device, statuses, modules) {
    const serverStatus = statuses.find(item => item.device_id === device.device_id);
    const sensors = device.sensors || {};
    const voiceModule = modules.find(module => module.device_id === device.device_id && (module.module_type === "voice.turn" || module.module_type === "voice.prompt"));
    const childLastSeenMs = integerOrNull(device.child_last_seen_ms);
    const lastSeenMs = integerOrNull(device.last_seen_ms);
    const serverReceivedMs = integerOrNull(device.server_received_ms);
    const online = Boolean(device.online);

    return {
        ...device,
        device_id: resolveDeviceId(device.device_id),
        device_type: serverStatus?.device_type || device.device_type || "C5",
        room_id: serverStatus?.room_id || device.room_id || "",
        room_name: serverStatus?.room_name || device.room_name || "",
        // S3 snapshot status wins over every cached server device_status field.
        online,
        status: device.status || (online ? "online" : "offline"),
        status_source: "s3",
        offline_reason: device.offline_reason ?? null,
        link_lost: Boolean(device.link_lost),
        voice_busy: Boolean(device.voice_busy),
        child_last_seen_ms: childLastSeenMs,
        server_received_ms: serverReceivedMs,
        lastSeen: lastSeenMs,
        last_seen_ms: lastSeenMs,
        air_quality_score: integerOrNull(sensors.air_quality_score),
        air_quality_level: sensors.air_quality_level || "unknown",
        air_quality_confidence: sensors.air_quality_confidence || null,
        air_quality: isPlainObject(sensors.air_quality) ? cloneJson(sensors.air_quality) : null,
        bme_diag: isPlainObject(sensors.bme_diag) ? cloneJson(sensors.bme_diag) : null,
        baseline_state: isPlainObject(sensors.baseline_state) ? cloneJson(sensors.baseline_state) : null,
        temperature_c: numberValueOrNull(sensors.temperature ?? sensors.temperature_c),
        humidity_percent: numberValueOrNull(sensors.humidity ?? sensors.humidity_percent),
        pressure_hpa: numberValueOrNull(sensors.pressure ?? sensors.pressure_hpa),
        voice: {
            online: Boolean(voiceModule?.online),
            last_event_ms: voiceModule?.last_seen_ms ?? null
        },
        csi: normalizeSnapshotCsi(device.csi, Date.now(), {
            availableDefault: Boolean(device.csi?.available)
        })
    };
}

async function attachUnifiedOverview(dbAll, snapshot, query = {}) {
    const [statuses, modules, alarms, recentCommands, recentVoiceEvents, systemLogs] = await Promise.all([
        readDeviceStatuses(dbAll),
        readModuleStatuses(dbAll),
        listEvents(dbAll, {
            event_type: "alarm",
            limit: 50
        }),
        listEvents(dbAll, {
            event_type: "command",
            limit: 50
        }),
        listEvents(dbAll, {
            event_type: "voice",
            limit: 50
        }),
        listEvents(dbAll, {
            event_types: ["system", "device", "csi"],
            limit: 50
        })
    ]);
    const deviceId = normalizeDashboardDeviceId(query.device_id);
    const filteredSnapshot = filterSnapshotForQuery(snapshot, query);

    filteredSnapshot.gateway = adaptGatewayForOverview(filteredSnapshot, statuses);
    filteredSnapshot.modules = buildModuleSummary(filteredSnapshot, modules);
    filteredSnapshot.devices = filteredSnapshot.devices.map(device => adaptDeviceForOverview(device, statuses, modules));
    filteredSnapshot.csi = normalizeSnapshotCsi(filteredSnapshot.csi, filteredSnapshot.received_at_ms || Date.now(), {
        availableDefault: Boolean(filteredSnapshot.csi?.available)
    });
    filteredSnapshot.alarms = alarms;
    filteredSnapshot.recent_commands = recentCommands.length > 0
        ? recentCommands
        : (filteredSnapshot.recent_commands || []);
    filteredSnapshot.recent_voice_events = recentVoiceEvents.length > 0
        ? recentVoiceEvents
        : (filteredSnapshot.recent_voice_events || []);
    filteredSnapshot.system_logs = systemLogs;

    if (deviceId) {
        filteredSnapshot.recent_commands = filteredSnapshot.recent_commands.filter(item => !item.device_id || item.device_id === deviceId);
        filteredSnapshot.recent_voice_events = filteredSnapshot.recent_voice_events.filter(item => !item.device_id || item.device_id === deviceId);
        filteredSnapshot.system_logs = filteredSnapshot.system_logs.filter(item => !item.device_id || item.device_id === deviceId);
        filteredSnapshot.alarms = filteredSnapshot.alarms.filter(item => !item.device_id || item.device_id === deviceId);
    }

    return filteredSnapshot;
}

function attachRuntimeOverview(snapshot, query = {}) {
    const deviceId = normalizeDashboardDeviceId(query.device_id);
    const filteredSnapshot = filterSnapshotForQuery(snapshot, query, {
        mergeCsi: false
    });
    const gatewayId = filteredSnapshot.gateway?.gateway_id || "sensair_s3_gateway_01";
    const gatewayStatuses = [{
        device_id: gatewayId,
        online: Boolean(filteredSnapshot.gateway?.online),
        last_seen_ms: filteredSnapshot.received_at_ms || Date.now()
    }];

    filteredSnapshot.gateway = adaptGatewayForOverview(filteredSnapshot, gatewayStatuses);
    filteredSnapshot.modules = buildModuleSummary(filteredSnapshot, []);
    filteredSnapshot.devices = filteredSnapshot.devices.map(device => adaptDeviceForOverview(device, [], []));
    filteredSnapshot.csi = normalizeSnapshotCsi(filteredSnapshot.csi, filteredSnapshot.received_at_ms || Date.now(), {
        availableDefault: Boolean(filteredSnapshot.csi?.available)
    });
    filteredSnapshot.home_summary = computeHomeSummary(filteredSnapshot.devices);
    filteredSnapshot.alarms = Array.isArray(filteredSnapshot.alarms) ? filteredSnapshot.alarms : [];
    filteredSnapshot.recent_commands = Array.isArray(filteredSnapshot.recent_commands) ? filteredSnapshot.recent_commands : [];
    filteredSnapshot.recent_voice_events = Array.isArray(filteredSnapshot.recent_voice_events) ? filteredSnapshot.recent_voice_events : [];
    filteredSnapshot.system_logs = Array.isArray(filteredSnapshot.system_logs) ? filteredSnapshot.system_logs : [];

    if (deviceId) {
        filteredSnapshot.recent_commands = filteredSnapshot.recent_commands.filter(item => !item.device_id || item.device_id === deviceId);
        filteredSnapshot.recent_voice_events = filteredSnapshot.recent_voice_events.filter(item => !item.device_id || item.device_id === deviceId);
        filteredSnapshot.system_logs = filteredSnapshot.system_logs.filter(item => !item.device_id || item.device_id === deviceId);
        filteredSnapshot.alarms = filteredSnapshot.alarms.filter(item => !item.device_id || item.device_id === deviceId);
    }

    return filteredSnapshot;
}

function pickSensorDelay(row, deviceStatus, moduleStatus) {
    return {
        latest_upload_delay_ms: moduleStatus?.latest_upload_delay_ms ?? deviceStatus?.latest_upload_delay_ms ?? integerOrNull(row?.upload_delay_ms),
        avg_upload_delay_ms: moduleStatus?.avg_upload_delay_ms ?? deviceStatus?.avg_upload_delay_ms ?? null,
        delay_sample_count: moduleStatus?.delay_sample_count ?? deviceStatus?.delay_sample_count ?? 0
    };
}

function mapDashboardSensor(row, deviceStatus = null, moduleStatus = null, options = {}) {
    if (!row) {
        return null;
    }

    const airQuality = readAirQuality(row);
    const delay = pickSensorDelay(row, deviceStatus, moduleStatus);
    const deviceObserved = Boolean(deviceStatus);
    const deviceOnline = deviceObserved ? Boolean(deviceStatus.online) : null;
    const sensorOnline = moduleStatus ? Boolean(moduleStatus.online) : null;
    const online = deviceOnline === null
        ? null
        : (sensorOnline === null ? deviceOnline : (deviceOnline && sensorOnline));

    return {
        id: row.id,
        timestamp: integerOrNull(row.history_time_ms) ?? integerOrNull(row.timestamp),
        temperature: numberOrNull(row.temperature),
        humidity: numberOrNull(row.humidity),
        pressure: numberOrNull(row.pressure),
        gas_resistance: numberOrNull(row.gas_resistance),
        device_id: resolveDeviceId(row.device_id),
        sensor_id: textOrNull(row.sensor_id),
        payload_type: textOrNull(row.payload_type || "sensor.bme690"),
        esp_time_ms: integerOrNull(row.esp_time_ms),
        esp_uptime_ms: integerOrNull(row.esp_uptime_ms),
        server_recv_ms: integerOrNull(row.server_recv_ms),
        server_time_iso: textOrNull(row.server_time_iso),
        upload_delay_ms: integerOrNull(row.upload_delay_ms),
        online,
        device_online: deviceOnline,
        sensor_online: sensorOnline,
        status: deviceStatus?.status || "unknown",
        status_source: deviceStatus?.status_source || "not_observed",
        offline_reason: deviceStatus?.offline_reason ?? null,
        ...delay,
        ...airQuality,
        time_sync: options.includeTimeSync ? getTimeSyncStatus() : undefined
    };
}

async function readLatestSensorRow(dbAll, deviceId = "") {
    const params = [];
    let where = "WHERE deleted_at IS NULL AND (payload_type='sensor.bme690' OR payload_type IS NULL OR payload_type='')";
    if (deviceId) {
        where += " AND device_id=?";
        params.push(deviceId);
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

async function readSensorStatusForRow(dbAll, row) {
    if (!row?.device_id) {
        return {
            deviceStatus: null,
            moduleStatus: null
        };
    }

    const [deviceStatus, modules] = await Promise.all([
        readDeviceStatus(dbAll, row.device_id),
        readModuleStatuses(dbAll, row.device_id)
    ]);
    const moduleStatus = modules.find(module => module.module_type === "sensor.bme690") || null;

    return {
        deviceStatus,
        moduleStatus
    };
}

async function readDashboardSensorLatest(dbAll, query = {}) {
    const deviceId = normalizeDashboardDeviceId(query.device_id);
    const row = await readLatestSensorRow(dbAll, deviceId);
    if (!row) {
        return null;
    }

    const {
        deviceStatus,
        moduleStatus
    } = await readSensorStatusForRow(dbAll, row);

    return mapDashboardSensor(row, deviceStatus, moduleStatus, {
        includeTimeSync: true
    });
}

async function readDashboardSensorHistory(dbAll, query = {}, options = {}) {
    const deviceId = normalizeDashboardDeviceId(query.device_id);
    const historyQuery = options.historyQuery || readDashboardSensorHistoryQuery(query);
    if (!historyQuery.ok) {
        return historyQuery;
    }

    const params = [];
    let where = "WHERE deleted_at IS NULL AND (payload_type='sensor.bme690' OR payload_type IS NULL OR payload_type='')";
    if (deviceId) {
        where += " AND device_id=?";
        params.push(deviceId);
    }
    where += ` AND ${SENSOR_HISTORY_TIME_SQL} IS NOT NULL AND ${SENSOR_HISTORY_TIME_SQL} >= ? AND ${SENSOR_HISTORY_TIME_SQL} <= ?`;
    params.push(historyQuery.from_ms, historyQuery.to_ms);

    const countRows = await dbAll(
        `SELECT COUNT(*) AS count FROM sensor_records ${where}`,
        params
    );
    const totalCount = integerOrNull(rowFirst(countRows)?.count) || 0;
    if (totalCount <= 0) {
        return [];
    }

    let sampleStep = 1;
    if (totalCount > DASHBOARD_HISTORY_MAX_LIMIT) {
        sampleStep = Math.max(2, Math.floor((totalCount - 1) / Math.max(1, historyQuery.target_points - 1)));
        const estimatedSampleCount = Math.ceil((totalCount - 1) / sampleStep) + 1;
        if (estimatedSampleCount > DASHBOARD_HISTORY_MAX_LIMIT) {
            sampleStep += 1;
        }
    }

    const rows = await dbAll(
        `WITH matched AS (
            SELECT *, ${SENSOR_HISTORY_TIME_SQL} AS history_time_ms
            FROM sensor_records
            ${where}
        ),
        numbered AS (
            SELECT *,
                ROW_NUMBER() OVER (ORDER BY history_time_ms ASC, id ASC) AS history_row_number
            FROM matched
        )
        SELECT *
        FROM numbered
        WHERE (? = 1 OR ((history_row_number - 1) % ?) = 0 OR history_row_number = ?)
        ORDER BY history_time_ms ASC, id ASC`,
        [...params, sampleStep, sampleStep, totalCount]
    );

    return (rows || []).map(row => mapDashboardSensor(row, null, null, {
        includeTimeSync: false
    }));
}

async function readDashboardCsiHistory(dbAll, query = {}) {
    const limitResult = readDashboardLimit(query.limit);
    if (!limitResult.ok) {
        return limitResult;
    }

    const rows = await readLatestCsiMotionEvents(dbAll, {
        device_id: normalizeDashboardDeviceId(query.device_id),
        limit: limitResult.limit
    });
    return {
        events: rows.map(row => ({
            id: row.id,
            device_id: resolveDeviceId(row.device_id),
            link_id: row.link_id,
            state: row.state,
            frame_energy: numberOrNull(row.frame_energy),
            variance: numberOrNull(row.variance),
            rssi: integerOrNull(row.rssi),
            motion_score: numberOrNull(row.motion_score),
            timestamp: integerOrNull(row.timestamp),
            gateway_id: row.gateway_id || "",
            server_recv_ms: integerOrNull(row.server_recv_ms),
            server_time_iso: row.server_time_iso || ""
        }))
    };
}

async function readDashboardAsrLatest(dbAll) {
    const rows = await dbAll(
        "SELECT * FROM asr_records WHERE deleted_at IS NULL ORDER BY id DESC LIMIT 1"
    );
    const row = rowFirst(rows);
    if (!row) {
        return null;
    }

    return {
        id: row.id,
        timestamp: integerOrNull(row.timestamp),
        text: row.text || ""
    };
}

async function readDashboardLlmLatest(dbAll) {
    const rows = await dbAll(
        "SELECT * FROM llm_records WHERE deleted_at IS NULL ORDER BY id DESC LIMIT 1"
    );
    const row = rowFirst(rows);
    if (!row) {
        return null;
    }

    return {
        id: row.id,
        timestamp: integerOrNull(row.timestamp),
        prompt: row.prompt || "",
        response: row.response || ""
    };
}

function readDashboardTimeStatus() {
    const status = getTimeSyncStatus();
    return {
        server_time_ms: status.server_time_ms,
        server_time_iso: status.server_time_iso,
        latest_ping: status.latest_ping,
        time_sync: status
    };
}

async function readDashboardDeviceStatus(dbAll, query = {}) {
    const deviceId = normalizeDashboardDeviceId(query.device_id);
    const status = await readDeviceStatus(dbAll, deviceId);
    return mapDashboardDeviceStatus(status, deviceId);
}

async function readDashboardModulesStatus(dbAll, query = {}) {
    const deviceId = normalizeDashboardDeviceId(query.device_id);
    const modules = await readModuleStatuses(dbAll, deviceId);
    return {
        modules: modules.map(mapDashboardModuleStatus)
    };
}

async function readDashboardOverview(dbAll, query = {}, options = {}) {
    const runtimeSnapshot = options.runtimeCache?.readDashboardOverviewSnapshot?.();
    const logger = options.logger || console;
    if (runtimeSnapshot) {
        logger.info("[CACHE_HIT] path=/api/dashboard/v1/overview source=runtimeStateCache");
        return attachRuntimeOverview(runtimeSnapshot, query);
    }

    logger.info("[CACHE_MISS] path=/api/dashboard/v1/overview source=runtimeStateCache fallback=sqlite");
    const restoredSnapshot = await readLatestDashboardSnapshot(dbAll);
    if (restoredSnapshot) {
        return attachUnifiedOverview(dbAll, restoredSnapshot, query);
    }

    const [
        sensorLatest,
        deviceStatus,
        history
    ] = await Promise.all([
        readDashboardSensorLatest(dbAll, query),
        readDashboardDeviceStatus(dbAll, query),
        readDashboardSensorHistory(dbAll, {
            device_id: query.device_id,
            limit: DASHBOARD_HISTORY_DEFAULT_LIMIT
        })
    ]);

    const deviceId = sensorLatest?.device_id ||
        deviceStatus?.device_id ||
        normalizeDashboardDeviceId(query.device_id);
    const devices = deviceId ? [{
        device_id: deviceId,
        local_id: null,
        name: "",
        room_name: "unassigned",
        online: sensorLatest?.online ?? deviceStatus?.online ?? null,
        wifi_rssi: null,
        timestamp: sensorLatest?.timestamp ?? deviceStatus?.last_seen_ms ?? Date.now(),
        sensors: sensorLatest ? {
            temperature: sensorLatest.temperature,
            humidity: sensorLatest.humidity,
            pressure: sensorLatest.pressure,
            gas_resistance: sensorLatest.gas_resistance,
            air_quality_score: sensorLatest.air_quality_score,
            air_quality_level: sensorLatest.air_quality_level,
            air_quality_confidence: sensorLatest.air_quality_confidence,
            air_quality_source: sensorLatest.air_quality_source,
            air_quality: sensorLatest.air_quality,
            ...(sensorLatest.bme_diag ? {
                bme_diag: sensorLatest.bme_diag
            } : {}),
            ...(sensorLatest.baseline_state ? {
                baseline_state: sensorLatest.baseline_state
            } : {})
        } : null,
        csi: normalizeSnapshotCsi(null, Date.now(), {
            availableDefault: false
        }),
        appliances: mockAppliances()
    }] : [];

    const snapshot = {
        gateway: {
            gateway_id: "sensair_s3_gateway_01",
            online: false,
            softap_ready: false,
            softap_enabled: false,
            sta_connected: false,
            server_available: true,
            server_connected: true,
            voice_busy: false,
            free_heap: null,
            psram_heap: null,
            last_error: latestDashboardSnapshot ? "" : "no_gateway_snapshot",
            timestamp: Date.now()
        },
        devices,
        home_summary: computeHomeSummary(devices),
        history: Array.isArray(history) ? history.map(row => ({
            device_id: resolveDeviceId(row.device_id),
            sensor_type: "bme690",
            timestamp: row.timestamp,
            temperature: row.temperature,
            humidity: row.humidity,
            pressure: row.pressure,
            gas_resistance: row.gas_resistance,
            air_quality_score: row.air_quality_score,
            air_quality_level: row.air_quality_level
        })) : [],
        recent_voice_events: [],
        recent_commands: [],
        csi: normalizeSnapshotCsi(null, Date.now(), {
            availableDefault: false
        })
    };
    mergeCsiMotionIntoSnapshot(snapshot, normalizeDashboardDeviceId(query.device_id));
    return attachUnifiedOverview(dbAll, snapshot, query);
}

module.exports = {
    CSI_MOTION_PAYLOAD_TYPE,
    DASHBOARD_HISTORY_DEFAULT_LIMIT,
    DASHBOARD_HISTORY_MAX_LIMIT,
    DASHBOARD_SNAPSHOT_PAYLOAD_TYPE,
    ingestDashboardSnapshot,
    mapDashboardSensor,
    normalizeSnapshotCsi,
    persistDashboardSnapshot,
    prepareDashboardSnapshot,
    recordCsiMotion,
    readDashboardCsiHistory,
    readDashboardSnapshotHistory,
    readDashboardAsrLatest,
    readDashboardDeviceStatus,
    readDashboardLimit,
    readLatestDashboardSnapshot,
    readDashboardLlmLatest,
    readDashboardModulesStatus,
    readDashboardOverview,
    readDashboardSensorHistoryQuery,
    readDashboardSensorHistory,
    readDashboardSensorLatest,
    readDashboardTimeStatus
};
