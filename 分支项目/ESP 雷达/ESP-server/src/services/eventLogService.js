const {
    makeEventId
} = require("../db/eventLogs");
const {
    trimText
} = require("./deviceMetadata");
const {
    broadcastEvent
} = require("./eventStreamService");

const EVENT_TYPES = new Set(["alarm", "system", "command", "voice", "device", "csi"]);
const EVENT_TYPE_TO_STREAM = {
    alarm: "alarm_created",
    system: "system_log_created",
    command: "command_created",
    voice: "voice_event_created",
    device: "device_status_changed",
    csi: "system_log_created"
};

function nowIso() {
    return new Date().toISOString();
}

function jsonText(value) {
    if (value === undefined) {
        return "{}";
    }

    try {
        return JSON.stringify(value === null ? null : value);
    } catch (_) {
        return JSON.stringify({
            serialization_error: true
        });
    }
}

function parseJson(value, fallback = {}) {
    if (!value) {
        return fallback;
    }

    try {
        return JSON.parse(value);
    } catch (_) {
        return fallback;
    }
}

function normalizePayload(value) {
    if (value === undefined || value === null) {
        return {};
    }

    return value && typeof value === "object" && !Array.isArray(value) ? value : {
        value
    };
}

function readLimit(value, fallback = 50, max = 200) {
    const numeric = Number.parseInt(value, 10);
    if (!Number.isFinite(numeric) || numeric <= 0) {
        return fallback;
    }

    return Math.min(numeric, max);
}

function normalizeLevel(value, allowed, fallback = "info") {
    const level = trimText(value, 40).toLowerCase();
    return allowed.has(level) ? level : fallback;
}

function normalizeEventType(value) {
    const text = trimText(value, 40).toLowerCase();
    return EVENT_TYPES.has(text) ? text : "system";
}

function mapAlarmRow(row) {
    const payload = parseJson(row.payload_json, {});
    return {
        id: row.event_id || String(row.id),
        level: normalizeLevel(row.severity, new Set(["info", "warning", "error", "critical"]), "info"),
        source: row.source || "",
        gateway_id: row.gateway_id || payload.gateway_id || "",
        device_id: row.device_id || "",
        room_id: payload.room_id || "",
        room_name: payload.room_name || "",
        title: payload.title || row.event_name || "",
        message: row.message || "",
        payload: payload.payload && typeof payload.payload === "object" ? payload.payload : payload,
        created_at_ms: Number(row.server_recv_ms) || null,
        acknowledged: Boolean(payload.acknowledged)
    };
}

function mapSystemLogRow(row) {
    const payload = parseJson(row.payload_json, {});
    return {
        id: row.event_id || String(row.id),
        level: normalizeLevel(row.severity, new Set(["info", "warning", "error"]), "info"),
        source: row.source || "",
        gateway_id: row.gateway_id || payload.gateway_id || "",
        device_id: row.device_id || "",
        message: row.message || "",
        payload,
        created_at_ms: Number(row.server_recv_ms) || null
    };
}

function mapEventRow(row) {
    const payload = parseJson(row.payload_json, {});
    return {
        id: row.id,
        event_id: row.event_id,
        event_type: row.event_type,
        event_name: row.event_name,
        event: payload.event || row.event_name,
        gateway_id: row.gateway_id || payload.gateway_id || "",
        command_id: payload.command_id || "",
        status: payload.status || "",
        name: payload.name || "",
        duration_ms: payload.duration_ms ?? null,
        device_id: row.device_id || "",
        severity: row.severity || "info",
        message: row.message || "",
        payload,
        source: row.source || "",
        server_recv_ms: Number.isFinite(Number(row.server_recv_ms)) ? Number(row.server_recv_ms) : null,
        timestamp: Number.isFinite(Number(row.server_recv_ms)) ? Number(row.server_recv_ms) : null,
        created_at: row.created_at || "",
        updated_at: row.updated_at || ""
    };
}

async function listEventRows(dbAll, filters = {}) {
    if (typeof dbAll !== "function") {
        return [];
    }

    const params = [];
    const where = [];
    if (filters.event_type) {
        where.push("event_type=?");
        params.push(normalizeEventType(filters.event_type));
    }
    if (Array.isArray(filters.event_types) && filters.event_types.length > 0) {
        const types = filters.event_types.map(normalizeEventType);
        where.push(`event_type IN (${types.map(() => "?").join(",")})`);
        params.push(...types);
    }
    if (filters.gateway_id) {
        where.push("gateway_id=?");
        params.push(trimText(filters.gateway_id, 128));
    }

    params.push(readLimit(filters.limit));
    return dbAll(
        `SELECT * FROM event_logs
        ${where.length ? `WHERE ${where.join(" AND ")}` : ""}
        ORDER BY id DESC
        LIMIT ?`,
        params
    );
}

