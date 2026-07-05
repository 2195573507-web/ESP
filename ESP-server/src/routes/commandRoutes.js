const express = require("express");
const {
    ackCommand,
    enqueueCommand,
    getDeviceCapabilities,
    listCommandHistory,
    listPendingCommands,
    upsertDeviceCapabilities
} = require("../commands/queue");
const {
    COMMAND_DEVICE_ID_MAX_LENGTH,
    listCommandDefinitions
} = require("../commands/schema");
const {
    readDeviceMetadata
} = require("../services/deviceMetadata");
const {
    refreshDeviceActivity
} = require("../services/deviceStatusService");
const {
    requireBoundDevice,
    requireGatewayAuth
} = require("../services/gatewayAuthService");
const {
    createNaturalLanguageCommand,
    listNaturalLanguageCommands
} = require("../services/naturalLanguageCommandService");
const {
    apiEnvelope,
    apiError
} = require("../utils/apiEnvelope");

function readRouteDeviceId(value) {
    return typeof value === "string" ? value.trim() : "";
}

function sendDeviceIdError(res, code, error) {
    return res.status(400).json({
        ok: false,
        code,
        error
    });
}

function validateRouteDeviceId(res, value) {
    const deviceId = readRouteDeviceId(value);
    if (!deviceId) {
        sendDeviceIdError(res, "DEVICE_ID_REQUIRED", "device_id is required");
        return "";
    }

    if (deviceId.length > COMMAND_DEVICE_ID_MAX_LENGTH) {
        sendDeviceIdError(res, "DEVICE_ID_INVALID", `device_id must be <= ${COMMAND_DEVICE_ID_MAX_LENGTH} characters`);
        return "";
    }

    return deviceId;
}

function validateOptionalRouteDeviceId(res, value) {
    const deviceId = readRouteDeviceId(value);
    if (!deviceId) {
        return "";
    }

    if (deviceId.length > COMMAND_DEVICE_ID_MAX_LENGTH) {
        sendDeviceIdError(res, "DEVICE_ID_INVALID", `device_id must be <= ${COMMAND_DEVICE_ID_MAX_LENGTH} characters`);
        return null;
    }

    return deviceId;
}

