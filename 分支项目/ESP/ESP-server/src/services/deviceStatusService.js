const {
    runUpdateThenInsert
} = require("../db/upsert");
const {
    readPositiveInteger
} = require("../utils/env");
const {
    isValidUploadDelay,
    trimText
} = require("./deviceMetadata");
const {
    recordEvent
} = require("./eventLogService");
const {
    resolveDeviceId
} = require("./deviceIdResolver");

const DEFAULT_DEVICE_OFFLINE_TIMEOUT_MS = 30000;
const MODULE_ONLINE_THRESHOLD_MS = 30000;
const STATUS_SOURCE_SERVER = "server";
const STATUS_SOURCE_S3 = "s3";

function readDeviceOfflineTimeoutMs() {
    return readPositiveInteger(
        process.env.DEVICE_OFFLINE_TIMEOUT_MS,
        DEFAULT_DEVICE_OFFLINE_TIMEOUT_MS
    );
}

function nowIso() {
    return new Date().toISOString();
}

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

function normalizeStatusSource(value) {
    return trimText(value, 16).toLowerCase() === STATUS_SOURCE_S3
        ? STATUS_SOURCE_S3
        : STATUS_SOURCE_SERVER;
}

function isS3ChildStatus(row) {
    return normalizeStatusSource(row?.status_source) === STATUS_SOURCE_S3;
}

function isoForTimestamp(timestampMs, fallback = "") {
    const normalized = integerOrNull(timestampMs);
    return normalized === null ? fallback : new Date(normalized).toISOString();
}

async function recordStatusEvent(dbRun, event) {
    try {
        await recordEvent(dbRun, event);
    } catch (_) {
        // Status refresh must not fail the device upload path because logging failed.
    }
}

async function recordClockSkewWarning(dbRun, metadata, deviceId) {
    if (!metadata.clock_skew_warning) {
        return;
    }

    await recordStatusEvent(dbRun, {
        event_type: "system",
        event_name: "system_log_created",
        device_id: deviceId,
        severity: "warning",
        message: metadata.clock_skew_warning.message,
        payload: metadata.clock_skew_warning,
        source: "device_clock",
        server_recv_ms: metadata.server_recv_ms
    });
}

function computeDelayStats(row, delayMs) {
    const previousCount = Math.max(0, integerOrNull(row?.delay_sample_count) || 0);
    const previousAvg = numberOrNull(row?.avg_upload_delay_ms);
    const previousLatest = integerOrNull(row?.latest_upload_delay_ms);
    if (!isValidUploadDelay(delayMs)) {
        return {
            latest_upload_delay_ms: previousLatest,
            avg_upload_delay_ms: previousAvg === null ? null : Math.round(previousAvg),
            delay_sample_count: previousCount
        };
    }

    const nextCount = previousCount + 1;
    const nextAvg = previousCount > 0 && previousAvg !== null
        ? Math.round(((previousAvg * previousCount) + delayMs) / nextCount)
        : Math.round(delayMs);

    return {
        latest_upload_delay_ms: Math.round(delayMs),
        avg_upload_delay_ms: nextAvg,
        delay_sample_count: nextCount
    };
}

async function getDeviceStatusRow(dbAll, deviceId) {
    const resolvedDeviceId = resolveDeviceId(deviceId);
    const rows = await dbAll(
        "SELECT * FROM device_status WHERE device_id=? AND deleted_at IS NULL LIMIT 1",
        [resolvedDeviceId]
    );
    return rowFirst(rows);
}

async function getModuleStatusRow(dbAll, deviceId, moduleType) {
    const resolvedDeviceId = resolveDeviceId(deviceId);
    const rows = await dbAll(
        "SELECT * FROM device_module_status WHERE device_id=? AND module_type=? AND deleted_at IS NULL LIMIT 1",
        [resolvedDeviceId, moduleType]
    );
    return rowFirst(rows);
}

function computeRebootCount(row, metadata) {
    const previousUptime = integerOrNull(row?.last_esp_uptime_ms);
    const currentUptime = integerOrNull(metadata.esp_uptime_ms);
    const previousCount = Math.max(0, integerOrNull(row?.reboot_count) || 0);
    if (previousUptime !== null && currentUptime !== null && currentUptime + 5000 < previousUptime) {
        return previousCount + 1;
    }

    return previousCount;
}

