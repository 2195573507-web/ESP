const crypto = require("crypto");
const {
    defaultRooms,
    checksumFor,
    HOME_AI_MAX_OVERRIDES,
    HOME_AI_MAX_RULE_PACKAGE_BYTES,
    HOME_AI_SCHEMA_VERSION,
    normalizeHomeAiEvents,
    normalizeRoomConfig,
    normalizeUserOverride,
    normalizeVirtualDeviceState,
    stableJson,
    validateRulePackage
} = require("../homeAi/schema");
const {
    broadcastEvent
} = require("./eventStreamService");
const {
    trimText
} = require("./deviceMetadata");
const {
    attachRuleCandidateProbation,
    evaluateRuleCandidate,
    recordHabitEvidence,
    recordProbationEvent,
    recordProbationFeedback,
    startProbationRuns
} = require("../homeAi/learningService");

const DEPLOYMENT_STATES = new Set(["ACTIVE", "ACTIVE_PARTIAL", "REJECTED", "ROLLED_BACK", "PENDING"]);
const FEEDBACK_TYPES = new Set(["accepted", "rejected", "modified", "cancelled", "reverted", "manual_override"]);
const HOME_AI_MAX_EMERGENCY_ACKNOWLEDGEMENTS = 8;

function makeId(prefix) {
    return `${prefix}_${typeof crypto.randomUUID === "function" ? crypto.randomUUID() : crypto.randomBytes(16).toString("hex")}`;
}

function jsonText(value) {
    return JSON.stringify(value === undefined ? {} : value);
}

function parseJson(value, fallback) {
    if (!value) {
        return fallback;
    }
    try {
        const parsed = typeof value === "string" ? JSON.parse(value) : value;
        return parsed === undefined ? fallback : parsed;
    } catch (_) {
        return fallback;
    }
}

function safeLimit(value, fallback = 50, maximum = 200) {
    const parsed = Number.parseInt(value, 10);
    return Number.isFinite(parsed) && parsed > 0 ? Math.min(parsed, maximum) : fallback;
}

function mapRoomRow(row) {
    return {
        room_id: row.room_id,
        room_name: row.room_name,
        ...parseJson(row.config_json, {}),
        config_version: Number(row.config_version) || 1,
        updated_at_ms: Number(row.updated_at_ms) || null
    };
}

function mapRulePackageRow(row) {
    if (!row) {
        return null;
    }
    return {
        ...parseJson(row.package_json, {}),
        status: row.status,
        resource: parseJson(row.resource_json, {}),
        published_at_ms: Number(row.published_at_ms) || null,
        created_at_ms: Number(row.created_at_ms) || null
    };
}

function ruleForTransport(rule) {
    const transportRule = { ...(rule || {}) };
    delete transportRule.binding;
    return transportRule;
}

function rulePackageTransportPayload(rulePackage) {
    return {
        schema_version: Number(rulePackage.schema_version) || HOME_AI_SCHEMA_VERSION,
        version: Number(rulePackage.version) || 1,
        generated_at_ms: Number(rulePackage.generated_at_ms) || Date.now(),
        rooms: rulePackage.rooms,
        rules: rulePackage.rules.map(ruleForTransport)
    };
}

function toRulePackageTransport(rulePackage) {
    if (!rulePackage || !Array.isArray(rulePackage.rooms) || !Array.isArray(rulePackage.rules)) {
        return null;
    }
    const payloadObject = rulePackageTransportPayload(rulePackage);
    const payload = stableJson(payloadObject);
    return {
        schema_version: payloadObject.schema_version,
        version: payloadObject.version,
        payload,
        checksum: checksumFor(payloadObject)
    };
}

function boundedSourceIds(value) {
    return Array.isArray(value)
        ? value.filter(item => typeof item === "string" && item).slice(0, 3)
        : [];
}

function migrateRuleBindings(current, previousRooms, nextRooms, configVersion) {
    if (!current || !Array.isArray(current.rules)) {
        return { changed: false, rules: [], migrated_rule_ids: [], pending_rebind_rule_ids: [] };
    }
    const previousById = new Map((previousRooms || []).map(room => [room.room_id, room]));
    const nextBySource = new Map();
    const nextRoomIds = new Set();
    for (const room of nextRooms || []) {
        nextRoomIds.add(room.room_id);
        for (const sourceId of boundedSourceIds(room.sensing_sources)) {
            nextBySource.set(sourceId, room.room_id);
        }
    }

    const migratedRuleIds = [];
    const pendingRuleIds = [];
    const rules = current.rules.map(rule => {
        const priorBinding = rule?.binding && typeof rule.binding === "object" ? rule.binding : null;
        const previousRoom = previousById.get(rule.room_id);
        const sourceIds = boundedSourceIds(priorBinding?.source_ids).length > 0
            ? boundedSourceIds(priorBinding.source_ids)
            : boundedSourceIds(previousRoom?.sensing_sources);
        const destinationRooms = [...new Set(sourceIds.map(sourceId => nextBySource.get(sourceId)).filter(Boolean))];
        const wasEnabled = priorBinding?.state === "PENDING_REBIND"
            ? priorBinding.was_enabled !== false
            : rule.enabled !== false;
        const fromRoomId = trimText(priorBinding?.from_room_id, 32) || rule.room_id;

        if (destinationRooms.length === 1) {
            const destinationRoomId = destinationRooms[0];
            if (destinationRoomId === rule.room_id && priorBinding?.state !== "PENDING_REBIND") {
                return rule;
            }
            migratedRuleIds.push(rule.rule_id);
            return {
                ...rule,
                room_id: destinationRoomId,
                enabled: wasEnabled,
                binding: {
                    state: "MIGRATED",
                    reason: "room_config_migration",
                    from_room_id: fromRoomId,
                    to_room_id: destinationRoomId,
                    source_ids: sourceIds,
                    config_version: configVersion,
                    was_enabled: wasEnabled
                }
            };
        }

        if (sourceIds.length === 0 && nextRoomIds.has(rule.room_id) && priorBinding?.state !== "PENDING_REBIND") {
            return rule;
        }
        pendingRuleIds.push(rule.rule_id);
        return {
            ...rule,
            enabled: false,
            binding: {
                state: "PENDING_REBIND",
                reason: "room_config_migration",
                from_room_id: fromRoomId,
                to_room_id: "",
                source_ids: sourceIds,
                config_version: configVersion,
                was_enabled: wasEnabled
            }
        };
    });
    return {
        changed: migratedRuleIds.length > 0 || pendingRuleIds.length > 0,
        rules,
        migrated_rule_ids: migratedRuleIds,
        pending_rebind_rule_ids: pendingRuleIds
    };
}

