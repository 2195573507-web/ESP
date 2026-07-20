const crypto = require("crypto");
const {
    isSqliteUniqueConstraintError,
    runUpdateThenInsert
} = require("../db/upsert");

const STATUSES = new Set(["candidate", "active", "rejected", "archived"]);
const REMINDER_STATUSES = new Set(["active", "paused", "archived"]);
const REMINDER_EVENT_STATUSES = new Set(["pending", "triggered", "confirmed", "canceled", "suppressed"]);
const EMERGENCY_SEVERITIES = new Set(["info", "warning", "critical"]);
const EMERGENCY_STATUSES = new Set(["received", "llm_pending", "forwarded", "resolved", "archived"]);
const EVENT_ID_MAX_LENGTH = 120;
const PROFILE_KEY_MAX_LENGTH = 120;
const DEVICE_ID_MAX_LENGTH = 128;
const EXPERIENCE_TITLE_MAX_LENGTH = 200;
const EVENT_TYPE_MAX_LENGTH = 120;
const EMERGENCY_LOCAL_ACTION_MAX_LENGTH = 500;
const RELATION_TYPE_MAX_LENGTH = 80;
const RELATION_SUBJECT_MAX_LENGTH = 200;
const RELATION_PREDICATE_MAX_LENGTH = 120;
const RELATION_OBJECT_MAX_LENGTH = 200;
const REMINDER_TITLE_MAX_LENGTH = 200;
const REMINDER_MESSAGE_MAX_LENGTH = 1000;
const REMINDER_EVENT_MESSAGE_MAX_LENGTH = 1000;
const REMINDER_CHANNEL_MAX_LENGTH = 40;
const REMINDER_TIME_FIELD_MAX_LENGTH = 80;
const CSI_SUMMARY_MAX_LENGTH = 1000;
const CSI_OCCURRED_AT_MAX_LENGTH = 80;
const LCD_PAGE_MAX_LENGTH = 80;
const LCD_LAST_COMMAND_ID_MAX_LENGTH = 120;

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

function clampNumber(value, min, max, fallback) {
    const numeric = Number(value);
    if (!Number.isFinite(numeric)) {
        return fallback;
    }

    return Math.min(Math.max(numeric, min), max);
}