function computeOnlineState(row, nowMs = Date.now()) {
    const statusSource = normalizeStatusSource(row?.status_source);
    const lastSeenMs = integerOrNull(row?.last_seen_ms);
    const ageMs = lastSeenMs === null ? null : Math.max(0, Math.trunc(nowMs - lastSeenMs));

    // C5 online state is authored by S3 child_registry. The timestamp remains
    // observable for API compatibility, but must never turn an S3 child offline.
    if (statusSource === STATUS_SOURCE_S3) {
        // A voice turn is an active child-registry state, not an offline C5.
        const online = booleanValue(row?.online, false) || booleanValue(row?.voice_busy, false);
        return {
            online,
            age_ms: ageMs,
            offline_reason: online ? null : (trimText(row?.offline_reason, 128) || "s3_reported_offline"),
            status_source: STATUS_SOURCE_S3
        };
    }

    if (lastSeenMs === null) {
        return {
            online: false,
            age_ms: null,
            offline_reason: "never_seen",
            status_source: STATUS_SOURCE_SERVER
        };
    }

    if (ageMs > readDeviceOfflineTimeoutMs()) {
        return {
            online: false,
            age_ms: ageMs,
            offline_reason: "timeout",
            status_source: STATUS_SOURCE_SERVER
        };
    }

    if (Number(row?.online) === 0 && row?.offline_reason && row.offline_reason !== "timeout") {
        return {
            online: false,
            age_ms: ageMs,
            offline_reason: row.offline_reason || "manual",
            status_source: STATUS_SOURCE_SERVER
        };
    }

    return {
        online: true,
        age_ms: ageMs,
        offline_reason: null,
        status_source: STATUS_SOURCE_SERVER
    };
}

