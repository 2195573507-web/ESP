const {
    ensureTableColumns,
    ensureUniqueIndex
} = require("./migrations");

const INACTIVE_STATUSES = ["deleted", "inactive", "archived"];

const DATA_DELETION_RUN_COLUMNS = [
    { name: "id", type: "INTEGER PRIMARY KEY AUTOINCREMENT" },
    { name: "run_id", type: "TEXT NOT NULL UNIQUE" },
    { name: "scope", type: "TEXT NOT NULL" },
    { name: "mode", type: "TEXT NOT NULL" },
    { name: "request_type", type: "TEXT NOT NULL DEFAULT 'delete'", addType: "TEXT" },
    { name: "reason", type: "TEXT" },
    { name: "requested_by", type: "TEXT" },
    { name: "include_audit_logs", type: "INTEGER NOT NULL DEFAULT 0", addType: "INTEGER" },
    { name: "preview_counts_json", type: "TEXT NOT NULL DEFAULT '{}'", addType: "TEXT" },
    { name: "affected_counts_json", type: "TEXT NOT NULL DEFAULT '{}'", addType: "TEXT" },
    { name: "started_at", type: "TEXT NOT NULL" },
    { name: "completed_at", type: "TEXT" },
    { name: "status", type: "TEXT NOT NULL DEFAULT 'running'" },
    { name: "error_message", type: "TEXT" },
    { name: "deleted_at", type: "TEXT" },
    { name: "delete_reason", type: "TEXT" },
    { name: "created_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))", addType: "TEXT" },
    { name: "updated_at", type: "TEXT NOT NULL DEFAULT (datetime('now'))", addType: "TEXT" }
];

const SOFT_DELETE_COLUMNS = [
    { name: "deleted_at", type: "TEXT" },
    { name: "delete_reason", type: "TEXT" },
    { name: "updated_at", type: "TEXT" }
];

const DAILY_MEMORY_DELETION_COLUMNS = [
    { name: "memory_type", type: "TEXT NOT NULL DEFAULT 'daily_summary'", addType: "TEXT NOT NULL DEFAULT 'daily_summary'" },
    { name: "window_start", type: "TEXT" },
    { name: "window_end", type: "TEXT" },
    { name: "sample_count", type: "INTEGER NOT NULL DEFAULT 0", addType: "INTEGER NOT NULL DEFAULT 0" },
    { name: "evidence_json", type: "TEXT" }
];

function columnSql(columns) {
    return columns.map(column => `${column.name} ${column.type}`).join(",\n            ");
}

function addableColumns(columns) {
    return columns.filter(column => !column.type.includes("PRIMARY KEY"));
}

function quoteIdentifier(identifier) {
    const value = String(identifier || "");
    if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(value)) {
        throw new Error(`Invalid SQLite identifier: ${value}`);
    }

    return `"${value}"`;
}

function qualify(alias, columnName) {
    return alias ? `${quoteIdentifier(alias)}.${quoteIdentifier(columnName)}` : quoteIdentifier(columnName);
}

async function tableExists(dbAll, tableName) {
    const rows = await dbAll(
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=? LIMIT 1",
        [tableName]
    );
    return rows.length > 0;
}

function notDeletedPredicate(alias = "") {
    return `${qualify(alias, "deleted_at")} IS NULL`;
}

function activeStatusPredicate(alias = "", statusColumn = "status") {
    const placeholders = INACTIVE_STATUSES.map(() => "?").join(",");
    return {
        sql: `COALESCE(${qualify(alias, statusColumn)}, '') NOT IN (${placeholders})`,
        params: [...INACTIVE_STATUSES]
    };
}

function activeMemoryLevelPredicate(alias = "") {
    return `${qualify(alias, "memory_level")} <> 'archived'`;
}

