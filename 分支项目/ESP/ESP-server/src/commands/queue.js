const crypto = require("crypto");
const {
    COMMAND_DEVICE_ID_MAX_LENGTH,
    isCommandWhitelisted,
    normalizeCommandName,
    normalizeCommand
} = require("./schema");
const {
    readPositiveInteger
} = require("../utils/env");
const {
    runUpdateThenInsert
} = require("../db/upsert");
const {
    recordEvent
} = require("../services/eventLogService");

const DEFAULT_COMMAND_DISPATCH_TIMEOUT_MS = 60000;
const TERMINAL_COMMAND_STATUSES = new Set(["succeeded", "failed", "expired", "rejected", "completed", "success"]);
const COMMAND_PROTOCOL_VERSION_MAX_LENGTH = 40;

function nowIso() {
    return new Date().toISOString();
}

function makeCommandId() {
    if (typeof crypto.randomUUID === "function") {
        return crypto.randomUUID();
    }

    return crypto.randomBytes(16).toString("hex");
}

function readCommandDispatchTimeoutMs() {
    return readPositiveInteger(
        process.env.COMMAND_DISPATCH_TIMEOUT_MS,
        DEFAULT_COMMAND_DISPATCH_TIMEOUT_MS
    );
}

function parseJsonObject(value, fallback = {}) {
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

function normalizeCommandStatus(status) {
    const text = typeof status === "string" ? status.trim().toLowerCase() : "";
    if (text === "completed" || text === "success") {
        return "succeeded";
    }
    return text;
}

function normalizeAckStatus(status) {
    const normalized = normalizeCommandStatus(status);
    return ["succeeded", "failed", "expired", "rejected"].includes(normalized)
        ? normalized
        : "";
}

function mapStatusForClient(status) {
    return normalizeCommandStatus(status) || status || "";
}

function normalizeCapabilityCommands(values) {
    if (!Array.isArray(values)) {
        return [];
    }

    const commands = [];
    const seen = new Set();
    for (const value of values) {
        const name = normalizeCommandName(value);
        if (!name || seen.has(name) || !isCommandWhitelisted(name)) {
            continue;
        }

        seen.add(name);
        commands.push(name);
    }

    return commands;
}

function normalizeCapabilities(capabilities) {
    if (Array.isArray(capabilities)) {
        return {
            commands: normalizeCapabilityCommands(capabilities)
        };
    }

    if (capabilities && typeof capabilities === "object") {
        return {
            ...capabilities,
            commands: normalizeCapabilityCommands(capabilities.commands)
        };
    }

    return {
        commands: []
    };
}

async function upsertDeviceCapabilities(dbRun, input) {
    const deviceId = typeof input?.device_id === "string" ? input.device_id.trim() : "";
    if (!deviceId) {
        return {
            ok: false,
            code: "DEVICE_ID_REQUIRED",
            error: "device_id is required"
        };
    }
    if (deviceId.length > COMMAND_DEVICE_ID_MAX_LENGTH) {
        return {
            ok: false,
            code: "DEVICE_ID_INVALID",
            error: `device_id must be <= ${COMMAND_DEVICE_ID_MAX_LENGTH} characters`
        };
    }

    const timestamp = nowIso();
    const protocolVersion = typeof input.protocol_version === "string"
        ? input.protocol_version.trim()
        : "";
    if (protocolVersion.length > COMMAND_PROTOCOL_VERSION_MAX_LENGTH) {
        return {
            ok: false,
            code: "PROTOCOL_VERSION_INVALID",
            error: `protocol_version must be <= ${COMMAND_PROTOCOL_VERSION_MAX_LENGTH} characters`
        };
    }
    const capabilities = normalizeCapabilities(input.capabilities);

    await runUpdateThenInsert(dbRun, {
        updateSql: `UPDATE device_capabilities
            SET protocol_version=?,
                capabilities_json=?,
                last_seen_at=?,
                updated_at=?
            WHERE device_id=?`,
        updateParams: [
            protocolVersion,
            JSON.stringify(capabilities),
            timestamp,
            timestamp,
            deviceId
        ],
        insertSql: `INSERT INTO device_capabilities
            (device_id,protocol_version,capabilities_json,last_seen_at,created_at,updated_at)
            VALUES(?,?,?,?,?,?)`,
        insertParams: [
            deviceId,
            protocolVersion,
            JSON.stringify(capabilities),
            timestamp,
            timestamp,
            timestamp
        ]
    });

    return {
        ok: true,
        device_id: deviceId,
        protocol_version: protocolVersion,
        capabilities,
        server_time_ms: Date.now()
    };
}

async function getDeviceCapabilities(dbAll, deviceId) {
    const rows = await dbAll(
        "SELECT * FROM device_capabilities WHERE device_id=? LIMIT 1",
        [deviceId]
    );
    const row = rows[0];
    if (!row) {
        return null;
    }

    return {
        device_id: row.device_id,
        protocol_version: row.protocol_version || "",
        capabilities: normalizeCapabilities(parseJsonObject(row.capabilities_json)),
        last_seen_at: row.last_seen_at,
        updated_at: row.updated_at
    };
}

async function checkDeviceSupportsCommand(dbAll, deviceId, commandName) {
    const snapshot = await getDeviceCapabilities(dbAll, deviceId);
    if (!snapshot) {
        return {
            ok: false,
            code: "DEVICE_CAPABILITIES_REQUIRED",
            error: "device capabilities must be registered before commands can be queued"
        };
    }

    const commands = snapshot.capabilities.commands || [];
    if (!commands.includes(commandName)) {
        return {
            ok: false,
            code: "DEVICE_COMMAND_UNSUPPORTED",
            error: "device did not register this command"
        };
    }

    return {
        ok: true
    };
}

async function enqueueCommand(dbRun, dbAll, input, options = {}) {
    const normalized = normalizeCommand(input, options);
    if (!normalized.ok) {
        return normalized;
    }

    const command = normalized.command;
    const support = await checkDeviceSupportsCommand(dbAll, command.target_device_id, command.name);
    if (!support.ok) {
        return {
            ...support,
            name: command.name,
            target_device_id: command.target_device_id
        };
    }

    const commandId = makeCommandId();
    const timestamp = nowIso();
    await dbRun(
        `INSERT INTO command_queue
        (command_id,device_id,name,payload_json,status,source,requested_by,related_llm_record_id,raw_json,created_at,updated_at)
        VALUES(?,?,?,?,?,?,?,?,?,?,?)`,
        [
            commandId,
            command.target_device_id,
            command.name,
            JSON.stringify(command.payload),
            "queued",
            options.source || "api",
            options.requestedBy || "",
            Number.isInteger(options.relatedLlmRecordId) ? options.relatedLlmRecordId : null,
            JSON.stringify({
                reason: command.reason || "",
                raw_command: input
            }),
            timestamp,
            timestamp
        ]
    );

    try {
        await recordEvent(dbRun, {
            event_type: "command",
            event_name: "command_created",
            device_id: command.target_device_id,
            severity: "info",
            message: command.name,
            payload: {
                command_id: commandId,
                name: command.name,
                payload: command.payload,
                status: "queued",
                source: options.source || "api",
                requested_by: options.requestedBy || ""
            },
            source: options.source || "api",
            server_recv_ms: Date.now()
        });
    } catch (_) {
        // Event logging is best-effort; command queuing remains the source of truth.
    }

    return {
        ok: true,
        command: {
            command_id: commandId,
            device_id: command.target_device_id,
            name: command.name,
            payload: command.payload,
            status: "queued",
            created_at: timestamp
        }
    };
}

function mapCommandRow(row) {
    return {
        command_id: row.command_id,
        device_id: row.device_id,
        name: row.name,
        payload: parseJsonObject(row.payload_json),
        source: row.source || "",
        requested_by: row.requested_by || "",
        gateway_id: row.gateway_id || "",
        error_code: row.error_code || "",
        error_message: row.error_message || "",
        status: mapStatusForClient(row.status),
        result: parseJsonObject(row.result_json, null),
        created_at: row.created_at,
        updated_at: row.updated_at,
        dispatched_at: row.dispatched_at,
        dispatch_count: Number(row.dispatch_count) || 0,
        acknowledged_at: row.acknowledged_at,
        completed_at: row.completed_at,
        expires_at: row.expires_at,
        reject_reason: row.reject_reason || ""
    };
}

async function listPendingCommands(dbRun, dbAll, deviceId, limit = 10, options = {}) {
    const safeLimit = Math.min(Math.max(Number.parseInt(limit, 10) || 10, 1), 50);
    const redispatchBefore = new Date(Date.now() - readCommandDispatchTimeoutMs()).toISOString();
    const gatewayId = typeof options.gatewayId === "string" ? options.gatewayId.trim() : "";
    const rows = await dbAll(
        `SELECT * FROM command_queue
        WHERE device_id=?
          AND (?='' OR gateway_id IS NULL OR gateway_id='' OR gateway_id=?)
          AND deleted_at IS NULL
          AND (
              status='queued'
              OR (
                  status='dispatched'
                  AND COALESCE(dispatched_at, updated_at, created_at) <= ?
              )
          )
        ORDER BY id ASC LIMIT ?`,
        [deviceId, gatewayId, gatewayId, redispatchBefore, safeLimit]
    );

    const dispatchedAt = rows.length > 0 ? nowIso() : "";
    const claimedRows = [];
    for (const row of rows) {
        const result = await dbRun(
            `UPDATE command_queue
            SET status='dispatched',
                gateway_id=CASE WHEN gateway_id IS NULL OR gateway_id='' THEN ? ELSE gateway_id END,
                dispatched_at=?,
                dispatch_count=COALESCE(dispatch_count, 0) + 1,
                updated_at=?
            WHERE command_id=?
              AND (?='' OR gateway_id IS NULL OR gateway_id='' OR gateway_id=?)
              AND deleted_at IS NULL
              AND (
                  status='queued'
                  OR (
                      status='dispatched'
                      AND COALESCE(dispatched_at, updated_at, created_at) <= ?
                  )
              )`,
            [gatewayId, dispatchedAt, dispatchedAt, row.command_id, gatewayId, gatewayId, redispatchBefore]
        );

        if (result.changes > 0) {
            claimedRows.push(row);
        }
    }

    return claimedRows.map(row => ({
        ...mapCommandRow(row),
        status: "dispatched",
        gateway_id: row.gateway_id || gatewayId,
        dispatched_at: dispatchedAt,
        updated_at: dispatchedAt,
        dispatch_count: (Number(row.dispatch_count) || 0) + 1
    }));
}

async function ackCommand(dbRun, commandId, input, dbAll = null, options = {}) {
    const status = normalizeAckStatus(input?.status);
    if (!status) {
        return {
            ok: false,
            code: "COMMAND_ACK_STATUS_INVALID",
            error: "status must be succeeded, failed, expired, or rejected"
        };
    }

    const timestamp = nowIso();
    const resultJson = input?.result && typeof input.result === "object"
        ? JSON.stringify(input.result)
        : null;
    const errorMessage = typeof input?.error_message === "string" ? input.error_message.slice(0, 500) : "";
    const errorCode = status === "failed" || status === "expired" || status === "rejected"
        ? (typeof input?.error_code === "string" && input.error_code.trim() ? input.error_code.trim().slice(0, 80) : "COMMAND_FAILED")
        : "";
    const gatewayId = typeof options.gatewayId === "string" ? options.gatewayId.trim() : "";
    const deviceId = typeof options.deviceId === "string" ? options.deviceId.trim() : "";
    let existingCommand = null;
    if (typeof dbAll === "function") {
        const rows = await dbAll(
            "SELECT * FROM command_queue WHERE command_id=? AND deleted_at IS NULL LIMIT 1",
            [commandId]
        );
        existingCommand = rows[0] || null;
    }
    if (typeof dbAll === "function" && !existingCommand) {
        return {
            ok: false,
            code: "COMMAND_ACK_NOT_ACCEPTED",
            error: "command not found",
            status,
            command_id: commandId,
            server_time_ms: Date.now()
        };
    }
    if (existingCommand && deviceId && existingCommand.device_id !== deviceId) {
        return {
            ok: false,
            code: "COMMAND_ACK_OWNERSHIP_MISMATCH",
            error: "command does not belong to device"
        };
    }
    if (existingCommand && gatewayId && existingCommand.gateway_id && existingCommand.gateway_id !== gatewayId) {
        return {
            ok: false,
            code: "COMMAND_ACK_OWNERSHIP_MISMATCH",
            error: "command does not belong to gateway"
        };
    }
    if (existingCommand && TERMINAL_COMMAND_STATUSES.has(normalizeCommandStatus(existingCommand.status))) {
        return {
            ok: true,
            idempotent: true,
            status: mapStatusForClient(existingCommand.status),
            command_id: commandId,
            server_time_ms: Date.now()
        };
    }

    const result = await dbRun(
        `UPDATE command_queue
        SET status=?,
            gateway_id=CASE WHEN gateway_id IS NULL OR gateway_id='' THEN ? ELSE gateway_id END,
            result_json=?,
            error_code=?,
            error_message=?,
            acknowledged_at=?,
            completed_at=?,
            updated_at=?
        WHERE command_id=?
          AND deleted_at IS NULL
          AND status IN ('queued','dispatched','acknowledged')`,
        [
            status,
            gatewayId,
            resultJson,
            errorCode,
            errorMessage,
            timestamp,
            timestamp,
            timestamp,
            commandId
        ]
    );

    if (result.changes > 0) {
        try {
            await recordEvent(dbRun, {
                event_type: "command",
                event_name: "command_acknowledged",
                device_id: existingCommand?.device_id || "",
                severity: status === "failed" ? "warning" : "info",
                message: existingCommand?.name || `command ${status}`,
                payload: {
                    command_id: commandId,
                    name: existingCommand?.name || "",
                    status,
                    gateway_id: gatewayId || existingCommand?.gateway_id || "",
                    result: parseJsonObject(resultJson, null),
                    error_code: errorCode,
                    error_message: errorMessage
                },
                source: "command_ack",
                server_recv_ms: Date.now()
            });
        } catch (_) {
            // Event logging is best-effort; ACK status has already been persisted.
        }
    }

    return {
        ok: result.changes > 0,
        ...(result.changes > 0 ? {} : {
            code: "COMMAND_ACK_NOT_ACCEPTED",
            error: "command not found or already completed"
        }),
        status,
        command_id: commandId,
        server_time_ms: Date.now()
    };
}

async function listCommandHistory(dbAll, filters = {}) {
    const limit = Math.min(Math.max(Number.parseInt(filters.limit, 10) || 50, 1), 200);
    const params = [];
    let where = "";
    if (filters.device_id) {
        where = "WHERE device_id=? AND deleted_at IS NULL";
        params.push(filters.device_id);
    } else {
        where = "WHERE deleted_at IS NULL";
    }
    params.push(limit);

    const rows = await dbAll(
        `SELECT * FROM command_queue ${where} ORDER BY id DESC LIMIT ?`,
        params
    );

    return rows.map(mapCommandRow);
}

module.exports = {
    ackCommand,
    enqueueCommand,
    getDeviceCapabilities,
    listCommandHistory,
    listPendingCommands,
    readCommandDispatchTimeoutMs,
    upsertDeviceCapabilities
};
