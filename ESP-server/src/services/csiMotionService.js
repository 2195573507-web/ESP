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
const {
    broadcastEvent
} = require("./eventStreamService");
const {
    insertCsiMotionEvent
} = require("../db/csiMotion");

const CSI_MOTION_PAYLOAD_TYPE = "csi.motion";
const CSI_STATES = new Set(["IDLE", "MOTION", "HOLD"]);

function clampMotionScore(value) {
    const numeric = toFiniteNumber(value);
    if (numeric === null) {
        return null;
    }
    return Math.min(Math.max(numeric, 0), 1);
}

function finiteOrNull(value) {
    return toFiniteNumber(value);
}

function readTimestamp(payload, body, serverRecvMs) {
    return toIntegerOrNull(payload.timestamp) ||
        toIntegerOrNull(payload.updated_at_ms) ||
        toIntegerOrNull(body.timestamp_ms) ||
        serverRecvMs;
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

    const payload = body.payload;
    if ("raw_csi" in payload ||
        "subcarrier_data" in payload ||
        "selected_subcarriers" in payload ||
        "iq" in payload ||
        "phase" in payload) {
        return {
            ok: false,
            code: "RAW_CSI_NOT_ACCEPTED",
            error: "raw CSI and subcarrier data are not accepted"
        };
    }
    if ("occupancy" in payload ||
        "mean_amplitude" in payload ||
        "cv" in payload ||
        "sample_count" in payload) {
        return {
            ok: false,
            code: "LEGACY_CSI_MODEL_NOT_ACCEPTED",
            error: "legacy occupancy and C5-derived CSI result fields are not accepted"
        };
    }

    const state = trimText(payload.state, 16).toUpperCase();
    if (!CSI_STATES.has(state)) {
        return {
            ok: false,
            code: "INVALID_CSI_STATE",
            error: "state must be IDLE, MOTION, or HOLD"
        };
    }

    const linkId = trimText(payload.link_id, 64);
    if (!linkId) {
        return {
            ok: false,
            code: "LINK_ID_REQUIRED",
            error: "link_id is required"
        };
    }

    const frameEnergy = finiteOrNull(payload.frame_energy);
    const variance = finiteOrNull(payload.variance);
    const motionScore = clampMotionScore(payload.motion_score);
    if (frameEnergy === null || frameEnergy < 0 ||
        variance === null || variance < 0 ||
        motionScore === null) {
        return {
            ok: false,
            code: "INVALID_CSI_NUMERIC_FIELDS",
            error: "frame_energy, variance, and motion_score must be finite non-negative numbers"
        };
    }

    return {
        ok: true,
        csi: {
            device_id: trimText(payload.device_id || body.device_id, 128),
            link_id: linkId,
            state,
            frame_energy: frameEnergy,
            variance,
            rssi: toIntegerOrNull(payload.rssi),
            motion_score: motionScore,
            timestamp: readTimestamp(payload, body, serverRecvMs)
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

    const fact = {
        ...validation.csi,
        device_id: metadata.device_id || validation.csi.device_id,
        gateway_id: metadata.gateway_id,
        server_recv_ms: metadata.server_recv_ms,
        server_time_iso: metadata.server_time_iso,
        raw_json: body
    };

    await refreshDeviceActivity(dbRun, dbAll, metadata, CSI_MOTION_PAYLOAD_TYPE);
    const id = await insertCsiMotionEvent(dbRun, fact);
    const dashboardRecord = recordCsiMotion(fact, {
        serverRecvMs
    });

    await recordEvent(dbRun, {
        event_type: "csi",
        event_name: "csi_motion_state_received",
        device_id: fact.device_id,
        severity: fact.state === "MOTION" ? "warning" : "info",
        message: `csi state ${fact.state}`,
        payload: fact,
        source: "device_ingest",
        server_recv_ms: metadata.server_recv_ms
    });

    broadcastEvent("csi_motion", fact);

    return {
        ok: true,
        status: 202,
        metadata,
        data: {
            id,
            device_id: fact.device_id,
            payload_type: CSI_MOTION_PAYLOAD_TYPE,
            link_id: fact.link_id,
            state: fact.state,
            frame_energy: fact.frame_energy,
            variance: fact.variance,
            rssi: fact.rssi,
            motion_score: fact.motion_score,
            timestamp: fact.timestamp,
            server_recv_ms: metadata.server_recv_ms,
            server_time_iso: metadata.server_time_iso,
            dashboard_recorded: Boolean(dashboardRecord)
        }
    };
}

module.exports = {
    CSI_MOTION_PAYLOAD_TYPE,
    CSI_STATES,
    ingestCsiMotion,
    validateCsiMotionEnvelope
};
