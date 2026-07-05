const crypto = require("crypto");
const {
    runUpdateThenInsert
} = require("../db/upsert");
const {
    trimText
} = require("./deviceMetadata");
const {
    broadcastEvent
} = require("./eventStreamService");
const {
    recordEvent,
    readLimit
} = require("./eventLogService");

const SMART_HOME_PROVIDERS = new Set(["local", "s3_gateway", "none"]);
const SMART_HOME_DEVICE_TYPES = new Set(["air_conditioner", "light", "fan", "tv", "curtain", "unknown"]);
const SMART_HOME_COMMAND_STATUSES = new Set(["queued", "dispatched", "acknowledged", "succeeded", "failed", "expired", "rejected", "success"]);

function makeCommandId(prefix = "shcmd") {
    if (typeof crypto.randomUUID === "function") {
        return `${prefix}_${crypto.randomUUID()}`;
    }

    return `${prefix}_${crypto.randomBytes(16).toString("hex")}`;
}

function parseJson(value, fallback = {}) {
    if (value === undefined || value === null || value === "") {
        return fallback;
    }

    try {
        const parsed = typeof value === "string" ? JSON.parse(value) : value;
        return parsed === undefined ? fallback : parsed;
    } catch (_) {
        return fallback;
    }
}

function jsonText(value) {
    try {
        return JSON.stringify(value === undefined ? {} : value);
    } catch (_) {
        return "{}";
    }
}

function normalizeProvider(value) {
    const provider = trimText(value, 40).toLowerCase();
    return SMART_HOME_PROVIDERS.has(provider) ? provider : "local";
}

function normalizeDeviceType(value) {
    const type = trimText(value, 40).toLowerCase();
    return SMART_HOME_DEVICE_TYPES.has(type) ? type : "unknown";
}

function normalizeBoolean(value, fallback = false) {
    if (typeof value === "boolean") {
        return value;
    }
    if (typeof value === "number") {
        return value !== 0;
    }
    if (typeof value === "string") {
        const text = value.trim().toLowerCase();
        if (["true", "1", "yes", "online"].includes(text)) {
            return true;
        }
        if (["false", "0", "no", "offline"].includes(text)) {
            return false;
        }
    }
    return fallback;
}

function normalizeObject(value, fallback = {}) {
    return value && typeof value === "object" && !Array.isArray(value) ? value : fallback;
}

function mapDeviceRow(row) {
    return {
        id: row.device_id,
        type: row.device_type || "unknown",
        name: row.name || "",
        room_id: row.room_id || "",
        room_name: row.room_name || "",
        online: Boolean(Number(row.online)),
        state: normalizeObject(parseJson(row.state_json, {})),
        updated_at_ms: Number(row.updated_at_ms) || null
    };
}

function mapCommandRow(row) {
    const status = row.status === "success" ? "succeeded" : row.status;
    return {
        command_id: row.command_id,
        target_id: row.target_id,
        gateway_id: row.gateway_id || "",
        room_id: row.room_id || "",
        room_name: row.room_name || "",
        action: row.action,
        params: normalizeObject(parseJson(row.params_json, {})),
        source: row.source || "",
        requested_by: row.requested_by || "",
        status,
        result: parseJson(row.result_json, null),
        error_message: row.error_message || "",
        created_at_ms: Number(row.created_at_ms) || null,
        updated_at_ms: Number(row.updated_at_ms) || null,
        acknowledged_at_ms: Number(row.acknowledged_at_ms) || null,
        executed_at_ms: Number(row.executed_at_ms) || null
    };
}

async function readSmartHomeStatus(dbAll) {
    const rows = await dbAll(
        `SELECT * FROM smart_home_devices
        ORDER BY updated_at_ms DESC, id DESC`
    );
    const devices = rows.map(mapDeviceRow);
    const latest = rows[0] || null;
    const provider = latest ? normalizeProvider(latest.provider) : "none";
    const lastUpdateMs = latest ? (Number(latest.updated_at_ms) || null) : null;

    return {
        available: devices.some(device => device.online),
        configured: devices.length > 0,
        provider: devices.length > 0 ? provider : "none",
        last_update_ms: lastUpdateMs,
        devices
    };
}

