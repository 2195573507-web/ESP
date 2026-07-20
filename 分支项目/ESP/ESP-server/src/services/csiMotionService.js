const {
    readDeviceMetadata,
    toFiniteNumber,
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
const CSI_EVENT_SCHEMA_VERSION = "v2";
const CSI_STATES = new Set(["IDLE", "MOTION", "HOLD"]);
const TOP_LEVEL_KEYS = new Set([
    "schema_version",
    "trace_id",
    "tick_id",
    "fused_state",
    "confidence",
    "links",
    "timestamp_ms"
]);

function isPlainObject(value) {
    return value && typeof value === "object" && !Array.isArray(value);
}

function hasOnlyKeys(object, allowedKeys) {
    return Object.keys(object).every(key => allowedKeys.has(key)) &&
        allowedKeys.size === Object.keys(object).length;
}

function finiteNumber(value, min = -Infinity, max = Infinity) {
    const numeric = toFiniteNumber(value);
    if (numeric === null || numeric < min || numeric > max) {
        return null;
    }
    return numeric;
}

function strictInteger(value, min = -Infinity, max = Infinity) {
    const numeric = finiteNumber(value, min, max);
    if (numeric === null || !Number.isInteger(numeric)) {
        return null;
    }
    return numeric;
}

function invalid(code, error) {
    return {
        ok: false,
        code,
        error
    };
}

function validateCanonicalLink(link, expectedIndex) {
    const expected = `link_${expectedIndex}`;
    if (link !== expected) {
        return invalid("INVALID_CSI_LINK", "links must use canonical link_N identifiers");
    }
    return {
        ok: true,
        link: expected
    };
}

function validateCanonicalCsiEventV2(body) {
    if (!isPlainObject(body) || !hasOnlyKeys(body, TOP_LEVEL_KEYS)) {
        return invalid("INVALID_CANONICAL_CSI_EVENT", "canonical CSI event v2 object is required");
    }
    if (body.schema_version !== CSI_EVENT_SCHEMA_VERSION) {
        return invalid("INVALID_SCHEMA_VERSION", "schema_version must be v2");
    }
    const traceId = trimText(body.trace_id, 128);
    if (!traceId || traceId !== body.trace_id) {
        return invalid("INVALID_TRACE_ID", "trace_id must be a non-empty canonical string");
    }

    const tickId = strictInteger(body.tick_id, 0);
    const timestampMs = strictInteger(body.timestamp_ms, 1);
    const confidence = finiteNumber(body.confidence, 0, 1);
    if (tickId === null || timestampMs === null || confidence === null) {
        return invalid("INVALID_CANONICAL_TIMING", "tick_id, timestamp_ms, and confidence are invalid");
    }

    const state = trimText(body.fused_state, 16).toUpperCase();
    if (!CSI_STATES.has(state)) {
        return invalid("INVALID_FUSED_STATE", "fused_state must be IDLE, MOTION, or HOLD");
    }

    if (!Array.isArray(body.links) || body.links.length === 0 || body.links.length > 8) {
        return invalid("INVALID_CSI_LINKS", "links must be a non-empty canonical array");
    }
    const links = [];
    for (let i = 0; i < body.links.length; i++) {
        const validation = validateCanonicalLink(body.links[i], i);
        if (!validation.ok) {
            return validation;
        }
        links.push(validation.link);
    }

    return {
        ok: true,
        csi: {
            trace_id: traceId,
            tick_id: tickId,
            link_id: "fused",
            state,
            frame_energy: null,
            variance: null,
            rssi: null,
            motion_score: null,
            confidence,
            timestamp: timestampMs,
            links
        }
    };
}

function prepareCanonicalCsiEventV2(body, options = {}) {
    const serverRecvMs = Number.isFinite(options.serverRecvMs) ? options.serverRecvMs : Date.now();
    const gatewayId = trimText(options.trustedGatewayId, 128);
    const metadata = readDeviceMetadata({
        body: {},
        headers: options.headers,
        query: options.query,
        deviceId: gatewayId,
        payloadType: CSI_MOTION_PAYLOAD_TYPE,
        serverRecvMs
    });
    metadata.gateway_id = gatewayId;
    metadata.device_id = gatewayId;

    const validation = validateCanonicalCsiEventV2(body);
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
        device_id: metadata.device_id,
        gateway_id: metadata.gateway_id,
        link_id: validation.csi.link_id,
        state: validation.csi.state,
        frame_energy: validation.csi.frame_energy,
        variance: validation.csi.variance,
        rssi: validation.csi.rssi,
        motion_score: validation.csi.motion_score,
        confidence: validation.csi.confidence,
        timestamp: validation.csi.timestamp,
        server_recv_ms: metadata.server_recv_ms,
        server_time_iso: metadata.server_time_iso,
        raw_json: body
    };

    return {
        ok: true,
        status: 202,
        metadata,
        validation,
        fact,
        data: {
            id: null,
            trace_id: validation.csi.trace_id,
            tick_id: validation.csi.tick_id,
            device_id: fact.device_id,
            payload_type: CSI_MOTION_PAYLOAD_TYPE,
            link_id: fact.link_id,
            state: fact.state,
            frame_energy: fact.frame_energy,
            variance: fact.variance,
            rssi: fact.rssi,
            motion_score: fact.motion_score,
            confidence: validation.csi.confidence,
            timestamp: fact.timestamp,
            server_recv_ms: metadata.server_recv_ms,
            server_time_iso: metadata.server_time_iso,
            dashboard_recorded: false
        }
    };
}

async function persistCanonicalCsiEventV2(dbRun, dbAll, prepared) {
    if (!prepared?.ok) {
        return null;
    }

    const metadata = prepared.metadata;
    const validation = prepared.validation;
    const fact = prepared.fact;

    await refreshDeviceActivity(dbRun, dbAll, metadata, CSI_MOTION_PAYLOAD_TYPE);
    const id = await insertCsiMotionEvent(dbRun, fact);
    const dashboardRecord = recordCsiMotion(fact, {
        serverRecvMs: metadata.server_recv_ms
    });

    await recordEvent(dbRun, {
        event_type: "csi",
        event_name: "canonical_csi_event_v2_received",
        device_id: fact.device_id,
        severity: fact.state === "MOTION" ? "warning" : "info",
        message: `canonical csi state ${fact.state}`,
        payload: {
            ...validation.csi,
            device_id: fact.device_id,
            gateway_id: fact.gateway_id
        },
        source: "kernel_csi_event",
        server_recv_ms: metadata.server_recv_ms
    });

    broadcastEvent("csi_motion", fact);

    prepared.data.id = id;
    prepared.data.dashboard_recorded = Boolean(dashboardRecord);
    return {
        ok: true,
        status: 202,
        metadata,
        data: prepared.data
    };
}

async function ingestCanonicalCsiEventV2(dbRun, dbAll, body, options = {}) {
    const prepared = prepareCanonicalCsiEventV2(body, options);
    if (!prepared.ok) {
        return prepared;
    }

    return persistCanonicalCsiEventV2(dbRun, dbAll, prepared);
}

module.exports = {
    CSI_EVENT_SCHEMA_VERSION,
    CSI_MOTION_PAYLOAD_TYPE,
    CSI_STATES,
    ingestCanonicalCsiEventV2,
    persistCanonicalCsiEventV2,
    prepareCanonicalCsiEventV2,
    validateCanonicalCsiEventV2
};