async function recordEvent(dbRun, input = {}, options = {}) {
    if (typeof dbRun !== "function") {
        return null;
    }

    const eventType = normalizeEventType(input.event_type || input.type);
    const serverRecvMs = Number.isFinite(input.server_recv_ms) ? Math.trunc(input.server_recv_ms) : Date.now();
    const eventName = trimText(input.event_name, 80) || EVENT_TYPE_TO_STREAM[eventType] || "system_log_created";
    const event = {
        event_id: trimText(input.event_id, 120) || makeEventId(eventType),
        event_type: eventType,
        event_name: eventName,
        gateway_id: trimText(input.gateway_id || input.payload?.gateway_id, 128),
        device_id: trimText(input.device_id, 128),
        severity: trimText(input.severity, 40) || "info",
        message: trimText(input.message, 1000),
        payload: normalizePayload(input.payload),
        source: trimText(input.source, 80) || "esp-server",
        server_recv_ms: serverRecvMs,
        created_at: nowIso()
    };

    await dbRun(
        `INSERT INTO event_logs
        (event_id,event_type,event_name,gateway_id,device_id,severity,message,payload_json,source,server_recv_ms,created_at,updated_at)
        VALUES(?,?,?,?,?,?,?,?,?,?,?,?)`,
        [
            event.event_id,
            event.event_type,
            event.event_name,
            event.gateway_id,
            event.device_id,
            event.severity,
            event.message,
            jsonText(event.payload),
            event.source,
            event.server_recv_ms,
            event.created_at,
            event.created_at
        ]
    );

    const mapped = {
        ...event,
        payload: event.payload
    };
    if (options.broadcast !== false) {
        broadcastEvent(event.event_name, mapped);
    }

    return mapped;
}

async function listEvents(dbAll, filters = {}) {
    if (typeof dbAll !== "function") {
        return [];
    }

    const params = [];
    const where = [];
    if (filters.event_type) {
        where.push("event_type=?");
        params.push(normalizeEventType(filters.event_type));
    }
    if (Array.isArray(filters.event_types) && filters.event_types.length > 0) {
        const types = filters.event_types.map(normalizeEventType);
        where.push(`event_type IN (${types.map(() => "?").join(",")})`);
        params.push(...types);
    }
    if (filters.device_id) {
        where.push("device_id=?");
        params.push(trimText(filters.device_id, 128));
    }
    if (filters.gateway_id) {
        where.push("gateway_id=?");
        params.push(trimText(filters.gateway_id, 128));
    }

    params.push(readLimit(filters.limit));
    const rows = await dbAll(
        `SELECT * FROM event_logs
        ${where.length ? `WHERE ${where.join(" AND ")}` : ""}
        ORDER BY id DESC
        LIMIT ?`,
        params
    );
    return rows.map(mapEventRow);
}

function normalizeCleanupTypes(types) {
    if (!Array.isArray(types)) {
        return [];
    }

    if (types.some(type => trimText(type, 40).toLowerCase() === "all")) {
        return Array.from(EVENT_TYPES);
    }

    const normalized = [];
    const seen = new Set();
    for (const type of types) {
        const eventType = normalizeEventType(type);
        if (!seen.has(eventType)) {
            seen.add(eventType);
            normalized.push(eventType);
        }
    }
    return normalized;
}

async function cleanupEvents(dbRun, dbAll, input = {}) {
    const requestedTypes = input.type === "all"
        ? Array.from(EVENT_TYPES)
        : normalizeCleanupTypes(input.types || [input.type]);
    const types = requestedTypes.length > 0 ? requestedTypes : ["system"];
    const olderThanMs = Number.parseInt(input.older_than_ms, 10);
    if (!Number.isFinite(olderThanMs) || olderThanMs <= 0) {
        return {
            ok: false,
            code: "LOG_CLEANUP_OLDER_THAN_INVALID",
            error: "older_than_ms must be a positive number"
        };
    }

    const dryRun = input.dry_run === true || input.dry_run === "true";
    const force = input.force === true || input.force === "true";
    if (!force && olderThanMs < 60 * 60 * 1000) {
        return {
            ok: false,
            code: "LOG_CLEANUP_WINDOW_TOO_RECENT",
            error: "older_than_ms must be at least 3600000 unless force is true"
        };
    }

    const cutoffMs = Date.now() - olderThanMs;
    const deleted = {};
    for (const type of types) {
        const countRows = await dbAll(
            "SELECT COUNT(*) AS count FROM event_logs WHERE event_type=? AND server_recv_ms IS NOT NULL AND server_recv_ms < ?",
            [type, cutoffMs]
        );
        deleted[type] = Number(countRows[0]?.count) || 0;
        if (!dryRun && deleted[type] > 0) {
            await dbRun(
                "DELETE FROM event_logs WHERE event_type=? AND server_recv_ms IS NOT NULL AND server_recv_ms < ?",
                [type, cutoffMs]
            );
        }
    }

    const cleanupEvent = await recordEvent(dbRun, {
        event_type: "system",
        event_name: "system_log_created",
        severity: "info",
        message: dryRun ? "logs cleanup dry run completed" : "logs cleanup completed",
        payload: {
            event: "logs_cleanup",
            deleted,
            dry_run: dryRun,
            older_than_ms: olderThanMs,
            types
        },
        source: "logs_cleanup",
        server_recv_ms: Date.now()
    });
    broadcastEvent("logs_cleaned", cleanupEvent);

    return {
        ok: true,
        deleted,
        dry_run: dryRun
    };
}

module.exports = {
    EVENT_TYPES,
    EVENT_TYPE_TO_STREAM,
    cleanupEvents,
    listEvents,
    listEventRows,
    mapAlarmRow,
    mapSystemLogRow,
    recordEvent,
    readLimit
};
