const crypto = require("crypto");
const {
    validateRulePackage
} = require("./schema");

const FEEDBACK_TYPES = new Set(["accepted", "rejected", "modified", "cancelled", "reverted", "manual_override"]);
const MEMORY_STATUSES = new Set(["CANDIDATE", "CONFIRMED", "REJECTED", "DISABLED"]);
const CANDIDATE_STATUSES = new Set(["CANDIDATE", "READY", "PUBLISHED", "PROBATION", "REJECTED", "SUSPENDED"]);
const PROBATION_STATUSES = new Set(["RUNNING", "ACTIVE", "ROLLED_BACK", "FAILED", "EXPIRED"]);
const RULE_CANDIDATE_MIN_SAMPLES = 5;
const RULE_CANDIDATE_MIN_CONFIDENCE = 0.85;
const RULE_CANDIDATE_MIN_REPLAY_SAMPLES = 5;
const RULE_CANDIDATE_MAX_REPLAY_EVENTS = 500;
const RULE_CANDIDATE_FRESHNESS_MS = 7 * 24 * 60 * 60 * 1000;
const RULE_CANDIDATE_DATA_FRESHNESS_MS = 24 * 60 * 60 * 1000;
const RULE_CANDIDATE_MAX_TRIGGER_RATE_PER_HOUR = 20;
const PROBATION_MIN_TRIGGERS = 5;
const PROBATION_MAX_FAILURE_RATE = 0.2;
const PROBATION_MAX_OVERRIDES = 3;
const PROBATION_MAX_ANOMALIES = 3;
const PROBATION_TRIGGER_RATE_WINDOW_MS = 60 * 60 * 1000;

function makeId(prefix) {
    const value = typeof crypto.randomUUID === "function"
        ? crypto.randomUUID()
        : crypto.randomBytes(16).toString("hex");
    return `${prefix}_${value}`;
}

function text(value, max = 256) {
    return typeof value === "string" ? value.trim().slice(0, max) : "";
}

function number(value, fallback, min, max) {
    const parsed = Number(value);
    return Number.isFinite(parsed) ? Math.min(max, Math.max(min, parsed)) : fallback;
}

function integer(value, fallback, min, max) {
    return Math.trunc(number(value, fallback, min, max));
}

