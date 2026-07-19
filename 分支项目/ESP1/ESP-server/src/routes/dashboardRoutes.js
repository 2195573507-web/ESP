const express = require("express");
const {
    readDashboardCsiHistory,
    readDashboardAsrLatest,
    readDashboardDeviceStatus,
    readDashboardLimit,
    readDashboardLlmLatest,
    readDashboardModulesStatus,
    readDashboardOverview,
    readDashboardSensorHistoryQuery,
    readDashboardSnapshotHistory,
    readDashboardSensorHistory,
    readDashboardSensorLatest,
    readLatestDashboardSnapshot,
    readDashboardTimeStatus,
    persistDashboardSnapshot,
    prepareDashboardSnapshot
} = require("../services/dashboardService");
const {
    bindDeviceToGateway,
    requireGatewayAuth
} = require("../services/gatewayAuthService");
const {
    PRIORITY_HIGH,
    enqueuePersistenceJob
} = require("../services/persistenceQueue");

function dashboardEnvelope(data, nowMs = Date.now()) {
    return {
        ok: true,
        server_time_ms: nowMs,
        data,
        error: null
    };
}

function dashboardError(code, message, nowMs = Date.now()) {
    return {
        ok: false,
        server_time_ms: nowMs,
        data: null,
        error: {
            code,
            message
        }
    };
}

function sendDashboardError(res, status, code, message) {
    return res.status(status).json(dashboardError(code, message));
}

