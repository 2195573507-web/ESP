const express = require("express");
const {
    buildSensorTimingFields,
    withTimeSyncStatus
} = require("../../server-time-sync/timeSync");
const {
    mapDeviceStatus,
    mapModuleStatus,
    refreshDeviceActivity
} = require("../services/deviceStatusService");
const {
    readDeviceMetadata
} = require("../services/deviceMetadata");
const {
    recordEvent
} = require("../services/eventLogService");

const SENSOR_DEVICE_ID_MAX_LENGTH = 128;
const LEGACY_SENSOR_FALLBACK_DEVICE_ID = "unknown_device";

function toFiniteSensorNumber(value) {
    if (value === undefined || value === null || value === "") {
        return null;
    }

    const numeric = Number(value);
    return Number.isFinite(numeric) ? numeric : null;
}

function normalizeSensorBody(body = {}) {
    const inferredDeviceId = typeof body.device_id === "string"
        ? body.device_id
        : (body.device_id ?? body.id ?? body.sensor_id ?? body.mac ?? body.client_id);

    return {
        ...body,
        temperature: toFiniteSensorNumber(body.temperature),
        humidity: toFiniteSensorNumber(body.humidity),
        pressure: toFiniteSensorNumber(body.pressure),
        gas_resistance: toFiniteSensorNumber(body.gas_resistance),
        payload_type: typeof body.payload_type === "string" && body.payload_type.trim()
            ? body.payload_type.trim().slice(0, 80)
            : "sensor.bme690",
        device_id: inferredDeviceId === undefined || inferredDeviceId === null
            ? ""
            : String(inferredDeviceId).trim().slice(0, SENSOR_DEVICE_ID_MAX_LENGTH)
    };
}

function readHistoryLimit(value) {
    const numeric = Number.parseInt(value, 10);
    if (!Number.isFinite(numeric) || numeric <= 0) {
        return 50;
    }

    return Math.min(numeric, 500);
}

function sendSensorDbError(res, err, includeSuccess = false) {
    return res.status(500).json({
        ok: false,
        ...(includeSuccess ? { success: false } : {}),
        error: err.message
    });
}

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

function enrichLatestSensorRow(row, deviceStatusRow, moduleStatusRow) {
    const enriched = withTimeSyncStatus(row);
    const deviceStatus = mapDeviceStatus(deviceStatusRow);
    const moduleStatus = mapModuleStatus(moduleStatusRow);
    const airQuality = parseJsonObject(row.air_quality_json, {
        air_quality_score: row.air_quality_score,
        air_quality_level: row.air_quality_level,
        air_quality_confidence: row.air_quality_confidence,
        air_quality_source: row.air_quality_source
    });

    return {
        ...enriched,
        online: Boolean(deviceStatus?.online),
        device_online: Boolean(deviceStatus?.online),
        sensor_online: Boolean(moduleStatus?.online),
        latest_upload_delay_ms: deviceStatus?.latest_upload_delay_ms ?? row.upload_delay_ms ?? null,
        avg_upload_delay_ms: deviceStatus?.avg_upload_delay_ms ?? null,
        delay_sample_count: deviceStatus?.delay_sample_count ?? 0,
        module_latest_upload_delay_ms: moduleStatus?.latest_upload_delay_ms ?? null,
        module_avg_upload_delay_ms: moduleStatus?.avg_upload_delay_ms ?? null,
        air_quality: airQuality,
        air_quality_score: row.air_quality_score ?? airQuality?.air_quality_score ?? null,
        air_quality_level: row.air_quality_level || airQuality?.air_quality_level || "",
        air_quality_confidence: row.air_quality_confidence || airQuality?.air_quality_confidence || "",
        air_quality_source: row.air_quality_source || airQuality?.air_quality_source || ""
    };
}

