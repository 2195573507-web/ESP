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

const SENSOR_ID_MAX_LENGTH = 80;
const AIR_QUALITY_LEVELS = new Set(["excellent", "good", "moderate", "poor", "bad", "unknown"]);
const AIR_QUALITY_CONFIDENCE = new Set(["none", "low", "medium", "high"]);
const AIR_QUALITY_ALGO_VERSION = "esp-bme690-relative-v1";

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

function normalizeAirQuality(payload, readings) {
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
        };
    }

    const fallbackBaseline = gasBaseline && gasBaseline > 0
        ? gasBaseline
        : (readings.gas_resistance_ohm > 0 ? readings.gas_resistance_ohm : null);
    if (!fallbackBaseline) {
        return {
            air_quality_score: null,
            air_quality_level: "unknown",
            air_quality_confidence: "none",
            air_quality_algo_version: AIR_QUALITY_ALGO_VERSION,
            air_quality_source: "server_fallback",
            gas_baseline_ohm: null,
            gas_ratio: null,
            gas_score: null,
            humidity_score: null,
            baseline_ready: false,
            warmup_done: false,
            sample_count: null
        };
    }

    const fallbackGasRatio = clamp(readings.gas_resistance_ohm / fallbackBaseline, 0, 1.5);
    const fallbackGasScore = Math.round(clamp(fallbackGasRatio * 100, 0, 100));
    const humidityDeviation = Math.abs(readings.humidity_percent - 50);
    const fallbackHumidityScore = Math.round(clamp(100 - humidityDeviation * 2.5, 0, 100));
    const fallbackScore = Math.round(clamp(fallbackGasScore * 0.75 + fallbackHumidityScore * 0.25, 0, 100));

    return {
        air_quality_score: fallbackScore,
        air_quality_level: levelForScore(fallbackScore),
        air_quality_confidence: "low",
        air_quality_algo_version: AIR_QUALITY_ALGO_VERSION,
        air_quality_source: "server_fallback",
        gas_baseline_ohm: fallbackBaseline,
        gas_ratio: fallbackGasRatio,
        gas_score: fallbackGasScore,
        humidity_score: fallbackHumidityScore,
        baseline_ready: false,
        warmup_done: false,
        sample_count: Number.isFinite(Number(payload.sample_count)) ? Math.trunc(Number(payload.sample_count)) : null
    };
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

async function ingestBme690(dbRun, dbAll, body, options = {}) {
    const validation = validateBmeEnvelope(body);
    const serverRecvMs = Number.isFinite(options.serverRecvMs) ? options.serverRecvMs : Date.now();
    const metadata = readDeviceMetadata({
        body,
        headers: options.headers,
        query: options.query,
        deviceId: options.trustedDeviceId,
        payloadType: "sensor.bme690",
        serverRecvMs
    });
    metadata.gateway_id = trimText(options.trustedGatewayId, 128);
    if (options.trustedDeviceId) {
        metadata.device_id = trimText(options.trustedDeviceId, 128);
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

    const payload = body.payload;
    const sensorId = trimText(payload.sensor_id || payload.module_id || "bme690_01", SENSOR_ID_MAX_LENGTH);
    const airQuality = normalizeAirQuality(payload, validation.readings);
    const rawJson = JSON.stringify(body);
    const metadataJson = JSON.stringify(metadataForStorage(metadata));
    const airQualityJson = JSON.stringify(airQuality);

    const result = await dbRun(
        `INSERT INTO sensor_records
        (timestamp,temperature,humidity,pressure,gas_resistance,device_id,esp_time_ms,esp_uptime_ms,server_recv_ms,server_time_iso,upload_delay_ms,schema_version,device_type,firmware_version,request_seq,time_synced,payload_type,sensor_id,metadata_json,raw_json,air_quality_json,air_quality_score,air_quality_level,air_quality_confidence,air_quality_algo_version,air_quality_source,gas_baseline_ohm,gas_ratio,gas_score,humidity_score)
        VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)`,
        [
            serverRecvMs,
            validation.readings.temperature_c,
            validation.readings.humidity_percent,
            validation.readings.pressure_hpa,
            validation.readings.gas_resistance_ohm,
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
            sensorId,
            metadataJson,
            rawJson,
            airQualityJson,
            airQuality.air_quality_score,
            airQuality.air_quality_level,
            airQuality.air_quality_confidence,
            airQuality.air_quality_algo_version,
            airQuality.air_quality_source,
            airQuality.gas_baseline_ohm,
            airQuality.gas_ratio,
            airQuality.gas_score,
            airQuality.humidity_score
        ]
    );

    await refreshDeviceActivity(dbRun, dbAll, metadata, "sensor.bme690");
    if (body.alarm || body.payload?.alarm || body.payload?.alarm_type) {
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

    return {
        ok: true,
        status: 201,
        metadata,
        data: {
            id: result.lastID,
            device_id: metadata.device_id,
            payload_type: "sensor.bme690",
            sensor_id: sensorId,
            server_recv_ms: metadata.server_recv_ms,
            server_time_iso: metadata.server_time_iso,
            upload_delay_ms: metadata.upload_delay_ms,
            air_quality: airQuality
        }
    };
}

module.exports = {
    AIR_QUALITY_ALGO_VERSION,
    ingestBme690,
    normalizeAirQuality,
    validateBmeEnvelope
};
