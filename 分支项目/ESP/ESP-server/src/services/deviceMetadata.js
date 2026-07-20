const DEVICE_ID_MAX_LENGTH = 128;
const METADATA_TEXT_MAX_LENGTH = 128;
const PAYLOAD_TYPE_MAX_LENGTH = 80;
const MAX_VALID_UPLOAD_DELAY_MS = 60000;
const DEFAULT_CLOCK_MAX_FUTURE_SKEW_MS = 10000;
const DEFAULT_CLOCK_MAX_PAST_SKEW_MS = 24 * 60 * 60 * 1000;
const {
    resolveDeviceId
} = require("./deviceIdResolver");

function readHeader(headers = {}, name) {
    return headers[String(name || "").toLowerCase()];
}

function readPositiveIntegerEnv(name, fallback) {
    const numeric = Number.parseInt(process.env[name], 10);
    if (!Number.isFinite(numeric) || numeric <= 0) {
        return fallback;
    }

    return numeric;
}

function trimText(value, maxLength = METADATA_TEXT_MAX_LENGTH) {
    if (value === undefined || value === null) {
        return "";
    }

    const text = String(value).trim();
    return maxLength > 0 ? text.slice(0, maxLength) : text;
}

function toFiniteNumber(value) {
    if (value === undefined || value === null || value === "") {
        return null;
    }

    const numeric = Number(value);
    return Number.isFinite(numeric) ? numeric : null;
}

function toIntegerOrNull(value) {
    const numeric = toFiniteNumber(value);
    return numeric === null ? null : Math.trunc(numeric);
}