async function updateDeviceStatus(dbRun, dbAll, metadata, options = {}) {
    const deviceId = resolveDeviceId(metadata?.device_id);
    if (!deviceId || typeof dbRun !== "function" || typeof dbAll !== "function") {
        return {
            ok: false,
            skipped: true
        };
    }

    const moduleType = trimText(options.moduleType || metadata.payload_type || "", 80);
    const timestampIso = metadata.server_time_iso || new Date(metadata.server_recv_ms || Date.now()).toISOString();
    const updatedAt = nowIso();
    const existing = await getDeviceStatusRow(dbAll, deviceId);
    const existingState = existing ? mapDeviceStatus(existing, metadata.server_recv_ms || Date.now()) : null;
    const stats = computeDelayStats(existing, metadata.upload_delay_ms);
    const rebootCount = computeRebootCount(existing, metadata);
    const roomId = metadata.room_id || existing?.room_id || "";
    const roomName = metadata.room_name || existing?.room_name || "";
    const deviceType = metadata.device_type || existing?.device_type || "unknown";

    // Preserve S3-authored status fields while still refreshing device telemetry.
    if (isS3ChildStatus(existing)) {
        await dbRun(
            `UPDATE device_status
             SET device_type=?,
                 room_id=?,
                 room_name=?,
                 firmware_version=?,
                 last_payload_type=?,
                 last_server_recv_ms=?,
                 last_module_type=?,
                 last_esp_uptime_ms=?,
                 last_esp_time_ms=?,
                 time_synced=?,
                 reboot_count=?,
                 latest_upload_delay_ms=?,
                 avg_upload_delay_ms=?,
                 delay_sample_count=?,
                 updated_at=?
             WHERE device_id=? AND deleted_at IS NULL`,
            [
                deviceType,
                roomId,
                roomName,
                metadata.firmware_version || existing?.firmware_version || "",
                metadata.payload_type || moduleType,
                metadata.server_recv_ms,
                moduleType,
                metadata.esp_uptime_ms,
                metadata.esp_time_ms,
                metadata.time_synced === null ? null : (metadata.time_synced ? 1 : 0),
                rebootCount,
                stats.latest_upload_delay_ms,
                stats.avg_upload_delay_ms,
                stats.delay_sample_count,
                updatedAt,
                deviceId
            ]
        );
        await recordClockSkewWarning(dbRun, metadata, deviceId);
        return {
            ok: true,
            device_id: deviceId,
            module_type: moduleType,
            status_source: STATUS_SOURCE_S3,
            skipped_status_override: true,
            ...stats
        };
    }

    await runUpdateThenInsert(dbRun, {
        updateSql: `UPDATE device_status
            SET device_type=?,
                room_id=?,
                room_name=?,
                firmware_version=?,
                last_seen_ms=?,
                last_seen_iso=?,
                last_payload_type=?,
                last_server_recv_ms=?,
                last_module_type=?,
                last_esp_uptime_ms=?,
                last_esp_time_ms=?,
                time_synced=?,
                online=1,
                offline_reason=NULL,
                reboot_count=?,
                latest_upload_delay_ms=?,
                avg_upload_delay_ms=?,
                delay_sample_count=?,
                created_at=CASE WHEN deleted_at IS NULL THEN created_at ELSE ? END,
                deleted_at=NULL,
                delete_reason=NULL,
                updated_at=?
            WHERE device_id=?`,
        updateParams: [
            deviceType,
            roomId,
            roomName,
            metadata.firmware_version || existing?.firmware_version || "",
            metadata.server_recv_ms,
            timestampIso,
            metadata.payload_type || moduleType,
            metadata.server_recv_ms,
            moduleType,
            metadata.esp_uptime_ms,
            metadata.esp_time_ms,
            metadata.time_synced === null ? null : (metadata.time_synced ? 1 : 0),
            rebootCount,
            stats.latest_upload_delay_ms,
            stats.avg_upload_delay_ms,
            stats.delay_sample_count,
            updatedAt,
            updatedAt,
            deviceId
        ],
        insertSql: `INSERT INTO device_status
            (device_id,device_type,room_id,room_name,firmware_version,last_seen_ms,last_seen_iso,last_payload_type,last_server_recv_ms,last_module_type,last_esp_uptime_ms,last_esp_time_ms,time_synced,online,offline_reason,reboot_count,latest_upload_delay_ms,avg_upload_delay_ms,delay_sample_count,created_at,updated_at)
            VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)`,
        insertParams: [
            deviceId,
            deviceType,
            roomId,
            roomName,
            metadata.firmware_version || "",
            metadata.server_recv_ms,
            timestampIso,
            metadata.payload_type || moduleType,
            metadata.server_recv_ms,
            moduleType,
            metadata.esp_uptime_ms,
            metadata.esp_time_ms,
            metadata.time_synced === null ? null : (metadata.time_synced ? 1 : 0),
            1,
            null,
            rebootCount,
            stats.latest_upload_delay_ms,
            stats.avg_upload_delay_ms,
            stats.delay_sample_count,
            updatedAt,
            updatedAt
        ]
    });

    if (!existingState || !existingState.online) {
        await recordStatusEvent(dbRun, {
            event_type: "device",
            event_name: "device_status_changed",
            device_id: deviceId,
            severity: "info",
            message: `device ${deviceId} online`,
            payload: {
                device_id: deviceId,
                online: true,
                previous_online: Boolean(existingState?.online),
                last_payload_type: metadata.payload_type || moduleType,
                last_seen_ms: metadata.server_recv_ms
            },
            source: "device_status",
            server_recv_ms: metadata.server_recv_ms
        });
    }

    await recordClockSkewWarning(dbRun, metadata, deviceId);

    return {
        ok: true,
        device_id: deviceId,
        module_type: moduleType,
        ...stats
    };
}