const TABLE_POLICIES = [
    {
        table: "daily_memory",
        scope: "summaries",
        displayName: "每日/每周总结",
        description: "daily_memory 中的 daily_summary 与 weekly_summary 记录",
        dangerLevel: "medium",
        dateColumn: "memory_date",
        updatedColumn: "updated_at",
        statusColumn: "status",
        rowFilterSql: "memory_type IN ('daily_summary','weekly_summary')"
    },
    {
        table: "long_term_profile",
        scope: "profiles",
        displayName: "长期用户画像",
        description: "long_term_profile 候选或 active 画像",
        dangerLevel: "high",
        dateColumn: "created_at",
        updatedColumn: "updated_at",
        statusColumn: "status"
    },
    {
        table: "environment_profile",
        scope: "profiles",
        displayName: "环境画像",
        description: "environment_profile 环境候选画像",
        dangerLevel: "high",
        dateColumn: "created_at",
        updatedColumn: "updated_at",
        statusColumn: "status"
    },
    {
        table: "experience_memory",
        scope: "profiles",
        displayName: "经验记忆",
        description: "experience_memory 候选经验记忆",
        dangerLevel: "high",
        dateColumn: "created_at",
        updatedColumn: "updated_at",
        statusColumn: "status"
    },
    {
        table: "relation_memory",
        scope: "profiles",
        displayName: "关系记忆",
        description: "relation_memory 候选关系记忆",
        dangerLevel: "high",
        dateColumn: "created_at",
        updatedColumn: "updated_at",
        statusColumn: "status"
    },
    {
        table: "memory_corrections",
        scope: "memory_corrections",
        displayName: "记忆纠错",
        description: "memory_corrections 用户纠错记录",
        dangerLevel: "medium",
        dateColumn: "created_at",
        updatedColumn: "updated_at",
        statusColumn: "status"
    },
    {
        table: "conversation_turns",
        scope: "conversations",
        displayName: "对话轮次",
        description: "conversation_turns 对话输入、回复和结构化输出",
        dangerLevel: "high",
        dateColumn: "created_at",
        updatedColumn: "updated_at",
        activeExtraPredicate: activeMemoryLevelPredicate
    },
    {
        table: "asr_records",
        scope: "conversations",
        displayName: "Legacy ASR 记录",
        description: "asr_records 历史语音识别文本",
        dangerLevel: "high",
        dateColumn: "timestamp",
        updatedColumn: "updated_at"
    },
    {
        table: "llm_records",
        scope: "conversations",
        displayName: "Legacy LLM 记录",
        description: "llm_records prompt 与回复文本",
        dangerLevel: "high",
        dateColumn: "timestamp",
        updatedColumn: "updated_at"
    },
    {
        table: "sensor_records",
        scope: "device_history",
        displayName: "传感器记录",
        description: "sensor_records BME690 与设备上传数据",
        dangerLevel: "high",
        dateColumn: "server_recv_ms",
        fallbackDateColumn: "timestamp",
        updatedColumn: "updated_at"
    },
    {
        table: "voice_turns",
        scope: "device_history",
        displayName: "语音 turn 诊断",
        description: "voice_turns 请求、错误、耗时和字节数",
        dangerLevel: "high",
        dateColumn: "created_at",
        updatedColumn: "updated_at"
    },
    {
        table: "command_queue",
        scope: "device_history",
        displayName: "命令队列历史",
        description: "command_queue 命令状态、结果和错误记录",
        dangerLevel: "high",
        dateColumn: "created_at",
        updatedColumn: "updated_at"
    },
    {
        table: "device_status",
        scope: "device_history",
        displayName: "设备状态快照",
        description: "device_status 最近设备状态",
        dangerLevel: "medium",
        dateColumn: "last_seen_iso",
        updatedColumn: "updated_at"
    },
    {
        table: "device_module_status",
        scope: "device_history",
        displayName: "模块状态快照",
        description: "device_module_status 最近模块状态",
        dangerLevel: "medium",
        dateColumn: "last_seen_iso",
        updatedColumn: "updated_at"
    },
    {
        table: "dashboard_snapshots",
        scope: "device_history",
        displayName: "Dashboard 快照",
        description: "dashboard_snapshots S3 网关快照历史",
        dangerLevel: "medium",
        dateColumn: "server_recv_ms",
        updatedColumn: "updated_at"
    },
    {
        table: "event_logs",
        scope: "device_history",
        displayName: "事件日志",
        description: "event_logs 设备、语音、命令、告警和系统事件",
        dangerLevel: "medium",
        dateColumn: "server_recv_ms",
        fallbackDateColumn: "created_at",
        updatedColumn: "updated_at"
    },
    {
        table: "csi_behavior_events",
        scope: "device_history",
        displayName: "CSI 行为事件",
        description: "csi_behavior_events 预留行为事件",
        dangerLevel: "medium",
        dateColumn: "occurred_at",
        updatedColumn: "updated_at"
    },
    {
        table: "lcd_status",
        scope: "device_history",
        displayName: "LCD 状态快照",
        description: "lcd_status 屏幕状态快照",
        dangerLevel: "medium",
        dateColumn: "updated_at",
        updatedColumn: "updated_at"
    },
    {
        table: "reminder_rules",
        scope: "device_history",
        displayName: "提醒规则",
        description: "reminder_rules 主动提醒规则",
        dangerLevel: "high",
        dateColumn: "created_at",
        updatedColumn: "updated_at",
        statusColumn: "status"
    },
    {
        table: "reminder_records",
        scope: "device_history",
        displayName: "提醒事件",
        description: "reminder_records 提醒运行记录",
        dangerLevel: "medium",
        dateColumn: "created_at",
        updatedColumn: "updated_at",
        statusColumn: "status"
    },
    {
        table: "emergency_events",
        scope: "device_history",
        displayName: "紧急事件",
        description: "emergency_events 紧急事件记录",
        dangerLevel: "critical",
        dateColumn: "created_at",
        updatedColumn: "updated_at",
        statusColumn: "status"
    },
    {
        table: "memory_job_runs",
        scope: "jobs",
        displayName: "记忆 job 运行记录",
        description: "memory_job_runs daily/weekly job 运行记录",
        dangerLevel: "medium",
        dateColumn: "created_at",
        updatedColumn: "updated_at",
        statusColumn: "status"
    }
];