async function upsertSmartHomeState(dbRun, body = {}) {
    const provider = normalizeProvider(body.provider || "s3_gateway");
    const gatewayId = trimText(body.gateway_id, 128);
    const devices = Array.isArray(body.devices) ? body.devices : [];
    if (devices.length === 0) {
        return {
            ok: false,
            code: "SMART_HOME_DEVICES_REQUIRED",
            error: "devices must be a non-empty array"
        };
    }

    const nowMs = Date.now();
    const storedDevices = [];
    for (const item of devices) {
        const deviceId = trimText(item?.id || item?.device_id, 128);
        if (!deviceId) {
            return {
                ok: false,
                code: "SMART_HOME_DEVICE_ID_REQUIRED",
                error: "device id is required"
            };
        }

        const deviceType = normalizeDeviceType(item?.type || item?.device_type);
        const state = normalizeObject(item?.state, {});
        await runUpdateThenInsert(dbRun, {
            updateSql: `UPDATE smart_home_devices
                SET provider=?,
                    gateway_id=?,
                    device_type=?,
                    name=?,
                    room_id=?,
                    room_name=?,
                    online=?,
                    state_json=?,
                    updated_at_ms=?
                WHERE device_id=?`,
            updateParams: [
                provider,
                gatewayId,
                deviceType,
                trimText(item?.name, 128),
                trimText(item?.room_id, 128),
                trimText(item?.room_name, 128),
                normalizeBoolean(item?.online) ? 1 : 0,
                jsonText(state),
                nowMs,
                deviceId
            ],
            insertSql: `INSERT INTO smart_home_devices
                (device_id,provider,gateway_id,device_type,name,room_id,room_name,online,state_json,created_at_ms,updated_at_ms)
                VALUES(?,?,?,?,?,?,?,?,?,?,?)`,
            insertParams: [
                deviceId,
                provider,
                gatewayId,
                deviceType,
                trimText(item?.name, 128),
                trimText(item?.room_id, 128),
                trimText(item?.room_name, 128),
                normalizeBoolean(item?.online) ? 1 : 0,
                jsonText(state),
                nowMs,
                nowMs
            ]
        });

        storedDevices.push({
            id: deviceId,
            type: deviceType,
            name: trimText(item?.name, 128),
            room_id: trimText(item?.room_id, 128),
            room_name: trimText(item?.room_name, 128),
            online: normalizeBoolean(item?.online),
            state,
            updated_at_ms: nowMs
        });
    }

    await recordEvent(dbRun, {
        event_type: "system",
        event_name: "smart_home_state_updated",
        device_id: gatewayId,
        severity: "info",
        message: "smart home state updated",
        payload: {
            provider,
            gateway_id: gatewayId,
            device_count: storedDevices.length
        },
        source: "smart_home",
        server_recv_ms: nowMs
    });

    return {
        ok: true,
        provider,
        gateway_id: gatewayId,
        devices: storedDevices,
        updated_at_ms: nowMs
    };
}

async function createSmartHomeCommand(dbRun, body = {}) {
    const targetId = trimText(body.target_id || body.device_id, 128);
    const action = trimText(body.action, 80);
    if (!targetId) {
        return {
            ok: false,
            code: "SMART_HOME_TARGET_REQUIRED",
            error: "target_id is required"
        };
    }
    if (!action) {
        return {
            ok: false,
            code: "SMART_HOME_ACTION_REQUIRED",
            error: "action is required"
        };
    }

    const nowMs = Date.now();
    const params = normalizeObject(body.params, {});
    const commandId = makeCommandId();
    const command = {
        command_id: commandId,
        target_id: targetId,
        gateway_id: trimText(body.gateway_id, 128),
        room_id: trimText(body.room_id, 128),
        room_name: trimText(body.room_name, 128),
        action,
        params,
        source: trimText(body.source, 80) || "dashboard",
        requested_by: trimText(body.requested_by, 128),
        status: "queued",
        created_at_ms: nowMs,
        updated_at_ms: nowMs
    };

    await dbRun(
        `INSERT INTO smart_home_commands
        (command_id,target_id,gateway_id,room_id,room_name,action,params_json,source,requested_by,status,created_at_ms,updated_at_ms)
        VALUES(?,?,?,?,?,?,?,?,?,?,?,?)`,
        [
            command.command_id,
            command.target_id,
            command.gateway_id,
            command.room_id,
            command.room_name,
            command.action,
            jsonText(command.params),
            command.source,
            command.requested_by,
            command.status,
            command.created_at_ms,
            command.updated_at_ms
        ]
    );

    await recordEvent(dbRun, {
        event_type: "command",
        event_name: "smart_home_command_created",
        device_id: targetId,
        severity: "info",
        message: action,
        payload: {
            ...command,
            message: "queued; waiting for gateway pull"
        },
        source: command.source,
        server_recv_ms: nowMs
    });
    broadcastEvent("command_created", command);

    return {
        ok: true,
        command,
        message: "queued; waiting for gateway pull"
    };
}

async function listSmartHomeCommands(dbAll, filters = {}) {
    const params = [];
    const where = [];
    if (filters.status) {
        where.push("status=?");
        params.push(trimText(filters.status, 40));
    }
    if (filters.gateway_id) {
        where.push("(gateway_id=? OR gateway_id='')");
        params.push(trimText(filters.gateway_id, 128));
    }
    params.push(readLimit(filters.limit, 50, 200));

    const rows = await dbAll(
        `SELECT * FROM smart_home_commands
        ${where.length ? `WHERE ${where.join(" AND ")}` : ""}
        ORDER BY id DESC
        LIMIT ?`,
        params
    );
    return rows.map(mapCommandRow);
}