async function updateChildStatusFromGatewaySnapshot(dbRun, dbAll, child, options = {}) {
    const deviceId = resolveDeviceId(child?.device_id);
    if (!deviceId || typeof dbRun !== "function" || typeof dbAll !== "function") {
        return {
            ok: false,
            skipped: true
        };
    }

    const serverReceivedMs = integerOrNull(options.serverReceivedMs ?? child?.server_received_ms) ?? Date.now();
    const payloadType = trimText(options.payloadType || "gateway.dashboard_snapshot", 80);
    const childStatus = trimText(child?.status, 40) || (booleanValue(child?.online, false) ? "online" : "offline");
    const online = booleanValue(child?.online, false);
    const existing = await getDeviceStatusRow(dbAll, deviceId);
    const childLastSeenMs = integerOrNull(child?.child_last_seen_ms) ??
        (isS3ChildStatus(existing) ? integerOrNull(existing?.child_last_seen_ms) : null);
    const lastSeenMs = integerOrNull(child?.last_seen_ms) ??
        (isS3ChildStatus(existing) ? integerOrNull(existing?.last_seen_ms) : null);
    const existingState = existing ? mapDeviceStatus(existing, serverReceivedMs) : null;
    const updatedAt = nowIso();
    const roomId = trimText(child?.room_id, 128) || existing?.room_id || "";
    const roomName = trimText(child?.room_name, 128) || existing?.room_name || "";
    const deviceType = trimText(child?.device_type, 40) || existing?.device_type || "C5";
    const lastSeenIso = isoForTimestamp(lastSeenMs);
    const offlineReason = online
        ? null
        : (trimText(child?.offline_reason, 128) ||
            (isS3ChildStatus(existing) ? trimText(existing?.offline_reason, 128) : "") ||
            "s3_reported_offline");
    const linkLost = booleanValue(child?.link_lost, false);
    const voiceBusy = booleanValue(child?.voice_busy, false);

    await runUpdateThenInsert(dbRun, {
        updateSql: `UPDATE device_status
            SET device_type=?,
                room_id=?,
                room_name=?,
                last_seen_ms=?,
                last_seen_iso=?,
                last_payload_type=?,
                last_server_recv_ms=?,
                last_module_type=?,
                status_source=?,
                child_status=?,
                child_last_seen_ms=?,
                server_received_ms=?,
                link_lost=?,
                voice_busy=?,
                online=?,
                offline_reason=?,
                created_at=CASE WHEN deleted_at IS NULL THEN created_at ELSE ? END,
                deleted_at=NULL,
                delete_reason=NULL,
                updated_at=?
            WHERE device_id=?`,
        updateParams: [
            deviceType,
            roomId,
            roomName,
            lastSeenMs,
            lastSeenIso,
            payloadType,
            serverReceivedMs,
            payloadType,
            STATUS_SOURCE_S3,
            childStatus,
            childLastSeenMs,
            serverReceivedMs,
            linkLost ? 1 : 0,
            voiceBusy ? 1 : 0,
            online ? 1 : 0,
            offlineReason,
            updatedAt,
            updatedAt,
            deviceId
        ],
        insertSql: `INSERT INTO device_status
            (device_id,device_type,room_id,room_name,last_seen_ms,last_seen_iso,last_payload_type,last_server_recv_ms,last_module_type,status_source,child_status,child_last_seen_ms,server_received_ms,link_lost,voice_busy,online,offline_reason,created_at,updated_at)
            VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)`,
        insertParams: [
            deviceId,
            deviceType,
            roomId,
            roomName,
            lastSeenMs,
            lastSeenIso,
            payloadType,
            serverReceivedMs,
            payloadType,
            STATUS_SOURCE_S3,
            childStatus,
            childLastSeenMs,
            serverReceivedMs,
            linkLost ? 1 : 0,
            voiceBusy ? 1 : 0,
            online ? 1 : 0,
            offlineReason,
            updatedAt,
            updatedAt
        ]
    });

    if (!existingState || existingState.online !== online) {
        await recordStatusEvent(dbRun, {
            event_type: "device",
            event_name: "device_status_changed",
            device_id: deviceId,
            severity: online ? "info" : "warning",
            message: `device ${deviceId} ${online ? "online" : "offline"} from S3 child_registry`,
            payload: {
                device_id: deviceId,
                online,
                previous_online: Boolean(existingState?.online),
                status: childStatus,
                offline_reason: offlineReason,
                link_lost: linkLost,
                voice_busy: voiceBusy,
                child_last_seen_ms: childLastSeenMs,
                last_seen_ms: lastSeenMs,
                server_received_ms: serverReceivedMs,
                status_source: STATUS_SOURCE_S3
            },
            source: "device_status_s3",
            server_recv_ms: serverReceivedMs
        });
    }

    return {
        ok: true,
        device_id: deviceId,
        online,
        status: childStatus,
        status_source: STATUS_SOURCE_S3,
        child_last_seen_ms: childLastSeenMs,
        last_seen_ms: lastSeenMs,
        server_received_ms: serverReceivedMs
    };
}

