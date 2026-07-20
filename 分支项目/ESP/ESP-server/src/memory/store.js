const crypto = require("crypto");
const {
    isSqliteUniqueConstraintError,
    runUpdateThenInsert
} = require("../db/upsert");
const {
    isIsoDateString
} = require("../utils/date");

const MEMORY_LEVELS = new Set(["volatile", "episodic", "important", "profile_candidate", "archived"]);
const PROFILE_STATUSES = new Set(["candidate", "active", "rejected", "archived"]);
const DAILY_MEMORY_STATUSES = new Set(["candidate", "active", "rejected", "archived"]);
const DAILY_MEMORY_TYPES = new Set(["daily_summary", "weekly_summary"]);
const MEMORY_CORRECTION_STATUSES = new Set(["applied", "pending", "rejected", "archived"]);
const MEMORY_JOB_STATUSES = new Set(["queued", "running", "completed", "failed", "skipped"]);
const CONVERSATION_TURN_ID_MAX_LENGTH = 80;
const CONVERSATION_ROLE_MAX_LENGTH = 40;
const CONVERSATION_SOURCE_MAX_LENGTH = 80;
const PROFILE_KEY_MAX_LENGTH = 120;
const CONVERSATION_CONTEXT_ID_MAX_LENGTH = 128;
const PROFILE_CATEGORY_MAX_LENGTH = 80;
const MEMORY_CORRECTION_TARGET_TYPE_MAX_LENGTH = 40;

function nowIso() {
    return new Date().toISOString();
}

function makeId(prefix) {
    const value = typeof crypto.randomUUID === "function"
        ? crypto.randomUUID()
        : crypto.randomBytes(16).toString("hex");
    return `${prefix}_${value}`;
}

function boundedString(value, maxLength, fallback = "") {
    if (value === undefined || value === null) {
        return fallback;
    }

    const text = String(value).trim();
    return text.length > maxLength ? text.slice(0, maxLength) : text;
}