function mapDeploymentRow(row) {
    return {
        deployment_id: row.deployment_id,
        gateway_id: row.gateway_id,
        package_version: Number(row.package_version),
        state: row.state,
        result: parseJson(row.result_json, {}),
        created_at_ms: Number(row.created_at_ms),
        updated_at_ms: Number(row.updated_at_ms)
    };
}

function mapEventRow(row) {
    return {
        event_id: row.event_id,
        gateway_id: row.gateway_id,
        room_id: row.room_id || "",
        event_type: row.event_type,
        priority: Number(row.priority),
        occurred_at_ms: Number(row.occurred_at_ms),
        received_at_ms: Number(row.received_at_ms),
        user_acknowledged: Boolean(Number(row.user_acknowledged)),
        payload: parseJson(row.payload_json, {})
    };
}

function mapFeedbackRow(row) {
    return {
        feedback_id: row.feedback_id,
        decision_id: row.decision_id || "",
        rule_id: row.rule_id || "",
        room_id: row.room_id || "",
        feedback_type: row.feedback_type,
        payload: parseJson(row.payload_json, {}),
        created_at_ms: Number(row.created_at_ms) || null
    };
}

function mapOverrideRow(row) {
    const payload = parseJson(row.payload_json, {});
    return {
        override_id: row.override_id,
        scope: {
            room_id: row.room_id || "",
            device_id: row.device_id || null
        },
        action: row.action,
        source: row.source,
        priority: Number(row.priority),
        expires_at_ms: Number(row.expires_at_ms) || null,
        until_condition: row.until_condition || "",
        allow_safety_override: Number(row.allow_safety_override) !== 0,
        ...payload,
        created_at_ms: Number(row.created_at_ms),
        updated_at_ms: Number(row.updated_at_ms)
    };
}

function canonicalGatewayRoom(room) {
    return {
        room_id: room.room_id,
        room_name: room.room_name,
        sensing_sources: Array.isArray(room.sensing_sources) ? room.sensing_sources : [],
        voice_terminal_device_id: room.voice_terminal_device_id || "",
        presence_confirm_ms: Number(room.presence_confirm_ms),
        vacant_confirm_ms: Number(room.vacant_confirm_ms),
        multiple_confirm_ms: Number(room.multiple_confirm_ms),
        single_confirm_ms: Number(room.single_confirm_ms),
        quiet_start: room.quiet_start,
        quiet_end: room.quiet_end
    };
}

function canonicalGatewayOverride(override) {
    return {
        override_id: override.override_id,
        scope: {
            room_id: override.scope?.room_id || "",
            device_id: override.scope?.device_id || null
        },
        action: override.action,
        source: override.source,
        priority: Number(override.priority),
        created_at_ms: Number(override.created_at_ms),
        expires_at_ms: override.expires_at_ms === null ? null : Number(override.expires_at_ms),
        until_condition: override.until_condition || "",
        allow_safety_override: override.allow_safety_override !== false
    };
}

function canonicalGatewayWeatherContext(value, nowMs) {
    const observedAtMs = Number(value?.observed_at_ms);
    const expiresAtMs = Number(value?.expires_at_ms);
    const fresh = value?.available === true && Number.isFinite(observedAtMs) && Number.isFinite(expiresAtMs) &&
        observedAtMs <= nowMs && nowMs < expiresAtMs;
    return {
        available: fresh,
        dark: fresh && value?.dark === true,
        observed_at_ms: Number.isFinite(observedAtMs) ? observedAtMs : null,
        expires_at_ms: Number.isFinite(expiresAtMs) ? expiresAtMs : null,
        reason: fresh ? "fresh" : trimText(value?.reason, 64) || "not_available"
    };
}

function configResourceError(code, message) {
    const error = new Error(message);
    error.code = code;
    return error;
}

function timezoneOffsetMinutes(timezone, timestampMs) {
    const fallback = -new Date(timestampMs).getTimezoneOffset();
    if (!timezone) return fallback;
    try {
        const parts = new Intl.DateTimeFormat("en-US", {
            timeZone: timezone,
            year: "numeric",
            month: "2-digit",
            day: "2-digit",
            hour: "2-digit",
            minute: "2-digit",
            second: "2-digit",
            hourCycle: "h23"
        }).formatToParts(new Date(timestampMs));
        const values = Object.fromEntries(parts.map(part => [part.type, part.value]));
        const representedAsUtc = Date.UTC(
            Number(values.year),
            Number(values.month) - 1,
            Number(values.day),
            Number(values.hour),
            Number(values.minute),
            Number(values.second)
        );
        return Math.round((representedAsUtc - timestampMs) / 60000);
    } catch (_) {
        return fallback;
    }
}