function readLimit(value, fallback = 50, max = 200) {
    const numeric = Number.parseInt(value, 10);
    if (!Number.isFinite(numeric) || numeric <= 0) {
        return fallback;
    }

    return Math.min(numeric, max);
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

function chooseStatus(value, allowed, fallback) {
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

function readOptionalId(value, maxLength, errorCode, field) {
    if (typeof value !== "string") {
        return {
            ok: true,
            value: ""
        };
    }

    const text = value.trim();
    if (text.length > maxLength) {
        return invalidFieldResult(
            errorCode,
            field,
            `${field} must be <= ${maxLength} characters`,
            text.slice(0, maxLength)
        );
    }

    return {
        ok: true,
        value: text
    };
}

function readUniqueKey(value, maxLength, errorCode, field) {
    if (typeof value === "string" && value.trim().length > maxLength) {
        return invalidFieldResult(
            errorCode,
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

function readDeviceId(value) {
    if (value === undefined || value === null) {
        return {
            ok: true,
            value: ""
        };
    }

    const text = String(value).trim();
    if (text.length > DEVICE_ID_MAX_LENGTH) {
        return invalidFieldResult(
            "DEVICE_ID_INVALID",
            "device_id",
            `device_id must be <= ${DEVICE_ID_MAX_LENGTH} characters`,
            text.slice(0, DEVICE_ID_MAX_LENGTH)
        );
    }

    return {
        ok: true,
        value: text
    };
}

function readRequiredBoundedString(value, maxLength, errorCode, field) {
    const text = boundedString(value, maxLength);
    if (!text) {
        return {
            ok: false,
            error: `${field} is required`
        };
    }

    if (typeof value === "string" && value.trim().length > maxLength) {
        return invalidFieldResult(
            errorCode,
            field,
            `${field} must be <= ${maxLength} characters`,
            value.trim().slice(0, maxLength)
        );
    }

    return {
        ok: true,
        value: text
    };
}

function readOptionalBoundedText(value, maxLength, errorCode, field, fallback = "") {
    if (value === undefined || value === null) {
        return {
            ok: true,
            value: fallback
        };
    }

    const text = String(value).trim();
    if (text.length > maxLength) {
        return invalidFieldResult(
            errorCode,
            field,
            `${field} must be <= ${maxLength} characters`,
            text.slice(0, maxLength)
        );
    }

    return {
        ok: true,
        value: text
    };
}

async function upsertEnvironmentProfile(dbRun, input) {
    const profileKeyResult = readUniqueKey(input?.profile_key || input?.key, PROFILE_KEY_MAX_LENGTH, "ENVIRONMENT_PROFILE_KEY_INVALID", "profile_key");
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
    const deviceIdResult = readDeviceId(input?.device_id);
    if (!deviceIdResult.ok) {
        return deviceIdResult;
    }
    const deviceId = deviceIdResult.value;

    const timestamp = nowIso();
    await runUpdateThenInsert(dbRun, {
        updateSql: `UPDATE environment_profile
            SET profile_value=?,
                device_id=?,
                status=?,
                confidence=?,
                evidence_json=?,
                source=?,
                updated_at=?
            WHERE profile_key=?`,
        updateParams: [
            profileValue,
            deviceId,
            chooseStatus(input?.status, STATUSES, "candidate"),
            clampNumber(input?.confidence, 0, 1, 0.5),
            jsonText(Array.isArray(input?.evidence) ? input.evidence : []),
            boundedString(input?.source, 80, "api"),
            timestamp,
            profileKey
        ],
        insertSql: `INSERT INTO environment_profile
            (profile_key,profile_value,device_id,status,confidence,evidence_json,source,created_at,updated_at)
            VALUES(?,?,?,?,?,?,?,?,?)`,
        insertParams: [
            profileKey,
            profileValue,
            deviceId,
            chooseStatus(input?.status, STATUSES, "candidate"),
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
        server_time_ms: Date.now()
    };
}

function mapEnvironmentProfile(row) {
    return {
        id: row.id,
        profile_key: row.profile_key,
        profile_value: row.profile_value,
        device_id: row.device_id || "",
        status: row.status,
        confidence: row.confidence,
        evidence: parseJson(row.evidence_json, []),
        source: row.source || "",
        created_at: row.created_at,
        updated_at: row.updated_at,
        deleted_at: row.deleted_at || ""
    };
}

async function listEnvironmentProfiles(dbAll, filters = {}) {
    const params = [];
    const where = [];
    const deviceId = boundedString(filters.device_id, 128);
    const status = boundedString(filters.status, 40);
    if (deviceId) {
        where.push("device_id=?");
        params.push(deviceId);
    }
    if (status) {
        where.push("status=?");
        params.push(status);
    }
    where.push("deleted_at IS NULL");
    params.push(readLimit(filters.limit));
    const rows = await dbAll(
        `SELECT * FROM environment_profile ${where.length ? `WHERE ${where.join(" AND ")}` : ""} ORDER BY id DESC LIMIT ?`,
        params
    );
    return rows.map(mapEnvironmentProfile);
}

async function createExperienceMemory(dbRun, input) {
    const titleResult = readRequiredBoundedString(input?.title, EXPERIENCE_TITLE_MAX_LENGTH, "EXPERIENCE_TITLE_INVALID", "title");
    if (!titleResult.ok) {
        return titleResult;
    }
    const title = titleResult.value;

    const experienceIdResult = readOptionalId(input?.experience_id, EVENT_ID_MAX_LENGTH, "EXPERIENCE_ID_INVALID", "experience_id");
    if (!experienceIdResult.ok) {
        return experienceIdResult;
    }

    const experienceId = experienceIdResult.value || makeId("experience");
    const timestamp = nowIso();
    let result;
    try {
        result = await dbRun(
            `INSERT INTO experience_memory
            (experience_id,title,situation,action,outcome,status,confidence,evidence_json,source,created_at,updated_at)
            VALUES(?,?,?,?,?,?,?,?,?,?,?)`,
            [
                experienceId,
                title,
                boundedString(input?.situation, 4000),
                boundedString(input?.action, 4000),
                boundedString(input?.outcome, 4000),
                chooseStatus(input?.status, STATUSES, "candidate"),
                clampNumber(input?.confidence, 0, 1, 0.5),
                jsonText(Array.isArray(input?.evidence) ? input.evidence : []),
                boundedString(input?.source, 80, "api"),
                timestamp,
                timestamp
            ]
        );
    } catch (error) {
        if (isSqliteUniqueConstraintError(error)) {
            return duplicateIdResult("EXPERIENCE_ID_DUPLICATE", "experience_id", experienceId);
        }
        throw error;
    }

    return {
        ok: true,
        id: result.lastID,
        experience_id: experienceId,
        server_time_ms: Date.now()
    };
}

function mapExperience(row) {
    return {
        id: row.id,
        experience_id: row.experience_id,
        title: row.title,
        situation: row.situation || "",
        action: row.action || "",
        outcome: row.outcome || "",
        status: row.status,
        confidence: row.confidence,
        evidence: parseJson(row.evidence_json, []),
        source: row.source || "",
        created_at: row.created_at,
        updated_at: row.updated_at,
        deleted_at: row.deleted_at || ""
    };
}

async function listExperienceMemory(dbAll, filters = {}) {
    const params = [];
    let where = "";
    const status = boundedString(filters.status, 40);
    if (status) {
        where = "WHERE status=? AND deleted_at IS NULL";
        params.push(status);
    } else {
        where = "WHERE deleted_at IS NULL";
    }
    params.push(readLimit(filters.limit));
    const rows = await dbAll(
        `SELECT * FROM experience_memory ${where} ORDER BY id DESC LIMIT ?`,
        params
    );
    return rows.map(mapExperience);
}

async function createRelationMemory(dbRun, input) {
    const subjectResult = readRequiredBoundedString(input?.subject, RELATION_SUBJECT_MAX_LENGTH, "RELATION_SUBJECT_INVALID", "subject");
    const predicateResult = readRequiredBoundedString(input?.predicate, RELATION_PREDICATE_MAX_LENGTH, "RELATION_PREDICATE_INVALID", "predicate");
    const objectResult = readRequiredBoundedString(input?.object, RELATION_OBJECT_MAX_LENGTH, "RELATION_OBJECT_INVALID", "object");
    if (!subjectResult.ok || !predicateResult.ok || !objectResult.ok) {
        const missingRequiredField = [subjectResult, predicateResult, objectResult].some((result) => !result.ok && !result.code);
        if (!missingRequiredField) {
            return [subjectResult, predicateResult, objectResult].find((result) => !result.ok);
        }

        return {
            ok: false,
            error: "subject, predicate, and object are required"
        };
    }
    const subject = subjectResult.value;
    const predicate = predicateResult.value;
    const object = objectResult.value;

    const relationIdResult = readOptionalId(input?.relation_id, EVENT_ID_MAX_LENGTH, "RELATION_ID_INVALID", "relation_id");
    if (!relationIdResult.ok) {
        return relationIdResult;
    }

    const relationId = relationIdResult.value || makeId("relation");
    const relationTypeResult = readOptionalId(input?.relation_type, RELATION_TYPE_MAX_LENGTH, "RELATION_TYPE_INVALID", "relation_type");
    if (!relationTypeResult.ok) {
        return relationTypeResult;
    }
    const relationType = relationTypeResult.value || "general";
    const timestamp = nowIso();
    let result;
    try {
        result = await dbRun(
            `INSERT INTO relation_memory
            (relation_id,subject,predicate,object,relation_type,status,confidence,evidence_json,source,created_at,updated_at)
            VALUES(?,?,?,?,?,?,?,?,?,?,?)`,
            [
                relationId,
                subject,
                predicate,
                object,
                relationType,
                chooseStatus(input?.status, STATUSES, "candidate"),
                clampNumber(input?.confidence, 0, 1, 0.5),
                jsonText(Array.isArray(input?.evidence) ? input.evidence : []),
                boundedString(input?.source, 80, "api"),
                timestamp,
                timestamp
            ]
        );
    } catch (error) {
        if (isSqliteUniqueConstraintError(error)) {
            return duplicateIdResult("RELATION_ID_DUPLICATE", "relation_id", relationId);
        }
        throw error;
    }

    return {
        ok: true,
        id: result.lastID,
        relation_id: relationId,
        server_time_ms: Date.now()
    };
}

function mapRelation(row) {
    return {
        id: row.id,
        relation_id: row.relation_id,
        subject: row.subject,
        predicate: row.predicate,
        object: row.object,
        relation_type: row.relation_type,
        status: row.status,
        confidence: row.confidence,
        evidence: parseJson(row.evidence_json, []),
        source: row.source || "",
        created_at: row.created_at,
        updated_at: row.updated_at,
        deleted_at: row.deleted_at || ""
    };
}

async function listRelationMemory(dbAll, filters = {}) {
    const params = [];
    const where = [];
    const status = boundedString(filters.status, 40);
    const relationType = boundedString(filters.relation_type, 80);
    if (status) {
        where.push("status=?");
        params.push(status);
    }
    if (relationType) {
        where.push("relation_type=?");
        params.push(relationType);
    }
    where.push("deleted_at IS NULL");
    params.push(readLimit(filters.limit));
    const rows = await dbAll(
        `SELECT * FROM relation_memory ${where.length ? `WHERE ${where.join(" AND ")}` : ""} ORDER BY id DESC LIMIT ?`,
        params
    );
    return rows.map(mapRelation);
}

async function createReminderRule(dbRun, input) {
    const titleResult = readRequiredBoundedString(input?.title, REMINDER_TITLE_MAX_LENGTH, "REMINDER_TITLE_INVALID", "title");
    const messageResult = readRequiredBoundedString(input?.message, REMINDER_MESSAGE_MAX_LENGTH, "REMINDER_MESSAGE_INVALID", "message");
    if (!titleResult.ok || !messageResult.ok) {
        if (titleResult.code || messageResult.code) {
            return titleResult.code ? titleResult : messageResult;
        }

        return {
            ok: false,
            error: "title and message are required"
        };
    }
    const title = titleResult.value;
    const message = messageResult.value;

    const reminderIdResult = readOptionalId(input?.reminder_id, EVENT_ID_MAX_LENGTH, "REMINDER_ID_INVALID", "reminder_id");
    if (!reminderIdResult.ok) {
        return reminderIdResult;
    }

    const reminderId = reminderIdResult.value || makeId("reminder");
    const channelResult = readOptionalBoundedText(input?.channel, REMINDER_CHANNEL_MAX_LENGTH, "REMINDER_CHANNEL_INVALID", "channel", "voice");
    if (!channelResult.ok) {
        return channelResult;
    }
    const nextRunAtResult = readOptionalBoundedText(input?.next_run_at, REMINDER_TIME_FIELD_MAX_LENGTH, "REMINDER_NEXT_RUN_AT_INVALID", "next_run_at");
    if (!nextRunAtResult.ok) {
        return nextRunAtResult;
    }
    const suppressUntilResult = readOptionalBoundedText(input?.suppress_until, REMINDER_TIME_FIELD_MAX_LENGTH, "REMINDER_SUPPRESS_UNTIL_INVALID", "suppress_until");
    if (!suppressUntilResult.ok) {
        return suppressUntilResult;
    }
    const timestamp = nowIso();
    try {
        await dbRun(
            `INSERT INTO reminder_rules
            (reminder_id,title,message,rule_json,channel,status,next_run_at,suppress_until,source,created_at,updated_at)
            VALUES(?,?,?,?,?,?,?,?,?,?,?)`,
            [
                reminderId,
                title,
                message,
                jsonText(input?.rule || {}),
                channelResult.value,
                chooseStatus(input?.status, REMINDER_STATUSES, "active"),
                nextRunAtResult.value,
                suppressUntilResult.value,
                boundedString(input?.source, 80, "api"),
                timestamp,
                timestamp
            ]
        );
    } catch (error) {
        if (isSqliteUniqueConstraintError(error)) {
            return duplicateIdResult("REMINDER_ID_DUPLICATE", "reminder_id", reminderId);
        }
        throw error;
    }

    return {
        ok: true,
        reminder_id: reminderId,
        server_time_ms: Date.now()
    };
}

function mapReminderRule(row) {
    return {
        reminder_id: row.reminder_id,
        title: row.title,
        message: row.message,
        rule: parseJson(row.rule_json, {}),
        channel: row.channel,
        status: row.status,
        next_run_at: row.next_run_at || "",
        suppress_until: row.suppress_until || "",
        source: row.source || "",
        created_at: row.created_at,
        updated_at: row.updated_at,
        deleted_at: row.deleted_at || ""
    };
}

async function listReminderRules(dbAll, filters = {}) {
    const params = [];
    let where = "";
    const status = boundedString(filters.status, 40);
    if (status) {
        where = "WHERE status=? AND deleted_at IS NULL";
        params.push(status);
    } else {
        where = "WHERE deleted_at IS NULL";
    }
    params.push(readLimit(filters.limit));
    const rows = await dbAll(
        `SELECT * FROM reminder_rules ${where} ORDER BY id DESC LIMIT ?`,
        params
    );
    return rows.map(mapReminderRule);
}

async function createReminderEvent(dbRun, input) {
    const messageResult = readRequiredBoundedString(input?.message, REMINDER_EVENT_MESSAGE_MAX_LENGTH, "REMINDER_EVENT_MESSAGE_INVALID", "message");
    if (!messageResult.ok) {
        return messageResult;
    }
    const message = messageResult.value;

    const reminderEventIdResult = readOptionalId(input?.reminder_event_id, EVENT_ID_MAX_LENGTH, "REMINDER_EVENT_ID_INVALID", "reminder_event_id");
    if (!reminderEventIdResult.ok) {
        return reminderEventIdResult;
    }

    const reminderEventId = reminderEventIdResult.value || makeId("reminder_event");
    const reminderIdResult = readOptionalId(input?.reminder_id, EVENT_ID_MAX_LENGTH, "REMINDER_ID_INVALID", "reminder_id");
    if (!reminderIdResult.ok) {
        return reminderIdResult;
    }
    const dueAtResult = readOptionalBoundedText(input?.due_at, REMINDER_TIME_FIELD_MAX_LENGTH, "REMINDER_DUE_AT_INVALID", "due_at");
    if (!dueAtResult.ok) {
        return dueAtResult;
    }
    const timestamp = nowIso();
    const status = chooseStatus(input?.status, REMINDER_EVENT_STATUSES, "pending");
    try {
        await dbRun(
            `INSERT INTO reminder_records
            (reminder_event_id,reminder_id,status,message,due_at,action_json,result_json,created_at,updated_at,completed_at)
            VALUES(?,?,?,?,?,?,?,?,?,?)`,
            [
                reminderEventId,
                reminderIdResult.value,
                status,
                message,
                dueAtResult.value,
                jsonText(input?.action || {}),
                jsonText(input?.result || null),
                timestamp,
                timestamp,
                status === "confirmed" || status === "canceled" ? timestamp : null
            ]
        );
    } catch (error) {
        if (isSqliteUniqueConstraintError(error)) {
            return duplicateIdResult("REMINDER_EVENT_ID_DUPLICATE", "reminder_event_id", reminderEventId);
        }
        throw error;
    }

    return {
        ok: true,
        reminder_event_id: reminderEventId,
        server_time_ms: Date.now()
    };
}

function mapReminderEvent(row) {
    return {
        reminder_event_id: row.reminder_event_id,
        reminder_id: row.reminder_id || "",
        status: row.status,
        message: row.message,
        due_at: row.due_at || "",
        action: parseJson(row.action_json, {}),
        result: parseJson(row.result_json, null),
        created_at: row.created_at,
        updated_at: row.updated_at,
        completed_at: row.completed_at || "",
        deleted_at: row.deleted_at || ""
    };
}

async function listReminderEvents(dbAll, filters = {}) {
    const params = [];
    let where = "";
    const status = boundedString(filters.status, 40);
    if (status) {
        where = "WHERE status=? AND deleted_at IS NULL";
        params.push(status);
    } else {
        where = "WHERE deleted_at IS NULL";
    }
    params.push(readLimit(filters.limit));
    const rows = await dbAll(
        `SELECT * FROM reminder_records ${where} ORDER BY id DESC LIMIT ?`,
        params
    );
    return rows.map(mapReminderEvent);
}

async function createEmergencyEvent(dbRun, input) {
    const eventTypeResult = readRequiredBoundedString(
        input?.event_type,
        EVENT_TYPE_MAX_LENGTH,
        "EMERGENCY_EVENT_TYPE_INVALID",
        "event_type"
    );
    if (!eventTypeResult.ok) {
        return eventTypeResult;
    }
    const eventType = eventTypeResult.value;

    const eventIdResult = readOptionalId(input?.event_id, EVENT_ID_MAX_LENGTH, "EMERGENCY_EVENT_ID_INVALID", "event_id");
    if (!eventIdResult.ok) {
        return eventIdResult;
    }

    const eventId = eventIdResult.value || makeId("emergency");
    const deviceIdResult = readDeviceId(input?.device_id);
    if (!deviceIdResult.ok) {
        return deviceIdResult;
    }
    const deviceId = deviceIdResult.value;
    const timestamp = nowIso();
    const status = chooseStatus(input?.status, EMERGENCY_STATUSES, "received");
    const localActionResult = readOptionalBoundedText(
        input?.local_action,
        EMERGENCY_LOCAL_ACTION_MAX_LENGTH,
        "EMERGENCY_LOCAL_ACTION_INVALID",
        "local_action"
    );
    if (!localActionResult.ok) {
        return localActionResult;
    }
    try {
        await dbRun(
            `INSERT INTO emergency_events
            (event_id,device_id,event_type,severity,local_action,payload_json,llm_decision_json,status,created_at,updated_at,resolved_at)
            VALUES(?,?,?,?,?,?,?,?,?,?,?)`,
            [
                eventId,
                deviceId,
                eventType,
                chooseStatus(input?.severity, EMERGENCY_SEVERITIES, "info"),
                localActionResult.value,
                jsonText(input?.payload || {}),
                jsonText(input?.llm_decision || null),
                status,
                timestamp,
                timestamp,
                status === "resolved" ? timestamp : null
            ]
        );
    } catch (error) {
        if (isSqliteUniqueConstraintError(error)) {
            return duplicateIdResult("EMERGENCY_EVENT_ID_DUPLICATE", "event_id", eventId);
        }
        throw error;
    }

    return {
        ok: true,
        event_id: eventId,
        server_time_ms: Date.now()
    };
}

function mapEmergency(row) {
    return {
        event_id: row.event_id,
        device_id: row.device_id || "",
        event_type: row.event_type,
        severity: row.severity,
        local_action: row.local_action || "",
        payload: parseJson(row.payload_json, {}),
        llm_decision: parseJson(row.llm_decision_json, null),
        status: row.status,
        created_at: row.created_at,
        updated_at: row.updated_at,
        resolved_at: row.resolved_at || "",
        deleted_at: row.deleted_at || ""
    };
}

async function listEmergencyEvents(dbAll, filters = {}) {
    const params = [];
    const where = [];
    const deviceId = boundedString(filters.device_id, 128);
    const status = boundedString(filters.status, 40);
    if (deviceId) {
        where.push("device_id=?");
        params.push(deviceId);
    }
    if (status) {
        where.push("status=?");
        params.push(status);
    }
    where.push("deleted_at IS NULL");
    params.push(readLimit(filters.limit));
    const rows = await dbAll(
        `SELECT * FROM emergency_events ${where.length ? `WHERE ${where.join(" AND ")}` : ""} ORDER BY id DESC LIMIT ?`,
        params
    );
    return rows.map(mapEmergency);
}

async function createCsiBehaviorEvent(dbRun, input) {
    const behaviorTypeResult = readRequiredBoundedString(
        input?.behavior_type,
        EVENT_TYPE_MAX_LENGTH,
        "CSI_BEHAVIOR_TYPE_INVALID",
        "behavior_type"
    );
    if (!behaviorTypeResult.ok) {
        return behaviorTypeResult;
    }
    const behaviorType = behaviorTypeResult.value;

    const eventIdResult = readOptionalId(input?.event_id, EVENT_ID_MAX_LENGTH, "CSI_EVENT_ID_INVALID", "event_id");
    if (!eventIdResult.ok) {
        return eventIdResult;
    }

    const eventId = eventIdResult.value || makeId("csi");
    const deviceIdResult = readDeviceId(input?.device_id);
    if (!deviceIdResult.ok) {
        return deviceIdResult;
    }
    const deviceId = deviceIdResult.value;
    const occurredAtFallback = nowIso();
    const occurredAtResult = readOptionalBoundedText(input?.occurred_at, CSI_OCCURRED_AT_MAX_LENGTH, "CSI_OCCURRED_AT_INVALID", "occurred_at", occurredAtFallback);
    if (!occurredAtResult.ok) {
        return occurredAtResult;
    }
    const summaryResult = readOptionalBoundedText(input?.summary, CSI_SUMMARY_MAX_LENGTH, "CSI_SUMMARY_INVALID", "summary");
    if (!summaryResult.ok) {
        return summaryResult;
    }
    const timestamp = nowIso();
    try {
        await dbRun(
            `INSERT INTO csi_behavior_events
            (event_id,device_id,behavior_type,confidence,features_json,summary,occurred_at,created_at,updated_at)
            VALUES(?,?,?,?,?,?,?,?,?)`,
            [
                eventId,
                deviceId,
                behaviorType,
                clampNumber(input?.confidence, 0, 1, 0.5),
                jsonText(input?.features || {}),
                summaryResult.value,
                occurredAtResult.value,
                timestamp,
                timestamp
            ]
        );
    } catch (error) {
        if (isSqliteUniqueConstraintError(error)) {
            return duplicateIdResult("CSI_EVENT_ID_DUPLICATE", "event_id", eventId);
        }
        throw error;
    }

    return {
        ok: true,
        event_id: eventId,
        server_time_ms: Date.now()
    };
}

function mapCsiEvent(row) {
    return {
        event_id: row.event_id,
        device_id: row.device_id || "",
        behavior_type: row.behavior_type,
        confidence: row.confidence,
        features: parseJson(row.features_json, {}),
        summary: row.summary || "",
        occurred_at: row.occurred_at,
        created_at: row.created_at,
        updated_at: row.updated_at,
        deleted_at: row.deleted_at || ""
    };
}

async function listCsiBehaviorEvents(dbAll, filters = {}) {
    const params = [];
    const where = [];
    const deviceId = boundedString(filters.device_id, 128);
    const behaviorType = boundedString(filters.behavior_type, 120);
    if (deviceId) {
        where.push("device_id=?");
        params.push(deviceId);
    }
    if (behaviorType) {
        where.push("behavior_type=?");
        params.push(behaviorType);
    }
    where.push("deleted_at IS NULL");
    params.push(readLimit(filters.limit));
    const rows = await dbAll(
        `SELECT * FROM csi_behavior_events ${where.length ? `WHERE ${where.join(" AND ")}` : ""} ORDER BY id DESC LIMIT ?`,
        params
    );
    return rows.map(mapCsiEvent);
}

async function upsertLcdStatus(dbRun, input) {
    const deviceIdResult = readDeviceId(input?.device_id);
    if (!deviceIdResult.ok) {
        return deviceIdResult;
    }
    const deviceId = deviceIdResult.value;
    if (!deviceId) {
        return {
            ok: false,
            error: "device_id is required"
        };
    }
    const pageResult = readOptionalBoundedText(input?.page, LCD_PAGE_MAX_LENGTH, "LCD_PAGE_INVALID", "page", "idle");
    if (!pageResult.ok) {
        return pageResult;
    }
    const lastCommandIdResult = readOptionalBoundedText(input?.last_command_id, LCD_LAST_COMMAND_ID_MAX_LENGTH, "LCD_LAST_COMMAND_ID_INVALID", "last_command_id");
    if (!lastCommandIdResult.ok) {
        return lastCommandIdResult;
    }

    const timestamp = nowIso();
    await runUpdateThenInsert(dbRun, {
        updateSql: `UPDATE lcd_status
            SET page=?,
                state_json=?,
                last_command_id=?,
                created_at=CASE WHEN deleted_at IS NULL THEN created_at ELSE ? END,
                deleted_at=NULL,
                delete_reason=NULL,
                updated_at=?
            WHERE device_id=?`,
        updateParams: [
            pageResult.value,
            jsonText(input?.state || {}),
            lastCommandIdResult.value,
            timestamp,
            timestamp,
            deviceId
        ],
        insertSql: `INSERT INTO lcd_status
            (device_id,page,state_json,last_command_id,updated_at,created_at)
            VALUES(?,?,?,?,?,?)`,
        insertParams: [
            deviceId,
            pageResult.value,
            jsonText(input?.state || {}),
            lastCommandIdResult.value,
            timestamp,
            timestamp
        ]
    });

    return {
        ok: true,
        device_id: deviceId,
        server_time_ms: Date.now()
    };
}

async function listLcdStatus(dbAll, filters = {}) {
    const params = [];
    let where = "";
    const deviceId = boundedString(filters.device_id, 128);
    if (deviceId) {
        where = "WHERE device_id=? AND deleted_at IS NULL";
        params.push(deviceId);
    } else {
        where = "WHERE deleted_at IS NULL";
    }
    params.push(readLimit(filters.limit));
    const rows = await dbAll(
        `SELECT * FROM lcd_status ${where} ORDER BY updated_at DESC LIMIT ?`,
        params
    );
    return rows.map(row => ({
        device_id: row.device_id,
        page: row.page,
        state: parseJson(row.state_json, {}),
        last_command_id: row.last_command_id || "",
        updated_at: row.updated_at,
        created_at: row.created_at,
        deleted_at: row.deleted_at || ""
    }));
}

module.exports = {
    createCsiBehaviorEvent,
    createEmergencyEvent,
    createExperienceMemory,
    createRelationMemory,
    createReminderEvent,
    createReminderRule,
    listCsiBehaviorEvents,
    listEmergencyEvents,
    listEnvironmentProfiles,
    listExperienceMemory,
    listLcdStatus,
    listRelationMemory,
    listReminderEvents,
    listReminderRules,
    upsertEnvironmentProfile,
    upsertLcdStatus
};
