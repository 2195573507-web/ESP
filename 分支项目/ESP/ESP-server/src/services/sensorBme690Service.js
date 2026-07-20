const {
    metadataForStorage,
    readDeviceMetadata,
    toFiniteNumber,
    trimText
} = require("./deviceMetadata");
const {
    refreshDeviceActivity
} = require("./deviceStatusService");
const {
    recordEvent
} = require("./eventLogService");
const {
    isSupportedC5DeviceId,
    resolveDeviceId
} = require("./deviceIdResolver");

const SENSOR_ID_MAX_LENGTH = 80;
const AIR_QUALITY_LEVELS = new Set(["excellent", "good", "moderate", "poor", "bad", "unknown"]);
const AIR_QUALITY_CONFIDENCE = new Set(["none", "low", "medium", "high"]);
const V3_AIR_QUALITY_CONFIDENCE = new Set(["low", "medium", "high"]);
const AIR_QUALITY_ALGO_VERSION = "esp-bme690-relative-v1";
const C5_BME690_AIR_QUALITY_V3 = "c5_bme690_air_quality_v3";

function clamp(value, min, max) {
    return Math.min(Math.max(value, min), max);
}

function roundOrNull(value) {
    return Number.isFinite(value) ? Math.round(value) : null;
}

function readRequiredNumber(payload, fieldName, errors) {
    const value = toFiniteNumber(payload[fieldName]);
    if (value === null) {
        errors.push(`${fieldName} is required`);
    }
    return value;
}

function levelForScore(score) {
    if (!Number.isFinite(score)) {
        return "unknown";
    }
    if (score >= 90) {
        return "excellent";
    }
    if (score >= 75) {
        return "good";
    }
    if (score >= 55) {
        return "moderate";
    }
    if (score >= 30) {
        return "poor";
    }
    return "bad";
}

function readBooleanOrNull(value) {
    if (typeof value === "boolean") {
        return value;
    }
    if (typeof value === "number") {
        return value === 1 ? true : (value === 0 ? false : null);
    }
    if (typeof value === "string") {
        const normalized = value.trim().toLowerCase();
        if (["true", "1", "yes", "y"].includes(normalized)) {
            return true;
        }
        if (["false", "0", "no", "n"].includes(normalized)) {
            return false;
        }
    }
    return null;
}

function copyOpaqueObject(value) {
    if (!value || typeof value !== "object" || Array.isArray(value)) {
        return undefined;
    }

    return JSON.parse(JSON.stringify(value));
}

function logAirQualityDecision(logger, source, reason = "") {
    const target = logger || console;
    if (source === "fallback") {
        const message = `[sensor.bme690] AIR_QUALITY_FALLBACK_USED reason=${reason || "invalid_or_missing_air_quality"}`;
        if (typeof target.warn === "function") {
            target.warn(message);
        } else {
            target.log(message);
        }
        return;
    }

    target.log(`[sensor.bme690] AIR_QUALITY_SOURCE=${source}`);
}

function normalizeV3AirQuality(payload) {
    const input = copyOpaqueObject(payload.air_quality);
    if (!input) {
        return {
            ok: false,
            reason: "v3_air_quality_missing"
        };
    }

    const algorithm = trimText(input.algorithm, 80);
    const rawScore = toFiniteNumber(input.score);
    const level = trimText(input.level, 40);
    const confidence = trimText(input.confidence, 40);
    const gasRatio = toFiniteNumber(input.gas_ratio);
    const stabilityScore = toFiniteNumber(input.stability_score);
    const sensorState = trimText(input.sensor_state, 80);
    const baselineReady = readBooleanOrNull(input.baseline_ready);

    if (algorithm !== C5_BME690_AIR_QUALITY_V3) {
        return {
            ok: false,
            reason: "v3_algorithm_unsupported"
        };
    }
    const score = rawScore !== null && rawScore >= 0 && rawScore <= 100
        ? rawScore
        : null;
    const normalizedLevel = AIR_QUALITY_LEVELS.has(level) ? level : null;
    const normalizedConfidence = V3_AIR_QUALITY_CONFIDENCE.has(confidence) ? confidence : null;

    if (score === null && normalizedLevel === null && normalizedConfidence === null) {
        return {
            ok: false,
            reason: "v3_core_fields_missing_or_invalid"
        };
    }

    // C5 owns this object. Keep every supplied field, including future v3 fields.
    return {
        ok: true,
        airQuality: input
    };
}