async function readRoomConfig(dbAll) {
    const rows = await dbAll("SELECT * FROM home_ai_rooms ORDER BY room_id ASC");
    if (rows.length > 0) {
        const configVersion = Math.max(...rows.map(row => Number(row.config_version) || 0));
        return {
            schema_version: HOME_AI_SCHEMA_VERSION,
            config_version: configVersion,
            updated_at_ms: Math.max(...rows.map(row => Number(row.updated_at_ms) || 0)),
            rooms: rows.map(mapRoomRow)
        };
    }
    return {
        schema_version: HOME_AI_SCHEMA_VERSION,
        config_version: 0,
        updated_at_ms: null,
        rooms: defaultRooms()
    };
}

async function readGatewayConfigTransport(dbAll) {
    const roomConfig = await readRoomConfig(dbAll);
    const activeOverrides = await listUserOverrides(dbAll, { limit: HOME_AI_MAX_OVERRIDES + 1 });
    if (activeOverrides.length > HOME_AI_MAX_OVERRIDES) {
        throw configResourceError("HOME_AI_CONFIG_OVERRIDE_LIMIT", "active overrides exceed the S3 fixed capacity");
    }
    const generatedAtMs = Date.now();
    const settings = await dbAll(
        "SELECT setting_key,value_json FROM home_ai_tool_settings WHERE setting_key IN ('home_location','weather_context')"
    );
    const settingValues = new Map(settings.map(row => [row.setting_key, parseJson(row.value_json, null)]));
    const location = settingValues.get("home_location") || null;
    const weatherContext = canonicalGatewayWeatherContext(settingValues.get("weather_context"), generatedAtMs);
    const emergencyAcknowledgements = await listEmergencyAcknowledgements(dbAll);
    const payloadObject = {
        schema_version: HOME_AI_SCHEMA_VERSION,
        rooms: roomConfig.rooms.map(canonicalGatewayRoom),
        overrides: activeOverrides.map(canonicalGatewayOverride),
        weather_context: weatherContext,
        emergency_acknowledgements: emergencyAcknowledgements
    };
    const payload = stableJson(payloadObject);
    if (Buffer.byteLength(payload, "utf8") > HOME_AI_MAX_RULE_PACKAGE_BYTES) {
        throw configResourceError("HOME_AI_CONFIG_TOO_LARGE", "gateway configuration exceeds the S3 fixed package budget");
    }
    return {
        schema_version: HOME_AI_SCHEMA_VERSION,
        room_config_version: Number(roomConfig.config_version) || 0,
        server_time_ms: generatedAtMs,
        timezone_offset_minutes: timezoneOffsetMinutes(location?.timezone, generatedAtMs),
        payload,
        checksum: checksumFor(payloadObject),
        resource: {
            room_count: payloadObject.rooms.length,
            override_count: payloadObject.overrides.length,
            max_overrides: HOME_AI_MAX_OVERRIDES,
            emergency_acknowledgement_count: emergencyAcknowledgements.length,
            max_emergency_acknowledgements: HOME_AI_MAX_EMERGENCY_ACKNOWLEDGEMENTS,
            weather_available: weatherContext.available,
            payload_bytes: Buffer.byteLength(payload, "utf8"),
            payload_budget_bytes: HOME_AI_MAX_RULE_PACKAGE_BYTES
        }
    };
}

async function writeRoomConfig(dbRun, dbAll, body = {}) {
    const normalized = normalizeRoomConfig(body);
    if (!normalized.ok) {
        return normalized;
    }
    const nowMs = Date.now();
    let version = 0;
    let migrationPackage = null;
    let migrationSummary = null;
    await dbRun("BEGIN IMMEDIATE TRANSACTION");
    try {
        const previousConfig = await readRoomConfig(dbAll);
        const current = await readCurrentRulePackage(dbAll);
        const rows = await dbAll("SELECT MAX(config_version) AS version FROM home_ai_rooms");
        version = Math.max(0, Number(rows[0]?.version) || 0) + 1;
        const migration = migrateRuleBindings(current, previousConfig.rooms, normalized.rooms, version);

        await dbRun("DELETE FROM home_ai_rooms");
        for (const room of normalized.rooms) {
            await dbRun(
                `INSERT INTO home_ai_rooms(room_id,room_name,config_json,config_version,updated_at_ms)
                 VALUES(?,?,?,?,?)`,
                [room.room_id, room.room_name, jsonText(room), version, nowMs]
            );
        }
        if (migration.changed) {
            const packageVersion = Number(current.version) + 1;
            if (!Number.isSafeInteger(packageVersion) || packageVersion > 2147483647) {
                throw configResourceError("RULE_PACKAGE_VERSION_EXHAUSTED", "rule package version cannot be advanced");
            }
            migrationSummary = {
                reason: "room_config_migration",
                from_config_version: Number(previousConfig.config_version) || 0,
                to_config_version: version,
                migrated_rule_ids: migration.migrated_rule_ids,
                pending_rebind_rule_ids: migration.pending_rebind_rule_ids
            };
            const packageDraft = {
                schema_version: Number(current.schema_version) || HOME_AI_SCHEMA_VERSION,
                version: packageVersion,
                generated_at_ms: nowMs,
                rooms: normalized.rooms,
                rules: migration.rules,
                control: migrationSummary
            };
            const transport = toRulePackageTransport(packageDraft);
            if (!transport || Buffer.byteLength(transport.payload, "utf8") > HOME_AI_MAX_RULE_PACKAGE_BYTES) {
                throw configResourceError("RULE_PACKAGE_TOO_LARGE", "migrated rule package exceeds the S3 fixed package budget");
            }
            const resource = {
                ...(current.resource || {}),
                room_count: normalized.rooms.length,
                rule_count: migration.rules.length,
                payload_bytes: Buffer.byteLength(transport.payload, "utf8"),
                payload_budget_bytes: HOME_AI_MAX_RULE_PACKAGE_BYTES
            };
            migrationPackage = {
                ...packageDraft,
                checksum: transport.checksum,
                status: "PUBLISHED",
                resource,
                created_at_ms: nowMs,
                published_at_ms: nowMs
            };
            await dbRun(
                `UPDATE home_ai_rule_packages
                 SET status='SUPERSEDED', superseded_at_ms=?
                 WHERE package_version=?`,
                [nowMs, current.version]
            );
            await dbRun(
                `INSERT INTO home_ai_rule_packages
                 (package_version,schema_version,checksum,status,package_json,resource_json,created_at_ms,published_at_ms)
                 VALUES(?,?,?,?,?,?,?,?)`,
                [
                    packageVersion,
                    packageDraft.schema_version,
                    transport.checksum,
                    "PUBLISHED",
                    jsonText(migrationPackage),
                    jsonText(resource),
                    nowMs,
                    nowMs
                ]
            );
            await dbRun(
                `INSERT INTO home_ai_rule_notifications
                 (notification_id,package_version,checksum,reason,created_at_ms)
                 VALUES(?,?,?,?,?)`,
                [`rule_${packageVersion}`, packageVersion, transport.checksum, "room_config_migration", nowMs]
            );
        }
        await dbRun("COMMIT");
    } catch (error) {
        await dbRun("ROLLBACK");
        throw error;
    }
    const config = {
        schema_version: HOME_AI_SCHEMA_VERSION,
        rooms: normalized.rooms,
        config_version: version,
        updated_at_ms: nowMs,
        migration: migrationSummary
    };
    broadcastEvent("home_ai_room_config_updated", config);
    if (migrationPackage) {
        broadcastEvent("home_ai_rule_published", migrationPackage);
        broadcastEvent("home_ai_room_config_migration", migrationSummary);
    }
    return { ok: true, config };
}

