const crypto = require("crypto");
const {
    AUDIT_POLICY,
    PUBLIC_SCOPES,
    getPublicScopes,
    policiesForScope,
    qualify,
    quoteIdentifier
} = require("../db/userDataDeletion");

const DELETE_MODES = new Set(["soft_delete", "hard_delete"]);
const MAX_REASON_LENGTH = 500;
const MAX_REQUESTED_BY_LENGTH = 120;
const DEFAULT_RUN_LIMIT = 50;
const MAX_RUN_LIMIT = 200;
const RUN_REQUEST_TYPES = new Set(["preview", "delete"]);

function nowIso() {
    return new Date().toISOString();
}

function makeRunId() {
    const value = typeof crypto.randomUUID === "function"
        ? crypto.randomUUID()
        : crypto.randomBytes(16).toString("hex");
    return `deletion_${value}`;
}

function boundedString(value, maxLength, fallback = "") {
    if (value === undefined || value === null) {
        return fallback;
    }

    const text = String(value).trim();
    return text.length > maxLength ? text.slice(0, maxLength) : text;
}

function parseBoolean(value) {
    return value === true || value === "true" || value === 1 || value === "1";
}

function readLimit(value, fallback = DEFAULT_RUN_LIMIT, max = MAX_RUN_LIMIT) {
    const numeric = Number.parseInt(value, 10);
    if (!Number.isFinite(numeric) || numeric <= 0) {
        return fallback;
    }

    return Math.min(numeric, max);
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

function invalidRequest(code, error, statusCode = 400) {
    return {
        ok: false,
        statusCode,
        code,
        error
    };
}

function readDeleteInput(input = {}, { requireConfirm = false } = {}) {
    const scope = boundedString(input.scope, 80);
    if (!PUBLIC_SCOPES[scope]) {
        return invalidRequest(
            "USER_DATA_SCOPE_INVALID",
            `scope must be one of ${getPublicScopes().join(", ")}`
        );
    }

    const mode = boundedString(input.mode || "soft_delete", 40);
    if (!DELETE_MODES.has(mode)) {
        return invalidRequest("USER_DATA_DELETE_MODE_INVALID", "mode must be soft_delete or hard_delete");
    }

    if (requireConfirm && input.confirm !== "DELETE") {
        return invalidRequest("USER_DATA_DELETE_CONFIRM_REQUIRED", "confirm must be DELETE", 409);
    }

    return {
        ok: true,
        scope,
        mode,
        reason: boundedString(input.reason || "user_request", MAX_REASON_LENGTH, "user_request"),
        requestedBy: boundedString(input.requested_by || "api", MAX_REQUESTED_BY_LENGTH, "api"),
        includeAuditLogs: parseBoolean(input.include_audit_logs)
    };
}

async function tableExists(dbAll, tableName) {
    const rows = await dbAll(
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=? LIMIT 1",
        [tableName]
    );
    return rows.length > 0;
}

function currentDataPredicate(policy, alias = "", options = {}) {
    const predicates = [`${qualify(alias, "deleted_at")} IS NULL`];
    const params = [];
    if (policy.rowFilterSql) {
        predicates.push(policy.rowFilterSql);
    }
    if (options.excludeRunId && policy.table === AUDIT_POLICY.table) {
        predicates.push(`${qualify(alias, "run_id")} <> ?`);
        params.push(options.excludeRunId);
    }

    return {
        sql: predicates.join(" AND "),
        params
    };
}

function deletedDataPredicate(policy, alias = "") {
    const predicates = [`${qualify(alias, "deleted_at")} IS NOT NULL`];
    if (policy.rowFilterSql) {
        predicates.push(policy.rowFilterSql);
    }

    return {
        sql: predicates.join(" AND "),
        params: []
    };
}

function hardDeletePredicate(policy, alias = "", options = {}) {
    const predicates = [];
    const params = [];
    if (policy.rowFilterSql) {
        predicates.push(policy.rowFilterSql);
    }
    if (options.excludeRunId && policy.table === AUDIT_POLICY.table) {
        predicates.push(`${qualify(alias, "run_id")} <> ?`);
        params.push(options.excludeRunId);
    }

    return {
        sql: predicates.length ? predicates.join(" AND ") : "1=1",
        params
    };
}

function dateExpression(policy) {
    if (policy.fallbackDateColumn) {
        return `COALESCE(${quoteIdentifier(policy.dateColumn)}, ${quoteIdentifier(policy.fallbackDateColumn)})`;
    }

    return quoteIdentifier(policy.dateColumn || "id");
}

async function readPolicyStats(dbAll, policy) {
    if (!await tableExists(dbAll, policy.table)) {
        return {
            table: policy.table,
            count: 0,
            date_range: {
                from: "",
                to: ""
            },
            last_updated_at: "",
            last_deleted_at: "",
            exists: false
        };
    }

    const current = currentDataPredicate(policy);
    const activeRows = await dbAll(
        `SELECT
            COUNT(*) AS count,
            MIN(${dateExpression(policy)}) AS date_from,
            MAX(${dateExpression(policy)}) AS date_to,
            MAX(${quoteIdentifier(policy.updatedColumn || "updated_at")}) AS last_updated_at
        FROM ${quoteIdentifier(policy.table)}
        WHERE ${current.sql}`,
        current.params
    );
    const deleted = deletedDataPredicate(policy);
    const deletedRows = await dbAll(
        `SELECT MAX(deleted_at) AS last_deleted_at
        FROM ${quoteIdentifier(policy.table)}
        WHERE ${deleted.sql}`,
        deleted.params
    );
    const row = activeRows[0] || {};
    return {
        table: policy.table,
        count: Number(row.count) || 0,
        date_range: {
            from: row.date_from || "",
            to: row.date_to || ""
        },
        last_updated_at: row.last_updated_at || "",
        last_deleted_at: deletedRows[0]?.last_deleted_at || "",
        exists: true
    };
}

function mergeDateRange(stats) {
    const values = stats
        .flatMap(stat => [stat.date_range.from, stat.date_range.to])
        .filter(Boolean)
        .sort();
    return {
        from: values[0] || "",
        to: values[values.length - 1] || ""
    };
}

function maxString(values) {
    return values.filter(Boolean).sort().pop() || "";
}

function maxDangerLevel(values) {
    const order = {
        low: 1,
        medium: 2,
        high: 3,
        critical: 4
    };
    return values.reduce((best, value) => {
        const level = order[value] || 0;
        return level > (order[best] || 0) ? value : best;
    }, "low");
}

async function getUserDataSummary(dbAll) {
    const scopes = [];
    for (const scope of getPublicScopes()) {
        const scopeInfo = PUBLIC_SCOPES[scope];
        const policies = policiesForScope(scope, false);
        const tableStats = [];
        for (const policy of policies) {
            tableStats.push(await readPolicyStats(dbAll, policy));
        }

        scopes.push({
            scope,
            display_name: scopeInfo.displayName,
            description: scopeInfo.description,
            count: tableStats.reduce((sum, stat) => sum + stat.count, 0),
            date_range: mergeDateRange(tableStats),
            last_updated_at: maxString(tableStats.map(stat => stat.last_updated_at)),
            last_deleted_at: maxString(tableStats.map(stat => stat.last_deleted_at)),
            supports_soft_delete: true,
            supports_hard_delete: true,
            danger_level: scopeInfo.dangerLevel,
            tables: tableStats
        });
    }

    return {
        ok: true,
        scopes
    };
}

async function countAffectedRows(dbAll, policies, mode) {
    const counts = {};
    const tables = [];
    for (const policy of policies) {
        if (!await tableExists(dbAll, policy.table)) {
            counts[policy.table] = 0;
            tables.push({
                table: policy.table,
                count: 0,
                danger_level: policy.dangerLevel,
                exists: false
            });
            continue;
        }

        const predicate = mode === "hard_delete"
            ? hardDeletePredicate(policy)
            : currentDataPredicate(policy);
        const rows = await dbAll(
            `SELECT COUNT(*) AS count FROM ${quoteIdentifier(policy.table)} WHERE ${predicate.sql}`,
            predicate.params
        );
        const count = Number(rows[0]?.count) || 0;
        counts[policy.table] = count;
        tables.push({
            table: policy.table,
            count,
            danger_level: policy.dangerLevel,
            exists: true
        });
    }

    return {
        counts,
        tables
    };
}

async function previewUserDataDelete(dbRun, dbAll, input = {}) {
    if (typeof dbAll !== "function") {
        input = dbAll || {};
        dbAll = dbRun;
        dbRun = null;
    }

    const parsed = readDeleteInput(input);
    if (!parsed.ok) {
        return parsed;
    }

    const policies = policiesForScope(parsed.scope, parsed.includeAuditLogs);
    const affected = await countAffectedRows(dbAll, policies, parsed.mode);
    const runId = typeof dbRun === "function" ? makeRunId() : "";
    const timestamp = nowIso();
    if (typeof dbRun === "function") {
        try {
            await insertDeletionRun(dbRun, {
                runId,
                scope: parsed.scope,
                mode: parsed.mode,
                requestType: "preview",
                reason: parsed.reason,
                requestedBy: parsed.requestedBy,
                includeAuditLogs: parsed.includeAuditLogs,
                previewCounts: affected.counts,
                affectedCounts: affected.counts,
                startedAt: timestamp,
                completedAt: timestamp,
                status: "completed"
            });
        } catch (error) {
            return invalidRequest(
                "USER_DATA_PREVIEW_AUDIT_FAILED",
                error?.message || "user data preview audit failed",
                500
            );
        }
    }

    return {
        ok: true,
        ...(runId ? { run_id: runId } : {}),
        scope: parsed.scope,
        mode: parsed.mode,
        request_type: "preview",
        include_audit_logs: parsed.includeAuditLogs,
        danger_level: parsed.includeAuditLogs
            ? "critical"
            : maxDangerLevel(policies.map(policy => policy.dangerLevel)),
        required_confirm: "DELETE",
        protected_tables: [
            "sqlite_sequence",
            "device_capabilities"
        ],
        affected_tables: affected.tables,
        affected_counts: affected.counts
    };
}

async function insertDeletionRun(dbRun, record) {
    await dbRun(
        `INSERT INTO data_deletion_runs
        (run_id,scope,mode,request_type,reason,requested_by,include_audit_logs,preview_counts_json,affected_counts_json,started_at,completed_at,status,error_message,created_at,updated_at)
        VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)`,
        [
            record.runId,
            record.scope,
            record.mode,
            record.requestType || "delete",
            record.reason,
            record.requestedBy,
            record.includeAuditLogs ? 1 : 0,
            JSON.stringify(record.previewCounts || {}),
            JSON.stringify(record.affectedCounts || {}),
            record.startedAt,
            record.completedAt || null,
            record.status,
            record.errorMessage || "",
            record.createdAt || record.startedAt,
            record.completedAt || record.startedAt
        ]
    );
}

async function updateDeletionRun(dbRun, record) {
    await dbRun(
        `UPDATE data_deletion_runs
        SET affected_counts_json=?,
            completed_at=?,
            status=?,
            error_message=?,
            updated_at=?
        WHERE run_id=?`,
        [
            JSON.stringify(record.affectedCounts || {}),
            record.completedAt || null,
            record.status,
            record.errorMessage || "",
            record.completedAt || nowIso(),
            record.runId
        ]
    );
}

async function runTransaction(dbRun, fn) {
    await dbRun("BEGIN IMMEDIATE");
    try {
        const value = await fn();
        await dbRun("COMMIT");
        return value;
    } catch (error) {
        try {
            await dbRun("ROLLBACK");
        } catch (_) {
            // Keep the original error; rollback failure is a secondary symptom.
        }
        throw error;
    }
}

function deleteSqlForPolicy(policy, mode, options = {}) {
    if (mode === "hard_delete") {
        const predicate = hardDeletePredicate(policy, "", options);
        return {
            sql: `DELETE FROM ${quoteIdentifier(policy.table)} WHERE ${predicate.sql}`,
            params: predicate.params
        };
    }

    const current = currentDataPredicate(policy, "", options);
    const updates = [
        "deleted_at=?",
        "delete_reason=?",
        "updated_at=?"
    ];

    return {
        sql: `UPDATE ${quoteIdentifier(policy.table)} SET ${updates.join(", ")} WHERE ${current.sql}`,
        paramsPrefix: true,
        params: current.params
    };
}

async function applyDeletion(dbRun, dbAll, policies, mode, reason, options = {}) {
    const timestamp = nowIso();
    const affectedCounts = {};
    for (const policy of policies) {
        if (!await tableExists(dbAll, policy.table)) {
            affectedCounts[policy.table] = 0;
            continue;
        }

        const statement = deleteSqlForPolicy(policy, mode, options);
        const params = statement.paramsPrefix
            ? [timestamp, reason, timestamp, ...statement.params]
            : statement.params;
        const result = await dbRun(statement.sql, params);
        affectedCounts[policy.table] = Number(result.changes) || 0;
    }

    return affectedCounts;
}

async function executeUserDataDelete(dbRun, dbAll, input = {}) {
    const parsed = readDeleteInput(input, {
        requireConfirm: true
    });
    if (!parsed.ok) {
        return parsed;
    }

    const policies = policiesForScope(parsed.scope, parsed.includeAuditLogs);
    const preview = await countAffectedRows(dbAll, policies, parsed.mode);
    const runId = makeRunId();
    const startedAt = nowIso();

    try {
        const affectedCounts = await runTransaction(dbRun, async () => {
            await insertDeletionRun(dbRun, {
                runId,
                scope: parsed.scope,
                mode: parsed.mode,
                requestType: "delete",
                reason: parsed.reason,
                requestedBy: parsed.requestedBy,
                includeAuditLogs: parsed.includeAuditLogs,
                previewCounts: preview.counts,
                affectedCounts: {},
                startedAt,
                status: "running"
            });
            const counts = await applyDeletion(dbRun, dbAll, policies, parsed.mode, parsed.reason, {
                excludeRunId: runId
            });
            await updateDeletionRun(dbRun, {
                runId,
                affectedCounts: counts,
                completedAt: nowIso(),
                status: "completed"
            });
            return counts;
        });

        return {
            ok: true,
            run_id: runId,
            scope: parsed.scope,
            mode: parsed.mode,
            status: "completed",
            affected_counts: affectedCounts,
            protected_tables: [
                "sqlite_sequence",
                "device_capabilities"
            ],
            server_time_ms: Date.now()
        };
    } catch (error) {
        await insertDeletionRun(dbRun, {
            runId,
            scope: parsed.scope,
            mode: parsed.mode,
            requestType: "delete",
            reason: parsed.reason,
            requestedBy: parsed.requestedBy,
            includeAuditLogs: parsed.includeAuditLogs,
            previewCounts: preview.counts,
            affectedCounts: {},
            startedAt,
            completedAt: nowIso(),
            status: "failed",
            errorMessage: boundedString(error?.message || "delete failed", 1000)
        });

        return {
            ok: false,
            statusCode: 500,
            code: "USER_DATA_DELETE_FAILED",
            error: "user data deletion failed",
            run_id: runId
        };
    }
}

async function listDeletionRuns(dbAll, filters = {}) {
    const where = [];
    const params = [];
    const scope = boundedString(filters.scope, 80);
    const mode = boundedString(filters.mode, 40);
    const status = boundedString(filters.status, 40);
    const requestType = boundedString(filters.request_type, 40);
    if (!parseBoolean(filters.include_deleted)) {
        where.push("deleted_at IS NULL");
    }
    if (scope) {
        where.push("scope=?");
        params.push(scope);
    }
    if (mode) {
        where.push("mode=?");
        params.push(mode);
    }
    if (status) {
        where.push("status=?");
        params.push(status);
    }
    if (requestType) {
        if (!RUN_REQUEST_TYPES.has(requestType)) {
            return invalidRequest("USER_DATA_RUN_REQUEST_TYPE_INVALID", "request_type must be preview or delete");
        }
        if (requestType === "delete") {
            where.push("(request_type=? OR request_type IS NULL OR request_type='')");
            params.push(requestType);
        } else {
            where.push("request_type=?");
            params.push(requestType);
        }
    }
    params.push(readLimit(filters.limit));

    const rows = await dbAll(
        `SELECT * FROM ${quoteIdentifier(AUDIT_POLICY.table)}
        ${where.length ? `WHERE ${where.join(" AND ")}` : ""}
        ORDER BY started_at DESC, id DESC
        LIMIT ?`,
        params
    );

    return {
        ok: true,
        runs: rows.map(row => ({
            run_id: row.run_id,
            scope: row.scope,
            mode: row.mode,
            request_type: row.request_type || "delete",
            reason: row.reason || "",
            requested_by: row.requested_by || "",
            include_audit_logs: Boolean(Number(row.include_audit_logs)),
            preview_counts: parseJson(row.preview_counts_json, {}),
            affected_counts: parseJson(row.affected_counts_json, {}),
            started_at: row.started_at,
            completed_at: row.completed_at || "",
            created_at: row.created_at || row.started_at,
            status: row.status,
            error_message: row.error_message || "",
            deleted_at: row.deleted_at || ""
        }))
    };
}

function isUserDataAdminAuthorized(req) {
    const requiredToken = process.env.USER_DATA_DELETE_TOKEN || process.env.ADMIN_TOKEN || "";
    if (!requiredToken) {
        return false;
    }

    const headerToken = req.get("X-Admin-Token") || req.get("X-User-Data-Token") || "";
    const auth = req.get("Authorization") || "";
    const bearerToken = auth.startsWith("Bearer ") ? auth.slice("Bearer ".length).trim() : "";
    return headerToken === requiredToken || bearerToken === requiredToken;
}

function requireUserDataAdmin(req, res, next) {
    if (!process.env.USER_DATA_DELETE_TOKEN && !process.env.ADMIN_TOKEN) {
        res.status(503).json({
            ok: false,
            code: "USER_DATA_ADMIN_TOKEN_NOT_CONFIGURED",
            error: "USER_DATA_DELETE_TOKEN or ADMIN_TOKEN must be configured"
        });
        return;
    }

    if (isUserDataAdminAuthorized(req)) {
        next();
        return;
    }

    res.status(401).json({
        ok: false,
        code: "USER_DATA_ADMIN_REQUIRED",
        error: "admin token is required"
    });
}

module.exports = {
    executeUserDataDelete,
    getUserDataSummary,
    listDeletionRuns,
    previewUserDataDelete,
    requireUserDataAdmin
};