function readAirQualityCompatibility(airQuality) {
    const source = airQuality && typeof airQuality === "object" ? airQuality : {};
    return {
        air_quality_score: toFiniteNumber(source.air_quality_score ?? source.score),
        air_quality_level: trimText(source.air_quality_level ?? source.level, 40) || null,
        air_quality_confidence: trimText(source.air_quality_confidence ?? source.confidence, 40) || null,
        air_quality_algo_version: trimText(source.air_quality_algo_version ?? source.algorithm, 80) || null,
        air_quality_source: trimText(source.air_quality_source ?? source.source, 40) || null,
        gas_baseline_ohm: toFiniteNumber(source.gas_baseline_ohm),
        gas_ratio: toFiniteNumber(source.gas_ratio),
        gas_score: roundOrNull(toFiniteNumber(source.gas_score)),
        humidity_score: roundOrNull(toFiniteNumber(source.humidity_score))
    };
}

function normalizeLegacyAirQuality(payload) {
    const gasBaseline = toFiniteNumber(payload.gas_baseline_ohm);
    const gasRatio = toFiniteNumber(payload.gas_ratio);
    const gasScore = roundOrNull(toFiniteNumber(payload.gas_score));
    const humidityScore = roundOrNull(toFiniteNumber(payload.humidity_score));
    const rawScore = toFiniteNumber(payload.air_quality_score);
    const espScore = rawScore === null ? null : Math.round(rawScore);
    const espScoreValid = espScore !== null && espScore >= 0 && espScore <= 100;
    const espLevel = trimText(payload.air_quality_level, 40);
    const espConfidence = trimText(payload.air_quality_confidence, 40);
    const espSource = trimText(payload.air_quality_source, 40);

    if (espScoreValid &&
        AIR_QUALITY_LEVELS.has(espLevel) &&
        AIR_QUALITY_CONFIDENCE.has(espConfidence)) {
        return {
            ok: true,
            airQuality: {
                air_quality_score: espScore,
                air_quality_level: espLevel,
                air_quality_confidence: espConfidence,
                air_quality_algo_version: trimText(payload.air_quality_algo_version, 80) || AIR_QUALITY_ALGO_VERSION,
                air_quality_source: espSource === "server_fallback" ? "server_fallback" : "esp",
                gas_baseline_ohm: gasBaseline,
                gas_ratio: gasRatio,
                gas_score: gasScore,
                humidity_score: humidityScore,
                baseline_ready: Boolean(payload.baseline_ready),
                warmup_done: Boolean(payload.warmup_done),
                sample_count: Number.isFinite(Number(payload.sample_count)) ? Math.trunc(Number(payload.sample_count)) : null
            }
        };
    }

    if (rawScore === null) {
        return {
            ok: false,
            reason: "legacy_score_missing"
        };
    }
    if (!espScoreValid) {
        return {
            ok: false,
            reason: "legacy_score_invalid"
        };
    }
    if (!AIR_QUALITY_LEVELS.has(espLevel)) {
        return {
            ok: false,
            reason: "legacy_level_invalid"
        };
    }
    return {
        ok: false,
        reason: "legacy_confidence_invalid"
    };
}

function buildFallbackAirQuality(payload, readings) {
    return {
        air_quality_score: toFiniteNumber(payload.air_quality_score),
        air_quality_level: trimText(payload.air_quality_level, 40) || "unknown",
        air_quality_confidence: trimText(payload.air_quality_confidence, 40) || "none",
        air_quality_algo_version: trimText(payload.air_quality_algo_version, 80) || null,
        air_quality_source: "unavailable",
        gas_baseline_ohm: toFiniteNumber(payload.gas_baseline_ohm),
        gas_ratio: toFiniteNumber(payload.gas_ratio),
        gas_score: roundOrNull(toFiniteNumber(payload.gas_score)),
        humidity_score: roundOrNull(toFiniteNumber(payload.humidity_score)),
        baseline_ready: readBooleanOrNull(payload.baseline_ready),
        warmup_done: readBooleanOrNull(payload.warmup_done),
        sample_count: Number.isFinite(Number(payload.sample_count)) ? Math.trunc(Number(payload.sample_count)) : null
    };
}