async function readCurrentRulePackage(dbAll) {
    const rows = await dbAll(
        `SELECT * FROM home_ai_rule_packages
         WHERE status='PUBLISHED'
         ORDER BY package_version DESC
         LIMIT 1`
    );
    return mapRulePackageRow(rows[0]);
}

async function readCurrentRulePackageTransport(dbAll) {
    return toRulePackageTransport(await readCurrentRulePackage(dbAll));
}

async function readRuleUpdateNotification(dbAll, knownVersion = 0, knownConfigChecksum = "") {
    const current = await readCurrentRulePackage(dbAll);
    const config = await readGatewayConfigTransport(dbAll);
    const version = Math.max(0, Number.parseInt(knownVersion, 10) || 0);
    const knownChecksum = /^[0-9a-f]{64}$/.test(String(knownConfigChecksum || ""))
        ? String(knownConfigChecksum)
        : "";
    const updateAvailable = Boolean(current && Number(current.version) > version);
    const configUpdateAvailable = knownChecksum !== config.checksum;
    let reason = "none";
    if (updateAvailable && current) {
        const rows = await dbAll(
            `SELECT reason FROM home_ai_rule_notifications
             WHERE package_version=?
             ORDER BY created_at_ms DESC LIMIT 1`,
            [current.version]
        );
        reason = trimText(rows[0]?.reason, 64) || "published";
    } else if (configUpdateAvailable) {
        reason = "room_config_updated";
    }
    return {
        update_available: updateAvailable,
        known_version: version,
        package_version: current ? Number(current.version) : 0,
        checksum: current?.checksum || "",
        published_at_ms: current?.published_at_ms || null,
        config_update_available: configUpdateAvailable,
        known_config_checksum: knownChecksum,
        config_checksum: config.checksum,
        reason
    };
}

async function listRulePackages(dbAll, options = {}) {
    const rows = await dbAll(
        `SELECT * FROM home_ai_rule_packages
         ORDER BY package_version DESC
         LIMIT ?`,
        [safeLimit(options.limit, 30, 100)]
    );
    return rows.map(mapRulePackageRow);
}

async function publishRulePackage(dbRun, dbAll, body = {}) {
    const validation = validateRulePackage(body, { requireChecksum: false });
    if (!validation.ok) {
        return validation;
    }
    const rulePackage = validation.package;
    const current = await readCurrentRulePackage(dbAll);
    if (current && rulePackage.version <= Number(current.version)) {
        return {
            ok: false,
            code: "RULE_PACKAGE_VERSION_STALE",
            error: "rule package version must be greater than the active version"
        };
    }
    const nowMs = Date.now();
    await dbRun("BEGIN IMMEDIATE TRANSACTION");
    try {
        if (current) {
            await dbRun(
                `UPDATE home_ai_rule_packages
                 SET status='SUPERSEDED', superseded_at_ms=?
                 WHERE package_version=?`,
                [nowMs, current.version]
            );
        }
        await dbRun(
            `INSERT INTO home_ai_rule_packages
             (package_version,schema_version,checksum,status,package_json,resource_json,created_at_ms,published_at_ms)
             VALUES(?,?,?,?,?,?,?,?)`,
            [
                rulePackage.version,
                rulePackage.schema_version,
                rulePackage.checksum,
                "PUBLISHED",
                jsonText(rulePackage),
                jsonText(validation.resource),
                nowMs,
                nowMs
            ]
        );
        await dbRun(
            `INSERT INTO home_ai_rule_notifications
             (notification_id,package_version,checksum,reason,created_at_ms)
             VALUES(?,?,?,?,?)`,
            [`rule_${rulePackage.version}`, rulePackage.version, rulePackage.checksum, "published", nowMs]
        );
        await dbRun("COMMIT");
    } catch (error) {
        await dbRun("ROLLBACK");
        if (String(error?.message || "").includes("UNIQUE")) {
            return { ok: false, code: "RULE_PACKAGE_VERSION_EXISTS", error: "rule package version already exists" };
        }
        throw error;
    }
    const published = {
        ...rulePackage,
        status: "PUBLISHED",
        resource: validation.resource,
        published_at_ms: nowMs,
        created_at_ms: nowMs
    };
    broadcastEvent("home_ai_rule_published", published);
    return { ok: true, rule_package: published };
}