function jsonText(value, fallback = null) {
    if (value === undefined || value === null) {
        return fallback;
    }

    return JSON.stringify(value);
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

function clampInteger(value, min, max, fallback) {
    const numeric = Number(value);
    if (!Number.isInteger(numeric)) {
        return fallback;
    }

    return Math.min(Math.max(numeric, min), max);
}

function clampNumber(value, min, max, fallback) {
    const numeric = Number(value);
    if (!Number.isFinite(numeric)) {
        return fallback;
    }

    return Math.min(Math.max(numeric, min), max);
}

function readLimit(value, fallback = 50, max = 200) {
    return clampInteger(value, 1, max, fallback);
}

function chooseSetValue(value, allowed, fallback) {
    const normalized = typeof value === "string" ? value.trim() : value;
    return allowed.has(normalized) ? normalized : fallback;
}

function duplicateIdResult(code, field, value) {
    return {
        ok: false,
        statusCode: 409,
        code,
        error: `${field} already exists`,
        [field]: value
    };
}

function invalidFieldResult(code, field, error, value) {
    return {
        ok: false,
        code,
        error,
        ...(value ? { [field]: value } : {})
    };
}

function readUniqueKey(value, maxLength, code, field) {
    if (typeof value === "string" && value.trim().length > maxLength) {
        return invalidFieldResult(
            code,
            field,
            `${field} must be <= ${maxLength} characters`,
            value.trim().slice(0, maxLength)
        );
    }

    return {
        ok: true,
        value: boundedString(value, maxLength)
    };
}

function readOptionalContextId(value, code, field) {
    if (value === undefined || value === null) {
        return {
            ok: true,
            value: ""
        };
    }

    const text = String(value).trim();
    if (text.length > CONVERSATION_CONTEXT_ID_MAX_LENGTH) {
        return invalidFieldResult(
            code,
            field,
            `${field} must be <= ${CONVERSATION_CONTEXT_ID_MAX_LENGTH} characters`,
            text.slice(0, CONVERSATION_CONTEXT_ID_MAX_LENGTH)
        );
    }

    return {
        ok: true,
        value: text
    };
}

function readOptionalBoundedString(value, maxLength, code, field, fallback = "") {
    if (value === undefined || value === null) {
        return {
            ok: true,
            value: fallback
        };
    }

    const text = String(value).trim();
    if (text.length > maxLength) {
        return invalidFieldResult(
            code,
            field,
            `${field} must be <= ${maxLength} characters`,
            text.slice(0, maxLength)
        );
    }

    return {
        ok: true,
        value: text || fallback
    };
}

function isTerminalJobStatus(status) {
    return status === "completed" || status === "failed" || status === "skipped";
}

function mapConversationTurn(row) {
    return {
        id: row.id,
        turn_id: row.turn_id,
        session_id: row.session_id || "",
        device_id: row.device_id || "",
        role: row.role,
        input_text: row.input_text || "",
        response_text: row.response_text || "",
        structured: parseJson(row.structured_json, null),
        command_ids: parseJson(row.command_ids_json, []),
        memory_level: row.memory_level,
        importance: row.importance,
        source: row.source || "",
        created_at: row.created_at,
        updated_at: row.updated_at,
        deleted_at: row.deleted_at || ""
    };
}

async function createConversationTurn(dbRun, input) {
    const roleResult = readOptionalBoundedString(input?.role, CONVERSATION_ROLE_MAX_LENGTH, "CONVERSATION_ROLE_INVALID", "role", "user");
    if (!roleResult.ok) {
        return roleResult;
    }
    const role = roleResult.value;
    const inputText = boundedString(input?.input_text, 8000);
    const responseText = boundedString(input?.response_text, 8000);
    if (!inputText && !responseText) {
        return {
            ok: false,
            error: "input_text or response_text is required"
        };
    }

    const memoryLevel = chooseSetValue(input?.memory_level, MEMORY_LEVELS, "episodic");
    const timestamp = nowIso();
    const requestedTurnId = typeof input?.turn_id === "string" ? input.turn_id.trim() : "";
    if (requestedTurnId.length > CONVERSATION_TURN_ID_MAX_LENGTH) {
        return invalidFieldResult(
            "CONVERSATION_TURN_ID_INVALID",
            "turn_id",
            `turn_id must be <= ${CONVERSATION_TURN_ID_MAX_LENGTH} characters`,
            requestedTurnId.slice(0, CONVERSATION_TURN_ID_MAX_LENGTH)
        );
    }

    const turnId = requestedTurnId || makeId("turn");
    const sessionIdResult = readOptionalContextId(input?.session_id, "SESSION_ID_INVALID", "session_id");
    if (!sessionIdResult.ok) {
        return sessionIdResult;
    }
    const deviceIdResult = readOptionalContextId(input?.device_id, "DEVICE_ID_INVALID", "device_id");
    if (!deviceIdResult.ok) {
        return deviceIdResult;
    }
    const sourceResult = readOptionalBoundedString(input?.source, CONVERSATION_SOURCE_MAX_LENGTH, "CONVERSATION_SOURCE_INVALID", "source", "api");
    if (!sourceResult.ok) {
        return sourceResult;
    }

    let result;
    try {
        result = await dbRun(
            `INSERT INTO conversation_turns
            (turn_id,session_id,device_id,role,input_text,response_text,structured_json,command_ids_json,memory_level,importance,source,created_at,updated_at)
            VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)`,
            [
                turnId,
                sessionIdResult.value,
                deviceIdResult.value,
                role,
                inputText,
                responseText,
                jsonText(input?.structured),
                jsonText(Array.isArray(input?.command_ids) ? input.command_ids : []),
                memoryLevel,
                clampInteger(input?.importance, 0, 5, 1),
                sourceResult.value,
                timestamp,
                timestamp
            ]
        );
    } catch (error) {
        if (isSqliteUniqueConstraintError(error)) {
            return duplicateIdResult("CONVERSATION_TURN_ID_DUPLICATE", "turn_id", turnId);
        }
        throw error;
    }

    return {
        ok: true,
        id: result.lastID,
        turn_id: turnId,
        server_time_ms: Date.now()
    };
}

async function listConversationTurns(dbAll, filters = {}) {
    const params = [];
    const where = [];
    const sessionId = boundedString(filters.session_id, 128);
    const deviceId = boundedString(filters.device_id, 128);
    if (sessionId) {
        where.push("session_id=?");
        params.push(sessionId);
    }
    if (deviceId) {
        where.push("device_id=?");
        params.push(deviceId);
    }
    where.push("deleted_at IS NULL");
    params.push(readLimit(filters.limit));
    const rows = await dbAll(
        `SELECT * FROM conversation_turns ${where.length ? `WHERE ${where.join(" AND ")}` : ""} ORDER BY id DESC LIMIT ?`,
        params
    );

    return rows.map(mapConversationTurn);
}

function mapDailyMemory(row) {
    return {
        id: row.id,
        memory_date: row.memory_date,
        summary: row.summary,
        memory_type: row.memory_type || "daily_summary",
        status: row.status,
        source: row.source || "",
        confidence: row.confidence,
        input: parseJson(row.input_json, null),
        raw: parseJson(row.raw_json, null),
        evidence: parseJson(row.evidence_json, []),
        window_start: row.window_start || "",
        window_end: row.window_end || "",
        sample_count: row.sample_count || 0,
        created_at: row.created_at,
        updated_at: row.updated_at,
        deleted_at: row.deleted_at || ""
    };
}

async function createDailyMemory(dbRun, input) {
    const memoryDate = boundedString(input?.memory_date || input?.date, 20);
    const summary = boundedString(input?.summary, 12000);
    if (!memoryDate || !summary) {
        return {
            ok: false,
            error: "memory_date and summary are required"
        };
    }
    if (!isIsoDateString(memoryDate)) {
        return invalidFieldResult(
            "DAILY_MEMORY_DATE_INVALID",
            "memory_date",
            "memory_date must use YYYY-MM-DD format",
            memoryDate
        );
    }

    const timestamp = nowIso();
    const status = chooseSetValue(input?.status, DAILY_MEMORY_STATUSES, "candidate");
    const memoryType = chooseSetValue(input?.memory_type, DAILY_MEMORY_TYPES, "daily_summary");
    const result = await dbRun(
        `INSERT INTO daily_memory
        (memory_date,summary,memory_type,status,source,confidence,input_json,raw_json,evidence_json,window_start,window_end,sample_count,created_at,updated_at)
        VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)`,
        [
            memoryDate,
            summary,
            memoryType,
            status,
            boundedString(input?.source, 80, "api"),
            clampNumber(input?.confidence, 0, 1, 0.5),
            jsonText(input?.input),
            jsonText(input?.raw || input),
            jsonText(Array.isArray(input?.evidence) ? input.evidence : []),
            boundedString(input?.window_start, 40),
            boundedString(input?.window_end, 40),
            clampInteger(input?.sample_count, 0, 1000000000, 0),
            timestamp,
            timestamp
        ]
    );

    return {
        ok: true,
        id: result.lastID,
        memory_date: memoryDate,
        server_time_ms: Date.now()
    };
}

async function listDailyMemory(dbAll, filters = {}) {
    const params = [];
    const where = ["deleted_at IS NULL"];
    const memoryDate = boundedString(filters.memory_date || filters.date, 20);
    const memoryType = chooseSetValue(filters.memory_type, DAILY_MEMORY_TYPES, "");
    if (memoryDate) {
        where.push("memory_date=?");
        params.push(memoryDate);
    }
    if (memoryType) {
        where.push("memory_type=?");
        params.push(memoryType);
    }
    params.push(readLimit(filters.limit));
    const rows = await dbAll(
        `SELECT * FROM daily_memory WHERE ${where.join(" AND ")} ORDER BY id DESC LIMIT ?`,
        params
    );

    return rows.map(mapDailyMemory);
}

function mapProfile(row) {
    return {
        id: row.id,
        profile_key: row.profile_key,
        profile_value: row.profile_value,
        category: row.category,
        status: row.status,
        confidence: row.confidence,
        evidence: parseJson(row.evidence_json, []),
        correction_count: row.correction_count,
        source: row.source || "",
        created_at: row.created_at,
        updated_at: row.updated_at,
        deleted_at: row.deleted_at || ""
    };
}

async function upsertProfile(dbRun, input) {
    const profileKeyResult = readUniqueKey(input?.profile_key || input?.key, PROFILE_KEY_MAX_LENGTH, "PROFILE_KEY_INVALID", "profile_key");
    if (!profileKeyResult.ok) {
        return profileKeyResult;
    }

    const profileKey = profileKeyResult.value;
    const profileValue = boundedString(input?.profile_value || input?.value, 4000);
    if (!profileKey || !profileValue) {
        return {
            ok: false,
            error: "profile_key and profile_value are required"
        };
    }
    const categoryResult = readUniqueKey(input?.category || "user", PROFILE_CATEGORY_MAX_LENGTH, "PROFILE_CATEGORY_INVALID", "category");
    if (!categoryResult.ok) {
        return categoryResult;
    }
    const category = categoryResult.value || "user";

    const status = chooseSetValue(input?.status, PROFILE_STATUSES, "candidate");
    const timestamp = nowIso();
    await runUpdateThenInsert(dbRun, {
        updateSql: `UPDATE long_term_profile
            SET profile_value=?,
                category=?,
                status=?,
                confidence=?,
                evidence_json=?,
                source=?,
                updated_at=?
            WHERE profile_key=?`,
        updateParams: [
            profileValue,
            category,
            status,
            clampNumber(input?.confidence, 0, 1, 0.5),
            jsonText(Array.isArray(input?.evidence) ? input.evidence : []),
            boundedString(input?.source, 80, "api"),
            timestamp,
            profileKey
        ],
        insertSql: `INSERT INTO long_term_profile
            (profile_key,profile_value,category,status,confidence,evidence_json,source,created_at,updated_at)
            VALUES(?,?,?,?,?,?,?,?,?)`,
        insertParams: [
            profileKey,
            profileValue,
            category,
            status,
            clampNumber(input?.confidence, 0, 1, 0.5),
            jsonText(Array.isArray(input?.evidence) ? input.evidence : []),
            boundedString(input?.source, 80, "api"),
            timestamp,
            timestamp
        ]
    });

    return {
        ok: true,
        profile_key: profileKey,
        status,
        server_time_ms: Date.now()
    };
}

async function listProfiles(dbAll, filters = {}) {
    const params = [];
    const where = [];
    const status = boundedString(filters.status, 40);
    const category = boundedString(filters.category, 80);
    if (status) {
        where.push("status=?");
        params.push(status);
    }
    if (category) {
        where.push("category=?");
        params.push(category);
    }
    where.push("deleted_at IS NULL");
    params.push(readLimit(filters.limit));
    const rows = await dbAll(
        `SELECT * FROM long_term_profile ${where.length ? `WHERE ${where.join(" AND ")}` : ""} ORDER BY id DESC LIMIT ?`,
        params
    );

    return rows.map(mapProfile);
}

async function applyMemoryCorrection(dbRun, input) {
    const targetTypeResult = readUniqueKey(
        input?.target_type,
        MEMORY_CORRECTION_TARGET_TYPE_MAX_LENGTH,
        "MEMORY_CORRECTION_TARGET_TYPE_INVALID",
        "target_type"
    );
    if (!targetTypeResult.ok) {
        return targetTypeResult;
    }
    const targetType = targetTypeResult.value;
    const targetIdResult = readUniqueKey(input?.target_id, PROFILE_KEY_MAX_LENGTH, "MEMORY_CORRECTION_TARGET_ID_INVALID", "target_id");
    if (!targetIdResult.ok) {
        return targetIdResult;
    }

    const targetId = targetIdResult.value;
    const correctionText = boundedString(input?.correction_text, 4000);
    if (!targetType || !targetId || !correctionText) {
        return {
            ok: false,
            error: "target_type, target_id, and correction_text are required"
        };
    }

    const correctionId = makeId("correction");
    const deviceIdResult = readOptionalContextId(input?.device_id, "DEVICE_ID_INVALID", "device_id");
    if (!deviceIdResult.ok) {
        return deviceIdResult;
    }
    const sessionIdResult = readOptionalContextId(input?.session_id, "SESSION_ID_INVALID", "session_id");
    if (!sessionIdResult.ok) {
        return sessionIdResult;
    }
    const timestamp = nowIso();
    const status = chooseSetValue(input?.status, MEMORY_CORRECTION_STATUSES, "applied");
    await dbRun(
        `INSERT INTO memory_corrections
        (correction_id,target_type,target_id,correction_text,corrected_value,device_id,session_id,status,raw_json,created_at,updated_at)
        VALUES(?,?,?,?,?,?,?,?,?,?,?)`,
        [
            correctionId,
            targetType,
            targetId,
            correctionText,
            boundedString(input?.corrected_value, 4000),
            deviceIdResult.value,
            sessionIdResult.value,
            status,
            jsonText(input),
            timestamp,
            timestamp
        ]
    );

    if (targetType === "profile") {
        await dbRun(
            `UPDATE long_term_profile
            SET profile_value=COALESCE(NULLIF(?, ''), profile_value),
                correction_count=correction_count + 1,
                status='candidate',
                updated_at=?
            WHERE (profile_key=? OR CAST(id AS TEXT)=?)
              AND deleted_at IS NULL
              AND COALESCE(status, '') NOT IN ('deleted','inactive','archived')`,
            [
                boundedString(input?.corrected_value, 4000),
                timestamp,
                targetId,
                targetId
            ]
        );
    }

    return {
        ok: true,
        correction_id: correctionId,
        server_time_ms: Date.now()
    };
}

async function createMemoryJobRun(dbRun, input) {
    const jobName = boundedString(input?.job_name, 80);
    if (!jobName) {
        return {
            ok: false,
            error: "job_name is required"
        };
    }

    const jobId = makeId("job");
    const timestamp = nowIso();
    const status = chooseSetValue(input?.status, MEMORY_JOB_STATUSES, "queued");
    await dbRun(
        `INSERT INTO memory_job_runs
        (job_id,job_name,target_date,status,input_json,result_json,error_message,created_at,updated_at,completed_at)
        VALUES(?,?,?,?,?,?,?,?,?,?)`,
        [
            jobId,
            jobName,
            boundedString(input?.target_date, 20),
            status,
            jsonText(input?.input || {}),
            jsonText(input?.result || null),
            boundedString(input?.error_message, 500),
            timestamp,
            timestamp,
            isTerminalJobStatus(status) ? timestamp : null
        ]
    );

    return {
        ok: true,
        job_id: jobId,
        job_name: jobName,
        status,
        server_time_ms: Date.now()
    };
}

async function listMemoryJobRuns(dbAll, filters = {}) {
    const params = [];
    const where = [];
    const jobName = boundedString(filters.job_name, 80);
    const targetDate = boundedString(filters.target_date, 20);
    if (jobName) {
        where.push("job_name=?");
        params.push(jobName);
    }
    if (targetDate) {
        where.push("target_date=?");
        params.push(targetDate);
    }
    where.push("deleted_at IS NULL");
    params.push(readLimit(filters.limit));
    const rows = await dbAll(
        `SELECT * FROM memory_job_runs ${where.length ? `WHERE ${where.join(" AND ")}` : ""} ORDER BY id DESC LIMIT ?`,
        params
    );

    return rows.map(row => ({
        job_id: row.job_id,
        job_name: row.job_name,
        target_date: row.target_date || "",
        status: row.status,
        input: parseJson(row.input_json, {}),
        result: parseJson(row.result_json, null),
        error_message: row.error_message || "",
        created_at: row.created_at,
        updated_at: row.updated_at,
        completed_at: row.completed_at
    }));
}

module.exports = {
    applyMemoryCorrection,
    createConversationTurn,
    createDailyMemory,
    createMemoryJobRun,
    listConversationTurns,
    listDailyMemory,
    listMemoryJobRuns,
    listProfiles,
    upsertProfile
};