function normalizeAirQuality(payload, readings, options = {}) {
    const v3 = normalizeV3AirQuality(payload);
    if (v3.ok) {
        logAirQualityDecision(options.logger, "v3");
        return v3.airQuality;
    }

    const legacy = normalizeLegacyAirQuality(payload);
    if (legacy.ok) {
        logAirQualityDecision(options.logger, "legacy");
        return legacy.airQuality;
    }

    logAirQualityDecision(options.logger, "fallback", v3.reason !== "v3_air_quality_missing" ? v3.reason : legacy.reason);
    return buildFallbackAirQuality(payload, readings);
}

function validateBmeEnvelope(body) {
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
    if (trimText(body.payload_type, 80) !== "sensor.bme690") {
        return {
            ok: false,
            code: "UNSUPPORTED_PAYLOAD_TYPE",
            error: "payload_type must be sensor.bme690"
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

    const errors = [];
    const readings = {
        temperature_c: readRequiredNumber(body.payload, "temperature_c", errors),
        humidity_percent: readRequiredNumber(body.payload, "humidity_percent", errors),
        pressure_hpa: readRequiredNumber(body.payload, "pressure_hpa", errors),
        gas_resistance_ohm: readRequiredNumber(body.payload, "gas_resistance_ohm", errors)
    };
    if (errors.length > 0) {
        return {
            ok: false,
            code: "INVALID_PAYLOAD",
            error: errors.join("; ")
        };
    }

    return {
        ok: true,
        readings
    };
}

function prepareBme690Ingest(body, options = {}) {
    const validation = validateBmeEnvelope(body);
    const serverRecvMs = Number.isFinite(options.serverRecvMs) ? options.serverRecvMs : Date.now();
    const requestedDeviceId = resolveDeviceId(body?.device_id);
    const trustedDeviceId = resolveDeviceId(options.trustedDeviceId);
    const metadata = readDeviceMetadata({
        body,
        headers: options.headers,
        query: options.query,
        deviceId: trustedDeviceId,
        payloadType: "sensor.bme690",
        serverRecvMs
    });
    metadata.gateway_id = trimText(options.trustedGatewayId, 128);
    if (trustedDeviceId) {
        metadata.device_id = trustedDeviceId;
    }

    if (!validation.ok) {
        return {
            ok: false,
            status: 400,
            code: validation.code,
            error: validation.error,
            metadata
        };
    }

    if (trustedDeviceId && requestedDeviceId !== trustedDeviceId) {
        return {
            ok: false,
            status: 400,
            code: "DEVICE_ID_MISMATCH",
            error: "device_id must match authenticated gateway metadata",
            metadata
        };
    }

    if (options.requireSupportedC5DeviceId === true && !isSupportedC5DeviceId(metadata.device_id)) {
        return {
            ok: false,
            status: 400,
            code: "DEVICE_ID_NOT_ALLOWED",
            error: "device_id must be sensair_shuttle_01 or sensair_shuttle_02",
            metadata
        };
    }

    const payload = body.payload;
    const sensorId = trimText(payload.sensor_id || payload.module_id || "bme690_01", SENSOR_ID_MAX_LENGTH);
    const airQuality = normalizeAirQuality(payload, validation.readings, {
        logger: options.logger
    });
    const airQualityCompatibility = readAirQualityCompatibility(airQuality);
    // These C5-owned objects remain opaque throughout the server pipeline.
    const bmeDiag = copyOpaqueObject(payload.bme_diag);
    const baselineState = copyOpaqueObject(payload.baseline_state);
    const rawJson = JSON.stringify(body);
    const metadataJson = JSON.stringify(metadataForStorage(metadata));
    const airQualityJson = JSON.stringify(airQuality);

    return {
        ok: true,
        status: 201,
        metadata,
        body,
        readings: validation.readings,
        sensorId,
        airQuality,
        airQualityCompatibility,
        bmeDiag,
        baselineState,
        rawJson,
        metadataJson,
        airQualityJson,
        hasAlarm: Boolean(body.alarm || body.payload?.alarm || body.payload?.alarm_type),
        data: {
            id: null,
            device_id: metadata.device_id,
            payload_type: "sensor.bme690",
            sensor_id: sensorId,
            server_recv_ms: metadata.server_recv_ms,
            server_time_iso: metadata.server_time_iso,
            upload_delay_ms: metadata.upload_delay_ms,
            air_quality: airQuality,
            air_quality_score: airQualityCompatibility.air_quality_score,
            air_quality_level: airQualityCompatibility.air_quality_level,
            air_quality_confidence: airQualityCompatibility.air_quality_confidence
        }
    };
}

async function persistBme690Ingest(dbRun, dbAll, prepared, options = {}) {
    if (!prepared?.ok) {
        return null;
    }

    const metadata = prepared.metadata;
    const useTransaction = options.transactional === true;
    let transactionStarted = false;

    try {
        if (useTransaction) {
            await dbRun("BEGIN IMMEDIATE TRANSACTION");
            transactionStarted = true;
        }

        const result = await dbRun(
        `INSERT INTO sensor_records
        (timestamp,temperature,humidity,pressure,gas_resistance,device_id,esp_time_ms,esp_uptime_ms,server_recv_ms,server_time_iso,upload_delay_ms,schema_version,device_type,firmware_version,request_seq,time_synced,payload_type,sensor_id,metadata_json,raw_json,air_quality_json,air_quality_score,air_quality_level,air_quality_confidence,air_quality_algo_version,air_quality_source,gas_baseline_ohm,gas_ratio,gas_score,humidity_score)
        VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)`,
        [
            metadata.server_recv_ms,
            prepared.readings.temperature_c,
            prepared.readings.humidity_percent,
            prepared.readings.pressure_hpa,
            prepared.readings.gas_resistance_ohm,
            metadata.device_id,
            metadata.esp_time_ms,
            metadata.esp_uptime_ms,
            metadata.server_recv_ms,
            metadata.server_time_iso,
            metadata.upload_delay_ms,
            metadata.schema_version,
            metadata.device_type,
            metadata.firmware_version,
            metadata.request_seq,
            metadata.time_synced === null ? null : (metadata.time_synced ? 1 : 0),
            "sensor.bme690",
            prepared.sensorId,
            prepared.metadataJson,
            prepared.rawJson,
            prepared.airQualityJson,
            prepared.airQualityCompatibility.air_quality_score,
            prepared.airQualityCompatibility.air_quality_level,
            prepared.airQualityCompatibility.air_quality_confidence,
            prepared.airQualityCompatibility.air_quality_algo_version,
            prepared.airQualityCompatibility.air_quality_source,
            prepared.airQualityCompatibility.gas_baseline_ohm,
            prepared.airQualityCompatibility.gas_ratio,
            prepared.airQualityCompatibility.gas_score,
            prepared.airQualityCompatibility.humidity_score
        ]
        );

        await refreshDeviceActivity(dbRun, dbAll, metadata, "sensor.bme690");
        if (prepared.hasAlarm) {
            const body = prepared.body;
            await recordEvent(dbRun, {
            event_type: "alarm",
            event_name: "alarm_created",
            device_id: metadata.device_id,
            severity: "warning",
            message: trimText(body.payload?.alarm_type || body.alarm_type || "sensor alarm", 200),
            payload: body.payload?.alarm || body.alarm || body.payload || {},
            source: "device_ingest",
            server_recv_ms: metadata.server_recv_ms
            });
        }

        if (transactionStarted) {
            await dbRun("COMMIT");
            transactionStarted = false;
        }

        prepared.data.id = result.lastID;
        return {
            ok: true,
            status: 201,
            metadata,
            data: prepared.data
        };
    } catch (error) {
        if (transactionStarted) {
            try {
                await dbRun("ROLLBACK");
            } catch (rollbackError) {
                throw new Error(`[device-v1] BME transaction failed: ${error?.message || error}; rollback failed: ${rollbackError?.message || rollbackError}`);
            }
        }
        throw error;
    }
}

async function ingestBme690(dbRun, dbAll, body, options = {}) {
    const prepared = prepareBme690Ingest(body, options);
    if (!prepared.ok) {
        return prepared;
    }

    return persistBme690Ingest(dbRun, dbAll, prepared);
}

module.exports = {
    AIR_QUALITY_ALGO_VERSION,
    ingestBme690,
    normalizeAirQuality,
    prepareBme690Ingest,
    persistBme690Ingest,
    validateBmeEnvelope
};
