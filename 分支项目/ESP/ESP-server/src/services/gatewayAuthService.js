const crypto = require("crypto");
const {
    trimText
} = require("./deviceMetadata");
const {
    resolveDeviceId
} = require("./deviceIdResolver");

const GATEWAY_ID_MAX_LENGTH = 128;
const GATEWAY_TOKEN_MAX_LENGTH = 256;

function normalizeGatewayId(value) {
    return trimText(value, GATEWAY_ID_MAX_LENGTH);
}

function normalizeDeviceId(value) {
    return resolveDeviceId(value);
}

function normalizeGatewayToken(value) {
    return trimText(value, GATEWAY_TOKEN_MAX_LENGTH);
}

function parseCsv(value) {
    if (typeof value !== "string" || !value.trim()) {
        return [];
    }

    return value
        .split(",")
        .map(item => normalizeGatewayToken(item))
        .filter(Boolean);
}

function configuredGatewayTokens() {
    const tokens = new Set(parseCsv(process.env.GATEWAY_AUTH_TOKENS));
    const singleToken = normalizeGatewayToken(process.env.GATEWAY_AUTH_TOKEN);
    if (singleToken) {
        tokens.add(singleToken);
    }
    return tokens;
}

function hasGatewayAuthSecret() {
    return configuredGatewayTokens().size > 0;
}

function timingSafeEquals(a, b) {
    if (!a || !b || a.length !== b.length) {
        return false;
    }

    return crypto.timingSafeEqual(Buffer.from(a), Buffer.from(b));
}

function readHeader(headers = {}, name) {
    return headers[String(name || "").toLowerCase()];
}

function readGatewayIdentity(req = {}) {
    const headers = req.headers || {};
    return {
        gateway_id: normalizeGatewayId(
            readHeader(headers, "x-gateway-id") ||
            readHeader(headers, "x-esp-gateway-id") ||
            req.body?.gateway_id ||
            req.query?.gateway_id ||
            ""
        ),
        device_id: normalizeDeviceId(
            readHeader(headers, "x-device-id") ||
            readHeader(headers, "x-esp-device-id") ||
            req.body?.device_id ||
            req.query?.device_id ||
            ""
        ),
        token: normalizeGatewayToken(
            readHeader(headers, "x-gateway-token") ||
            String(readHeader(headers, "authorization") || "").replace(/^Bearer\s+/i, "")
        )
    };
}

function authenticateGateway(req = {}) {
    const identity = readGatewayIdentity(req);
    if (!identity.gateway_id) {
        return {
            ok: false,
            code: "GATEWAY_ID_REQUIRED",
            error: "gateway_id is required",
            ...identity
        };
    }

    const tokens = configuredGatewayTokens();
    if (tokens.size === 0) {
        return {
            ok: true,
            auth_required: false,
            source: "declared",
            ...identity
        };
    }

    for (const token of tokens) {
        if (timingSafeEquals(identity.token, token)) {
            return {
                ok: true,
                auth_required: true,
                source: "authenticated",
                ...identity
            };
        }
    }

    return {
        ok: false,
        code: "GATEWAY_AUTH_REQUIRED",
        error: "gateway authentication required",
        auth_required: true,
        ...identity
    };
}

function authError(res, result, status) {
    return res.status(status).json({
        ok: false,
        code: result.code || "GATEWAY_AUTH_FAILED",
        error: result.error || "gateway authentication failed",
        server_time_ms: Date.now()
    });
}

async function recordGatewaySeen(dbRun, gatewayId, serverRecvMs = Date.now()) {
    if (typeof dbRun !== "function" || !gatewayId) {
        return;
    }

    const nowMs = Number.isFinite(serverRecvMs) ? Math.trunc(serverRecvMs) : Date.now();
    const nowIso = new Date(nowMs).toISOString();
    await dbRun(
        `INSERT INTO gateway_auth
        (gateway_id,enabled,last_seen_ms,last_seen_at,created_at,updated_at)
        VALUES(?,?,?,?,?,?)
        ON CONFLICT(gateway_id) DO UPDATE SET
            enabled=1,
            last_seen_ms=excluded.last_seen_ms,
            last_seen_at=excluded.last_seen_at,
            updated_at=excluded.updated_at`,
        [gatewayId, 1, nowMs, nowIso, nowIso, nowIso]
    );
}

function requireGatewayAuth(context = {}) {
    return async (req, res, next) => {
        const result = authenticateGateway(req);
        if (!result.ok) {
            return authError(res, result, result.code === "GATEWAY_ID_REQUIRED" ? 400 : 401);
        }

        req.gatewayAuth = {
            gateway_id: result.gateway_id,
            device_id: result.device_id,
            auth_required: result.auth_required,
            source: result.source
        };

        try {
            if (typeof context.enqueuePersistenceJob === "function") {
                context.enqueuePersistenceJob({
                    type: "gateway_auth.seen",
                    priority: "high",
                    run: () => recordGatewaySeen(context.dbRun, result.gateway_id)
                });
                context.persistenceWorker?.scheduleImmediateFlushIfNeeded?.();
            } else {
                await recordGatewaySeen(context.dbRun, result.gateway_id);
            }
        } catch (_) {
            // Auth has already succeeded; last_seen bookkeeping is not part of authorization.
        }

        return next();
    };
}