function createDashboardRouter(options) {
    const router = express.Router();
    const dbRun = options.dbRun;
    const dbAll = options.dbAll;
    const logger = options.logger || console;
    const runtimeCache = options.runtimeCache;
    const persistenceWorker = options.persistenceWorker;
    const gatewayContext = {
        dbRun,
        dbAll,
        enqueuePersistenceJob,
        persistenceWorker
    };
    const gatewayOnly = requireGatewayAuth(gatewayContext);

    function route(handler, errorCode) {
        return async (req, res) => {
            try {
                const data = await handler(req);
                return res.json(dashboardEnvelope(data));
            } catch (error) {
                logger.error(`[dashboard-v1] ${errorCode} ${error?.message || error}`);
                return sendDashboardError(res, 500, errorCode, "dashboard read failed");
            }
        };
    }

    router.get("/overview", route(
        req => readDashboardOverview(dbAll, req.query, {
            logger,
            runtimeCache
        }),
        "DASHBOARD_OVERVIEW_READ_FAILED"
    ));

    router.post("/snapshot", gatewayOnly, async (req, res) => {
        const serverRecvMs = Date.now();
        const gatewayId = req.gatewayAuth?.gateway_id || "";
        try {
            const result = prepareDashboardSnapshot(req.body, {
                headers: req.headers,
                serverRecvMs,
                trustedGatewayId: gatewayId
            });
            if (!result.ok) {
                return sendDashboardError(res, result.status || 400, result.code || "INVALID_DASHBOARD_SNAPSHOT", result.error || "invalid dashboard snapshot");
            }
            runtimeCache?.updateDashboardSnapshot?.(result.snapshot, {
                serverRecvMs
            });
            const queued = enqueuePersistenceJob({
                type: "gateway.dashboard_snapshot",
                priority: PRIORITY_HIGH,
                snapshot_id: result.snapshotId,
                run: async () => {
                    await persistDashboardSnapshot(dbRun, dbAll, result);
                    for (const deviceId of result.data.bound_device_ids || []) {
                        await bindDeviceToGateway(dbRun, gatewayId, deviceId, "dashboard_snapshot", serverRecvMs, dbAll);
                    }
                }
            });
            persistenceWorker?.scheduleImmediateFlushIfNeeded?.();
            logger.log(`[dashboard-v1] snapshot queued gateway_id=${result.data.gateway_id || "-"} job_id=${queued.job_id}`);

            return res.status(result.status || 202).json(dashboardEnvelope(result.data, serverRecvMs));
        } catch (error) {
            logger.error(`[dashboard-v1] DASHBOARD_SNAPSHOT_WRITE_FAILED ${error?.message || error}`);
            return sendDashboardError(res, 500, "DASHBOARD_SNAPSHOT_WRITE_FAILED", "dashboard snapshot write failed");
        }
    });

    router.get("/latest", route(
        () => readLatestDashboardSnapshot(dbAll).then(snapshot => snapshot || null),
        "DASHBOARD_LATEST_READ_FAILED"
    ));

    router.get("/history", async (req, res) => {
        const limit = readDashboardLimit(req.query.limit);
        if (!limit.ok) {
            return sendDashboardError(res, 400, limit.code, limit.message);
        }

        try {
            const data = await readDashboardSnapshotHistory(dbAll, req.query);
            return res.json(dashboardEnvelope({
                snapshots: data
            }));
        } catch (error) {
            logger.error(`[dashboard-v1] DASHBOARD_HISTORY_READ_FAILED ${error?.message || error}`);
            return sendDashboardError(res, 500, "DASHBOARD_HISTORY_READ_FAILED", "dashboard history read failed");
        }
    });

    router.get("/sensors/latest", route(
        req => readDashboardSensorLatest(dbAll, req.query),
        "DASHBOARD_SENSOR_LATEST_READ_FAILED"
    ));

    router.get("/sensors/history", async (req, res) => {
        const historyQuery = readDashboardSensorHistoryQuery(req.query);
        if (!historyQuery.ok) {
            return sendDashboardError(res, 400, historyQuery.code, historyQuery.message);
        }

        try {
            const data = await readDashboardSensorHistory(dbAll, req.query, {
                historyQuery
            });
            return res.json(dashboardEnvelope(data));
        } catch (error) {
            logger.error(`[dashboard-v1] DASHBOARD_SENSOR_HISTORY_READ_FAILED ${error?.message || error}`);
            return sendDashboardError(res, 500, "DASHBOARD_SENSOR_HISTORY_READ_FAILED", "dashboard read failed");
        }
    });

    router.get("/csi/history", async (req, res) => {
        const limit = readDashboardLimit(req.query.limit);
        if (!limit.ok) {
            return sendDashboardError(res, 400, limit.code, limit.message);
        }

        try {
            const data = await readDashboardCsiHistory(dbAll, req.query);
            return res.json(dashboardEnvelope(data));
        } catch (error) {
            logger.error(`[dashboard-v1] DASHBOARD_CSI_HISTORY_READ_FAILED ${error?.message || error}`);
            return sendDashboardError(res, 500, "DASHBOARD_CSI_HISTORY_READ_FAILED", "dashboard read failed");
        }
    });

    router.get("/devices/:device_id/history", async (req, res) => {
        const historyQuery = readDashboardSensorHistoryQuery(req.query);
        if (!historyQuery.ok) {
            return sendDashboardError(res, 400, historyQuery.code, historyQuery.message);
        }

        try {
            const data = await readDashboardSensorHistory(dbAll, {
                ...req.query,
                device_id: req.params.device_id
            }, {
                historyQuery
            });
            return res.json(dashboardEnvelope(data));
        } catch (error) {
            logger.error(`[dashboard-v1] DASHBOARD_DEVICE_HISTORY_READ_FAILED ${error?.message || error}`);
            return sendDashboardError(res, 500, "DASHBOARD_DEVICE_HISTORY_READ_FAILED", "dashboard read failed");
        }
    });

    router.get("/asr/latest", route(
        () => readDashboardAsrLatest(dbAll),
        "DASHBOARD_ASR_LATEST_READ_FAILED"
    ));

    router.get("/llm/latest", route(
        () => readDashboardLlmLatest(dbAll),
        "DASHBOARD_LLM_LATEST_READ_FAILED"
    ));

    router.get("/time/status", route(
        () => readDashboardTimeStatus(),
        "DASHBOARD_TIME_STATUS_READ_FAILED"
    ));

    router.get("/device/status", route(
        req => readDashboardDeviceStatus(dbAll, req.query),
        "DASHBOARD_DEVICE_STATUS_READ_FAILED"
    ));

    router.get("/modules/status", route(
        req => readDashboardModulesStatus(dbAll, req.query),
        "DASHBOARD_MODULE_STATUS_READ_FAILED"
    ));

    return router;
}

module.exports = {
    createDashboardRouter,
    dashboardEnvelope,
    dashboardError
};
