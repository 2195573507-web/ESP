const {
    trimText
} = require("./deviceMetadata");
const {
    resolveDeviceId
} = require("./deviceIdResolver");

const MIN_PLAUSIBLE_UNIX_MS = Date.UTC(2000, 0, 1);

const state = {
    initialized_at_ms: Date.now(),
    gateway: null,
    gateway_state: null,
    devices: new Map(),
    csi_by_device: new Map(),
    latest_csi: null,
    latest_snapshot: null,
    history_by_device: new Map()
};

function cloneJson(value) {
    return value === undefined ? undefined : JSON.parse(JSON.stringify(value));
}

function isPlainObject(value) {
    return value && typeof value === "object" && !Array.isArray(value);
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

function defaultGateway(serverRecvMs = Date.now()) {
    return {
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
        last_error: "no_gateway_snapshot",
        timestamp: serverRecvMs
    };
}

function makeGatewayState(gateway, serverRecvMs = Date.now()) {
    const source = isPlainObject(gateway) ? gateway : defaultGateway(serverRecvMs);
    const lastSeen = integerOrNull(source.last_seen_ms ?? source.lastSeen ?? source.timestamp) ?? serverRecvMs;
    return {
        gateway_id: resolveDeviceId(source.gateway_id || "sensair_s3_gateway_01"),
        online: booleanValue(source.online, false),
        last_seen: lastSeen,
        last_seen_ms: lastSeen,
        network: {
            softap_ready: booleanValue(source.softap_ready, false),
            softap_enabled: booleanValue(source.softap_enabled ?? source.softap_ready, false),
            sta_connected: booleanValue(source.sta_connected, false),
            server_available: booleanValue(source.server_available, false),
            server_connected: booleanValue(source.server_connected ?? source.server_available, false)
        }
    };
}

function sensorFromBmePrepared(prepared) {
    const readings = prepared?.readings || {};
    const airQuality = prepared?.airQuality || {};
    const airQualityCompatibility = prepared?.airQualityCompatibility || airQuality;
    const bmeDiag = prepared?.bmeDiag;
    const baselineState = prepared?.baselineState;
    return {
        temperature: readings.temperature_c,
        humidity: readings.humidity_percent,
        pressure: readings.pressure_hpa,
        gas_resistance: readings.gas_resistance_ohm,
        air_quality_score: airQualityCompatibility.air_quality_score,
        air_quality_level: airQualityCompatibility.air_quality_level,
        air_quality_confidence: airQualityCompatibility.air_quality_confidence,
        air_quality_source: airQualityCompatibility.air_quality_source,
        air_quality: cloneJson(airQuality),
        ...(isPlainObject(bmeDiag) ? {
            bme_diag: cloneJson(bmeDiag)
        } : {}),
        ...(isPlainObject(baselineState) ? {
            baseline_state: cloneJson(baselineState)
        } : {})
    };
}

function normalizeCsiRecord(record, serverRecvMs = Date.now()) {
    if (!record) {
        return null;
    }

    const stateText = trimText(record.state || record.fused_state || "IDLE", 16).toUpperCase() || "IDLE";
    const confidence = numberOrNull(record.confidence);
    return {
        device_id: resolveDeviceId(record.device_id),
        link_id: trimText(record.link_id || "fused", 64),
        link_state: stateText,
        state: stateText,
        fused_state: stateText,
        available: true,
        frame_energy: numberOrNull(record.frame_energy),
        variance: numberOrNull(record.variance),
        rssi: integerOrNull(record.rssi),
        motion_score: numberOrNull(record.motion_score),
        confidence,
        timestamp: integerOrNull(record.timestamp ?? record.timestamp_ms) || serverRecvMs,
        server_recv_ms: integerOrNull(record.server_recv_ms) || serverRecvMs
    };
}

function upsertDevice(device) {
    if (!isPlainObject(device)) {
        return null;
    }

    const deviceId = resolveDeviceId(device.device_id);
    if (!deviceId) {
        return null;
    }

    const previous = state.devices.get(deviceId) || {};
    const merged = {
        ...previous,
        ...cloneJson(device),
        device_id: deviceId,
        status_source: trimText(device.status_source, 16) || previous.status_source || null,
        online: booleanValue(device.online, booleanValue(previous.online, false)),
        status: trimText(device.status, 40) || previous.status || (booleanValue(device.online, false) ? "online" : "offline"),
        last_seen_ms: integerOrNull(device.last_seen_ms ?? device.lastSeen) ?? previous.last_seen_ms ?? null,
        last_seen: integerOrNull(device.last_seen ?? device.last_seen_ms ?? device.lastSeen) ?? previous.last_seen ?? null,
        sensors: isPlainObject(device.sensors) ? cloneJson(device.sensors) : previous.sensors || null,
        csi: isPlainObject(device.csi) ? cloneJson(device.csi) : previous.csi || null
    };

    state.devices.set(deviceId, merged);
    return cloneJson(merged);
}

function canonicalizeSnapshotForCache(snapshot) {
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

function updateDashboardSnapshot(snapshot, options = {}) {
    if (!isPlainObject(snapshot)) {
        return null;
    }

    const serverRecvMs = integerOrNull(options.serverRecvMs ?? snapshot.received_at_ms) ?? Date.now();
    const normalized = canonicalizeSnapshotForCache(snapshot);
    state.latest_snapshot = normalized;
    state.gateway = cloneJson(normalized.gateway || defaultGateway(serverRecvMs));
    state.gateway_state = makeGatewayState(state.gateway, serverRecvMs);

    for (const device of normalized.devices || []) {
        upsertDevice({
            ...device,
            status_source: "s3"
        });
        if (Array.isArray(normalized.history)) {
            const deviceHistory = normalized.history.filter(item => item.device_id === device.device_id);
            if (deviceHistory.length > 0) {
                state.history_by_device.set(device.device_id, cloneJson(deviceHistory));
            }
        }
    }

    if (normalized.csi?.available) {
        updateCsiMotion({
            ...normalized.csi,
            device_id: normalized.csi.device_id || state.gateway_state?.gateway_id || ""
        }, {
            serverRecvMs
        });
    }

    return cloneJson(normalized);
}

function updateBmeSensor(prepared, options = {}) {
    const metadata = prepared?.metadata || {};
    const deviceId = resolveDeviceId(metadata.device_id);
    if (!deviceId) {
        return null;
    }

    const serverRecvMs = integerOrNull(options.serverRecvMs ?? metadata.server_recv_ms) ?? Date.now();
    const sensor = sensorFromBmePrepared(prepared);
    const previous = state.devices.get(deviceId) || {};
    const device = upsertDevice({
        ...previous,
        device_id: deviceId,
        device_type: metadata.device_type || previous.device_type || "C5",
        room_id: metadata.room_id || previous.room_id || "",
        room_name: metadata.room_name || previous.room_name || "unassigned",
        online: previous.status_source === "s3" ? previous.online : true,
        status: previous.status_source === "s3" ? previous.status : "online",
        last_seen_ms: previous.status_source === "s3" ? previous.last_seen_ms : serverRecvMs,
        server_received_ms: serverRecvMs,
        timestamp: serverRecvMs,
        sensors: sensor
    });

    const history = state.history_by_device.get(deviceId) || [];
    history.push({
        device_id: deviceId,
        sensor_type: "bme690",
        timestamp: serverRecvMs,
        temperature: sensor.temperature,
        humidity: sensor.humidity,
        pressure: sensor.pressure,
        gas_resistance: sensor.gas_resistance,
        air_quality_score: sensor.air_quality_score,
        air_quality_level: sensor.air_quality_level
    });
    state.history_by_device.set(deviceId, history.slice(-50));

    return cloneJson(device);
}

function updateCsiMotion(record, options = {}) {
    const serverRecvMs = integerOrNull(options.serverRecvMs ?? record?.server_recv_ms) ?? Date.now();
    const csi = normalizeCsiRecord(record, serverRecvMs);
    if (!csi) {
        return null;
    }

    const deviceId = csi.device_id;
    if (deviceId) {
        state.csi_by_device.set(deviceId, cloneJson(csi));
        const previous = state.devices.get(deviceId);
        if (previous) {
            upsertDevice({
                ...previous,
                csi
            });
        }
    }

    if (csi.link_id === "fused" || !state.latest_csi) {
        state.latest_csi = cloneJson(csi);
    }

    return cloneJson(csi);
}

function buildSnapshotFromCache() {
    if (state.latest_snapshot) {
        const snapshot = cloneJson(state.latest_snapshot);
        const devices = Array.from(state.devices.values()).map(cloneJson);
        if (devices.length > 0) {
            snapshot.devices = devices;
        }
        if (state.latest_csi) {
            snapshot.csi = cloneJson(state.latest_csi);
        }
        return snapshot;
    }

    const devices = Array.from(state.devices.values()).map(cloneJson);
    if (devices.length === 0 && !state.latest_csi && !state.gateway) {
        return null;
    }

    const history = [];
    for (const items of state.history_by_device.values()) {
        history.push(...items);
    }

    return {
        gateway: cloneJson(state.gateway || defaultGateway()),
        devices,
        home_summary: null,
        history: history
            .sort((a, b) => (integerOrNull(a.timestamp) || 0) - (integerOrNull(b.timestamp) || 0))
            .slice(-50),
        recent_voice_events: [],
        recent_commands: [],
        csi: cloneJson(state.latest_csi || {
            device_id: "",
            link_id: "fused",
            state: "IDLE",
            available: false,
            frame_energy: null,
            variance: null,
            rssi: null,
            motion_score: null,
            confidence: null,
            timestamp: null
        }),
        received_at_ms: Date.now(),
        source: "runtime_cache"
    };
}

function readDashboardOverviewSnapshot() {
    return buildSnapshotFromCache();
}

function getRuntimeStateCacheStats() {
    return {
        initialized_at_ms: state.initialized_at_ms,
        has_gateway: Boolean(state.gateway),
        has_snapshot: Boolean(state.latest_snapshot),
        device_count: state.devices.size,
        csi_count: state.csi_by_device.size,
        has_latest_csi: Boolean(state.latest_csi)
    };
}

function resetRuntimeStateCache() {
    state.initialized_at_ms = Date.now();
    state.gateway = null;
    state.gateway_state = null;
    state.devices.clear();
    state.csi_by_device.clear();
    state.latest_csi = null;
    state.latest_snapshot = null;
    state.history_by_device.clear();
}

function initRuntimeStateCache() {
    state.initialized_at_ms = Date.now();
    return getRuntimeStateCacheStats();
}

module.exports = {
    getRuntimeStateCacheStats,
    initRuntimeStateCache,
    readDashboardOverviewSnapshot,
    resetRuntimeStateCache,
    updateBmeSensor,
    updateCsiMotion,
    updateDashboardSnapshot
};
