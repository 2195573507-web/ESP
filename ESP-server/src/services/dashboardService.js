const {
    getTimeSyncStatus
} = require("../../server-time-sync/timeSync");
const {
    readDeviceStatuses,
    readDeviceStatus,
    readModuleStatuses,
    refreshDeviceActivity
} = require("./deviceStatusService");
const {
    readDeviceMetadata,
    toIntegerOrNull,
    trimText
} = require("./deviceMetadata");
const {
    makeSnapshotId
} = require("../db/dashboardSnapshots");
const {
    listEvents,
    recordEvent
} = require("./eventLogService");

const DASHBOARD_HISTORY_DEFAULT_LIMIT = 50;
const DASHBOARD_HISTORY_MAX_LIMIT = 500;
const DASHBOARD_SNAPSHOT_PAYLOAD_TYPE = "gateway.dashboard_snapshot";
const CSI_MOTION_PAYLOAD_TYPE = "csi.motion";
const CSI_OCCUPANCY_STATES = new Set(["unknown", "vacant", "occupied"]);

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
    return trimText(value, 128);
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
        gateway_id: trimText(source.gateway_id || "sensair_s3_gateway_01", 128),
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

function applyTrustedGatewayId(snapshot, trustedGatewayId) {
    const gatewayId = trimText(trustedGatewayId, 128);
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

    return {
        temperature: numberValueOrNull(sensors.temperature ?? sensors.temperature_c),
        humidity: numberValueOrNull(sensors.humidity ?? sensors.humidity_percent),
        pressure: numberValueOrNull(sensors.pressure ?? sensors.pressure_hpa),
        gas_resistance: numberValueOrNull(sensors.gas_resistance ?? sensors.gas_resistance_ohm),
        air_quality_score: integerOrNull(sensors.air_quality_score),
        air_quality_level: trimText(sensors.air_quality_level, 40) || "unknown",
        air_quality_source: trimText(sensors.air_quality_source, 40) || "s3_mapped"
    };
}

function normalizeOccupancyState(value) {
    const state = trimText(value, 16).toLowerCase();
    return CSI_OCCUPANCY_STATES.has(state) ? state : "unknown";
}

function clampMotionScore(value) {
    const numeric = numberValueOrNull(value);
    if (numeric === null) {
        return null;
    }

    return Math.min(Math.max(numeric, 0), 1);
}

function normalizeSnapshotOccupancy(occupancy, serverRecvMs, options = {}) {
    const source = isPlainObject(occupancy) ? occupancy : {};
    const hasSource = isPlainObject(occupancy);
    const available = booleanValue(source.available, options.availableDefault ?? hasSource);
    const state = normalizeOccupancyState(source.state);

    if (!available) {
        return {
            state: "unknown",
            available: false,
            motion_score: null,
            variance: null,
            rssi: null,
            sample_count: 0,
            updated_at: null
        };
    }

    const sampleCount = integerOrNull(source.sample_count);
    return {
        state,
        available: true,
        motion_score: clampMotionScore(source.motion_score),
        variance: numberValueOrNull(source.variance),
        rssi: integerOrNull(source.rssi),
        sample_count: sampleCount === null ? 0 : Math.max(0, sampleCount),
        updated_at: integerOrNull(source.updated_at) || serverRecvMs
    };
}

function normalizeSnapshotDevice(device, serverRecvMs) {
    if (!isPlainObject(device)) {
        return null;
    }

    const deviceId = trimText(device.device_id, 128);
    if (!deviceId) {
        return null;
    }

    return {
        device_id: deviceId,
        local_id: integerOrNull(device.local_id),
        device_type: trimText(device.device_type, 40) || "C5",
        name: trimText(device.name || device.alias, 128),
        room_id: trimText(device.room_id, 128),
        room_name: trimText(device.room_name || device.room_id || "unassigned", 128),
        online: booleanValue(device.online, false),
        wifi_rssi: integerOrNull(device.wifi_rssi),
        timestamp: integerOrNull(device.timestamp) || serverRecvMs,
        sensors: normalizeSnapshotSensors(device.sensors),
        occupancy: normalizeSnapshotOccupancy(device.occupancy, serverRecvMs),
        appliances: normalizeAppliances(device.appliances)
    };
}