async function bindDeviceToGateway(dbRun, gatewayId, deviceId, source = "gateway", serverRecvMs = Date.now()) {
    const safeGatewayId = normalizeGatewayId(gatewayId);
    const safeDeviceId = normalizeDeviceId(deviceId);
    if (!safeGatewayId || !safeDeviceId || typeof dbRun !== "function") {
        return {
            ok: false,
            code: "GATEWAY_BINDING_INVALID",
            error: "gateway_id and device_id are required"
        };
    }

    const nowMs = Number.isFinite(serverRecvMs) ? Math.trunc(serverRecvMs) : Date.now();
    const nowIso = new Date(nowMs).toISOString();
    await dbRun(
        `INSERT INTO gateway_device_bindings
        (gateway_id,device_id,source,last_seen_ms,last_seen_at,created_at,updated_at)
        VALUES(?,?,?,?,?,?,?)
        ON CONFLICT(gateway_id,device_id) DO UPDATE SET
            source=excluded.source,
            last_seen_ms=excluded.last_seen_ms,
            last_seen_at=excluded.last_seen_at,
            updated_at=excluded.updated_at`,
        [
            safeGatewayId,
            safeDeviceId,
            trimText(source, 40) || "gateway",
            nowMs,
            nowIso,
            nowIso,
            nowIso
        ]
    );

    return {
        ok: true,
        gateway_id: safeGatewayId,
        device_id: safeDeviceId
    };
}

function queueGatewayBinding(dbRun, gatewayId, deviceId, source, serverRecvMs, options = {}) {
    if (typeof options.enqueuePersistenceJob !== "function") {
        return null;
    }

    options.enqueuePersistenceJob({
        type: "gateway_device_binding",
        priority: "high",
        run: () => bindDeviceToGateway(dbRun, gatewayId, deviceId, source, serverRecvMs)
    });
    options.persistenceWorker?.scheduleImmediateFlushIfNeeded?.();
    return {
        ok: true,
        gateway_id: gatewayId,
        device_id: deviceId,
        queued: true
    };
}

async function requireGatewayBinding(dbRun, dbAll, binding, options = {}) {
    const gatewayId = normalizeGatewayId(binding?.gateway_id);
    const deviceId = normalizeDeviceId(binding?.device_id);
    if (!gatewayId || !deviceId) {
        return {
            ok: false,
            code: "GATEWAY_BINDING_REQUIRED",
            error: "gateway_id and device_id are required"
        };
    }

    if (typeof dbAll !== "function") {
        return {
            ok: false,
            code: "GATEWAY_BINDING_UNAVAILABLE",
            error: "gateway binding storage unavailable"
        };
    }

    const rows = await dbAll(
        "SELECT gateway_id,device_id,source FROM gateway_device_bindings WHERE gateway_id=? AND device_id=? LIMIT 1",
        [gatewayId, deviceId]
    );
    if (!rows[0]) {
        if (options.createIfMissing === true) {
            const queued = queueGatewayBinding(dbRun, gatewayId, deviceId, options.source, options.serverRecvMs, options);
            if (queued) {
                return queued;
            }

            return bindDeviceToGateway(dbRun, gatewayId, deviceId, options.source, options.serverRecvMs);
        }

        return {
            ok: false,
            code: "GATEWAY_DEVICE_BINDING_MISSING",
            error: "device is not bound to gateway"
        };
    }

    const queued = queueGatewayBinding(
        dbRun,
        gatewayId,
        deviceId,
        rows[0].source || options.source || "gateway",
        options.serverRecvMs,
        options
    );
    if (!queued) {
        await bindDeviceToGateway(dbRun, gatewayId, deviceId, rows[0].source || options.source || "gateway", options.serverRecvMs);
    }

    return {
        ok: true,
        gateway_id: gatewayId,
        device_id: deviceId
    };
}

async function requireBoundDevice(req, res, context = {}, options = {}) {
    const gatewayId = req.gatewayAuth?.gateway_id || readGatewayIdentity(req).gateway_id;
    const deviceId = normalizeDeviceId(options.deviceId || req.gatewayAuth?.device_id || readGatewayIdentity(req).device_id);
    const result = await requireGatewayBinding(context.dbRun, context.dbAll, {
        gateway_id: gatewayId,
        device_id: deviceId
    }, {
        createIfMissing: options.createIfMissing === true || options.allowNewBinding === true,
        source: options.source,
        serverRecvMs: options.serverRecvMs,
        enqueuePersistenceJob: context.enqueuePersistenceJob,
        persistenceWorker: context.persistenceWorker
    });

    if (!result.ok) {
        return {
            ok: false,
            response: authError(res, result, result.code === "GATEWAY_BINDING_REQUIRED" ? 400 : 403)
        };
    }

    return result;
}

module.exports = {
    authenticateGateway,
    bindDeviceToGateway,
    hasGatewayAuthSecret,
    normalizeDeviceId,
    normalizeGatewayId,
    normalizeGatewayToken,
    readGatewayIdentity,
    requireBoundDevice,
    requireGatewayAuth,
    requireGatewayBinding
};