function createCommandRouter(options) {
    const router = express.Router();
    const dbRun = options.dbRun;
    const dbAll = options.dbAll;
    const logger = options.logger || console;
    const gatewayContext = {
        dbRun,
        dbAll
    };
    const gatewayOnly = requireGatewayAuth(gatewayContext);

    async function refreshCommandStatus(req, payloadType, fallbackDeviceId) {
        const metadata = readDeviceMetadata({
            body: req.body && typeof req.body === "object" && !Buffer.isBuffer(req.body) ? req.body : {},
            headers: req.headers,
            query: req.query,
            deviceId: fallbackDeviceId,
            payloadType,
            serverRecvMs: Date.now()
        });
        if (!metadata.device_id) {
            return;
        }

        try {
            await refreshDeviceActivity(dbRun, dbAll, metadata, payloadType);
        } catch (error) {
            logger.warn(`[commands] status refresh failed payload_type=${payloadType} device_id=${metadata.device_id} message=${JSON.stringify(error?.message || "-")}`);
        }
    }

    async function readCommandDeviceId(commandId) {
        if (!commandId) {
            return "";
        }

        const rows = await dbAll(
            "SELECT device_id FROM command_queue WHERE command_id=? AND deleted_at IS NULL LIMIT 1",
            [commandId]
        );
        return rows[0]?.device_id || "";
    }

    router.get("/api/commands/whitelist", (req, res) => {
        res.json({
            ok: true,
            commands: listCommandDefinitions()
        });
    });

    router.post("/api/devices/capabilities", gatewayOnly, async (req, res) => {
        const boundDevice = await requireBoundDevice(req, res, gatewayContext, {
            source: "command.capabilities",
            deviceId: req.body?.device_id,
            allowNewBinding: true
        });
        if (!boundDevice.ok) {
            return boundDevice.response;
        }
        const result = await upsertDeviceCapabilities(dbRun, req.body);
        if (!result.ok) {
            return res.status(400).json(result);
        }

        await refreshCommandStatus(req, "command.capabilities", result.device_id);
        return res.json(result);
    });

    router.get("/api/devices/:device_id/capabilities", async (req, res) => {
        const deviceId = validateRouteDeviceId(res, req.params.device_id);
        if (!deviceId) {
            return;
        }

        const snapshot = await getDeviceCapabilities(dbAll, deviceId);
        if (!snapshot) {
            return res.status(404).json({
                ok: false,
                error: "device capabilities not found"
            });
        }

        return res.json({
            ok: true,
            ...snapshot
        });
    });

    router.post("/api/commands", async (req, res) => {
        const result = await enqueueCommand(dbRun, dbAll, req.body, {
            source: "api",
            requestedBy: "server"
        });
        if (!result.ok) {
            return res.status(400).json(result);
        }

        return res.status(201).json({
            ok: true,
            command: result.command
        });
    });

    router.get("/api/commands/pending", gatewayOnly, async (req, res) => {
        const deviceId = validateRouteDeviceId(res, req.query.device_id);
        if (!deviceId) {
            return;
        }

        const boundDevice = await requireBoundDevice(req, res, gatewayContext, {
            source: "command.poll",
            deviceId
        });
        if (!boundDevice.ok) {
            return boundDevice.response;
        }
        const commands = await listPendingCommands(dbRun, dbAll, deviceId, req.query.limit, {
            gatewayId: boundDevice.gateway_id
        });
        await refreshCommandStatus(req, "command.poll", deviceId);
        return res.json({
            ok: true,
            commands,
            server_time_ms: Date.now()
        });
    });

    router.post("/api/commands/:command_id/ack", gatewayOnly, async (req, res) => {
        const commandDeviceId = await readCommandDeviceId(req.params.command_id);
        const boundDevice = await requireBoundDevice(req, res, gatewayContext, {
            source: "command.ack",
            deviceId: commandDeviceId || req.body?.device_id
        });
        if (!boundDevice.ok) {
            return boundDevice.response;
        }
        const result = await ackCommand(dbRun, req.params.command_id, req.body, dbAll, {
            gatewayId: boundDevice.gateway_id,
            deviceId: boundDevice.device_id
        });
        if (!result.ok) {
            if (result.code === "COMMAND_ACK_STATUS_INVALID" || result.code === "COMMAND_ACK_OWNERSHIP_MISMATCH") {
                return res.status(400).json(result);
            }

            return res.status(404).json(result);
        }

        await refreshCommandStatus(req, "command.ack", commandDeviceId);
        return res.json(result);
    });

    router.get("/api/commands/history", async (req, res) => {
        const deviceId = validateOptionalRouteDeviceId(res, req.query.device_id);
        if (deviceId === null) {
            return;
        }

        const history = await listCommandHistory(dbAll, {
            device_id: deviceId,
            limit: req.query.limit
        });

        return res.json({
            ok: true,
            commands: history
        });
    });

    router.post("/api/commands/v1/natural-language", async (req, res) => {
        try {
            const result = await createNaturalLanguageCommand(dbRun, req.body);
            if (!result.ok) {
                return res.status(400).json(apiError(result.code, result.error));
            }

            return res.status(202).json(apiEnvelope({
                command: result.command
            }));
        } catch (error) {
            logger.error(`[commands] natural-language failed ${error?.message || error}`);
            return res.status(500).json(apiError("NATURAL_LANGUAGE_COMMAND_FAILED", "natural language command failed"));
        }
    });

    router.get("/api/commands/v1/recent", async (req, res) => {
        try {
            const commands = await listNaturalLanguageCommands(dbAll, {
                limit: req.query.limit
            });
            return res.json(apiEnvelope({
                commands
            }));
        } catch (error) {
            logger.error(`[commands] recent v1 failed ${error?.message || error}`);
            return res.status(500).json(apiError("COMMAND_RECENT_READ_FAILED", "recent commands read failed"));
        }
    });

    return router;
}

module.exports = {
    createCommandRouter
};