async function rollbackRulePackage(dbRun, dbAll, body = {}) {
    const current = await readCurrentRulePackage(dbAll);
    if (!current) {
        return { ok: false, code: "RULE_ROLLBACK_NO_ACTIVE_PACKAGE", error: "no active rule package exists" };
    }
    const expectedActiveVersion = Number.parseInt(body.expected_active_version, 10);
    if (Number.isFinite(expectedActiveVersion) && expectedActiveVersion > 0 &&
        Number(current.version) !== expectedActiveVersion) {
        return {
            ok: false,
            code: "RULE_ROLLBACK_ACTIVE_VERSION_CHANGED",
            error: "active rule package changed since probation started"
        };
    }
    const requestedVersion = Number.parseInt(body.target_version, 10);
    const params = [Number(current.version)];
    let targetSql = "package_version<?";
    if (Number.isFinite(requestedVersion) && requestedVersion > 0) {
        targetSql = "package_version=?";
        params[0] = requestedVersion;
    }
    const rows = await dbAll(
        `SELECT * FROM home_ai_rule_packages
         WHERE ${targetSql}
         ORDER BY package_version DESC
         LIMIT 1`,
        params
    );
    const target = mapRulePackageRow(rows[0]);
    if (!target || Number(target.version) === Number(current.version)) {
        return { ok: false, code: "RULE_ROLLBACK_TARGET_NOT_FOUND", error: "rollback target package was not found" };
    }
    const rollbackVersion = Number(current.version) + 1;
    const result = await publishRulePackage(dbRun, dbAll, {
        schema_version: target.schema_version,
        version: rollbackVersion,
        generated_at_ms: Date.now(),
        rooms: target.rooms,
        rules: target.rules
    });
    if (!result.ok) return result;
    const control = {
        rollback_of_version: Number(current.version),
        restored_from_version: Number(target.version),
        reason: trimText(body.reason, 240) || "manual_rollback"
    };
    const stored = { ...result.rule_package, control };
    await dbRun(
        "UPDATE home_ai_rule_packages SET package_json=? WHERE package_version=?",
        [jsonText(stored), rollbackVersion]
    );
    await dbRun(
        "UPDATE home_ai_rule_notifications SET reason='rollback' WHERE package_version=?",
        [rollbackVersion]
    );
    broadcastEvent("home_ai_rule_rollback", control);
    return { ok: true, rule_package: stored, control };
}

async function recordRuleDeployment(dbRun, body = {}, gatewayId = "") {
    const deploymentId = trimText(body.deployment_id, 160) || makeId("deploy");
    const safeGatewayId = trimText(gatewayId || body.gateway_id, 128);
    const packageVersion = Number.parseInt(body.package_version, 10);
    const state = trimText(body.state, 32).toUpperCase();
    if (!safeGatewayId || !Number.isFinite(packageVersion) || !DEPLOYMENT_STATES.has(state)) {
        return { ok: false, code: "RULE_DEPLOYMENT_INVALID", error: "gateway_id, package_version, and state are required" };
    }
    const nowMs = Date.now();
    const rawResult = body.result && typeof body.result === "object" ? body.result : {};
    const result = {
        accepted_count: Math.max(0, Number.parseInt(rawResult.accepted_count, 10) || 0),
        rejected_count: Math.max(0, Number.parseInt(rawResult.rejected_count, 10) || 0),
        active_rule_count: Math.max(0, Number.parseInt(rawResult.active_rule_count, 10) || 0),
        error_code: trimText(rawResult.error_code, 64),
        items: Array.isArray(rawResult.items) ? rawResult.items.slice(0, 16).map(item => ({
            rule_id: trimText(item?.rule_id, 64),
            accepted: item?.accepted === true,
            retained_previous: item?.retained_previous === true,
            code: trimText(item?.code, 64)
        })) : []
    };
    await dbRun(
        `INSERT INTO home_ai_rule_deployments
         (deployment_id,gateway_id,package_version,state,result_json,created_at_ms,updated_at_ms)
         VALUES(?,?,?,?,?,?,?)
         ON CONFLICT(deployment_id) DO UPDATE SET
            state=excluded.state,
            result_json=excluded.result_json,
            updated_at_ms=excluded.updated_at_ms`,
        [deploymentId, safeGatewayId, packageVersion, state, jsonText(result), nowMs, nowMs]
    );
    const deployment = {
        deployment_id: deploymentId,
        gateway_id: safeGatewayId,
        package_version: packageVersion,
        state,
        result,
        updated_at_ms: nowMs
    };
    broadcastEvent("home_ai_rule_deployment", deployment);
    return { ok: true, deployment };
}

async function listRuleDeployments(dbAll, options = {}) {
    const params = [];
    let where = "";
    const gatewayId = trimText(options.gateway_id, 128);
    if (gatewayId) {
        where = "WHERE gateway_id=?";
        params.push(gatewayId);
    }
    params.push(safeLimit(options.limit, 50, 200));
    const rows = await dbAll(
        `SELECT * FROM home_ai_rule_deployments ${where}
         ORDER BY updated_at_ms DESC
         LIMIT ?`,
        params
    );
    return rows.map(mapDeploymentRow);
}