function readBoolean(value) {
    if (typeof value === "boolean") {
        return value;
    }

    if (typeof value === "number") {
        if (value === 1) {
            return true;
        }
        if (value === 0) {
            return false;
        }
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

    return null;
}

function isObject(value) {
    return value && typeof value === "object" && !Array.isArray(value);
}

function firstValue(...values) {
    for (const value of values) {
        if (value !== undefined && value !== null && value !== "") {
            return value;
        }
    }

    return undefined;
}

function isValidUploadDelay(value) {
    return Number.isFinite(value) && value >= 0 && value <= MAX_VALID_UPLOAD_DELAY_MS;
}

function buildUploadDelayMs(metadata, serverRecvMs) {
    if (metadata.time_synced !== true) {
        return null;
    }
    if (!Number.isFinite(metadata.esp_time_ms) || metadata.esp_time_ms <= 0) {
        return null;
    }

    const delay = Math.trunc(serverRecvMs - metadata.esp_time_ms);
    return isValidUploadDelay(delay) ? delay : null;
}

function readDeviceTimestampMs(body = {}, headers = {}) {
    return toIntegerOrNull(firstValue(
        body.esp_time_ms,
        body.timestamp,
        body.client_time_ms,
        readHeader(headers, "x-esp-time-ms"),
        readHeader(headers, "x-client-time-ms")
    ));
}

function readClockSkewWarning(body = {}, headers = {}, serverRecvMs = Date.now()) {
    const deviceTimeMs = readDeviceTimestampMs(body, headers);
    if (deviceTimeMs === null || deviceTimeMs <= 0) {
        return null;
    }

    const futureSkewMs = readPositiveIntegerEnv("DEVICE_CLOCK_MAX_FUTURE_SKEW_MS", DEFAULT_CLOCK_MAX_FUTURE_SKEW_MS);
    const pastSkewMs = readPositiveIntegerEnv("DEVICE_CLOCK_MAX_PAST_SKEW_MS", DEFAULT_CLOCK_MAX_PAST_SKEW_MS);
    const skewMs = deviceTimeMs - serverRecvMs;
    if (skewMs > futureSkewMs) {
        return {
            code: "DEVICE_CLOCK_FUTURE_SKEW",
            message: "device timestamp is too far in the future",
            device_time_ms: deviceTimeMs,
            server_recv_ms: serverRecvMs,
            skew_ms: skewMs,
            max_future_skew_ms: futureSkewMs
        };
    }

    if (skewMs < -pastSkewMs) {
        return {
            code: "DEVICE_CLOCK_PAST_SKEW",
            message: "device timestamp is too far in the past",
            device_time_ms: deviceTimeMs,
            server_recv_ms: serverRecvMs,
            skew_ms: skewMs,
            max_past_skew_ms: pastSkewMs
        };
    }

    return null;
}

function readDeviceMetadata(input = {}) {
    const body = isObject(input.body) ? input.body : {};
    const query = isObject(input.query) ? input.query : {};
    const headers = isObject(input.headers) ? input.headers : {};
    const serverRecvMs = Number.isFinite(input.serverRecvMs) ? input.serverRecvMs : Date.now();
    const payloadType = trimText(
        firstValue(
            input.payloadType,
            body.payload_type,
            readHeader(headers, "x-payload-type")
        ),
        PAYLOAD_TYPE_MAX_LENGTH
    );
    const timeSynced = readBoolean(firstValue(body.time_synced, readHeader(headers, "x-time-synced")));
    const espTimeMs = toIntegerOrNull(firstValue(body.esp_time_ms, readHeader(headers, "x-esp-time-ms")));
    const metadata = {
        schema_version: toIntegerOrNull(firstValue(body.schema_version, readHeader(headers, "x-schema-version"))),
        device_id: resolveDeviceId(
            firstValue(
                body.device_id,
                body.id,
                body.sensor_id,
                body.mac,
                body.client_id,
                readHeader(headers, "x-device-id"),
                readHeader(headers, "x-esp-device-id"),
                readHeader(headers, "x-client-id"),
                query.device_id,
                input.deviceId
            )
        ),
        device_type: trimText(firstValue(body.device_type, readHeader(headers, "x-device-type"))),
        room_id: trimText(firstValue(body.room_id, query.room_id, readHeader(headers, "x-room-id"))),
        room_name: trimText(firstValue(body.room_name, body.room, query.room_name, readHeader(headers, "x-room-name"))),
        firmware_version: trimText(firstValue(body.firmware_version, readHeader(headers, "x-firmware-version"))),
        request_seq: toIntegerOrNull(firstValue(body.request_seq, readHeader(headers, "x-request-seq"))),
        esp_uptime_ms: toIntegerOrNull(firstValue(body.esp_uptime_ms, readHeader(headers, "x-esp-uptime-ms"))),
        esp_time_ms: timeSynced === false ? null : espTimeMs,
        time_synced: timeSynced,
        payload_type: payloadType,
        server_recv_ms: serverRecvMs,
        server_time_iso: new Date(serverRecvMs).toISOString(),
        upload_delay_ms: null,
        clock_skew_warning: readClockSkewWarning(body, headers, serverRecvMs)
    };

    metadata.upload_delay_ms = buildUploadDelayMs(metadata, serverRecvMs);
    return metadata;
}

function metadataForStorage(metadata) {
    return {
        schema_version: metadata.schema_version,
        device_id: metadata.device_id,
        device_type: metadata.device_type,
        room_id: metadata.room_id,
        room_name: metadata.room_name,
        firmware_version: metadata.firmware_version,
        request_seq: metadata.request_seq,
        esp_uptime_ms: metadata.esp_uptime_ms,
        esp_time_ms: metadata.esp_time_ms,
        time_synced: metadata.time_synced,
        payload_type: metadata.payload_type,
        server_recv_ms: metadata.server_recv_ms,
        server_time_iso: metadata.server_time_iso,
        upload_delay_ms: metadata.upload_delay_ms,
        clock_skew_warning: metadata.clock_skew_warning
    };
}

function makeDeviceEnvelope({ ok, serverRecvMs = Date.now(), requestId = "", data = null, error = null }) {
    return {
        ok,
        server_time_ms: serverRecvMs,
        server_recv_ms: serverRecvMs,
        server_time_iso: new Date(serverRecvMs).toISOString(),
        request_id: requestId || "",
        error,
        data
    };
}

module.exports = {
    DEVICE_ID_MAX_LENGTH,
    DEFAULT_CLOCK_MAX_FUTURE_SKEW_MS,
    DEFAULT_CLOCK_MAX_PAST_SKEW_MS,
    MAX_VALID_UPLOAD_DELAY_MS,
    isValidUploadDelay,
    makeDeviceEnvelope,
    metadataForStorage,
    readDeviceMetadata,
    readClockSkewWarning,
    toFiniteNumber,
    toIntegerOrNull,
    trimText
};