function json(value, fallback = {}) {
    if (value === undefined || value === null || value === "") return fallback;
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

function limit(value, fallback = 50, maximum = 200) {
    const parsed = Number.parseInt(value, 10);
    return Number.isFinite(parsed) && parsed > 0 ? Math.min(parsed, maximum) : fallback;
}

function mapFeedback(row) {
    return {
        feedback_id: row.feedback_id,
        decision_id: row.decision_id || "",
        rule_id: row.rule_id || "",
        room_id: row.room_id || "",
        feedback_type: row.feedback_type,
        payload: json(row.payload_json, {}),
        created_at_ms: Number(row.created_at_ms) || null
    };
}

function mapCandidate(row) {
    const source = json(row.source_json, {});
    return {
        candidate_id: row.candidate_id,
        room_id: row.room_id || "",
        category: row.category,
        content: row.content,
        confidence: Number(row.confidence) || 0,
        status: row.status,
        source,
        automation_allowed: row.automation_allowed === undefined || row.automation_allowed === null
            ? null
            : Number(row.automation_allowed) !== 0,
        affected_rule_ids: Array.isArray(source.affected_rule_ids)
            ? source.affected_rule_ids.map(value => text(value, 64)).filter(Boolean).slice(0, 16)
            : [],
        created_at_ms: Number(row.created_at_ms) || null,
        updated_at_ms: Number(row.updated_at_ms) || null
    };
}

function mapRuleCandidate(row) {
    const evaluation = json(row.gate_json, {});
    return {
        candidate_id: row.candidate_id,
        room_id: row.room_id || "",
        rule_package: json(row.rule_json, {}),
        source: json(row.source_json, {}),
        confidence: Number(row.confidence) || 0,
        sample_count: Number(row.sample_count) || 0,
        status: row.status,
        gates: evaluation.gates && typeof evaluation.gates === "object" ? evaluation.gates : evaluation,
        gate_details: evaluation.details && typeof evaluation.details === "object" ? evaluation.details : {},
        gates_passed: evaluation.passed === true,
        published_version: Number(row.published_version) || null,
        probation_run_ids: json(row.probation_run_ids_json, []),
        created_at_ms: Number(row.created_at_ms) || null,
        updated_at_ms: Number(row.updated_at_ms) || null
    };
}

function mapHabit(row) {
    return {
        habit_id: row.habit_id,
        room_id: row.room_id || "",
        pattern: json(row.pattern_json, {}),
        evidence_count: Number(row.evidence_count) || 0,
        confidence: Number(row.confidence) || 0,
        status: row.status,
        updated_at_ms: Number(row.updated_at_ms) || null
    };
}

function mapProbation(row) {
    return {
        run_id: row.run_id,
        candidate_id: row.candidate_id || "",
        rule_id: row.rule_id,
        package_version: Number(row.package_version) || 0,
        gateway_id: row.gateway_id || "",
        status: row.status,
        trigger_count: Number(row.trigger_count) || 0,
        failure_count: Number(row.failure_count) || 0,
        override_count: Number(row.override_count) || 0,
        metrics: json(row.metrics_json, {}),
        started_at_ms: Number(row.started_at_ms) || null,
        ends_at_ms: Number(row.ends_at_ms) || null,
        updated_at_ms: Number(row.updated_at_ms) || null
    };
}

async function listFeedback(dbAll, options = {}) {
    const params = [];
    const where = [];
    if (text(options.rule_id, 64)) {
        where.push("rule_id=?");
        params.push(text(options.rule_id, 64));
    }
    if (text(options.room_id, 32)) {
        where.push("room_id=?");
        params.push(text(options.room_id, 32));
    }
    params.push(limit(options.limit, 100, 500));
    const rows = await dbAll(
        `SELECT * FROM home_ai_feedback ${where.length ? `WHERE ${where.join(" AND ")}` : ""}
         ORDER BY created_at_ms DESC LIMIT ?`,
        params
    );
    return rows.map(mapFeedback);
}

async function listMemoryCandidates(dbAll, options = {}) {
    const params = [];
    const where = [];
    if (text(options.status, 32)) {
        where.push("candidate.status=?");
        params.push(text(options.status, 32).toUpperCase());
    }
    if (text(options.room_id, 32)) {
        where.push("candidate.room_id=?");
        params.push(text(options.room_id, 32));
    }
    params.push(limit(options.limit, 100, 300));
    const rows = await dbAll(
        `SELECT candidate.*,confirmed.automation_allowed
         FROM home_ai_memory_candidates AS candidate
         LEFT JOIN home_ai_confirmed_memories AS confirmed
           ON confirmed.candidate_id=candidate.candidate_id
         ${where.length ? `WHERE ${where.join(" AND ")}` : ""}
         ORDER BY candidate.updated_at_ms DESC LIMIT ?`,
        params
    );
    return rows.map(mapCandidate);
}

async function createMemoryCandidate(dbRun, body = {}) {
    const category = text(body.category, 64);
    const content = text(body.content, 512);
    if (!category || !content) {
        return { ok: false, code: "MEMORY_CANDIDATE_INVALID", error: "category and content are required" };
    }
    const nowMs = Date.now();
    const candidate = {
        candidate_id: text(body.candidate_id, 160) || makeId("memory"),
        room_id: text(body.room_id, 32),
        category,
        content,
        confidence: number(body.confidence, 0, 0, 1),
        status: "CANDIDATE",
        source: body.source && typeof body.source === "object" ? body.source : {},
        automation_allowed: null,
        affected_rule_ids: Array.isArray(body.source?.affected_rule_ids)
            ? body.source.affected_rule_ids.map(value => text(value, 64)).filter(Boolean).slice(0, 16)
            : []
    };
    await dbRun(
        `INSERT INTO home_ai_memory_candidates
         (candidate_id,room_id,category,content,confidence,status,source_json,created_at_ms,updated_at_ms)
         VALUES(?,?,?,?,?,?,?,?,?)`,
        [candidate.candidate_id, candidate.room_id, candidate.category, candidate.content,
            candidate.confidence, candidate.status, jsonText(candidate.source), nowMs, nowMs]
    );
    return { ok: true, candidate: { ...candidate, created_at_ms: nowMs, updated_at_ms: nowMs } };
}

async function updateMemoryCandidate(dbRun, dbAll, candidateId, body = {}) {
    const id = text(candidateId, 160);
    const rows = await dbAll("SELECT * FROM home_ai_memory_candidates WHERE candidate_id=?", [id]);
    if (!rows[0]) return { ok: false, code: "MEMORY_CANDIDATE_NOT_FOUND", error: "memory candidate not found" };
    const current = mapCandidate(rows[0]);
    const status = text(body.status, 32).toUpperCase();
    if (status && !MEMORY_STATUSES.has(status)) {
        return { ok: false, code: "MEMORY_STATUS_INVALID", error: "memory status is invalid" };
    }
    const nowMs = Date.now();
    const next = {
        ...current,
        category: text(body.category, 64) || current.category,
        content: text(body.content, 512) || current.content,
        confidence: body.confidence === undefined ? current.confidence : number(body.confidence, current.confidence, 0, 1),
        status: status || current.status,
        source: body.source && typeof body.source === "object" ? body.source : current.source
    };
    await dbRun(
        `UPDATE home_ai_memory_candidates
         SET category=?,content=?,confidence=?,status=?,source_json=?,updated_at_ms=?
         WHERE candidate_id=?`,
        [next.category, next.content, next.confidence, next.status, jsonText(next.source), nowMs, id]
    );
    let automationAllowed = null;
    if (next.status === "CONFIRMED") {
        const confirmedRows = await dbAll(
            "SELECT automation_allowed FROM home_ai_confirmed_memories WHERE candidate_id=? OR memory_id=? LIMIT 1",
            [id, id]
        );
        automationAllowed = body.automation_allowed === undefined
            ? (confirmedRows[0] ? Number(confirmedRows[0].automation_allowed) !== 0 : false)
            : body.automation_allowed === true;
        await dbRun(
            `INSERT INTO home_ai_confirmed_memories
             (memory_id,candidate_id,room_id,category,content,automation_allowed,source_json,created_at_ms,updated_at_ms)
             VALUES(?,?,?,?,?,?,?,?,?)
             ON CONFLICT(memory_id) DO UPDATE SET
               content=excluded.content, category=excluded.category,
               automation_allowed=excluded.automation_allowed, source_json=excluded.source_json,
               updated_at_ms=excluded.updated_at_ms`,
            [id, id, next.room_id, next.category, next.content, automationAllowed ? 1 : 0,
                jsonText(next.source), current.created_at_ms || nowMs, nowMs]
        );
    } else {
        await dbRun("DELETE FROM home_ai_confirmed_memories WHERE candidate_id=? OR memory_id=?", [id, id]);
    }
    return { ok: true, candidate: { ...next, automation_allowed: automationAllowed, updated_at_ms: nowMs } };
}

async function deleteMemoryCandidate(dbRun, candidateId) {
    const id = text(candidateId, 160);
    await dbRun("DELETE FROM home_ai_confirmed_memories WHERE candidate_id=? OR memory_id=?", [id, id]);
    const result = await dbRun("DELETE FROM home_ai_memory_candidates WHERE candidate_id=?", [id]);
    return result.changes > 0
        ? { ok: true, candidate_id: id }
        : { ok: false, code: "MEMORY_CANDIDATE_NOT_FOUND", error: "memory candidate not found" };
}

async function listHabits(dbAll, options = {}) {
    const params = [];
    const where = [];
    if (text(options.room_id, 32)) {
        where.push("room_id=?");
        params.push(text(options.room_id, 32));
    }
    params.push(limit(options.limit, 100, 300));
    const rows = await dbAll(
        `SELECT * FROM home_ai_habits ${where.length ? `WHERE ${where.join(" AND ")}` : ""}
         ORDER BY updated_at_ms DESC LIMIT ?`,
        params
    );
    return rows.map(mapHabit);
}

async function recordHabitEvidence(dbRun, dbAll, feedback) {
    if (!feedback || !FEEDBACK_TYPES.has(feedback.feedback_type)) return { ok: false, ignored: true };
    const ruleId = text(feedback.rule_id, 64);
    const roomId = text(feedback.room_id, 32);
    if (!ruleId && !roomId) return { ok: true, ignored: true };
    const habitId = `habit_${roomId || "global"}_${ruleId || "all"}`;
    const evidenceId = makeId("evidence");
    const nowMs = Date.now();
    const insert = await dbRun(
        `INSERT OR IGNORE INTO home_ai_habit_evidence
         (evidence_id,habit_id,feedback_id,room_id,evidence_type,payload_json,created_at_ms)
         VALUES(?,?,?,?,?,?,?)`,
        [evidenceId, habitId, feedback.feedback_id, roomId, feedback.feedback_type, jsonText(feedback.payload), nowMs]
    );
    if (insert.changes === 0) return { ok: true, ignored: true };
    const rows = await dbAll("SELECT * FROM home_ai_habits WHERE habit_id=?", [habitId]);
    const current = rows[0] ? mapHabit(rows[0]) : {
        habit_id: habitId,
        room_id: roomId,
        pattern: { rule_id: ruleId },
        evidence_count: 0,
        confidence: 0.5,
        status: "PROBATION"
    };
    const positive = feedback.feedback_type === "accepted";
    const negative = ["rejected", "cancelled", "reverted", "manual_override"].includes(feedback.feedback_type);
    const nextConfidence = Math.min(0.99, Math.max(0.01,
        current.confidence + (positive ? 0.05 : (negative ? -0.1 : -0.01))));
    const evidenceCount = current.evidence_count + 1;
    const status = evidenceCount >= RULE_CANDIDATE_MIN_SAMPLES && nextConfidence >= RULE_CANDIDATE_MIN_CONFIDENCE ? "ACTIVE" : "PROBATION";
    await dbRun(
        `INSERT INTO home_ai_habits(habit_id,room_id,pattern_json,evidence_count,confidence,status,updated_at_ms)
         VALUES(?,?,?,?,?,?,?)
         ON CONFLICT(habit_id) DO UPDATE SET
           evidence_count=excluded.evidence_count, confidence=excluded.confidence,
           status=excluded.status, pattern_json=excluded.pattern_json, updated_at_ms=excluded.updated_at_ms`,
        [habitId, roomId, jsonText(current.pattern), evidenceCount, nextConfidence, status, nowMs]
    );
    let ruleCandidate = null;
    const proposedPackage = feedback.payload?.rule_package;
    if (status === "ACTIVE" && proposedPackage && typeof proposedPackage === "object") {
        const habitPackage = json(jsonText(proposedPackage), {});
        habitPackage.rules = Array.isArray(habitPackage.rules) ? habitPackage.rules.map(rule =>
            rule?.rule_id === ruleId ? {
                ...rule,
                rule_type: "habit_learning",
                source: "habit_learning",
                probation: { enabled: true, until_ms: null }
            } : rule) : [];
        const candidateId = `habit_${habitId}`;
        const existingCandidateRows = await dbAll(
            "SELECT * FROM home_ai_rule_candidates WHERE candidate_id=? LIMIT 1",
            [candidateId]
        );
        const source = {
            ...feedback.payload,
            habit_id: habitId,
            feedback_id: feedback.feedback_id,
            rule_id: ruleId,
            room_id: roomId,
            explicit_feedback: true,
            generated_from_habit: true
        };
        if (!existingCandidateRows[0]) {
            const created = await createRuleCandidate(dbRun, {
                candidate_id: candidateId,
                room_id: roomId,
                rule_package: habitPackage,
                confidence: nextConfidence,
                sample_count: evidenceCount,
                source
            });
            if (created.ok) ruleCandidate = created.candidate;
        } else {
            const existingCandidate = mapRuleCandidate(existingCandidateRows[0]);
            if (["CANDIDATE", "READY", "REJECTED"].includes(existingCandidate.status)) {
                const validation = validateRulePackage(habitPackage);
                if (validation.ok) {
                    await dbRun(
                        `UPDATE home_ai_rule_candidates
                         SET rule_json=?,source_json=?,confidence=?,sample_count=?,status='CANDIDATE',updated_at_ms=?
                         WHERE candidate_id=?`,
                        [jsonText(validation.package), jsonText(source), nextConfidence, evidenceCount, nowMs, candidateId]
                    );
                    ruleCandidate = {
                        ...existingCandidate,
                        rule_package: validation.package,
                        source,
                        confidence: nextConfidence,
                        sample_count: evidenceCount,
                        status: "CANDIDATE",
                        updated_at_ms: nowMs
                    };
                }
            }
        }
    }
    return {
        ok: true,
        ignored: false,
        habit: { ...current, evidence_count: evidenceCount, confidence: nextConfidence, status, updated_at_ms: nowMs },
        rule_candidate: ruleCandidate
    };
}

async function createRuleCandidate(dbRun, body = {}) {
    const validation = validateRulePackage(body.rule_package || body.package || body);
    if (!validation.ok) return validation;
    const nowMs = Date.now();
    const candidate = {
        candidate_id: text(body.candidate_id, 160) || makeId("rule_candidate"),
        room_id: text(body.room_id, 32),
        rule_package: validation.package,
        source: body.source && typeof body.source === "object" ? body.source : {},
        confidence: number(body.confidence, 0, 0, 1),
        sample_count: integer(body.sample_count, 0, 0, 1000000),
        status: "CANDIDATE",
        gates: {}
    };
    await dbRun(
        `INSERT INTO home_ai_rule_candidates
         (candidate_id,room_id,rule_json,source_json,confidence,sample_count,status,gate_json,published_version,probation_run_ids_json,created_at_ms,updated_at_ms)
         VALUES(?,?,?,?,?,?,?,?,?,?,?,?)`,
        [candidate.candidate_id, candidate.room_id, jsonText(candidate.rule_package), jsonText(candidate.source),
            candidate.confidence, candidate.sample_count, candidate.status, "{}", null, "[]", nowMs, nowMs]
    );
    return { ok: true, candidate: { ...candidate, created_at_ms: nowMs, updated_at_ms: nowMs } };
}

async function listRuleCandidates(dbAll, options = {}) {
    const params = [];
    const where = [];
    if (text(options.status, 32)) {
        where.push("status=?");
        params.push(text(options.status, 32).toUpperCase());
    }
    params.push(limit(options.limit, 100, 300));
    const rows = await dbAll(
        `SELECT * FROM home_ai_rule_candidates ${where.length ? `WHERE ${where.join(" AND ")}` : ""}
         ORDER BY updated_at_ms DESC LIMIT ?`,
        params
    );
    return rows.map(mapRuleCandidate);
}

function scalarValues(value) {
    return Array.isArray(value) ? value : [value];
}

function valuesOverlap(left, right) {
    return scalarValues(left).some(value => scalarValues(right).includes(value));
}

function conditionRange(condition) {
    if (!condition || !["stable_target_count", "temperature_c", "humidity_percent", "air_quality_score"].includes(condition.field)) {
        return null;
    }
    const values = scalarValues(condition.value).map(Number);
    if (values.some(value => !Number.isFinite(value))) return null;
    switch (condition.operator) {
    case "eq": return [values[0], values[0]];
    case "gt": return [values[0] + Number.EPSILON, Number.POSITIVE_INFINITY];
    case "gte": return [values[0], Number.POSITIVE_INFINITY];
    case "lt": return [Number.NEGATIVE_INFINITY, values[0] - Number.EPSILON];
    case "lte": return [Number.NEGATIVE_INFINITY, values[0]];
    case "range": return values.length === 2 ? [values[0], values[1]] : null;
    case "in": return [Math.min(...values), Math.max(...values)];
    default: return null;
    }
}

function conditionsMayOverlap(left = [], right = []) {
    const sharedFields = new Set(left.map(condition => condition.field).filter(field => right.some(item => item.field === field)));
    for (const field of sharedFields) {
        const leftItems = left.filter(condition => condition.field === field);
        const rightItems = right.filter(condition => condition.field === field);
        for (const leftItem of leftItems) {
            for (const rightItem of rightItems) {
                const leftRange = conditionRange(leftItem);
                const rightRange = conditionRange(rightItem);
                if (leftRange && rightRange && (leftRange[1] < rightRange[0] || rightRange[1] < leftRange[0])) return false;
                if (["eq", "in"].includes(leftItem.operator) && ["eq", "in"].includes(rightItem.operator) &&
                    !valuesOverlap(leftItem.value, rightItem.value)) return false;
            }
        }
    }
    return true;
}

function findRuleConflicts(rulePackage) {
    const conflicts = [];
    const rules = Array.isArray(rulePackage?.rules) ? rulePackage.rules.filter(rule => rule.enabled !== false) : [];
    for (let leftIndex = 0; leftIndex < rules.length; leftIndex += 1) {
        for (let rightIndex = leftIndex + 1; rightIndex < rules.length; rightIndex += 1) {
            const left = rules[leftIndex];
            const right = rules[rightIndex];
            if (left.room_id !== right.room_id || !conditionsMayOverlap(left.conditions, right.conditions)) continue;
            for (const leftAction of left.actions || []) {
                for (const rightAction of right.actions || []) {
                    if (leftAction.device_id && leftAction.device_id === rightAction.device_id &&
                        new Set([leftAction.action, rightAction.action]).size > 1 &&
                        [leftAction.action, rightAction.action].every(action => ["turn_on", "turn_off"].includes(action))) {
                        conflicts.push({ left_rule_id: left.rule_id, right_rule_id: right.rule_id, device_id: leftAction.device_id });
                    }
                }
            }
        }
    }
    return conflicts;
}

function compareNumber(operator, actual, value) {
    const expected = scalarValues(value).map(Number);
    if (!Number.isFinite(actual) || expected.some(item => !Number.isFinite(item))) return false;
    switch (operator) {
    case "eq": return actual === expected[0];
    case "neq": return actual !== expected[0];
    case "gt": return actual > expected[0];
    case "gte": return actual >= expected[0];
    case "lt": return actual < expected[0];
    case "lte": return actual <= expected[0];
    case "range": return expected.length === 2 && actual >= expected[0] && actual <= expected[1];
    case "in": return expected.includes(actual);
    default: return false;
    }
}

function replayConditionMatches(condition, payload) {
    if (!condition || !payload || !Object.prototype.hasOwnProperty.call(payload, condition.field)) return false;
    const actual = payload[condition.field];
    if (typeof actual === "number") return compareNumber(condition.operator, actual, condition.value);
    if (condition.operator === "eq") return actual === condition.value;
    if (condition.operator === "neq") return actual !== condition.value;
    if (condition.operator === "in" && Array.isArray(condition.value)) return condition.value.includes(actual);
    return false;
}

function actionKeys(rule) {
    return (rule.actions || []).filter(action => action.device_id && ["turn_on", "turn_off"].includes(action.action))
        .map(action => `${action.device_id}:${action.action}`);
}

async function computeRuleCandidateGates(dbAll, candidate) {
    const packageValidation = validateRulePackage(candidate.rule_package);
    const nowMs = Date.now();
    const generatedAt = Number(candidate.rule_package.generated_at_ms) || 0;
    const habitId = text(candidate.source?.habit_id, 160);
    const feedbackId = text(candidate.source?.feedback_id, 160);
    const sourceRuleId = text(candidate.source?.rule_id, 64);
    const habitRows = habitId
        ? await dbAll("SELECT * FROM home_ai_habits WHERE habit_id=? LIMIT 1", [habitId])
        : [];
    const habit = habitRows[0] ? mapHabit(habitRows[0]) : null;
    const evidenceRows = habitId
        ? await dbAll("SELECT COUNT(*) AS count,MAX(created_at_ms) AS latest FROM home_ai_habit_evidence WHERE habit_id=?", [habitId])
        : feedbackId
            ? await dbAll("SELECT COUNT(*) AS count,MAX(created_at_ms) AS latest FROM home_ai_feedback WHERE feedback_id=?", [feedbackId])
            : [{ count: 0, latest: null }];
    const evidenceCount = Number(evidenceRows[0]?.count) || 0;
    const latestEvidenceAt = Number(evidenceRows[0]?.latest) || 0;
    const targetRules = (candidate.rule_package.rules || []).filter(rule => !sourceRuleId || rule.rule_id === sourceRuleId);
    const roomIds = [...new Set(targetRules.map(rule => rule.room_id).filter(Boolean))];
    const placeholders = roomIds.map(() => "?").join(",");
    const replayRows = roomIds.length === 0 ? [] : await dbAll(
        `SELECT room_id,payload_json,occurred_at_ms FROM home_ai_events
         WHERE event_type='room_state' AND room_id IN (${placeholders}) AND occurred_at_ms>=?
         ORDER BY occurred_at_ms ASC LIMIT ?`,
        [...roomIds, nowMs - RULE_CANDIDATE_FRESHNESS_MS, RULE_CANDIDATE_MAX_REPLAY_EVENTS]
    );
    const replayEvents = replayRows.map(row => ({
        room_id: row.room_id,
        occurred_at_ms: Number(row.occurred_at_ms) || 0,
        payload: json(row.payload_json, {})
    }));
    const simulatedActions = new Map();
    let matchedEvents = 0;
    const replaySampleCounts = new Map(targetRules.map(rule => [rule.rule_id, 0]));
    for (const event of replayEvents) {
        for (const rule of targetRules) {
            if (rule.enabled === false || rule.room_id !== event.room_id) continue;
            const evaluable = (rule.conditions || []).every(condition =>
                Object.prototype.hasOwnProperty.call(event.payload, condition.field));
            if (!evaluable) continue;
            replaySampleCounts.set(rule.rule_id, (replaySampleCounts.get(rule.rule_id) || 0) + 1);
            if (!(rule.conditions || []).every(condition => replayConditionMatches(condition, event.payload))) continue;
            matchedEvents += 1;
            for (const key of actionKeys(rule)) simulatedActions.set(key, (simulatedActions.get(key) || 0) + 1);
        }
    }
    const firstEventAt = replayEvents[0]?.occurred_at_ms || 0;
    const lastEventAt = replayEvents[replayEvents.length - 1]?.occurred_at_ms || firstEventAt;
    const replayHours = replayEvents.length > 1 ? Math.max(1, (lastEventAt - firstEventAt) / (60 * 60 * 1000)) : 1;
    const predictedTriggerRate = matchedEvents / replayHours;
    const highFrequencyActions = [...simulatedActions.entries()].filter(([, count]) => count / replayHours > RULE_CANDIDATE_MAX_TRIGGER_RATE_PER_HOUR);
    const hasSafetyRule = (candidate.rule_package.rules || []).some(rule => rule.rule_type === "safety");
    const safetyRulesValid = (candidate.rule_package.rules || []).every(rule =>
        rule.rule_type !== "safety" || (rule.source === "system" && rule.priority === 1000));
    const habitRulesValid = targetRules.length > 0 && targetRules.every(rule =>
        rule.rule_type === "habit_learning" && rule.source === "habit_learning" && rule.probation?.enabled === true);
    const conflicts = findRuleConflicts(candidate.rule_package);
    const latestReplayAt = replayEvents.reduce((latest, event) => Math.max(latest, event.occurred_at_ms), 0);
    const habitMatches = habit !== null && habit.status === "ACTIVE" &&
        text(habit.pattern?.rule_id, 64) === sourceRuleId && habit.room_id === text(candidate.room_id, 32) &&
        targetRules.length === 1 && targetRules[0].room_id === habit.room_id &&
        candidate.sample_count <= habit.evidence_count && candidate.confidence <= habit.confidence + 0.000001;
    const explicitEvidence = candidate.source?.generated_from_habit === true && habitMatches &&
        evidenceCount >= RULE_CANDIDATE_MIN_SAMPLES &&
        latestEvidenceAt > 0 && latestEvidenceAt <= nowMs && nowMs - latestEvidenceAt <= RULE_CANDIDATE_FRESHNESS_MS;
    const replaySamplesPassed = targetRules.length > 0 && targetRules.every(rule =>
        (replaySampleCounts.get(rule.rule_id) || 0) >= RULE_CANDIDATE_MIN_REPLAY_SAMPLES);
    const gates = {
        sample_count: candidate.sample_count >= RULE_CANDIDATE_MIN_SAMPLES &&
            evidenceCount >= candidate.sample_count && habitMatches,
        confidence: candidate.confidence >= RULE_CANDIDATE_MIN_CONFIDENCE && habitMatches,
        freshness: generatedAt > 0 && generatedAt <= nowMs && nowMs - generatedAt <= RULE_CANDIDATE_FRESHNESS_MS,
        schema: packageValidation.ok,
        explicit_evidence: explicitEvidence,
        conflict_free: conflicts.length === 0,
        resource_budget: packageValidation.ok && Number(packageValidation.resource?.package_bytes) <= Number(packageValidation.resource?.package_budget_bytes),
        historical_replay: replaySamplesPassed,
        safety_review: habitRulesValid && (!hasSafetyRule || safetyRulesValid),
        data_fresh: latestReplayAt > 0 && latestReplayAt <= nowMs && nowMs - latestReplayAt <= RULE_CANDIDATE_DATA_FRESHNESS_MS,
        impact_scope: roomIds.length > 0 && roomIds.length <= 3,
        trigger_rate: predictedTriggerRate <= RULE_CANDIDATE_MAX_TRIGGER_RATE_PER_HOUR && highFrequencyActions.length === 0
    };
    return {
        gates,
        passed: Object.values(gates).every(Boolean),
        details: {
            evidence_count: evidenceCount,
            latest_evidence_at_ms: latestEvidenceAt || null,
            replay_event_count: replayEvents.length,
            replay_samples_by_rule: Object.fromEntries(replaySampleCounts),
            matched_event_count: matchedEvents,
            predicted_trigger_rate_per_hour: Number(predictedTriggerRate.toFixed(2)),
            conflict_count: conflicts.length,
            conflicts: conflicts.slice(0, 8),
            high_frequency_actions: highFrequencyActions.slice(0, 8).map(([action, count]) => ({ action, count })),
            impacted_rooms: roomIds,
            package_bytes: Number(packageValidation.resource?.package_bytes) || null,
            package_budget_bytes: Number(packageValidation.resource?.package_budget_bytes) || null
        }
    };
}

async function evaluateRuleCandidate(dbRun, dbAll, candidateId, body = {}, publishFn = null) {
    const rows = await dbAll("SELECT * FROM home_ai_rule_candidates WHERE candidate_id=?", [text(candidateId, 160)]);
    if (!rows[0]) return { ok: false, code: "RULE_CANDIDATE_NOT_FOUND", error: "rule candidate not found" };
    const candidate = mapRuleCandidate(rows[0]);
    if (["PUBLISHED", "PROBATION", "SUSPENDED"].includes(candidate.status)) {
        return {
            ok: true,
            candidate_id: candidate.candidate_id,
            status: candidate.status,
            gates: candidate.gates,
            gate_details: candidate.gate_details,
            target_rule_ids: [text(candidate.source?.rule_id, 64)].filter(Boolean),
            published: null
        };
    }
    const evaluation = await computeRuleCandidateGates(dbAll, candidate);
    const nowMs = Date.now();
    let status = evaluation.passed ? "READY" : "REJECTED";
    let published = null;
    let publishedVersion = candidate.published_version;
    if (evaluation.passed && body.auto_publish === true && typeof publishFn === "function") {
        const result = await publishFn(candidate.rule_package);
        if (result.ok) {
            status = "PUBLISHED";
            published = result.rule_package;
            publishedVersion = Number(result.rule_package?.version) || null;
        } else {
            status = "SUSPENDED";
            evaluation.gates.publish = false;
            evaluation.publish_error = result.code || "RULE_PUBLISH_FAILED";
        }
    }
    await dbRun(
        `UPDATE home_ai_rule_candidates
         SET status=?,gate_json=?,published_version=?,updated_at_ms=? WHERE candidate_id=?`,
        [status, jsonText(evaluation), publishedVersion, nowMs, candidate.candidate_id]
    );
    const targetRuleIds = [text(candidate.source?.rule_id, 64)].filter(Boolean);
    return {
        ok: true,
        candidate_id: candidate.candidate_id,
        status,
        gates: evaluation.gates,
        gate_details: evaluation.details,
        target_rule_ids: targetRuleIds.length > 0 ? targetRuleIds : (candidate.rule_package.rules || []).map(rule => rule.rule_id),
        published
    };
}

async function listProbationRuns(dbAll, options = {}) {
    const params = [];
    const where = [];
    if (text(options.status, 32)) {
        where.push("status=?");
        params.push(text(options.status, 32).toUpperCase());
    }
    params.push(limit(options.limit, 100, 300));
    const rows = await dbAll(
        `SELECT * FROM home_ai_rule_probation_runs ${where.length ? `WHERE ${where.join(" AND ")}` : ""}
         ORDER BY updated_at_ms DESC LIMIT ?`,
        params
    );
    return rows.map(mapProbation);
}

async function startProbationRuns(dbRun, rulePackage, options = {}) {
    const requestedRuleIds = new Set(Array.isArray(options.rule_ids)
        ? options.rule_ids.map(value => text(value, 64)).filter(Boolean)
        : []);
    const rules = Array.isArray(rulePackage?.rules)
        ? rulePackage.rules.filter(rule => requestedRuleIds.size === 0 || requestedRuleIds.has(text(rule?.rule_id, 64)))
        : [];
    const nowMs = Date.now();
    const durationDays = integer(options.duration_days, 3, 3, 7);
    const endsAtMs = nowMs + durationDays * 24 * 60 * 60 * 1000;
    const runs = [];
    for (const rule of rules) {
        const run = {
            run_id: makeId("probation"),
            candidate_id: text(options.candidate_id, 160),
            rule_id: text(rule?.rule_id, 64),
            package_version: integer(rulePackage?.version, 0, 1, 2147483647),
            gateway_id: text(options.gateway_id, 128),
            status: "RUNNING",
            trigger_count: 0,
            failure_count: 0,
            override_count: 0,
            metrics: {},
            started_at_ms: nowMs,
            ends_at_ms: endsAtMs,
            updated_at_ms: nowMs
        };
        if (!run.rule_id || run.package_version <= 0) continue;
        await dbRun(
            `INSERT INTO home_ai_rule_probation_runs
             (run_id,candidate_id,rule_id,package_version,gateway_id,status,trigger_count,failure_count,override_count,metrics_json,started_at_ms,ends_at_ms,updated_at_ms)
             VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)`,
            [run.run_id, run.candidate_id, run.rule_id, run.package_version, run.gateway_id, run.status,
                0, 0, 0, "{}", nowMs, endsAtMs, nowMs]
        );
        runs.push(run);
    }
    return runs;
}

async function attachRuleCandidateProbation(dbRun, candidateId, runs) {
    const id = text(candidateId, 160);
    const runIds = Array.isArray(runs) ? runs.map(run => text(run?.run_id, 160)).filter(Boolean) : [];
    const status = runIds.length > 0 ? "PROBATION" : "PUBLISHED";
    await dbRun(
        `UPDATE home_ai_rule_candidates
         SET status=?,probation_run_ids_json=?,updated_at_ms=? WHERE candidate_id=?`,
        [status, jsonText(runIds), Date.now(), id]
    );
    return status;
}

async function evaluateProbationRun(dbRun, dbAll, runId, body = {}, rollbackFn = null) {
    const rows = await dbAll("SELECT * FROM home_ai_rule_probation_runs WHERE run_id=?", [text(runId, 160)]);
    if (!rows[0]) return { ok: false, code: "PROBATION_NOT_FOUND", error: "probation run not found" };
    const current = mapProbation(rows[0]);
    if (["ACTIVE", "ROLLED_BACK", "FAILED", "EXPIRED"].includes(current.status)) {
        return { ok: true, run_id: current.run_id, status: current.status, failure_rate: current.metrics?.failure_rate || 0, rollback: null };
    }
    const metrics = body.metrics && typeof body.metrics === "object" ? body.metrics : current.metrics;
    const triggerCount = integer(metrics.trigger_count, current.trigger_count, 0, 1000000);
    const failureCount = integer(metrics.failure_count, current.failure_count, 0, 1000000);
    const overrideCount = integer(metrics.override_count, current.override_count, 0, 1000000);
    const failureRate = triggerCount > 0 ? failureCount / triggerCount : 0;
    const requestedNowMs = Number(body.now_ms);
    const nowMs = Number.isFinite(requestedNowMs) && requestedNowMs > 0 ? requestedNowMs : Date.now();
    const observationMs = Math.max(1, nowMs - current.started_at_ms);
    const triggerRatePerHour = triggerCount * PROBATION_TRIGGER_RATE_WINDOW_MS / observationMs;
    const rapidToggleCount = integer(metrics.rapid_toggle_count, 0, 0, 1000000);
    const conflictCount = integer(metrics.conflict_count, 0, 0, 1000000);
    const sensorUnreliableCount = integer(metrics.sensor_unreliable_count, 0, 0, 1000000);
    const repeatedPromptCount = integer(metrics.repeated_prompt_count, 0, 0, 1000000);
    let status = current.status;
    let rollback = null;
    const safetyViolation = metrics.safety_violation === true;
    const highTriggerRate = observationMs >= PROBATION_TRIGGER_RATE_WINDOW_MS &&
        triggerCount >= 3 && triggerRatePerHour > RULE_CANDIDATE_MAX_TRIGGER_RATE_PER_HOUR;
    const anomaly = rapidToggleCount >= PROBATION_MAX_ANOMALIES ||
        conflictCount >= PROBATION_MAX_ANOMALIES ||
        sensorUnreliableCount >= PROBATION_MAX_ANOMALIES ||
        repeatedPromptCount >= PROBATION_MAX_ANOMALIES || highTriggerRate;
    if (safetyViolation || anomaly ||
        (triggerCount >= 3 && failureRate > PROBATION_MAX_FAILURE_RATE) ||
        overrideCount >= PROBATION_MAX_OVERRIDES) {
        status = "ROLLED_BACK";
        if (typeof rollbackFn === "function") {
            const rollbackRequest = {
                reason: safetyViolation ? "probation_safety_gate_failed" : anomaly ? "probation_anomaly_gate_failed" : "probation_gate_failed",
                expected_active_version: current.package_version
            };
            try {
                const targets = await dbAll(
                    `SELECT package_version FROM home_ai_rule_packages
                     WHERE package_version<? ORDER BY package_version DESC LIMIT 1`,
                    [current.package_version]
                );
                if (targets[0]) rollbackRequest.target_version = Number(targets[0].package_version) || 0;
            } catch (_) {
                /* The rollback service still performs its own active-version guard. */
            }
            rollback = await rollbackFn(rollbackRequest);
            if (!rollback?.ok) status = "FAILED";
        }
    } else if (nowMs >= current.ends_at_ms && triggerCount >= PROBATION_MIN_TRIGGERS) {
        status = "ACTIVE";
    } else if (nowMs >= current.ends_at_ms) {
        status = "EXPIRED";
    } else {
        status = "RUNNING";
    }
    const storedMetrics = {
        ...metrics,
        failure_rate: failureRate,
        trigger_rate_per_hour: triggerRatePerHour,
        rapid_toggle_count: rapidToggleCount,
        conflict_count: conflictCount,
        sensor_unreliable_count: sensorUnreliableCount,
        repeated_prompt_count: repeatedPromptCount
    };
    await dbRun(
        `UPDATE home_ai_rule_probation_runs
         SET status=?,trigger_count=?,failure_count=?,override_count=?,metrics_json=?,updated_at_ms=?
         WHERE run_id=?`,
        [status, triggerCount, failureCount, overrideCount, jsonText(storedMetrics), nowMs, current.run_id]
    );
    if (current.candidate_id && ["ROLLED_BACK", "FAILED", "EXPIRED"].includes(status)) {
        await dbRun("UPDATE home_ai_rule_candidates SET status=?,updated_at_ms=? WHERE candidate_id=?", ["SUSPENDED", nowMs, current.candidate_id]);
    }
    return { ok: true, run_id: current.run_id, status, failure_rate: failureRate, trigger_rate_per_hour: triggerRatePerHour, rollback };
}

async function evaluateDueProbationRuns(dbRun, dbAll, options = {})
{
    const nowMs = Number(options.now_ms) > 0 ? Number(options.now_ms) : Date.now();
    const batchLimit = integer(options.limit, 100, 1, 100);
    const rows = await dbAll(
        `SELECT run_id FROM home_ai_rule_probation_runs
         WHERE status='RUNNING' AND ends_at_ms<=?
         ORDER BY ends_at_ms ASC, run_id ASC LIMIT ?`,
        [nowMs, batchLimit]
    );
    const updated = [];
    const errors = [];
    for (const row of rows) {
        try {
            updated.push(await evaluateProbationRun(
                dbRun,
                dbAll,
                row.run_id,
                { now_ms: nowMs },
                options.rollbackFn
            ));
        } catch (error) {
            errors.push({
                run_id: row.run_id,
                code: "PROBATION_SWEEP_FAILED",
                message: text(error?.message, 240) || "probation sweep failed"
            });
        }
    }
    return {
        ok: errors.length === 0,
        evaluated: updated.length,
        updated,
        errors,
        now_ms: nowMs,
        limit: batchLimit
    };
}

async function recordProbationEvent(dbRun, dbAll, event, rollbackFn = null) {
    const eventType = text(event?.event_type, 48);
    const payload = event?.payload && typeof event.payload === "object" ? event.payload : {};
    const ruleId = text(payload.rule_id, 64);
    if (!ruleId || !["decision", "suppressed_action"].includes(eventType)) {
        return { ok: true, updated: [] };
    }
    const rows = await dbAll(
        "SELECT * FROM home_ai_rule_probation_runs WHERE rule_id=? AND status='RUNNING'",
        [ruleId]
    );
    const occurredAtMs = Number(event.occurred_at_ms) || Date.now();
    const updated = [];
    for (const row of rows) {
        const current = mapProbation(row);
        const metrics = { ...current.metrics };
        if (eventType === "decision") {
            metrics.trigger_count = current.trigger_count + 1;
            const executionResult = text(payload.execution_result, 48);
            metrics.failure_count = current.failure_count + (["rejected", "not_executed"].includes(executionResult) ? 1 : 0);
            const action = text(payload.action, 32);
            const lastAction = text(metrics.last_action, 32);
            const lastActionAtMs = Number(metrics.last_action_at_ms) || 0;
            const opposite = new Set([action, lastAction]).size > 1 &&
                [action, lastAction].every(value => ["turn_on", "turn_off"].includes(value));
            metrics.rapid_toggle_count = integer(metrics.rapid_toggle_count, 0, 0, 1000000) +
                (opposite && occurredAtMs >= lastActionAtMs && occurredAtMs - lastActionAtMs <= 10 * 60 * 1000 ? 1 : 0);
            if (action === "play_prompt") {
                const lastPromptAtMs = Number(metrics.last_prompt_at_ms) || 0;
                metrics.repeated_prompt_count = integer(metrics.repeated_prompt_count, 0, 0, 1000000) +
                    (occurredAtMs >= lastPromptAtMs && occurredAtMs - lastPromptAtMs <= 5 * 60 * 1000 ? 1 : 0);
                metrics.last_prompt_at_ms = occurredAtMs;
            }
            metrics.last_action = action;
            metrics.last_action_at_ms = occurredAtMs;
        } else {
            metrics.conflict_count = integer(metrics.conflict_count, 0, 0, 1000000) + 1;
        }
        const reason = text(payload.reason, 160).toLowerCase();
        if (reason.includes("sensor") || reason.includes("stale") || reason.includes("unreliable")) {
            metrics.sensor_unreliable_count = integer(metrics.sensor_unreliable_count, 0, 0, 1000000) + 1;
        }
        updated.push(await evaluateProbationRun(dbRun, dbAll, current.run_id, { metrics }, rollbackFn));
    }
    return { ok: true, updated };
}

async function recordProbationFeedback(dbRun, dbAll, feedback, rollbackFn = null) {
    const ruleId = text(feedback?.rule_id, 64);
    if (!ruleId || typeof dbAll !== "function") return { ok: true, updated: [] };
    const rows = await dbAll(
        "SELECT * FROM home_ai_rule_probation_runs WHERE rule_id=? AND status='RUNNING'",
        [ruleId]
    );
    const negative = ["rejected", "cancelled", "reverted", "manual_override"].includes(feedback.feedback_type);
    const updated = [];
    for (const row of rows) {
        const current = mapProbation(row);
        const metrics = { ...current.metrics,
            trigger_count: current.trigger_count + 1,
            failure_count: current.failure_count + (negative ? 1 : 0),
            override_count: current.override_count + (["manual_override", "rejected", "cancelled", "reverted"].includes(feedback.feedback_type) ? 1 : 0),
            last_feedback_type: feedback.feedback_type
        };
        updated.push(await evaluateProbationRun(dbRun, dbAll, current.run_id, { metrics }, rollbackFn));
    }
    return { ok: true, updated };
}

module.exports = {
    CANDIDATE_STATUSES,
    FEEDBACK_TYPES,
    MEMORY_STATUSES,
    PROBATION_STATUSES,
    attachRuleCandidateProbation,
    createMemoryCandidate,
    createRuleCandidate,
    deleteMemoryCandidate,
    evaluateProbationRun,
    evaluateDueProbationRuns,
    evaluateRuleCandidate,
    listFeedback,
    listHabits,
    listMemoryCandidates,
    listProbationRuns,
    listRuleCandidates,
    mapProbation,
    recordProbationFeedback,
    recordProbationEvent,
    recordHabitEvidence,
    startProbationRuns,
    updateMemoryCandidate
};