async function recordHomeAiEvents(dbRun, body = {}, gatewayId = "", dbAll = null) {
    const safeGatewayId = trimText(gatewayId || body.gateway_id, 128);
    const normalized = normalizeHomeAiEvents(body);
    if (!safeGatewayId || !normalized.ok) {
        return { ok: false, code: "HOME_AI_EVENTS_INVALID", error: "gateway_id and at least one event are required" };
    }
    const nowMs = Date.now();
    let accepted = 0;
    const rejected = [...normalized.rejected];
    const learningErrors = [];
    for (const item of normalized.events) {
        const payloadJson = jsonText({
            ...item.payload,
            request_id: item.request_id,
            trace_id: item.trace_id,
            source_device_id: item.source_device_id,
            sequence_no: item.sequence_no,
            schema_version: item.schema_version
        });
        const result = await dbRun(
            item.event_type === "emergency"
                ? `INSERT INTO home_ai_events
                   (event_id,gateway_id,room_id,event_type,priority,occurred_at_ms,received_at_ms,payload_json)
                   VALUES(?,?,?,?,?,?,?,?)
                   ON CONFLICT(event_id) DO UPDATE SET
                      gateway_id=excluded.gateway_id,
                      room_id=excluded.room_id,
                      event_type=excluded.event_type,
                      priority=excluded.priority,
                      occurred_at_ms=excluded.occurred_at_ms,
                      received_at_ms=excluded.received_at_ms,
                      payload_json=excluded.payload_json
                   WHERE home_ai_events.event_type='emergency'
                     AND excluded.occurred_at_ms>=home_ai_events.occurred_at_ms`
                : `INSERT OR IGNORE INTO home_ai_events
                   (event_id,gateway_id,room_id,event_type,priority,occurred_at_ms,received_at_ms,payload_json)
                   VALUES(?,?,?,?,?,?,?,?)`,
            [
                item.event_id,
                safeGatewayId,
                item.room_id,
                item.event_type,
                item.priority,
                item.occurred_at_ms,
                nowMs,
                payloadJson
            ]
        );
        if (result.changes > 0) {
            accepted += 1;
            if (item.event_type === "emergency" &&
                String(item.payload?.state || "").toUpperCase() === "RESOLVED") {
                await dbRun("DELETE FROM home_ai_emergency_acknowledgements WHERE event_id=?", [item.event_id]);
            }
            if (typeof dbAll === "function") {
                try {
                    if (item.event_type === "feedback") {
                        const overrideId = trimText(item.payload?.override_id, 160);
                        await recordFeedback(dbRun, {
                            feedback_id: overrideId ? `override_${overrideId}` : `gateway_${safeGatewayId}_${item.event_id}`,
                            rule_id: trimText(item.payload?.rule_id, 64),
                            room_id: item.room_id,
                            feedback_type: item.payload?.feedback_type,
                            payload: { ...item.payload, source: "s3_event", event_id: item.event_id, gateway_id: safeGatewayId }
                        }, dbAll);
                    }
                    await recordProbationEvent(
                        dbRun,
                        dbAll,
                        item,
                        rollbackBody => rollbackRulePackage(dbRun, dbAll, rollbackBody)
                    );
                } catch (error) {
                    learningErrors.push({ event_id: item.event_id, code: "HOME_AI_LEARNING_UPDATE_FAILED" });
                }
            }
        }
    }
    broadcastEvent("home_ai_events", { gateway_id: safeGatewayId, accepted, rejected, learning_errors: learningErrors });
    return { ok: true, accepted, rejected, learning_errors: learningErrors };
}

async function listEmergencyAcknowledgements(dbAll) {
    const rows = await dbAll(
        `SELECT event_id,acknowledged_at_ms
         FROM home_ai_emergency_acknowledgements
         ORDER BY acknowledged_at_ms DESC
         LIMIT ?`,
        [HOME_AI_MAX_EMERGENCY_ACKNOWLEDGEMENTS]
    );
    return rows.map(row => ({
        event_id: trimText(row.event_id, 63),
        acknowledged_at_ms: Number(row.acknowledged_at_ms) || 0
    })).filter(item => item.event_id && item.acknowledged_at_ms > 0);
}

async function acknowledgeEmergency(dbRun, dbAll, eventId, body = {}) {
    const id = trimText(eventId, 63);
    if (!id || id !== String(eventId || "").trim()) {
        return { ok: false, code: "HOME_AI_EMERGENCY_ID_INVALID", error: "emergency event id is invalid" };
    }
    const rows = await dbAll(
        "SELECT event_type,payload_json FROM home_ai_events WHERE event_id=? LIMIT 1",
        [id]
    );
    if (!rows[0] || rows[0].event_type !== "emergency") {
        return { ok: false, code: "HOME_AI_EMERGENCY_NOT_FOUND", error: "emergency event was not found" };
    }
    const state = String(parseJson(rows[0].payload_json, {})?.state || "").toUpperCase();
    if (["RECOVERING", "RESOLVED"].includes(state)) {
        return { ok: false, code: "HOME_AI_EMERGENCY_NOT_ACTIVE", error: "emergency event is no longer active" };
    }
    const nowMs = Date.now();
    const source = trimText(body.source, 32) || "web";
    const existing = await dbAll(
        "SELECT acknowledged_at_ms FROM home_ai_emergency_acknowledgements WHERE event_id=? LIMIT 1",
        [id]
    );
    await dbRun(
        `INSERT INTO home_ai_emergency_acknowledgements(event_id,source,acknowledged_at_ms,updated_at_ms)
         VALUES(?,?,?,?)
         ON CONFLICT(event_id) DO UPDATE SET
            source=excluded.source,
            updated_at_ms=excluded.updated_at_ms`,
        [id, source, nowMs, nowMs]
    );
    const acknowledgement = {
        event_id: id,
        source,
        acknowledged_at_ms: Number(existing[0]?.acknowledged_at_ms) || nowMs,
        duplicate: Boolean(existing[0])
    };
    broadcastEvent("home_ai_emergency_acknowledged", acknowledgement);
    return { ok: true, acknowledgement };
}