async function claimPendingSmartHomeCommands(dbRun, dbAll, filters = {}) {
    const gatewayId = trimText(filters.gateway_id, 128);
    const commands = await listSmartHomeCommands(dbAll, {
        status: "queued",
        gateway_id: gatewayId,
        limit: filters.limit || 20
    });
    const nowMs = Date.now();
    const claimed = [];
    for (const command of commands.slice().reverse()) {
        const result = await dbRun(
            `UPDATE smart_home_commands
            SET status='dispatched', gateway_id=CASE WHEN gateway_id='' THEN ? ELSE gateway_id END, updated_at_ms=?
            WHERE command_id=? AND status='queued'`,
            [gatewayId, nowMs, command.command_id]
        );
        if (result.changes > 0) {
            claimed.push({
                ...command,
                gateway_id: command.gateway_id || gatewayId,
                status: "dispatched",
                updated_at_ms: nowMs
            });
        }
    }

    return claimed;
}

async function ackSmartHomeCommand(dbRun, dbAll, commandId, body = {}) {
    const safeCommandId = trimText(commandId, 160);
    if (!safeCommandId) {
        return {
            ok: false,
            code: "SMART_HOME_COMMAND_ID_REQUIRED",
            error: "command_id is required"
        };
    }

    const statusInput = trimText(body.status, 40).toLowerCase();
    const status = statusInput === "completed" || statusInput === "success" ? "succeeded" : statusInput;
    if (!SMART_HOME_COMMAND_STATUSES.has(status) || ["queued", "dispatched", "acknowledged", "success"].includes(status)) {
        return {
            ok: false,
            code: "SMART_HOME_COMMAND_ACK_STATUS_INVALID",
            error: "status must be succeeded, failed, expired, or rejected"
        };
    }

    const rows = await dbAll(
        "SELECT * FROM smart_home_commands WHERE command_id=? LIMIT 1",
        [safeCommandId]
    );
    const existing = rows[0] || null;
    if (!existing) {
        return {
            ok: false,
            code: "SMART_HOME_COMMAND_NOT_FOUND",
            error: "command not found"
        };
    }
    const gatewayId = trimText(body.gateway_id, 128);
    if (gatewayId && existing.gateway_id && existing.gateway_id !== gatewayId) {
        return {
            ok: false,
            code: "SMART_HOME_COMMAND_OWNERSHIP_MISMATCH",
            error: "command does not belong to gateway"
        };
    }
    const targetId = trimText(body.target_id || body.device_id, 128);
    if (targetId && existing.target_id !== targetId) {
        return {
            ok: false,
            code: "SMART_HOME_COMMAND_OWNERSHIP_MISMATCH",
            error: "command does not belong to device"
        };
    }
    if (["succeeded", "failed", "expired", "rejected", "success"].includes(existing.status)) {
        return {
            ok: true,
            command: mapCommandRow(existing),
            idempotent: true
        };
    }

    const nowMs = Date.now();
    const executedAtMs = Number.isFinite(Number(body.executed_at_ms))
        ? Math.trunc(Number(body.executed_at_ms))
        : nowMs;
    const resultPayload = body.result && typeof body.result === "object" ? body.result : null;
    await dbRun(
        `UPDATE smart_home_commands
        SET status=?,
            gateway_id=CASE WHEN gateway_id IS NULL OR gateway_id='' THEN ? ELSE gateway_id END,
            result_json=?,
            error_message=?,
            acknowledged_at_ms=?,
            executed_at_ms=?,
            updated_at_ms=?
        WHERE command_id=?`,
        [
            status,
            gatewayId,
            resultPayload === null ? null : jsonText(resultPayload),
            trimText(body.error_message, 500),
            nowMs,
            executedAtMs,
            nowMs,
            safeCommandId
        ]
    );

    const command = {
        ...mapCommandRow(existing),
        status,
        result: resultPayload,
        error_message: trimText(body.error_message, 500),
        acknowledged_at_ms: nowMs,
        executed_at_ms: executedAtMs,
        updated_at_ms: nowMs
    };
    await recordEvent(dbRun, {
        event_type: "command",
        event_name: "command_acknowledged",
        device_id: command.target_id,
        severity: status === "failed" ? "warning" : "info",
        message: `${command.action} ${status}`,
        payload: command,
        source: "smart_home_ack",
        server_recv_ms: nowMs
    });

    return {
        ok: true,
        command
    };
}

module.exports = {
    ackSmartHomeCommand,
    createSmartHomeCommand,
    claimPendingSmartHomeCommands,
    listSmartHomeCommands,
    readSmartHomeStatus,
    upsertSmartHomeState
};