async function updateDeviceModuleStatus(dbRun, dbAll, metadata, moduleType) {
    const deviceId = resolveDeviceId(metadata?.device_id);
    const safeModuleType = trimText(moduleType || metadata?.payload_type || "", 80);
    if (!deviceId || !safeModuleType || typeof dbRun !== "function" || typeof dbAll !== "function") {
        return {
            ok: false,
            skipped: true
        };
    }

    const timestampIso = metadata.server_time_iso || new Date(metadata.server_recv_ms || Date.now()).toISOString();
    const updatedAt = nowIso();
    const existing = await getModuleStatusRow(dbAll, deviceId, safeModuleType);
    const stats = computeDelayStats(existing, metadata.upload_delay_ms);

    await runUpdateThenInsert(dbRun, {
        updateSql: `UPDATE device_module_status
            SET room_id=?,
                room_name=?,
                last_seen_ms=?,
                last_seen_iso=?,
                last_payload_type=?,
                last_server_recv_ms=?,
                last_esp_uptime_ms=?,
                last_esp_time_ms=?,
                time_synced=?,
                latest_upload_delay_ms=?,
                avg_upload_delay_ms=?,
                delay_sample_count=?,
                created_at=CASE WHEN deleted_at IS NULL THEN created_at ELSE ? END,
                deleted_at=NULL,
                delete_reason=NULL,
                updated_at=?
            WHERE device_id=? AND module_type=?`,
        updateParams: [
            metadata.room_id || existing?.room_id || "",
            metadata.room_name || existing?.room_name || "",
            metadata.server_recv_ms,
            timestampIso,
            metadata.payload_type || safeModuleType,
            metadata.server_recv_ms,
            metadata.esp_uptime_ms,
            metadata.esp_time_ms,
            metadata.time_synced === null ? null : (metadata.time_synced ? 1 : 0),
            stats.latest_upload_delay_ms,
            stats.avg_upload_delay_ms,
            stats.delay_sample_count,
            updatedAt,
            updatedAt,
            deviceId,
            safeModuleType
        ],
        insertSql: `INSERT INTO device_module_status
            (device_id,module_type,room_id,room_name,last_seen_ms,last_seen_iso,last_payload_type,last_server_recv_ms,last_esp_uptime_ms,last_esp_time_ms,time_synced,latest_upload_delay_ms,avg_upload_delay_ms,delay_sample_count,created_at,updated_at)
            VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)`,
        insertParams: [
            deviceId,
            safeModuleType,
            metadata.room_id || "",
            metadata.room_name || "",
            metadata.server_recv_ms,
            timestampIso,
            metadata.payload_type || safeModuleType,
            metadata.server_recv_ms,
            metadata.esp_uptime_ms,
            metadata.esp_time_ms,
            metadata.time_synced === null ? null : (metadata.time_synced ? 1 : 0),
            stats.latest_upload_delay_ms,
            stats.avg_upload_delay_ms,
            stats.delay_sample_count,
            updatedAt,
            updatedAt
        ]
    });

    return {
        ok: true,
        device_id: deviceId,
        module_type: safeModuleType,
        ...stats
    };
}

async function refreshDeviceActivity(dbRun, dbAll, metadata, moduleType) {
    const device = await updateDeviceStatus(dbRun, dbAll, metadata, { moduleType });
    const module = await updateDeviceModuleStatus(dbRun, dbAll, metadata, moduleType);
    return {
        device,
        module
    };
}

