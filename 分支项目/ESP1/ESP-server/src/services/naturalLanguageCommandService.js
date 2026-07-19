const crypto = require("crypto");
const {
    trimText
} = require("./deviceMetadata");
const {
    recordEvent,
    readLimit
} = require("./eventLogService");

function makeNaturalLanguageCommandId() {
    if (typeof crypto.randomUUID === "function") {
        return `nlcmd_${crypto.randomUUID()}`;
    }

    return `nlcmd_${crypto.randomBytes(16).toString("hex")}`;
}

function parseJson(value, fallback = null) {
    if (!value) {
        return fallback;
    }

    try {
        return JSON.parse(value);
    } catch (_) {
        return fallback;
    }
}

function jsonText(value) {
    if (value === undefined || value === null) {
        return null;
    }

    try {
        return JSON.stringify(value);
    } catch (_) {
        return null;
    }
}

function mapNaturalLanguageCommand(row) {
    return {
        command_id: row.command_id,
        type: "natural_language",
        text: row.text,
        source: row.source || "",
        room_id: row.room_id || "",
        device_id: row.device_id || "",
        status: row.status || "queued",
        parsed_intent: parseJson(row.parsed_intent_json, null),
        created_at_ms: Number(row.created_at_ms) || null,
        updated_at_ms: Number(row.updated_at_ms) || null
    };
}

async function createNaturalLanguageCommand(dbRun, body = {}) {
    const text = trimText(body.text, 2000);
    if (!text) {
        return {
            ok: false,
            code: "NATURAL_LANGUAGE_TEXT_REQUIRED",
            error: "text is required"
        };
    }

    const nowMs = Date.now();
    const command = {
        command_id: makeNaturalLanguageCommandId(),
        type: "natural_language",
        text,
        source: trimText(body.source, 80) || "dashboard",
        room_id: trimText(body.room_id, 128),
        device_id: trimText(body.device_id, 128),
        status: "queued",
        parsed_intent: null,
        created_at_ms: nowMs,
        updated_at_ms: nowMs
    };

    await dbRun(
        `INSERT INTO natural_language_commands
        (command_id,text,source,room_id,device_id,parsed_intent_json,status,created_at_ms,updated_at_ms)
        VALUES(?,?,?,?,?,?,?,?,?)`,
        [
            command.command_id,
            command.text,
            command.source,
            command.room_id,
            command.device_id,
            jsonText(command.parsed_intent),
            command.status,
            command.created_at_ms,
            command.updated_at_ms
        ]
    );

    await recordEvent(dbRun, {
        event_type: "command",
        event_name: "command_created",
        device_id: command.device_id,
        severity: "info",
        message: "natural language command queued",
        payload: command,
        source: command.source,
        server_recv_ms: nowMs
    });

    return {
        ok: true,
        command
    };
}

async function listNaturalLanguageCommands(dbAll, filters = {}) {
    const rows = await dbAll(
        `SELECT * FROM natural_language_commands
        ORDER BY id DESC
        LIMIT ?`,
        [readLimit(filters.limit, 50, 200)]
    );
    return rows.map(mapNaturalLanguageCommand);
}

module.exports = {
    createNaturalLanguageCommand,
    listNaturalLanguageCommands,
    mapNaturalLanguageCommand
};

