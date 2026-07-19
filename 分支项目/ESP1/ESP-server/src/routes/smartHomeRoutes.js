const express = require("express");
const {
    apiEnvelope,
    apiError
} = require("../utils/apiEnvelope");
const {
    ackSmartHomeCommand,
    createSmartHomeCommand,
    claimPendingSmartHomeCommands,
    listSmartHomeCommands,
    readSmartHomeStatus,
    upsertSmartHomeState
} = require("../services/smartHomeService");
const {
    bindDeviceToGateway,
    requireGatewayAuth
} = require("../services/gatewayAuthService");

function createSmartHomeRouter(options) {
    const router = express.Router();
    const dbRun = options.dbRun;
    const dbAll = options.dbAll;
    const logger = options.logger || console;
    const gatewayContext = {
        dbRun,
        dbAll
    };
    const gatewayOnly = requireGatewayAuth(gatewayContext);

    router.get("/api/smart-home/v1/status", async (req, res) => {
        try {
            const status = await readSmartHomeStatus(dbAll);
            return res.json(apiEnvelope(status));
        } catch (error) {
            logger.error(`[smart-home] status failed ${error?.message || error}`);
            return res.status(500).json(apiError("SMART_HOME_STATUS_READ_FAILED", "smart home status read failed"));
        }
    });

    router.post("/api/smart-home/v1/state", gatewayOnly, async (req, res) => {
        try {
            const gatewayId = req.gatewayAuth?.gateway_id || "";
            const result = await upsertSmartHomeState(dbRun, {
                ...req.body,
                gateway_id: gatewayId
            });
            if (!result.ok) {
                return res.status(400).json(apiError(result.code, result.error));
            }
            for (const device of result.devices || []) {
                await bindDeviceToGateway(dbRun, gatewayId, device.id, "smart_home_state", result.updated_at_ms, dbAll);
            }

            return res.status(202).json(apiEnvelope({
                provider: result.provider,
                gateway_id: result.gateway_id,
                devices: result.devices,
                updated_at_ms: result.updated_at_ms
            }));
        } catch (error) {
            logger.error(`[smart-home] state failed ${error?.message || error}`);
            return res.status(500).json(apiError("SMART_HOME_STATE_WRITE_FAILED", "smart home state write failed"));
        }
    });

    router.post("/api/smart-home/v1/control", async (req, res) => {
        try {
            const result = await createSmartHomeCommand(dbRun, req.body);
            if (!result.ok) {
                return res.status(400).json(apiError(result.code, result.error));
            }

            return res.status(202).json(apiEnvelope({
                command: result.command,
                message: result.message
            }));
        } catch (error) {
            logger.error(`[smart-home] control failed ${error?.message || error}`);
            return res.status(500).json(apiError("SMART_HOME_COMMAND_WRITE_FAILED", "smart home command write failed"));
        }
    });

    router.get("/api/smart-home/v1/commands", async (req, res) => {
        try {
            const commands = await listSmartHomeCommands(dbAll, {
                limit: req.query.limit
            });
            return res.json(apiEnvelope({
                commands
            }));
        } catch (error) {
            logger.error(`[smart-home] commands failed ${error?.message || error}`);
            return res.status(500).json(apiError("SMART_HOME_COMMAND_READ_FAILED", "smart home commands read failed"));
        }
    });

    router.get("/api/smart-home/v1/commands/pending", gatewayOnly, async (req, res) => {
        try {
            const gatewayId = req.gatewayAuth?.gateway_id || "";
            const commands = await claimPendingSmartHomeCommands(dbRun, dbAll, {
                gateway_id: gatewayId,
                limit: req.query.limit || 20
            });
            return res.json(apiEnvelope({
                commands
            }));
        } catch (error) {
            logger.error(`[smart-home] pending failed ${error?.message || error}`);
            return res.status(500).json(apiError("SMART_HOME_PENDING_COMMAND_READ_FAILED", "smart home pending commands read failed"));
        }
    });

    router.post("/api/smart-home/v1/commands/:command_id/ack", gatewayOnly, async (req, res) => {
        try {
            const result = await ackSmartHomeCommand(dbRun, dbAll, req.params.command_id, {
                ...req.body,
                gateway_id: req.gatewayAuth?.gateway_id || ""
            });
            if (!result.ok) {
                const status = result.code === "SMART_HOME_COMMAND_NOT_FOUND" ? 404 : 400;
                return res.status(status).json(apiError(result.code, result.error));
            }

            return res.json(apiEnvelope({
                command: result.command
            }));
        } catch (error) {
            logger.error(`[smart-home] ack failed ${error?.message || error}`);
            return res.status(500).json(apiError("SMART_HOME_COMMAND_ACK_FAILED", "smart home command ack failed"));
        }
    });

    return router;
}

module.exports = {
    createSmartHomeRouter
};