function mapDeviceStatus(row, nowMs = Date.now()) {
    if (!row) {
        return null;
    }

    const state = computeOnlineState(row, nowMs);
    const lastSeenMs = integerOrNull(row.last_seen_ms);
    const childLastSeenMs = integerOrNull(row.child_last_seen_ms);
    const serverReceivedMs = integerOrNull(row.server_received_ms) ?? integerOrNull(row.last_server_recv_ms);
    return {
        device_id: resolveDeviceId(row.device_id),
        device_type: row.device_type || "unknown",
        room_id: row.room_id || "",
        room_name: row.room_name || "",
        firmware_version: row.firmware_version || "",
        lastSeen: lastSeenMs,
        last_seen_ms: lastSeenMs,
        last_seen_iso: row.last_seen_iso || "",
        last_seen_age_ms: state.age_ms,
        age_ms: state.age_ms,
        device_online: state.online,
        online: state.online,
        status: row.child_status || "",
        status_source: state.status_source,
        offline_reason: state.offline_reason,
        child_last_seen_ms: childLastSeenMs,
        server_received_ms: serverReceivedMs,
        link_lost: Boolean(Number(row.link_lost)),
        voice_busy: Boolean(Number(row.voice_busy)),
        last_payload_type: row.last_payload_type || "",
        last_server_recv_ms: integerOrNull(row.last_server_recv_ms) ?? lastSeenMs,
        last_module_type: row.last_module_type || "",
        last_esp_uptime_ms: integerOrNull(row.last_esp_uptime_ms),
        last_esp_time_ms: integerOrNull(row.last_esp_time_ms),
        time_synced: row.time_synced === null || row.time_synced === undefined ? null : Boolean(Number(row.time_synced)),
        reboot_count: integerOrNull(row.reboot_count) || 0,
        latest_upload_delay_ms: integerOrNull(row.latest_upload_delay_ms),
        avg_upload_delay_ms: integerOrNull(row.avg_upload_delay_ms),
        delay_sample_count: integerOrNull(row.delay_sample_count) || 0,
        updated_at: row.updated_at || ""
    };
}

function mapStableDeviceStatus(row, nowMs = Date.now(), fallbackDeviceId = "") {
    const mapped = mapDeviceStatus(row, nowMs);
    if (!mapped) {
        return {
            device_id: resolveDeviceId(fallbackDeviceId),
            device_type: "unknown",
            room_id: "",
            room_name: "",
            online: null,
            device_online: null,
            status: "unknown",
            status_source: "not_observed",
            observed: false,
            lastSeen: null,
            last_seen_ms: null,
            age_ms: null,
            offline_reason: null,
            link_lost: null,
            voice_busy: null,
            last_payload_type: ""
        };
    }

    return {
        device_id: mapped.device_id,
        device_type: mapped.device_type || "unknown",
        room_id: mapped.room_id || "",
        room_name: mapped.room_name || "",
        online: mapped.online,
        status: mapped.status || "",
        status_source: mapped.status_source,
        lastSeen: mapped.last_seen_ms,
        last_seen_ms: mapped.last_seen_ms,
        child_last_seen_ms: mapped.child_last_seen_ms,
        server_received_ms: mapped.server_received_ms,
        age_ms: mapped.age_ms,
        offline_reason: mapped.offline_reason,
        link_lost: mapped.link_lost,
        voice_busy: mapped.voice_busy,
        last_payload_type: mapped.last_payload_type || ""
    };
}

function mapModuleStatus(row, nowMs = Date.now()) {
    if (!row) {
        return null;
    }

    const lastSeenMs = integerOrNull(row.last_seen_ms);
    const ageMs = lastSeenMs === null ? null : Math.max(0, nowMs - lastSeenMs);
    return {
        device_id: resolveDeviceId(row.device_id),
        module_type: row.module_type,
        room_id: row.room_id || "",
        room_name: row.room_name || "",
        last_seen_ms: lastSeenMs,
        last_seen_iso: row.last_seen_iso || "",
        last_seen_age_ms: ageMs,
        module_online: ageMs !== null && ageMs <= MODULE_ONLINE_THRESHOLD_MS,
        online: ageMs !== null && ageMs <= MODULE_ONLINE_THRESHOLD_MS,
        last_payload_type: row.last_payload_type || "",
        last_server_recv_ms: integerOrNull(row.last_server_recv_ms) ?? lastSeenMs,
        last_esp_uptime_ms: integerOrNull(row.last_esp_uptime_ms),
        last_esp_time_ms: integerOrNull(row.last_esp_time_ms),
        time_synced: row.time_synced === null || row.time_synced === undefined ? null : Boolean(Number(row.time_synced)),
        latest_upload_delay_ms: integerOrNull(row.latest_upload_delay_ms),
        avg_upload_delay_ms: integerOrNull(row.avg_upload_delay_ms),
        delay_sample_count: integerOrNull(row.delay_sample_count) || 0,
        updated_at: row.updated_at || ""
    };
}