function createSensorRouter(options) {
    const router = express.Router();
    const db = options.db;
    const dbRun = options.dbRun;
    const dbAll = options.dbAll;
    const logger = options.logger || console;

    router.post("/sensor", (req, res) => {
        const normalizedBody = normalizeSensorBody(req.body);
        const {
            temperature,
            humidity,
            pressure,
            gas_resistance
        } = normalizedBody;
        const serverRecvMs = Date.now();
        const timing = buildSensorTimingFields(normalizedBody, serverRecvMs);
        let deviceId = timing.device_id || normalizedBody.device_id;
        const usedFallbackDeviceId = !deviceId;
        if (!deviceId) {
            deviceId = LEGACY_SENSOR_FALLBACK_DEVICE_ID;
        }
        timing.device_id = deviceId;
        const payloadType = normalizedBody.payload_type || "sensor.bme690";
        const rawPayload = JSON.stringify(req.body || {});
        const payloadJson = JSON.stringify({
            temperature,
            humidity,
            pressure,
            gas_resistance
        });

        db.run(
            `INSERT INTO sensor_records
            (timestamp,temperature,humidity,pressure,gas_resistance,device_id,esp_time_ms,esp_uptime_ms,server_recv_ms,server_time_iso,upload_delay_ms,payload_type,raw_payload,payload_json,raw_json)
            VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)`,
            [
                serverRecvMs,
                temperature,
                humidity,
                pressure,
                gas_resistance,
                deviceId,
                timing.esp_time_ms,
                timing.esp_uptime_ms,
                timing.server_recv_ms,
                timing.server_time_iso,
                timing.upload_delay_ms,
                payloadType,
                rawPayload,
                payloadJson,
                rawPayload
            ],
            async function (err) {
                if (err) {
                    return sendSensorDbError(res, err, true);
                }

                if (typeof dbRun === "function" && typeof dbAll === "function") {
                    try {
                        await refreshDeviceActivity(dbRun, dbAll, readDeviceMetadata({
                            body: {
                                ...normalizedBody,
                                device_id: deviceId,
                                payload_type: payloadType
                            },
                            headers: req.headers,
                            payloadType,
                            serverRecvMs
                        }), payloadType);
                        if (usedFallbackDeviceId) {
                            await recordEvent(dbRun, {
                                event_type: "system",
                                event_name: "system_log_created",
                                device_id: deviceId,
                                severity: "warning",
                                message: "legacy /sensor upload missing device_id; using unknown_device",
                                payload: {
                                    payload_type: payloadType,
                                    route: "/sensor"
                                },
                                source: "legacy_sensor",
                                server_recv_ms: serverRecvMs
                            });
                        }
                    } catch (error) {
                        logger.warn(`[sensor] legacy status refresh failed device_id=${deviceId || "-"} message=${JSON.stringify(error?.message || "-")}`);
                    }
                }

                logger.log(
                    `[sensor] upload device_id=${deviceId || "-"} server_recv_ms=${timing.server_recv_ms} payload_type=${payloadType} upload_delay_ms=${timing.upload_delay_ms ?? "null"}`
                );

                res.json({
                    ok: true,
                    success: true,
                    id: this.lastID,
                    payload_type: payloadType,
                    raw_payload: rawPayload,
                    payload: JSON.parse(payloadJson),
                    ...timing
                });
            }
        );
    });

    router.get("/sensor/latest", (req, res) => {
        db.get(
            "SELECT * FROM sensor_records WHERE deleted_at IS NULL ORDER BY id DESC LIMIT 1",
            [],
            (err, row) => {
                if (err) {
                    return sendSensorDbError(res, err);
                }

                if (!row) {
                    res.json({});
                    return;
                }

                db.get(
                    "SELECT * FROM device_status WHERE device_id=? AND deleted_at IS NULL LIMIT 1",
                    [row.device_id],
                    (statusErr, deviceStatusRow) => {
                        if (statusErr) {
                            return sendSensorDbError(res, statusErr);
                        }

                        db.get(
                            "SELECT * FROM device_module_status WHERE device_id=? AND module_type='sensor.bme690' AND deleted_at IS NULL LIMIT 1",
                            [row.device_id],
                            (moduleErr, moduleStatusRow) => {
                                if (moduleErr) {
                                    return sendSensorDbError(res, moduleErr);
                                }

                                res.json(enrichLatestSensorRow(row, deviceStatusRow, moduleStatusRow));
                            }
                        );
                    }
                );
            }
        );
    });

    router.get("/sensor/history", (req, res) => {
        const limit = readHistoryLimit(req.query.limit);

        db.all(
            `SELECT * FROM (
                SELECT * FROM sensor_records WHERE deleted_at IS NULL ORDER BY id DESC LIMIT ?
            ) ORDER BY id ASC`,
            [limit],
            (err, rows) => {
                if (err) {
                    return sendSensorDbError(res, err);
                }

                res.json(rows || []);
            }
        );
    });

    return router;
}

module.exports = {
    createSensorRouter,
    normalizeSensorBody,
    readHistoryLimit
};