function normalizeSnapshotHistoryItem(item, serverRecvMs) {
    if (!isPlainObject(item)) {
        return null;
    }

    const deviceId = trimText(item.device_id, 128);
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
    const deviceId = trimText(item.device_id, 128);
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
    const deviceId = trimText(item.device_id, 128);
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
        avg_temperature: null,
        avg_humidity: null,
        avg_air_quality: null
    };
    let tempSum = 0;
    let humiditySum = 0;
    let airSum = 0;
    let count = 0;

    for (const device of devices) {
        if (device.online) {
            summary.online_device_count += 1;
        } else {
            summary.offline_device_count += 1;
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

    const devices = (Array.isArray(body.devices) ? body.devices : [])
        .map(device => normalizeSnapshotDevice(device, serverRecvMs))
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
            gateway: normalizeSnapshotGateway(body.gateway, serverRecvMs),
            devices,
            home_summary: normalizeHomeSummary(body.home_summary, devices),
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

function shouldApplyCsiMotion(existingOccupancy, nextOccupancy) {
    const existingUpdatedAt = integerOrNull(existingOccupancy?.updated_at) || 0;
    const nextUpdatedAt = integerOrNull(nextOccupancy?.updated_at) || 0;
    return nextUpdatedAt >= existingUpdatedAt;
}

function deviceFromCsiMotion(record) {
    return {
        device_id: record.device_id,
        local_id: record.local_id,
        name: record.name,
        room_name: record.room_name,
        online: true,
        wifi_rssi: record.occupancy.rssi,
        timestamp: record.occupancy.updated_at,
        sensors: null,
        occupancy: cloneJson(record.occupancy),
        appliances: mockAppliances()
    };
}

function mergeCsiMotionIntoSnapshot(snapshot, onlyDeviceId = "") {
    if (!snapshot || !Array.isArray(snapshot.devices)) {
        return snapshot;
    }

    for (const record of latestCsiMotionByDevice.values()) {
        if (onlyDeviceId && record.device_id !== onlyDeviceId) {
            continue;
        }

        const device = snapshot.devices.find(item => item.device_id === record.device_id);
        if (!device) {
            snapshot.devices.push(deviceFromCsiMotion(record));
            continue;
        }

        if (shouldApplyCsiMotion(device.occupancy, record.occupancy)) {
            device.occupancy = cloneJson(record.occupancy);
            device.timestamp = Math.max(integerOrNull(device.timestamp) || 0, record.occupancy.updated_at);
            if (device.wifi_rssi === null || device.wifi_rssi === undefined) {
                device.wifi_rssi = record.occupancy.rssi;
            }
        }
    }

    snapshot.home_summary = computeHomeSummary(snapshot.devices);
    return snapshot;
}

function filterSnapshotForQuery(snapshot, query = {}) {
    const deviceId = normalizeDashboardDeviceId(query.device_id);
    const cloned = cloneJson(snapshot);
    mergeCsiMotionIntoSnapshot(cloned, deviceId);
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
    const payload = snapshotRowToPayload(rows[0]);
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
        gateway_id: row.gateway_id,
        server_recv_ms: integerOrNull(row.server_recv_ms),
        schema_version: integerOrNull(row.schema_version),
        payload: snapshotRowToPayload(row),
        created_at: row.created_at || ""
    }));
}

function recordCsiMotion(record, options = {}) {
    const serverRecvMs = Number.isFinite(options.serverRecvMs) ? options.serverRecvMs : Date.now();
    const deviceId = normalizeDashboardDeviceId(record?.device_id);
    if (!deviceId) {
        return null;
    }

    const occupancy = normalizeSnapshotOccupancy(record?.occupancy, serverRecvMs, {
        availableDefault: true
    });
    occupancy.available = true;

    const normalized = {
        device_id: deviceId,
        local_id: integerOrNull(record?.local_id),
        name: trimText(record?.name || record?.alias, 128),
        room_name: trimText(record?.room_name || record?.room_id || "unassigned", 128),
        occupancy,
        received_at_ms: serverRecvMs,
        payload_type: CSI_MOTION_PAYLOAD_TYPE
    };

    latestCsiMotionByDevice.set(deviceId, normalized);
    return cloneJson(normalized);
}

