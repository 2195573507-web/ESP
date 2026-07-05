const {
    readDeviceMetadata,
    toFiniteNumber,
    toIntegerOrNull,
    trimText
} = require("./deviceMetadata");
const {
    refreshDeviceActivity
} = require("./deviceStatusService");
const {
    recordCsiMotion
} = require("./dashboardService");
const {
    recordEvent
} = require("./eventLogService");

const CSI_MOTION_PAYLOAD_TYPE = "csi.motion";
const CSI_OCCUPANCY_STATES = new Set(["unknown", "vacant", "occupied"]);

function clampMotionScore(value) {
    const numeric = toFiniteNumber(value);
    if (numeric === null) {
        return null;
    }

    return Math.min(Math.max(numeric, 0), 1);
}

function readOptionalInteger(value) {
    const numeric = toIntegerOrNull(value);
    return numeric === null ? null : numeric;
}

function validateCsiMotionEnvelope(body, serverRecvMs) {
    if (!body || typeof body !== "object" || Array.isArray(body)) {
        return {
            ok: false,
            code: "INVALID_ENVELOPE",
            error: "JSON object envelope is required"
        };
    }
    if (Number(body.schema_version) !== 1) {
        return {
            ok: false,
            code: "INVALID_SCHEMA_VERSION",
            error: "schema_version must be 1"
        };
    }
    if (trimText(body.payload_type, 80) !== CSI_MOTION_PAYLOAD_TYPE) {
        return {
            ok: false,
            code: "UNSUPPORTED_PAYLOAD_TYPE",
            error: "payload_type must be csi.motion"
        };
    }
    if (!trimText(body.device_id, 128)) {
        return {
            ok: false,
            code: "DEVICE_ID_REQUIRED",
            error: "device_id is required"
        };
    }
    if (!body.payload || typeof body.payload !== "object" || Array.isArray(body.payload)) {
        return {
            ok: false,
            code: "INVALID_PAYLOAD",
            error: "payload object is required"
        };
    }

    const occupancy = body.payload.occupancy;
    if (!occupancy || typeof occupancy !== "object" || Array.isArray(occupancy)) {
        return {
            ok: false,
            code: "INVALID_PAYLOAD",
            error: "payload.occupancy object is required"
        };
    }

    const state = trimText(occupancy.state, 16).toLowerCase() || "unknown";
    if (!CSI_OCCUPANCY_STATES.has(state)) {
        return {
            ok: false,
            code: "INVALID_CSI_OCCUPANCY_STATE",
            error: "occupancy.state must be unknown, vacant, or occupied"
        };
    }

    const sampleCount = toIntegerOrNull(body.payload.sample_count);
    const updatedAt = toIntegerOrNull(body.payload.updated_at) ||
        toIntegerOrNull(body.timestamp_ms) ||
        serverRecvMs;

    return {
        ok: true,
        csi: {
            occupancy: {
                state,
                available: true,
                motion_score: clampMotionScore(body.payload.motion_score),
                variance: toFiniteNumber(body.payload.variance),
                rssi: readOptionalInteger(body.payload.rssi),
                sample_count: sampleCount === null ? 0 : Math.max(0, sampleCount),
                updated_at: updatedAt
            },
            room_id: trimText(body.room_id, 128),
            local_id: toIntegerOrNull(body.local_id)
        }
    };
}

async function ingestCsiMotion(dbRun, dbAll, body, options = {}) {
    const serverRecvMs = Number.isFinite(options.serverRecvMs) ? options.serverRecvMs : Date.now();
    const metadata = readDeviceMetadata({
        body,
        headers: options.headers,
        query: options.query,
        deviceId: options.trustedDeviceId,
        payloadType: CSI_MOTION_PAYLOAD_TYPE,
        serverRecvMs
    });
    metadata.gateway_id = trimText(options.trustedGatewayId, 128);
    if (options.trustedDeviceId) {
        metadata.device_id = trimText(options.trustedDeviceId, 128);
    }
    const validation = validateCsiMotionEnvelope(body, serverRecvMs);
    if (!validation.ok) {
        return {
            ok: false,
            status: 400,
            code: validation.code,
            error: validation.error,
            metadata
        };
    }

    await refreshDeviceActivity(dbRun, dbAll, metadata, CSI_MOTION_PAYLOAD_TYPE);
    const dashboardRecord = recordCsiMotion({
        device_id: metadata.device_id,
        local_id: validation.csi.local_id,
        room_id: validation.csi.room_id,
        occupancy: validation.csi.occupancy
    }, {
        serverRecvMs
    });
    await recordEvent(dbRun, {
        event_type: "csi",
        event_name: "system_log_created",
        device_id: metadata.device_id,
        severity: "info",
        message: `csi motion ${validation.csi.occupancy.state}`,
        payload: {
            room_id: validation.csi.room_id,
            occupancy: validation.csi.occupancy
        },
        source: "device_ingest",
        server_recv_ms: metadata.server_recv_ms
    });

    return {
        ok: true,
        status: 202,
        metadata,
        data: {
            device_id: metadata.device_id,
            payload_type: CSI_MOTION_PAYLOAD_TYPE,
            occupancy: {
                state: validation.csi.occupancy.state
            },
            motion_score: validation.csi.occupancy.motion_score,
            variance: validation.csi.occupancy.variance,
            rssi: validation.csi.occupancy.rssi,
            sample_count: validation.csi.occupancy.sample_count,
            updated_at: validation.csi.occupancy.updated_at,
            server_recv_ms: metadata.server_recv_ms,
            server_time_iso: metadata.server_time_iso,
            dashboard_recorded: Boolean(dashboardRecord)
        }
    };
}

module.exports = {
    CSI_MOTION_PAYLOAD_TYPE,
    ingestCsiMotion,
    validateCsiMotionEnvelope
};