async function listHomeAiEvents(dbAll, options = {}) {
    const params = [];
    const where = [];
    if (trimText(options.room_id, 32)) {
        where.push("room_id=?");
        params.push(trimText(options.room_id, 32));
    }
    if (trimText(options.event_type, 48)) {
        where.push("event_type=?");
        params.push(trimText(options.event_type, 48));
    }
    params.push(safeLimit(options.limit, 100, 500));
    const rows = await dbAll(
        `SELECT home_ai_events.*,
                CASE WHEN home_ai_events.event_type='emergency'
                     AND home_ai_emergency_acknowledgements.event_id IS NOT NULL
                     THEN 1 ELSE 0 END AS user_acknowledged
         FROM home_ai_events
         LEFT JOIN home_ai_emergency_acknowledgements
           ON home_ai_emergency_acknowledgements.event_id=home_ai_events.event_id
         ${where.length ? `WHERE ${where.join(" AND ")}` : ""}
         ORDER BY received_at_ms DESC
         LIMIT ?`,
        params
    );
    return rows.map(mapEventRow);
}

async function upsertVirtualDeviceStates(dbRun, body = {}) {
    const devices = Array.isArray(body.devices) ? body.devices.slice(0, 32) : [];
    if (devices.length === 0) {
        return { ok: false, code: "VIRTUAL_DEVICE_STATE_INVALID", error: "devices must be a non-empty array" };
    }
    const nowMs = Date.now();
    const stored = [];
    for (const item of devices) {
        const normalized = normalizeVirtualDeviceState(item);
        if (!normalized.ok) {
            return normalized;
        }
        const state = normalized.device;
        await dbRun(
            `INSERT INTO home_ai_virtual_devices(device_id,room_id,device_type,state_json,updated_at_ms)
             VALUES(?,?,?,?,?)
             ON CONFLICT(device_id) DO UPDATE SET
                room_id=excluded.room_id,
                device_type=excluded.device_type,
                state_json=excluded.state_json,
                updated_at_ms=excluded.updated_at_ms`,
            [state.device_id, state.room_id, state.device_type, jsonText(state), state.updated_at_ms || nowMs]
        );
        stored.push({
            device_id: state.device_id,
            room_id: state.room_id,
            device_type: state.device_type,
            state,
            updated_at_ms: state.updated_at_ms || nowMs
        });
    }
    broadcastEvent("home_ai_virtual_device_state", { devices: stored });
    return { ok: true, devices: stored };
}

async function readVirtualDevices(dbAll) {
    const rows = await dbAll("SELECT * FROM home_ai_virtual_devices ORDER BY room_id, device_id");
    return rows.map(row => ({
        device_id: row.device_id,
        room_id: row.room_id,
        device_type: row.device_type,
        state: parseJson(row.state_json, {}),
        updated_at_ms: Number(row.updated_at_ms) || null
    }));
}

async function writeUserOverride(dbRun, body = {}, dbAll = null) {
    const normalized = normalizeUserOverride(body);
    if (!normalized.ok) {
        return normalized;
    }
    const override = {
        ...normalized.override,
        override_id: normalized.override.override_id || makeId("override")
    };
    const nowMs = Date.now();
    if (typeof dbAll === "function") {
        const rows = await dbAll(
            `SELECT COUNT(*) AS count FROM home_ai_overrides
             WHERE (expires_at_ms IS NULL OR expires_at_ms>?) AND override_id<>?`,
            [nowMs, override.override_id]
        );
        if ((Number(rows[0]?.count) || 0) >= HOME_AI_MAX_OVERRIDES) {
            return { ok: false, code: "HOME_AI_OVERRIDE_LIMIT", error: "active overrides exceed the S3 fixed capacity" };
        }
    }
    await dbRun(
        `INSERT INTO home_ai_overrides
         (override_id,room_id,device_id,action,source,priority,expires_at_ms,until_condition,allow_safety_override,payload_json,created_at_ms,updated_at_ms)
         VALUES(?,?,?,?,?,?,?,?,?,?,?,?)
         ON CONFLICT(override_id) DO UPDATE SET
            room_id=excluded.room_id,
            device_id=excluded.device_id,
            action=excluded.action,
            source=excluded.source,
            priority=excluded.priority,
            expires_at_ms=excluded.expires_at_ms,
            until_condition=excluded.until_condition,
            allow_safety_override=excluded.allow_safety_override,
            payload_json=excluded.payload_json,
            updated_at_ms=excluded.updated_at_ms`,
        [
            override.override_id,
            override.scope.room_id,
            override.scope.device_id,
            override.action,
            override.source,
            override.priority,
            override.expires_at_ms,
            override.until_condition,
            override.allow_safety_override ? 1 : 0,
            jsonText(override),
            override.created_at_ms,
            nowMs
        ]
    );
    const stored = { ...override, updated_at_ms: nowMs };
    broadcastEvent("home_ai_override", stored);
    let feedback = null;
    let learningError = null;
    if (typeof dbAll === "function") {
        try {
            feedback = await recordFeedback(dbRun, {
                feedback_id: `override_${override.override_id}`,
                room_id: override.scope.room_id,
                feedback_type: "manual_override",
                payload: {
                    source: override.source,
                    override_id: override.override_id,
                    device_id: override.scope.device_id,
                    action: override.action,
                    expires_at_ms: override.expires_at_ms
                }
            }, dbAll);
        } catch (_) {
            learningError = "HOME_AI_LEARNING_UPDATE_FAILED";
        }
    }
    return { ok: true, override: stored, feedback, learning_error: learningError };
}

