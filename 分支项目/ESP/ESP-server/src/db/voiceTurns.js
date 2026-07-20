const {
    ensureTableColumns
} = require("./migrations");

const VOICE_TURN_COLUMNS = [
    { name: "id", type: "INTEGER PRIMARY KEY AUTOINCREMENT" },
    { name: "request_id", type: "TEXT" },
    { name: "device_id", type: "TEXT" },
    { name: "mode", type: "TEXT" },
    { name: "status", type: "TEXT NOT NULL DEFAULT 'unknown'" },
    { name: "status_code", type: "INTEGER" },
    { name: "error_code", type: "TEXT" },
    { name: "error_message", type: "TEXT" },
    { name: "input_bytes", type: "INTEGER NOT NULL DEFAULT 0" },
    { name: "response_bytes", type: "INTEGER NOT NULL DEFAULT 0" },
    { name: "asr_ms", type: "INTEGER" },
    { name: "llm_ms", type: "INTEGER" },
    { name: "tts_ms", type: "INTEGER" },
    { name: "total_ms", type: "INTEGER NOT NULL DEFAULT 0" },
    { name: "asr_text_length", type: "INTEGER NOT NULL DEFAULT 0" },
    { name: "llm_reply_length", type: "INTEGER NOT NULL DEFAULT 0" },
    { name: "tts_pcm_bytes", type: "INTEGER NOT NULL DEFAULT 0" },
    { name: "raw_json", type: "TEXT" },
    { name: "created_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))", addType: "TEXT" },
    { name: "updated_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))", addType: "TEXT" }
];

async function ensureVoiceTurnsTable(dbRun, dbAll) {
    await dbRun(`
        CREATE TABLE IF NOT EXISTS voice_turns (
            ${VOICE_TURN_COLUMNS.map(column => `${column.name} ${column.type}`).join(",\n            ")}
        )
    `);

    if (typeof dbAll === "function") {
        await ensureTableColumns(
            dbRun,
            dbAll,
            "voice_turns",
            VOICE_TURN_COLUMNS.filter(column => !column.type.includes("PRIMARY KEY"))
        );
    }
}

function toNullableInteger(value) {
    return Number.isFinite(value) ? value : null;
}

function toBoundedText(value, maxLength) {
    if (value === undefined || value === null) {
        return null;
    }

    const text = String(value);
    return text.length > maxLength ? text.slice(0, maxLength) : text;
}

async function insertVoiceTurn(dbRun, record) {
    const rawJson = {
        stage: record.stage || null,
        timeout_ms: toNullableInteger(record.timeoutMs),
        active_limit: toNullableInteger(record.activeLimit),
        asr_text_preview: record.asrTextPreview || "",
        upstream_status: toNullableInteger(record.upstreamStatus),
        reason: record.reason || ""
    };

    return dbRun(
        `INSERT INTO voice_turns
        (request_id,device_id,mode,status,status_code,error_code,error_message,input_bytes,response_bytes,asr_ms,llm_ms,tts_ms,total_ms,asr_text_length,llm_reply_length,tts_pcm_bytes,raw_json,created_at,updated_at)
        VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)`,
        [
            toBoundedText(record.requestId, 64),
            toBoundedText(record.deviceId, 128),
            toBoundedText(record.mode, 32),
            toBoundedText(record.status || "unknown", 32),
            toNullableInteger(record.statusCode),
            toBoundedText(record.errorCode, 64),
            toBoundedText(record.errorMessage, 500),
            toNullableInteger(record.inputBytes) || 0,
            toNullableInteger(record.responseBytes) || 0,
            toNullableInteger(record.asrMs),
            toNullableInteger(record.llmMs),
            toNullableInteger(record.ttsMs),
            toNullableInteger(record.totalMs) || 0,
            toNullableInteger(record.asrTextLength) || 0,
            toNullableInteger(record.llmReplyLength) || 0,
            toNullableInteger(record.ttsPcmBytes) || 0,
            JSON.stringify(rawJson),
            new Date().toISOString(),
            new Date().toISOString()
        ]
    );
}

module.exports = {
    ensureVoiceTurnsTable,
    insertVoiceTurn
};