async function ingestDashboardSnapshot(body, options = {}) {
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

    latestDashboardSnapshot = applyTrustedGatewayId(validation.snapshot, options.trustedGatewayId);
    const dbRun = options.dbRun;
    const dbAll = options.dbAll;
    const gatewayId = latestDashboardSnapshot.gateway.gateway_id;
    const snapshotId = makeSnapshotId(gatewayId, serverRecvMs);

    if (typeof dbRun === "function") {
        await dbRun(
            `INSERT INTO dashboard_snapshots
            (snapshot_id,gateway_id,server_recv_ms,payload_json,schema_version,created_at,updated_at)
            VALUES(?,?,?,?,?,?,?)`,
            [
                snapshotId,
                gatewayId,
                serverRecvMs,
                JSON.stringify(stripMockAppliancesForStorage(latestDashboardSnapshot)),
                Number(body.schema_version) || 2,
                new Date(serverRecvMs).toISOString(),
                new Date(serverRecvMs).toISOString()
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
            headers: options.headers,
            payloadType: DASHBOARD_SNAPSHOT_PAYLOAD_TYPE,
            serverRecvMs
        });
        await refreshDeviceActivity(dbRun, dbAll, gatewayMetadata, DASHBOARD_SNAPSHOT_PAYLOAD_TYPE);

        for (const device of latestDashboardSnapshot.devices) {
            const metadata = readDeviceMetadata({
                body: {
                    device_id: device.device_id,
                    device_type: device.device_type || "C5",
                    room_id: device.room_id,
                    room_name: device.room_name,
                    payload_type: DASHBOARD_SNAPSHOT_PAYLOAD_TYPE,
                    timestamp: device.timestamp
                },
                headers: options.headers,
                payloadType: DASHBOARD_SNAPSHOT_PAYLOAD_TYPE,
                serverRecvMs
            });
            await refreshDeviceActivity(dbRun, dbAll, metadata, DASHBOARD_SNAPSHOT_PAYLOAD_TYPE);
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
                device_count: latestDashboardSnapshot.devices.length,
                history_count: latestDashboardSnapshot.history.length
            },
            source: "dashboard_snapshot",
            server_recv_ms: serverRecvMs
        });

        for (const voiceEvent of latestDashboardSnapshot.recent_voice_events || []) {
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

        for (const commandEvent of latestDashboardSnapshot.recent_commands || []) {
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
    }

    return {
        ok: true,
        status: 202,
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

function readAirQuality(row) {
    const parsed = parseJsonObject(row?.air_quality_json, {});
    const score = row?.air_quality_score ?? parsed.air_quality_score ?? null;
    const level = row?.air_quality_level || parsed.air_quality_level || null;
    const confidence = row?.air_quality_confidence || parsed.air_quality_confidence || null;
    const source = row?.air_quality_source || parsed.air_quality_source || null;

    return {
        air_quality: {
            air_quality_score: score,
            air_quality_level: level,
            air_quality_confidence: confidence,
            air_quality_source: source
        },
        air_quality_score: score,
        air_quality_level: level,
        air_quality_confidence: confidence,
        air_quality_source: source
    };
}

function mapDashboardDeviceStatus(status, fallbackDeviceId = "") {
    return {
        device_id: status?.device_id || fallbackDeviceId || null,
        online: Boolean(status?.online),
        device_online: Boolean(status?.device_online),
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
        device_id: moduleStatus?.device_id || null,
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
        (Array.isArray(snapshot.devices) && snapshot.devices.some(device => Boolean(device.occupancy?.available)));
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
        gateway_id: gateway.gateway_id || "sensair_s3_gateway_01",
        online: Boolean(gatewayStatus?.online ?? gateway.online),
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
    const status = statuses.find(item => item.device_id === device.device_id);
    const sensors = device.sensors || {};
    const csiModule = modules.find(module => module.device_id === device.device_id && module.module_type === "csi.motion");
    const voiceModule = modules.find(module => module.device_id === device.device_id && (module.module_type === "voice.turn" || module.module_type === "voice.prompt"));
    const lastSeen = status?.last_seen_ms ?? integerOrNull(device.timestamp);

    return {
        ...device,
        device_id: device.device_id,
        device_type: status?.device_type || device.device_type || "C5",
        room_id: status?.room_id || device.room_id || "",
        room_name: status?.room_name || device.room_name || "",
        online: Boolean(status?.online ?? device.online),
        lastSeen,
        last_seen_ms: lastSeen,
        air_quality_score: integerOrNull(sensors.air_quality_score),
        air_quality_level: sensors.air_quality_level || "unknown",
        temperature_c: numberValueOrNull(sensors.temperature ?? sensors.temperature_c),
        humidity_percent: numberValueOrNull(sensors.humidity ?? sensors.humidity_percent),
        pressure_hpa: numberValueOrNull(sensors.pressure ?? sensors.pressure_hpa),
        voice: {
            online: Boolean(voiceModule?.online),
            last_event_ms: voiceModule?.last_seen_ms ?? null
        },
        csi: {
            online: Boolean(csiModule?.online ?? device.occupancy?.available),
            last_result_ms: csiModule?.last_seen_ms ?? integerOrNull(device.occupancy?.updated_at),
            occupancy: device.occupancy || null
        }
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
    const deviceOnline = Boolean(deviceStatus?.online);
    const sensorOnline = Boolean(moduleStatus?.online);

    return {
        id: row.id,
        timestamp: integerOrNull(row.timestamp),
        temperature: numberOrNull(row.temperature),
        humidity: numberOrNull(row.humidity),
        pressure: numberOrNull(row.pressure),
        gas_resistance: numberOrNull(row.gas_resistance),
        device_id: textOrNull(row.device_id),
        sensor_id: textOrNull(row.sensor_id),
        payload_type: textOrNull(row.payload_type || "sensor.bme690"),
        esp_time_ms: integerOrNull(row.esp_time_ms),
        esp_uptime_ms: integerOrNull(row.esp_uptime_ms),
        server_recv_ms: integerOrNull(row.server_recv_ms),
        server_time_iso: textOrNull(row.server_time_iso),
        upload_delay_ms: integerOrNull(row.upload_delay_ms),
        online: deviceOnline && sensorOnline,
        device_online: deviceOnline,
        sensor_online: sensorOnline,
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

async function readDashboardSensorHistory(dbAll, query = {}) {
    const deviceId = normalizeDashboardDeviceId(query.device_id);
    const limitResult = readDashboardLimit(query.limit);
    if (!limitResult.ok) {
        return limitResult;
    }

    const params = [];
    let where = "WHERE deleted_at IS NULL AND (payload_type='sensor.bme690' OR payload_type IS NULL OR payload_type='')";
    if (deviceId) {
        where += " AND device_id=?";
        params.push(deviceId);
    }
    params.push(limitResult.limit);

    const rows = await dbAll(
        `SELECT * FROM (
            SELECT * FROM sensor_records
            ${where}
            ORDER BY COALESCE(server_recv_ms, timestamp, id) DESC, id DESC
            LIMIT ?
        ) ORDER BY COALESCE(server_recv_ms, timestamp, id) ASC, id ASC`,
        params
    );

    return (rows || []).map(row => mapDashboardSensor(row, null, null, {
        includeTimeSync: false
    }));
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

async function readDashboardOverview(dbAll, query = {}) {
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
        online: Boolean(sensorLatest?.online ?? deviceStatus?.online),
        wifi_rssi: null,
        timestamp: sensorLatest?.timestamp ?? deviceStatus?.last_seen_ms ?? Date.now(),
        sensors: sensorLatest ? {
            temperature: sensorLatest.temperature,
            humidity: sensorLatest.humidity,
            pressure: sensorLatest.pressure,
            gas_resistance: sensorLatest.gas_resistance,
            air_quality_score: sensorLatest.air_quality_score,
            air_quality_level: sensorLatest.air_quality_level,
            air_quality_source: sensorLatest.air_quality_source
        } : null,
        occupancy: normalizeSnapshotOccupancy(null, Date.now(), {
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
            device_id: row.device_id,
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
        recent_commands: []
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
    normalizeSnapshotOccupancy,
    recordCsiMotion,
    readDashboardSnapshotHistory,
    readDashboardAsrLatest,
    readDashboardDeviceStatus,
    readDashboardLimit,
    readLatestDashboardSnapshot,
    readDashboardLlmLatest,
    readDashboardModulesStatus,
    readDashboardOverview,
    readDashboardSensorHistory,
    readDashboardSensorLatest,
    readDashboardTimeStatus
};
