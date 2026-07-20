const express = require("express");
const {
    apiEnvelope,
    apiError
} = require("../utils/apiEnvelope");
const {
    cleanupEvents,
    listEventRows,
    mapAlarmRow,
    mapSystemLogRow,
    recordEvent
} = require("../services/eventLogService");
const {
    subscribeSse
} = require("../services/eventStreamService");
const {
    bindDeviceToGateway,
    requireGatewayAuth
} = require("../services/gatewayAuthService");

function createEventRouter(options) {
    const router = express.Router();
    const dbRun = options.dbRun;
    const dbAll = options.dbAll;
    const logger = options.logger || console;
    const gatewayContext = {
        dbRun,
        dbAll
    };
    const gatewayOnly = requireGatewayAuth(gatewayContext);

    function eventListRoute(filtersForRequest, mapRow, dataKey, errorCode) {
        return async (req, res) => {
            try {
                const rows = await listEventRows(dbAll, filtersForRequest(req));
                return res.json(apiEnvelope({
                    [dataKey]: rows.map(mapRow)
                }));
            } catch (error) {
                logger.error(`[events] ${errorCode} ${error?.message || error}`);
                return res.status(500).json(apiError(errorCode, "event log read failed"));
            }
        };
    }

    router.get("/api/logs/v1/alarms", eventListRoute(req => ({
        event_type: "alarm",
        limit: req.query.limit
    }), mapAlarmRow, "alarms", "ALARM_LOG_READ_FAILED"));

    router.post("/api/logs/v1/alarms", gatewayOnly, async (req, res) => {
        try {
            const gatewayId = req.gatewayAuth?.gateway_id || "";
            const deviceId = req.body?.device_id || gatewayId;
            await bindDeviceToGateway(dbRun, gatewayId, deviceId, "alarm_log", Date.now(), dbAll);
            const event = await recordEvent(dbRun, {
                event_type: "alarm",
                event_name: "alarm_created",
                device_id: deviceId,
                severity: req.body?.level || "warning",
                message: req.body?.message || req.body?.title || "alarm",
                payload: {
                    title: req.body?.title || "",
                    room_id: req.body?.room_id || "",
                    room_name: req.body?.room_name || "",
                    acknowledged: Boolean(req.body?.acknowledged),
                    payload: req.body?.payload && typeof req.body.payload === "object" ? req.body.payload : {}
                },
                source: req.body?.source || "gateway",
                server_recv_ms: Date.now()
            });
            return res.status(201).json(apiEnvelope({
                alarm: {
                    id: event.event_id,
                    level: event.severity,
                    source: event.source,
                    device_id: event.device_id,
                    room_id: event.payload.room_id || "",
                    room_name: event.payload.room_name || "",
                    title: event.payload.title || "",
                    message: event.message,
                    payload: event.payload.payload || {},
                    created_at_ms: event.server_recv_ms,
                    acknowledged: Boolean(event.payload.acknowledged)
                }
            }));
        } catch (error) {
            logger.error(`[events] ALARM_LOG_WRITE_FAILED ${error?.message || error}`);
            return res.status(500).json(apiError("ALARM_LOG_WRITE_FAILED", "alarm log write failed"));
        }
    });

    router.get("/api/logs/v1/system", eventListRoute(req => ({
        event_types: ["system", "device", "csi", "command", "voice"],
        limit: req.query.limit
    }), mapSystemLogRow, "logs", "SYSTEM_LOG_READ_FAILED"));

    router.post("/api/logs/v1/system", gatewayOnly, async (req, res) => {
        try {
            const gatewayId = req.gatewayAuth?.gateway_id || "";
            const deviceId = req.body?.device_id || gatewayId;
            await bindDeviceToGateway(dbRun, gatewayId, deviceId, "system_log", Date.now(), dbAll);
            const event = await recordEvent(dbRun, {
                event_type: "system",
                event_name: "system_log_created",
                device_id: deviceId,
                severity: req.body?.level || "info",
                message: req.body?.message || "system log",
                payload: req.body?.payload && typeof req.body.payload === "object" ? req.body.payload : {},
                source: req.body?.source || "gateway",
                server_recv_ms: Date.now()
            });
            return res.status(201).json(apiEnvelope({
                log: {
                    id: event.event_id,
                    level: event.severity,
                    source: event.source,
                    message: event.message,
                    payload: event.payload,
                    created_at_ms: event.server_recv_ms
                }
            }));
        } catch (error) {
            logger.error(`[events] SYSTEM_LOG_WRITE_FAILED ${error?.message || error}`);
            return res.status(500).json(apiError("SYSTEM_LOG_WRITE_FAILED", "system log write failed"));
        }
    });

    router.get("/api/voice/v1/events", eventListRoute(req => ({
        event_type: "voice",
        limit: req.query.limit
    }), row => {
        const payload = (() => {
            try {
                return JSON.parse(row.payload_json || "{}");
            } catch (_) {
                return {};
            }
        })();
        return {
            id: row.event_id || String(row.id),
            event: row.event_name,
            device_id: row.device_id || "",
            message: row.message || "",
            payload,
            created_at_ms: Number(row.server_recv_ms) || null
        };
    }, "events", "VOICE_EVENT_READ_FAILED"));

    router.post("/api/logs/v1/cleanup", async (req, res) => {
        try {
            const result = await cleanupEvents(dbRun, dbAll, req.body || {});
            if (!result.ok) {
                return res.status(400).json(apiError(result.code, result.error));
            }

            return res.json(apiEnvelope({
                deleted: result.deleted,
                dry_run: result.dry_run
            }));
        } catch (error) {
            logger.error(`[events] LOG_CLEANUP_FAILED ${error?.message || error}`);
            return res.status(500).json(apiError("LOG_CLEANUP_FAILED", "log cleanup failed"));
        }
    });

    router.delete("/api/logs/v1/events", async (req, res) => {
        try {
            const result = await cleanupEvents(dbRun, dbAll, {
                type: req.query.type || "system",
                older_than_ms: req.query.older_than_ms,
                force: req.query.force
            });
            if (!result.ok) {
                return res.status(400).json(apiError(result.code, result.error));
            }

            return res.json(apiEnvelope({
                deleted: result.deleted,
                dry_run: false
            }));
        } catch (error) {
            logger.error(`[events] LOG_DELETE_FAILED ${error?.message || error}`);
            return res.status(500).json(apiError("LOG_DELETE_FAILED", "log delete failed"));
        }
    });

    router.get("/api/events/v1/stream", (req, res) => {
        subscribeSse(req, res, {
            dbRun
        });
    });

    return router;
}

module.exports = {
    createEventRouter
};