const AUDIT_POLICY = {
    table: "data_deletion_runs",
    scope: "audit_logs",
    displayName: "数据删除审计",
    description: "data_deletion_runs 删除审计记录",
    dangerLevel: "critical",
    dateColumn: "started_at",
    updatedColumn: "updated_at",
    statusColumn: "status"
};

const PUBLIC_SCOPES = {
    summaries: {
        displayName: "总结数据",
        description: "每日和每周总结候选数据",
        dangerLevel: "medium",
        scopes: ["summaries"]
    },
    profiles: {
        displayName: "画像与记忆",
        description: "用户画像、环境画像、经验记忆和关系记忆",
        dangerLevel: "high",
        scopes: ["profiles"]
    },
    memory: {
        displayName: "AI 记忆",
        description: "总结、画像和记忆纠错，不包含原始对话",
        dangerLevel: "high",
        scopes: ["summaries", "profiles", "memory_corrections"]
    },
    conversations: {
        displayName: "对话数据",
        description: "conversation_turns、legacy ASR 和 LLM 文本记录",
        dangerLevel: "high",
        scopes: ["conversations"]
    },
    device_history: {
        displayName: "设备运行数据",
        description: "传感器、语音 turn、命令队列、设备状态、提醒和预留事件记录",
        dangerLevel: "high",
        scopes: ["device_history"]
    },
    jobs: {
        displayName: "Job 运行记录",
        description: "daily/weekly memory job 运行记录",
        dangerLevel: "medium",
        scopes: ["jobs"]
    },
    all_user_data: {
        displayName: "全部用户数据",
        description: "AI 记忆、原始对话、设备历史和 job 记录，不包含系统配置与设备能力定义",
        dangerLevel: "critical",
        scopes: ["summaries", "profiles", "memory_corrections", "conversations", "device_history", "jobs"]
    }
};

function getPolicyByTable(tableName) {
    return TABLE_POLICIES.find(policy => policy.table === tableName) ||
        (tableName === AUDIT_POLICY.table ? AUDIT_POLICY : null);
}

function policiesForScope(scope, includeAuditLogs = false) {
    const scopeInfo = PUBLIC_SCOPES[scope];
    if (!scopeInfo) {
        return null;
    }

    const scopeNames = new Set(scopeInfo.scopes);
    const policies = TABLE_POLICIES.filter(policy => scopeNames.has(policy.scope));
    if (includeAuditLogs) {
        policies.push(AUDIT_POLICY);
    }

    return policies;
}

function getPublicScopes() {
    return Object.keys(PUBLIC_SCOPES);
}

async function ensureUserDataDeletionTables(dbRun, dbAll) {
    await dbRun(`
        CREATE TABLE IF NOT EXISTS data_deletion_runs (
            ${columnSql(DATA_DELETION_RUN_COLUMNS)}
        )
    `);

    if (typeof dbAll === "function") {
        await ensureTableColumns(dbRun, dbAll, "data_deletion_runs", addableColumns(DATA_DELETION_RUN_COLUMNS));
        await ensureUniqueIndex(dbRun, dbAll, "data_deletion_runs", "idx_data_deletion_runs_run_id_unique", ["run_id"]);

        for (const policy of TABLE_POLICIES) {
            if (await tableExists(dbAll, policy.table)) {
                await ensureTableColumns(dbRun, dbAll, policy.table, SOFT_DELETE_COLUMNS);
            }
        }
        if (await tableExists(dbAll, "daily_memory")) {
            await ensureTableColumns(dbRun, dbAll, "daily_memory", DAILY_MEMORY_DELETION_COLUMNS);
        }
    }

    await dbRun("CREATE INDEX IF NOT EXISTS idx_data_deletion_runs_started ON data_deletion_runs(started_at DESC,id DESC)");
    await dbRun("CREATE INDEX IF NOT EXISTS idx_data_deletion_runs_scope_status ON data_deletion_runs(scope,mode,status,id)");
    await dbRun("CREATE INDEX IF NOT EXISTS idx_data_deletion_runs_request_type ON data_deletion_runs(request_type,started_at DESC,id DESC)");
    if (typeof dbAll !== "function" || await tableExists(dbAll, "daily_memory")) {
        await dbRun("CREATE INDEX IF NOT EXISTS idx_daily_memory_type_date ON daily_memory(memory_type,memory_date,id)");
    }

    for (const policy of TABLE_POLICIES) {
        if (typeof dbAll !== "function" || await tableExists(dbAll, policy.table)) {
            await dbRun(`CREATE INDEX IF NOT EXISTS ${quoteIdentifier(`idx_${policy.table}_deleted_at`)} ON ${quoteIdentifier(policy.table)} (deleted_at)`);
        }
    }
}

module.exports = {
    AUDIT_POLICY,
    INACTIVE_STATUSES,
    PUBLIC_SCOPES,
    TABLE_POLICIES,
    activeStatusPredicate,
    getPolicyByTable,
    getPublicScopes,
    ensureUserDataDeletionTables,
    notDeletedPredicate,
    policiesForScope,
    qualify,
    quoteIdentifier
};