async function listUserOverrides(dbAll, options = {}) {
    const nowMs = Date.now();
    const includeExpired = String(options.include_expired || "") === "1";
    const rows = await dbAll(
        `SELECT * FROM home_ai_overrides
         ${includeExpired ? "" : "WHERE expires_at_ms IS NULL OR expires_at_ms>?"}
         ORDER BY priority DESC, updated_at_ms DESC
         LIMIT ?`,
        includeExpired ? [safeLimit(options.limit, 100, 200)] : [nowMs, safeLimit(options.limit, 100, 200)]
    );
    return rows.map(mapOverrideRow);
}

async function deleteUserOverride(dbRun, overrideId) {
    const id = trimText(overrideId, 160);
    if (!id) {
        return { ok: false, code: "HOME_AI_OVERRIDE_ID_INVALID", error: "override id is required" };
    }
    const result = await dbRun("DELETE FROM home_ai_overrides WHERE override_id=?", [id]);
    if (result.changes === 0) {
        return { ok: false, code: "HOME_AI_OVERRIDE_NOT_FOUND", error: "override was not found" };
    }
    const removed = { override_id: id, removed_at_ms: Date.now() };
    broadcastEvent("home_ai_override_removed", removed);
    return { ok: true, removed };
}

async function recordFeedback(dbRun, body = {}, dbAll = null) {
    const type = trimText(body.feedback_type, 32);
    if (!FEEDBACK_TYPES.has(type)) {
        return { ok: false, code: "HOME_AI_FEEDBACK_INVALID", error: "feedback_type is unsupported" };
    }
    const nowMs = Date.now();
    const feedback = {
        feedback_id: trimText(body.feedback_id, 160) || makeId("feedback"),
        decision_id: trimText(body.decision_id, 160),
        rule_id: trimText(body.rule_id, 64),
        room_id: trimText(body.room_id, 32),
        feedback_type: type,
        payload: body.payload && typeof body.payload === "object" ? body.payload : {},
        created_at_ms: nowMs
    };
    const inserted = await dbRun(
        `INSERT OR IGNORE INTO home_ai_feedback(feedback_id,decision_id,rule_id,room_id,feedback_type,payload_json,created_at_ms)
         VALUES(?,?,?,?,?,?,?)`,
        [feedback.feedback_id, feedback.decision_id, feedback.rule_id, feedback.room_id, feedback.feedback_type, jsonText(feedback.payload), nowMs]
    );
    if (inserted.changes === 0) {
        const rows = typeof dbAll === "function"
            ? await dbAll("SELECT * FROM home_ai_feedback WHERE feedback_id=? LIMIT 1", [feedback.feedback_id])
            : [];
        const existing = rows[0] ? mapFeedbackRow(rows[0]) : feedback;
        return { ok: true, duplicate: true, feedback: existing, learning: { habit: null, probation: null, candidate: null } };
    }
    let habit = null;
    let probation = null;
    let candidate = null;
    if (typeof dbAll === "function") {
        habit = await recordHabitEvidence(dbRun, dbAll, feedback);
        if (habit?.rule_candidate) {
            candidate = await evaluateRuleCandidate(
                dbRun,
                dbAll,
                habit.rule_candidate.candidate_id,
                { auto_publish: true },
                async candidatePackage => {
                    const current = await readCurrentRulePackage(dbAll);
                    return publishRulePackage(dbRun, dbAll, {
                        ...candidatePackage,
                        version: Math.max(Number(candidatePackage.version) || 1, (Number(current?.version) || 0) + 1),
                        generated_at_ms: Date.now()
                    });
                }
            );
            if (candidate.status === "PUBLISHED" && candidate.published) {
                candidate.probation_runs = await startProbationRuns(dbRun, candidate.published, {
                    candidate_id: habit.rule_candidate.candidate_id,
                    rule_ids: candidate.target_rule_ids,
                    duration_days: 3
                });
                candidate.status = await attachRuleCandidateProbation(
                    dbRun,
                    habit.rule_candidate.candidate_id,
                    candidate.probation_runs
                );
            }
        }
        probation = await recordProbationFeedback(
            dbRun,
            dbAll,
            feedback,
            rollbackBody => rollbackRulePackage(dbRun, dbAll, rollbackBody)
        );
    }
    broadcastEvent("home_ai_feedback", feedback);
    return { ok: true, feedback, learning: { habit, probation, candidate } };
}

module.exports = {
    acknowledgeEmergency,
    deleteUserOverride,
    listHomeAiEvents,
    listEmergencyAcknowledgements,
    listRuleDeployments,
    listRulePackages,
    publishRulePackage,
    rollbackRulePackage,
    readCurrentRulePackage,
    readCurrentRulePackageTransport,
    readGatewayConfigTransport,
    readRuleUpdateNotification,
    readRoomConfig,
    readVirtualDevices,
    listUserOverrides,
    recordFeedback,
    recordHomeAiEvents,
    recordRuleDeployment,
    upsertVirtualDeviceStates,
    writeUserOverride,
    writeRoomConfig
};