async function markTimedOutDevices(dbRun, dbAll, nowMs = Date.now()) {
    if (typeof dbRun !== "function" || typeof dbAll !== "function") {
        return [];
    }

    const cutoffMs = nowMs - readDeviceOfflineTimeoutMs();
    const rows = await dbAll(
        `SELECT * FROM device_status
        WHERE deleted_at IS NULL
          AND COALESCE(online,0) <> 0
          AND COALESCE(status_source, 'server') <> 's3'
          AND last_seen_ms IS NOT NULL
          AND last_seen_ms < ?`,
        [cutoffMs]
    );
    if (rows.length === 0) {
        return [];
    }

    const updatedAt = nowIso();
    for (const row of rows) {
        await dbRun(
            `UPDATE device_status
            SET online=0, offline_reason='timeout', updated_at=?
            WHERE device_id=? AND deleted_at IS NULL`,
            [updatedAt, row.device_id]
        );
        await recordStatusEvent(dbRun, {
            event_type: "device",
            event_name: "device_status_changed",
            device_id: row.device_id,
            severity: "warning",
            message: `device ${row.device_id} offline by timeout`,
            payload: {
                device_id: row.device_id,
                online: false,
                offline_reason: "timeout",
                last_seen_ms: integerOrNull(row.last_seen_ms),
                age_ms: integerOrNull(row.last_seen_ms) === null ? null : Math.max(0, nowMs - integerOrNull(row.last_seen_ms))
            },
            source: "device_status",
            server_recv_ms: nowMs
        });
    }

    return rows.map(row => mapDeviceStatus({
        ...row,
        online: 0,
        offline_reason: "timeout",
        updated_at: updatedAt
    }, nowMs));
}

async function readDeviceStatus(dbAll, deviceId, nowMs = Date.now()) {
    const resolvedDeviceId = resolveDeviceId(deviceId);
    const rows = await dbAll(
        resolvedDeviceId
            ? "SELECT * FROM device_status WHERE device_id=? AND deleted_at IS NULL LIMIT 1"
            : "SELECT * FROM device_status WHERE deleted_at IS NULL ORDER BY last_seen_ms DESC LIMIT 1",
        resolvedDeviceId ? [resolvedDeviceId] : []
    );
    return mapDeviceStatus(rowFirst(rows), nowMs);
}

async function readDeviceStatuses(dbAll, filters = {}, nowMs = Date.now()) {
    const deviceId = resolveDeviceId(filters.device_id);
    const rows = await dbAll(
        deviceId
            ? "SELECT * FROM device_status WHERE device_id=? AND deleted_at IS NULL LIMIT 1"
            : "SELECT * FROM device_status WHERE deleted_at IS NULL ORDER BY last_seen_ms DESC",
        deviceId ? [deviceId] : []
    );
    if (deviceId && rows.length === 0) {
        return [mapStableDeviceStatus(null, nowMs, deviceId)];
    }

    return rows.map(row => mapStableDeviceStatus(row, nowMs));
}

async function readModuleStatuses(dbAll, deviceId, nowMs = Date.now()) {
    const resolvedDeviceId = resolveDeviceId(deviceId);
    const params = [];
    let where = "";
    if (resolvedDeviceId) {
        where = "WHERE device_id=? AND deleted_at IS NULL";
        params.push(resolvedDeviceId);
    } else {
        where = "WHERE deleted_at IS NULL";
    }

    const rows = await dbAll(
        `SELECT * FROM device_module_status ${where} ORDER BY last_seen_ms DESC`,
        params
    );
    return rows.map(row => mapModuleStatus(row, nowMs));
}

module.exports = {
    DEFAULT_DEVICE_OFFLINE_TIMEOUT_MS,
    MODULE_ONLINE_THRESHOLD_MS,
    computeOnlineState,
    mapDeviceStatus,
    mapModuleStatus,
    mapStableDeviceStatus,
    markTimedOutDevices,
    readDeviceOfflineTimeoutMs,
    readDeviceStatus,
    readDeviceStatuses,
    readModuleStatuses,
    refreshDeviceActivity,
    updateChildStatusFromGatewaySnapshot,
    updateDeviceModuleStatus,
    updateDeviceStatus
};
