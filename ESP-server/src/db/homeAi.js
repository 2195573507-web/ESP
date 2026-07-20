const { ensureTableColumns } = require("./migrations");

const HOME_AI_RULE_CANDIDATE_COLUMNS = [
    { name: "published_version", type: "INTEGER", addType: "INTEGER" },
    { name: "probation_run_ids_json", type: "TEXT NOT NULL DEFAULT '[]'", addType: "TEXT NOT NULL DEFAULT '[]'" }
];
const HOME_AI_PROBATION_COLUMNS = [
    { name: "candidate_id", type: "TEXT", addType: "TEXT" }
];

async function ensureHomeAiTables(dbRun, dbAll) {
    await dbRun(`
        CREATE TABLE IF NOT EXISTS home_ai_rooms (
            room_id TEXT PRIMARY KEY,
            room_name TEXT NOT NULL,
            config_json TEXT NOT NULL,
            config_version INTEGER NOT NULL,
            updated_at_ms INTEGER NOT NULL
        )
    `);
    await dbRun(`
        CREATE TABLE IF NOT EXISTS home_ai_rule_packages (
            package_version INTEGER PRIMARY KEY,
            schema_version INTEGER NOT NULL,
            checksum TEXT NOT NULL,
            status TEXT NOT NULL,
            package_json TEXT NOT NULL,
            resource_json TEXT NOT NULL,
            created_at_ms INTEGER NOT NULL,
            published_at_ms INTEGER,
            superseded_at_ms INTEGER
        )
    `);
    await dbRun(`
        CREATE TABLE IF NOT EXISTS home_ai_rule_deployments (
            deployment_id TEXT PRIMARY KEY,
            gateway_id TEXT NOT NULL,
            package_version INTEGER NOT NULL,
            state TEXT NOT NULL,
            result_json TEXT NOT NULL,
            created_at_ms INTEGER NOT NULL,
            updated_at_ms INTEGER NOT NULL
        )
    `);
    await dbRun(`
        CREATE TABLE IF NOT EXISTS home_ai_events (
            event_id TEXT PRIMARY KEY,
            gateway_id TEXT NOT NULL,
            room_id TEXT,
            event_type TEXT NOT NULL,
            priority INTEGER NOT NULL,
            occurred_at_ms INTEGER NOT NULL,
            received_at_ms INTEGER NOT NULL,
            payload_json TEXT NOT NULL
        )
    `);
    await dbRun(`
        CREATE TABLE IF NOT EXISTS home_ai_virtual_devices (
            device_id TEXT PRIMARY KEY,
            room_id TEXT NOT NULL,
            device_type TEXT NOT NULL,
            state_json TEXT NOT NULL,
            updated_at_ms INTEGER NOT NULL
        )
    `);
    await dbRun(`
        CREATE TABLE IF NOT EXISTS home_ai_feedback (
            feedback_id TEXT PRIMARY KEY,
            decision_id TEXT,
            rule_id TEXT,
            room_id TEXT,
            feedback_type TEXT NOT NULL,
            payload_json TEXT NOT NULL,
            created_at_ms INTEGER NOT NULL
        )
    `);
    await dbRun(`
        CREATE TABLE IF NOT EXISTS home_ai_overrides (
            override_id TEXT PRIMARY KEY,
            room_id TEXT,
            device_id TEXT,
            action TEXT NOT NULL,
            source TEXT NOT NULL,
            priority INTEGER NOT NULL,
            expires_at_ms INTEGER,
            until_condition TEXT,
            allow_safety_override INTEGER NOT NULL,
            payload_json TEXT NOT NULL,
            created_at_ms INTEGER NOT NULL,
            updated_at_ms INTEGER NOT NULL
        )
    `);
    await dbRun(`
        CREATE TABLE IF NOT EXISTS home_ai_memory_candidates (
            candidate_id TEXT PRIMARY KEY,
            room_id TEXT,
            category TEXT NOT NULL,
            content TEXT NOT NULL,
            confidence REAL NOT NULL,
            status TEXT NOT NULL,
            source_json TEXT NOT NULL,
            created_at_ms INTEGER NOT NULL,
            updated_at_ms INTEGER NOT NULL
        )
    `);
    await dbRun(`
        CREATE TABLE IF NOT EXISTS home_ai_habits (
            habit_id TEXT PRIMARY KEY,
            room_id TEXT,
            pattern_json TEXT NOT NULL,
            evidence_count INTEGER NOT NULL,
            confidence REAL NOT NULL,
            status TEXT NOT NULL,
            updated_at_ms INTEGER NOT NULL
        )
    `);
    await dbRun(`
        CREATE TABLE IF NOT EXISTS home_ai_habit_evidence (
            evidence_id TEXT PRIMARY KEY,
            habit_id TEXT,
            feedback_id TEXT UNIQUE,
            room_id TEXT,
            evidence_type TEXT NOT NULL,
            payload_json TEXT NOT NULL,
            created_at_ms INTEGER NOT NULL
        )
    `);
    await dbRun(`
        CREATE TABLE IF NOT EXISTS home_ai_confirmed_memories (
            memory_id TEXT PRIMARY KEY,
            candidate_id TEXT,
            room_id TEXT,
            category TEXT NOT NULL,
            content TEXT NOT NULL,
            automation_allowed INTEGER NOT NULL DEFAULT 0,
            source_json TEXT NOT NULL,
            created_at_ms INTEGER NOT NULL,
            updated_at_ms INTEGER NOT NULL
        )
    `);
    await dbRun(`
        CREATE TABLE IF NOT EXISTS home_ai_rule_candidates (
            candidate_id TEXT PRIMARY KEY,
            room_id TEXT,
            rule_json TEXT NOT NULL,
            source_json TEXT NOT NULL,
            confidence REAL NOT NULL,
            sample_count INTEGER NOT NULL,
            status TEXT NOT NULL,
            gate_json TEXT NOT NULL,
            published_version INTEGER,
            probation_run_ids_json TEXT NOT NULL DEFAULT '[]',
            created_at_ms INTEGER NOT NULL,
            updated_at_ms INTEGER NOT NULL
        )
    `);
    await dbRun(`
        CREATE TABLE IF NOT EXISTS home_ai_rule_probation_runs (
            run_id TEXT PRIMARY KEY,
            candidate_id TEXT,
            rule_id TEXT NOT NULL,
            package_version INTEGER NOT NULL,
            gateway_id TEXT,
            status TEXT NOT NULL,
            trigger_count INTEGER NOT NULL DEFAULT 0,
            failure_count INTEGER NOT NULL DEFAULT 0,
            override_count INTEGER NOT NULL DEFAULT 0,
            metrics_json TEXT NOT NULL,
            started_at_ms INTEGER NOT NULL,
            ends_at_ms INTEGER NOT NULL,
            updated_at_ms INTEGER NOT NULL
        )
    `);
    await dbRun(`
        CREATE TABLE IF NOT EXISTS home_ai_rule_notifications (
            notification_id TEXT PRIMARY KEY,
            package_version INTEGER NOT NULL UNIQUE,
            checksum TEXT NOT NULL,
            reason TEXT NOT NULL,
            created_at_ms INTEGER NOT NULL
        )
    `);
    await dbRun(`
        CREATE TABLE IF NOT EXISTS home_ai_tool_settings (
            setting_key TEXT PRIMARY KEY,
            value_json TEXT NOT NULL,
            updated_at_ms INTEGER NOT NULL
        )
    `);
    await dbRun(`
        CREATE TABLE IF NOT EXISTS home_ai_tool_audit (
            audit_id TEXT PRIMARY KEY,
            tool_name TEXT NOT NULL,
            status TEXT NOT NULL,
            request_json TEXT NOT NULL,
            result_json TEXT NOT NULL,
            error_code TEXT,
            elapsed_ms INTEGER NOT NULL,
            created_at_ms INTEGER NOT NULL
        )
    `);
    await dbRun(`
        CREATE TABLE IF NOT EXISTS home_ai_agent_decisions (
            decision_id TEXT PRIMARY KEY,
            response_type TEXT NOT NULL,
            intent_json TEXT NOT NULL,
            plan_json TEXT NOT NULL,
            action_json TEXT NOT NULL,
            speech_json TEXT NOT NULL,
            status TEXT NOT NULL,
            created_at_ms INTEGER NOT NULL,
            updated_at_ms INTEGER NOT NULL
        )
    `);
    await dbRun(`
        CREATE TABLE IF NOT EXISTS home_ai_decision_steps (
            decision_id TEXT NOT NULL,
            step_index INTEGER NOT NULL,
            status TEXT NOT NULL,
            step_json TEXT NOT NULL,
            result_json TEXT NOT NULL,
            updated_at_ms INTEGER NOT NULL,
            PRIMARY KEY(decision_id, step_index)
        )
    `);
    await dbRun(`
        CREATE TABLE IF NOT EXISTS home_ai_notification_deliveries (
            delivery_id TEXT PRIMARY KEY,
            event_id TEXT NOT NULL,
            channel TEXT NOT NULL,
            status TEXT NOT NULL,
            payload_json TEXT NOT NULL,
            created_at_ms INTEGER NOT NULL,
            updated_at_ms INTEGER NOT NULL
        )
    `);
    await dbRun(`
        CREATE TABLE IF NOT EXISTS home_ai_agent_acks (
            command_id TEXT PRIMARY KEY,
            decision_id TEXT NOT NULL,
            status TEXT NOT NULL,
            result_json TEXT NOT NULL,
            error_message TEXT NOT NULL DEFAULT '',
            received_at_ms INTEGER NOT NULL
        )
    `);
    await dbRun(`
        CREATE TABLE IF NOT EXISTS home_ai_briefing_runs (
            briefing_id TEXT PRIMARY KEY,
            scene TEXT NOT NULL,
            room_id TEXT NOT NULL,
            decision_id TEXT,
            delivery_channel TEXT NOT NULL,
            status TEXT NOT NULL,
            content_hash TEXT NOT NULL,
            created_at_ms INTEGER NOT NULL
        )
    `);
    await dbRun(`
        CREATE TABLE IF NOT EXISTS home_ai_emergency_acknowledgements (
            event_id TEXT PRIMARY KEY,
            source TEXT NOT NULL,
            acknowledged_at_ms INTEGER NOT NULL,
            updated_at_ms INTEGER NOT NULL
        )
    `);
    if (typeof dbAll === "function") {
        await ensureTableColumns(dbRun, dbAll, "home_ai_rule_candidates", HOME_AI_RULE_CANDIDATE_COLUMNS);
        await ensureTableColumns(dbRun, dbAll, "home_ai_rule_probation_runs", HOME_AI_PROBATION_COLUMNS);
    }
    await dbRun(`CREATE INDEX IF NOT EXISTS idx_home_ai_events_gateway_time ON home_ai_events(gateway_id, received_at_ms DESC)`);
    await dbRun(`CREATE INDEX IF NOT EXISTS idx_home_ai_events_room_time ON home_ai_events(room_id, occurred_at_ms DESC)`);
    await dbRun(`CREATE INDEX IF NOT EXISTS idx_home_ai_rule_deployments_gateway_time ON home_ai_rule_deployments(gateway_id, updated_at_ms DESC)`);
    await dbRun(`CREATE INDEX IF NOT EXISTS idx_home_ai_feedback_rule_time ON home_ai_feedback(rule_id, created_at_ms DESC)`);
    await dbRun(`CREATE INDEX IF NOT EXISTS idx_home_ai_overrides_scope_expiry ON home_ai_overrides(room_id, device_id, expires_at_ms)`);
    await dbRun(`CREATE INDEX IF NOT EXISTS idx_home_ai_memory_status_time ON home_ai_memory_candidates(status, updated_at_ms DESC)`);
    await dbRun(`CREATE INDEX IF NOT EXISTS idx_home_ai_habit_evidence_room_time ON home_ai_habit_evidence(room_id, created_at_ms DESC)`);
    await dbRun(`CREATE INDEX IF NOT EXISTS idx_home_ai_candidates_status_time ON home_ai_rule_candidates(status, updated_at_ms DESC)`);
    await dbRun(`CREATE INDEX IF NOT EXISTS idx_home_ai_probation_status_time ON home_ai_rule_probation_runs(status, updated_at_ms DESC)`);
    await dbRun(`CREATE INDEX IF NOT EXISTS idx_home_ai_tool_audit_name_time ON home_ai_tool_audit(tool_name, created_at_ms DESC)`);
    await dbRun(`CREATE INDEX IF NOT EXISTS idx_home_ai_agent_status_time ON home_ai_agent_decisions(status, updated_at_ms DESC)`);
    await dbRun(`CREATE INDEX IF NOT EXISTS idx_home_ai_agent_acks_decision ON home_ai_agent_acks(decision_id, received_at_ms DESC)`);
    await dbRun(`CREATE INDEX IF NOT EXISTS idx_home_ai_briefing_dedupe ON home_ai_briefing_runs(scene, room_id, content_hash, created_at_ms DESC)`);
    await dbRun(`CREATE INDEX IF NOT EXISTS idx_home_ai_emergency_ack_time ON home_ai_emergency_acknowledgements(acknowledged_at_ms DESC)`);
}

module.exports = {
    ensureHomeAiTables
};
