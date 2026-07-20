const assert = require("assert");
const express = require("express");
const sqlite3 = require("sqlite3").verbose();
const { ensureHomeAiTables } = require("../src/db/homeAi");
const { ensureSmartHomeTables } = require("../src/db/smartHome");
const { ensureEventLogTables } = require("../src/db/eventLogs");
const {
    acknowledgeEmergency,
    publishRulePackage,
    readGatewayConfigTransport,
    readCurrentRulePackage,
    readCurrentRulePackageTransport,
    readRuleUpdateNotification,
    listHomeAiEvents,
    recordFeedback,
    recordHomeAiEvents,
    writeRoomConfig,
    writeUserOverride
} = require("../src/services/homeAiService");
const { createHomeAiRouter } = require("../src/routes/homeAiRoutes");
const {
    createMemoryCandidate,
    createRuleCandidate,
    evaluateRuleCandidate,
    listHabits,
    listProbationRuns,
    listRuleCandidates,
    startProbationRuns,
    evaluateProbationRun
} = require("../src/homeAi/learningService");
const {
    ackSmartHomeCommand,
    createSmartHomeCommand,
    listSmartHomeCommands
} = require("../src/services/smartHomeService");
const {
    executeTool,
    listTools,
    writeSetting
} = require("../src/homeAi/toolRegistry");
const {
    handleSmartHomeAck,
    listAgentDecisions,
    orchestrate
} = require("../src/homeAi/agentOrchestrator");
const { defaultRooms } = require("../src/homeAi/schema");

function helpers(db) {
    return {
        dbRun(sql, params = []) {
            return new Promise((resolve, reject) => db.run(sql, params, function (error) {
                if (error) reject(error);
                else resolve(this);
            }));
        },
        dbAll(sql, params = []) {
            return new Promise((resolve, reject) => db.all(sql, params, (error, rows) => {
                if (error) reject(error);
                else resolve(rows);
            }));
        }
    };
}

function baseRule(version = 1) {
    return {
        schema_version: 1,
        version,
        generated_at_ms: Date.now(),
        rooms: defaultRooms(),
        rules: [{
            rule_id: "bedroom_light",
            version: 1,
            rule_type: "basic_automation",
            source: "manual",
            room_id: "bedroom_01",
            conditions: [{ field: "presence_state", operator: "eq", value: "occupied" }],
            actions: [{ device_id: "bedroom_01_light", device_type: "light", action: "turn_on" }]
        }]
    };
}

function closeDatabase(db) {
    return new Promise((resolve, reject) => db.close(error => error ? reject(error) : resolve()));
}

async function runRoomMigrationTests() {
    {
        const db = new sqlite3.Database(":memory:");
        const { dbRun, dbAll } = helpers(db);
        await ensureHomeAiTables(dbRun, dbAll);
        const rulePackage = baseRule(1);
        rulePackage.rules[0] = {
            ...rulePackage.rules[0],
            room_id: "living_room",
            actions: [{ device_id: "living_room_light", device_type: "light", action: "turn_on" }]
        };
        assert.equal((await publishRulePackage(dbRun, dbAll, rulePackage)).ok, true);
        const renamedRooms = defaultRooms();
        renamedRooms[0] = { ...renamedRooms[0], room_id: "family_room", room_name: "家庭厅" };
        const written = await writeRoomConfig(dbRun, dbAll, { rooms: renamedRooms });
        assert.deepEqual(written.config.migration.migrated_rule_ids, ["bedroom_light"]);
        assert.deepEqual(written.config.migration.pending_rebind_rule_ids, []);
        const current = await readCurrentRulePackage(dbAll);
        assert.equal(current.version, 2);
        assert.equal(current.rules[0].room_id, "family_room");
        assert.equal(current.rules[0].enabled, true);
        assert.equal(current.rules[0].binding.state, "MIGRATED");
        const transport = await readCurrentRulePackageTransport(dbAll);
        assert.equal(Object.prototype.hasOwnProperty.call(JSON.parse(transport.payload).rules[0], "binding"), false);
        const notification = await readRuleUpdateNotification(dbAll, 1);
        assert.equal(notification.reason, "room_config_migration");
        await closeDatabase(db);
    }

    {
        const db = new sqlite3.Database(":memory:");
        const { dbRun, dbAll } = helpers(db);
        await ensureHomeAiTables(dbRun, dbAll);
        const initialRooms = [
            {
                ...defaultRooms()[0],
                room_id: "shared_zone",
                sensing_sources: ["s3_local", "sensair_shuttle_01"],
                voice_terminal_device_id: "sensair_shuttle_01"
            },
            defaultRooms()[2]
        ];
        assert.equal((await writeRoomConfig(dbRun, dbAll, { rooms: initialRooms })).ok, true);
        const rulePackage = baseRule(1);
        rulePackage.rooms = initialRooms;
        rulePackage.rules[0] = {
            ...rulePackage.rules[0],
            room_id: "shared_zone",
            actions: [{ device_id: "shared_zone_light", device_type: "light", action: "turn_on" }]
        };
        assert.equal((await publishRulePackage(dbRun, dbAll, rulePackage)).ok, true);
        const written = await writeRoomConfig(dbRun, dbAll, { rooms: defaultRooms() });
        assert.deepEqual(written.config.migration.pending_rebind_rule_ids, ["bedroom_light"]);
        const current = await readCurrentRulePackage(dbAll);
        assert.equal(current.rules[0].enabled, false);
        assert.equal(current.rules[0].binding.state, "PENDING_REBIND");
        assert.deepEqual(current.rules[0].binding.source_ids, ["s3_local", "sensair_shuttle_01"]);
        assert.equal((await readRuleUpdateNotification(dbAll, 1)).reason, "room_config_migration");
        await closeDatabase(db);
    }

    {
        const db = new sqlite3.Database(":memory:");
        const { dbRun, dbAll } = helpers(db);
        await ensureHomeAiTables(dbRun, dbAll);
        const rulePackage = baseRule(1);
        rulePackage.rules[0] = {
            ...rulePackage.rules[0],
            room_id: "living_room",
            actions: [{ device_id: "living_room_light", device_type: "light", action: "turn_on" }]
        };
        assert.equal((await publishRulePackage(dbRun, dbAll, rulePackage)).ok, true);
        let failed = false;
        const failingDbRun = async (sql, params = []) => {
            if (!failed && String(sql).includes("INSERT INTO home_ai_rule_packages")) {
                failed = true;
                throw new Error("forced migration insert failure");
            }
            return dbRun(sql, params);
        };
        const renamedRooms = defaultRooms();
        renamedRooms[0] = { ...renamedRooms[0], room_id: "family_room" };
        await assert.rejects(
            writeRoomConfig(failingDbRun, dbAll, { rooms: renamedRooms }),
            /forced migration insert failure/
        );
        const roomsAfterRollback = await dbAll("SELECT * FROM home_ai_rooms");
        assert.equal(roomsAfterRollback.length, 0);
        assert.equal((await readCurrentRulePackage(dbAll)).version, 1);
        assert.equal((await dbAll("SELECT COUNT(*) AS count FROM home_ai_rule_packages"))[0].count, 1);
        assert.equal((await dbAll("SELECT COUNT(*) AS count FROM home_ai_rule_notifications"))[0].count, 1);
        await closeDatabase(db);
    }
}

(async () => {
    await runRoomMigrationTests();
    const db = new sqlite3.Database(":memory:");
    const { dbRun, dbAll } = helpers(db);
    await ensureEventLogTables(dbRun, dbAll);
    await ensureSmartHomeTables(dbRun, dbAll);
    await ensureHomeAiTables(dbRun, dbAll);

    const first = await publishRulePackage(dbRun, dbAll, baseRule(1));
    assert.equal(first.ok, true);
    const notification = await readRuleUpdateNotification(dbAll, 0);
    assert.equal(notification.update_available, true);
    assert.equal(notification.package_version, 1);
    assert.equal((await readRuleUpdateNotification(dbAll, 1)).update_available, false);

    const configuredRooms = defaultRooms();
    configuredRooms[0] = {
        ...configuredRooms[0],
        room_id: "family_room",
        room_name: "家庭厅",
        vacant_confirm_ms: 90000
    };
    const roomWrite = await writeRoomConfig(dbRun, dbAll, { rooms: configuredRooms });
    assert.equal(roomWrite.ok, true);
    const overrideWrite = await writeUserOverride(dbRun, {
        override_id: "web_keep_light_off",
        scope: { room_id: "bedroom_01", device_id: "bedroom_01_light" },
        action: "keep_off",
        source: "web_user_command",
        priority: 910,
        expires_at_ms: Date.now() + 60000,
        until_condition: "user_restores_automation"
    }, dbAll);
    assert.equal(overrideWrite.ok, true);
    const gatewayConfig = await readGatewayConfigTransport(dbAll);
    const gatewayPayload = JSON.parse(gatewayConfig.payload);
    assert.equal(gatewayConfig.checksum.length, 64);
    assert.equal(gatewayPayload.rooms.length, 3);
    assert.equal(gatewayPayload.rooms[0].room_id, "bedroom_01");
    assert(gatewayPayload.rooms.some(room => room.room_id === "family_room" && room.vacant_confirm_ms === 90000));
    assert.equal(gatewayPayload.overrides.length, 1);
    assert.equal(gatewayPayload.overrides[0].override_id, "web_keep_light_off");
    assert.equal(gatewayPayload.weather_context.available, false);
    assert.deepEqual(gatewayPayload.emergency_acknowledgements, []);
    assert.equal((await readRuleUpdateNotification(dbAll, 1, gatewayConfig.checksum)).config_update_available, false);
    const emergencyEventId = "env_00000000000000aa";
    const emergencyOccurredAt = Date.now();
    assert.equal((await recordHomeAiEvents(dbRun, {
        events: [{
            event_id: emergencyEventId,
            event_type: "emergency",
            room_id: "bedroom_01",
            priority: 1000,
            occurred_at_ms: emergencyOccurredAt,
            schema_version: 1,
            payload: { state: "ACTIVE_UNACKNOWLEDGED" }
        }]
    }, "sensair_s3_gateway_01", dbAll)).accepted, 1);
    const emergencyAck = await acknowledgeEmergency(dbRun, dbAll, emergencyEventId, { source: "test" });
    assert.equal(emergencyAck.ok, true);
    assert.equal(emergencyAck.acknowledgement.duplicate, false);
    assert.equal((await acknowledgeEmergency(dbRun, dbAll, emergencyEventId, { source: "test" })).acknowledgement.duplicate, true);
    assert.equal((await listHomeAiEvents(dbAll, { event_type: "emergency", limit: 10 }))[0].user_acknowledged, true);
    const emergencyConfig = await readGatewayConfigTransport(dbAll);
    assert.equal(JSON.parse(emergencyConfig.payload).emergency_acknowledgements[0].event_id, emergencyEventId);
    assert.equal((await readRuleUpdateNotification(dbAll, 1, gatewayConfig.checksum)).config_update_available, true);
    assert.equal((await recordHomeAiEvents(dbRun, {
        events: [{
            event_id: emergencyEventId,
            event_type: "emergency",
            room_id: "bedroom_01",
            priority: 1000,
            occurred_at_ms: emergencyOccurredAt - 1,
            schema_version: 1,
            payload: { state: "DETECTED" }
        }]
    }, "sensair_s3_gateway_01", dbAll)).accepted, 0);
    const overrideFeedbackEvent = {
        events: [{
            event_id: "web_keep_light_off",
            event_type: "feedback",
            room_id: "bedroom_01",
            priority: 910,
            occurred_at_ms: Date.now(),
            schema_version: 1,
            payload: {
                feedback_type: "manual_override",
                override_id: "web_keep_light_off",
                device_id: "bedroom_01_light",
                action: "keep_off"
            }
        }]
    };
    assert.equal((await recordHomeAiEvents(dbRun, overrideFeedbackEvent, "sensair_s3_gateway_01", dbAll)).accepted, 1);
    assert.equal((await recordHomeAiEvents(dbRun, overrideFeedbackEvent, "sensair_s3_gateway_01", dbAll)).accepted, 0);
    assert.equal((await dbAll("SELECT COUNT(*) AS count FROM home_ai_feedback WHERE feedback_id='override_web_keep_light_off'"))[0].count, 1);

    for (let index = 0; index < 7; index += 1) {
        const feedback = await recordFeedback(dbRun, {
            feedback_id: `feedback_${index}`,
            rule_id: "bedroom_light",
            room_id: "bedroom_01",
            feedback_type: "accepted",
            payload: { explicit: true }
        }, dbAll);
        assert.equal(feedback.ok, true);
    }
    const habits = await listHabits(dbAll, { room_id: "bedroom_01" });
    const ruleHabit = habits.find(item => item.pattern.rule_id === "bedroom_light");
    const overrideHabit = habits.find(item => !item.pattern.rule_id);
    assert(ruleHabit);
    assert(overrideHabit);
    assert.equal(ruleHabit.status, "ACTIVE");
    assert.equal(ruleHabit.evidence_count, 7);
    assert.equal(overrideHabit.evidence_count, 1);

    const habitPackage = baseRule(10);
    habitPackage.rules[0] = {
        ...habitPackage.rules[0],
        rule_id: "habit_light",
        room_id: "bedroom_02",
        actions: [{ device_id: "bedroom_02_light", device_type: "light", action: "turn_on" }]
    };
    habitPackage.rules.push({
        ...baseRule(10).rules[0],
        rule_id: "existing_bedroom_fan",
        actions: [{ device_id: "bedroom_01_fan", device_type: "fan", action: "turn_on" }]
    });
    const replayNow = Date.now();
    const replay = await recordHomeAiEvents(dbRun, {
        events: Array.from({ length: 5 }, (_, index) => ({
            event_id: `habit_room_state_${index}`,
            event_type: "room_state",
            room_id: "bedroom_02",
            priority: 400,
            occurred_at_ms: replayNow - (4 - index) * 60 * 60 * 1000,
            schema_version: 1,
            payload: {
                presence_state: "occupied",
                stable_target_count: 1,
                occupancy_mode: "single",
                environment_fresh: true,
                radar_fresh: true
            }
        }))
    }, "sensair_s3_gateway_01", dbAll);
    assert.equal(replay.accepted, 5);
    for (let index = 0; index < 7; index += 1) {
        const evidence = await recordFeedback(dbRun, {
            feedback_id: `habit_feedback_${index}`,
            rule_id: "habit_light",
            room_id: "bedroom_02",
            feedback_type: "accepted",
            payload: { explicit: true, rule_package: habitPackage }
        }, dbAll);
        assert.equal(evidence.ok, true);
    }
    const generatedCandidates = await listRuleCandidates(dbAll, { status: "PROBATION" });
    const generatedCandidate = generatedCandidates.find(item => item.source.generated_from_habit === true);
    assert(generatedCandidate);
    assert.equal(generatedCandidate.gates_passed, true);
    assert.equal(generatedCandidate.gates.historical_replay, true);
    assert.equal(generatedCandidate.rule_package.rules[0].source, "habit_learning");
    assert.equal((await readCurrentRulePackage(dbAll)).version, 10);
    const initialProbationRuns = await listProbationRuns(dbAll, { status: "RUNNING" });
    assert.equal(initialProbationRuns.length, 1);
    assert.equal(initialProbationRuns[0].rule_id, "habit_light");

    const memory = await createMemoryCandidate(dbRun, {
        room_id: "bedroom_01",
        category: "preference",
        content: "用户偏好安静",
        confidence: 0.9
    });
    assert.equal(memory.ok, true);
    const memoryUpdate = await require("../src/homeAi/learningService").updateMemoryCandidate(
        dbRun,
        dbAll,
        memory.candidate.candidate_id,
        { status: "CONFIRMED", automation_allowed: false }
    );
    assert.equal(memoryUpdate.ok, true);
    const confirmed = await dbAll("SELECT * FROM home_ai_confirmed_memories WHERE candidate_id=?", [memory.candidate.candidate_id]);
    assert.equal(confirmed.length, 1);
    assert.equal(Number(confirmed[0].automation_allowed), 0);
    const disabledMemory = await require("../src/homeAi/learningService").updateMemoryCandidate(
        dbRun,
        dbAll,
        memory.candidate.candidate_id,
        { status: "DISABLED" }
    );
    assert.equal(disabledMemory.ok, true);
    assert.equal((await dbAll("SELECT * FROM home_ai_confirmed_memories WHERE candidate_id=?", [memory.candidate.candidate_id])).length, 0);

    const candidate = await createRuleCandidate(dbRun, {
        rule_package: baseRule(11),
        confidence: 0.9,
        sample_count: 5,
        source: { explicit_feedback: true }
    });
    assert.equal(candidate.ok, true);
    const evaluated = await evaluateRuleCandidate(dbRun, dbAll, candidate.candidate.candidate_id, {
        gates: {
            conflict_free: true,
            historical_replay_passed: true,
            safety_review_passed: true,
            predicted_trigger_rate: 2
        }
    });
    assert.equal(evaluated.status, "REJECTED");
    assert.equal(evaluated.gates.explicit_evidence, false);

    const noEvidenceCandidate = await createRuleCandidate(dbRun, {
        rule_package: baseRule(12),
        confidence: 0.95,
        sample_count: 8,
        source: { generated_from_habit: false }
    });
    assert.equal(noEvidenceCandidate.ok, true);
    const noEvidenceEvaluation = await evaluateRuleCandidate(dbRun, dbAll, noEvidenceCandidate.candidate.candidate_id, {
        gates: {
            conflict_free: true,
            historical_replay_passed: true,
            safety_review_passed: true,
            predicted_trigger_rate: 2
        }
    });
    assert.equal(noEvidenceEvaluation.status, "REJECTED");
    assert.equal(noEvidenceEvaluation.gates.explicit_evidence, false);

    const current = await readCurrentRulePackage(dbAll);
    const generatedRuns = await listProbationRuns(dbAll, { status: "RUNNING" });
    assert.equal(generatedRuns.length, 1);
    const probation = await evaluateProbationRun(dbRun, dbAll, generatedRuns[0].run_id, {
        metrics: { trigger_count: 3, failure_count: 1, override_count: 0 }
    });
    assert.equal(probation.status, "ROLLED_BACK");

    const autoRuns = await startProbationRuns(dbRun, current, {
        duration_days: 3,
        candidate_id: generatedCandidate.candidate_id,
        rule_ids: ["habit_light"]
    });
    assert.equal(autoRuns.length, 1);
    for (let index = 0; index < 3; index += 1) {
        const negative = await recordFeedback(dbRun, {
            feedback_id: `auto_negative_${index}`,
            rule_id: "habit_light",
            room_id: "bedroom_02",
            feedback_type: "rejected",
            payload: { explicit: true }
        }, dbAll);
        assert.equal(negative.ok, true);
    }
    const autoRunRow = (await dbAll("SELECT * FROM home_ai_rule_probation_runs WHERE run_id=?", [autoRuns[0].run_id]))[0];
    assert.equal(autoRunRow.status, "ROLLED_BACK");
    assert.equal((await dbAll("SELECT status FROM home_ai_rule_candidates WHERE candidate_id=?", [generatedCandidate.candidate_id]))[0].status, "SUSPENDED");
    assert((await listRuleCandidates(dbAll, { status: "SUSPENDED" })).some(item => item.candidate_id === generatedCandidate.candidate_id));

    const eventPackage = await readCurrentRulePackage(dbAll);
    const eventRuns = await startProbationRuns(dbRun, eventPackage, { duration_days: 3 });
    assert.equal(eventRuns.length, 1);
    const failedDecisionEvents = {
        events: Array.from({ length: 3 }, (_, index) => ({
            event_id: `probation_failed_decision_${index}`,
            event_type: "decision",
            room_id: eventPackage.rules[0].room_id,
            priority: 500,
            occurred_at_ms: Date.now() + index,
            schema_version: 1,
            payload: {
                rule_id: eventPackage.rules[0].rule_id,
                device_id: eventPackage.rules[0].actions[0].device_id,
                action: "turn_on",
                execution_result: "rejected",
                reason: "virtual_execution_rejected"
            }
        }))
    };
    const eventProbationUpdate = await recordHomeAiEvents(
        dbRun,
        failedDecisionEvents,
        "sensair_s3_gateway_01",
        dbAll
    );
    assert.equal(eventProbationUpdate.accepted, 3);
    assert.equal(eventProbationUpdate.learning_errors.length, 0);
    assert.equal((await dbAll("SELECT status FROM home_ai_rule_probation_runs WHERE run_id=?", [eventRuns[0].run_id]))[0].status, "ROLLED_BACK");
    assert.equal((await recordHomeAiEvents(dbRun, failedDecisionEvents, "sensair_s3_gateway_01", dbAll)).accepted, 0);

    await writeSetting(dbRun, "home_location", {
        city: "上海",
        latitude: 31.23,
        longitude: 121.47,
        timezone: "Asia/Shanghai"
    });
    assert(listTools().some(tool => tool.name === "get_weather"));
    const weather = await executeTool("get_weather", {}, {
        dbRun,
        dbAll,
        fetchImpl: async () => ({
            ok: true,
            json: async () => ({ current: { time: Math.floor(Date.now() / 1000), temperature_2m: 25, relative_humidity_2m: 60, weather_code: 3, is_day: 0 } })
        })
    });
    assert.equal(weather.ok, true);
    assert.equal(weather.result.fresh, true);
    assert.equal(weather.result.dark, true);
    const weatherGatewayConfig = await readGatewayConfigTransport(dbAll);
    const weatherGatewayPayload = JSON.parse(weatherGatewayConfig.payload);
    assert.equal(weatherGatewayPayload.weather_context.available, true);
    assert.equal(weatherGatewayPayload.weather_context.dark, true);

    await assert.rejects(
        executeTool("get_weather", {}, {
            dbRun,
            dbAll,
            fetchImpl: async () => ({
                ok: true,
                json: async () => ({ current: { time: Math.floor(Date.now() / 1000) + 3600, temperature_2m: 25, is_day: 1 } })
            })
        }),
        error => error.code === "WEATHER_STALE"
    );
    assert.equal(JSON.parse((await readGatewayConfigTransport(dbAll)).payload).weather_context.available, false);

    const previousNewsUrl = process.env.HOME_AI_NEWS_API_URL;
    delete process.env.HOME_AI_NEWS_API_URL;
    await dbRun("DELETE FROM home_ai_tool_settings WHERE setting_key='news_provider'");
    await assert.rejects(
        executeTool("get_news", {}, { dbRun, dbAll }),
        error => error.code === "NEWS_NOT_CONFIGURED" && error.status === 409
    );
    await writeSetting(dbRun, "news_provider", { endpoint: "not a URL", api_key: "test" });
    await assert.rejects(
        executeTool("get_news", {}, { dbRun, dbAll }),
        error => error.code === "NEWS_NOT_CONFIGURED" && error.status === 409
    );
    await writeSetting(dbRun, "news_provider", { endpoint: "http://news.example.test/v1", api_key: "test" });
    await assert.rejects(
        executeTool("get_news", {}, { dbRun, dbAll }),
        error => error.code === "NEWS_NOT_CONFIGURED" && error.status === 409
    );
    if (previousNewsUrl === undefined) delete process.env.HOME_AI_NEWS_API_URL;
    else process.env.HOME_AI_NEWS_API_URL = previousNewsUrl;

    await writeSetting(dbRun, "news_provider", { endpoint: "https://news.example.test/v1", api_key: "test" });
    await assert.rejects(
        executeTool("get_news", { limit: 2 }, {
            dbRun,
            dbAll,
            fetchImpl: async () => ({
                ok: true,
                json: async () => ({ articles: [{ title: "未来新闻", publishedAt: new Date(Date.now() + 60000).toISOString() }] })
            })
        }),
        error => error.code === "NEWS_STALE"
    );

    const briefingIntent = {
        type: "briefing",
        confidence: 0.95,
        scene: "user_requested",
        room_id: "living_room",
        presence_state: "occupied",
        presence_confidence: 0.95,
        data_valid: true,
        voice_terminal_available: false,
        query: "家庭简报"
    };
    const briefing = await orchestrate({
        intent: briefingIntent
    }, {
        dbRun,
        dbAll,
        fetchImpl: async url => String(url).includes("open-meteo") ? ({
            ok: true,
            json: async () => ({ current: { time: Math.floor(Date.now() / 1000), temperature_2m: 24, relative_humidity_2m: 55, weather_code: 1, is_day: 1 } })
        }) : ({
            ok: true,
            json: async () => ({ articles: [{ title: "家庭新闻", publishedAt: new Date().toISOString() }] })
        })
    });
    assert.equal(briefing.ok, true);
    assert.equal(briefing.decision.response_type, "scene_briefing");
    assert.equal(briefing.decision.speech_policy.channel, "web_only");
    assert.equal(briefing.decision.status, "COMPLETED");
    assert(briefing.decision.speech.final.includes("24 摄氏度"));
    assert(briefing.decision.speech.final.includes("家庭新闻"));
    const duplicateBriefing = await orchestrate({
        intent: briefingIntent
    }, { dbRun, dbAll });
    assert.equal(duplicateBriefing.decision.status, "SUPPRESSED");
    assert.equal(duplicateBriefing.decision.suppression_code, "BRIEFING_DUPLICATE");

    const planned = await orchestrate({
        execute: false,
        intent: {
            type: "control",
            confidence: 0.99,
            room_id: "bedroom_01",
            device_id: "bedroom_01_light",
            action: "turn_on"
        }
    }, { dbRun, dbAll });
    assert.equal(planned.ok, true);
    assert.equal(planned.decision.response_type, "complex_command");
    assert.equal(planned.decision.speech.final, "");
    assert.equal(Array.isArray(planned.decision.steps), true);
    assert.equal(planned.decision.speech_policy.based_on_actual_results, true);

    const disallowed = await orchestrate({
        execute: false,
        intent: {
            type: "news",
            confidence: 0.99,
            actions: [{ tool: "control_virtual_device", args: { device_id: "bedroom_01_light", action: "turn_on" } }]
        }
    }, { dbRun, dbAll });
    assert.equal(disallowed.ok, false);
    assert.equal(disallowed.code, "AGENT_TOOL_NOT_ALLOWED");

    const executed = await orchestrate({
        intent: {
            type: "control",
            confidence: 0.99,
            room_id: "bedroom_01",
            device_id: "bedroom_01_light",
            action: "turn_on"
        }
    }, { dbRun, dbAll });
    assert.equal(executed.ok, true);
    assert.equal(executed.decision.status, "WAITING_ACK");
    assert.equal(executed.decision.speech.final, "命令已提交，等待设备确认。");
    assert.equal(executed.decision.speech.final.includes("command_id"), false);
    const executedCommands = await listSmartHomeCommands(dbAll, { limit: 20 });
    const executedCommand = executedCommands.find(command => command.decision_id === executed.decision.decision_id);
    assert(executedCommand);
    const executedAck = await ackSmartHomeCommand(dbRun, dbAll, executedCommand.command_id, {
        status: "succeeded",
        result: { applied: true, verified: true, execution_mode: "virtual" }
    });
    assert.equal(executedAck.ok, true);
    const completed = (await listAgentDecisions(dbAll, { decision_id: executed.decision.decision_id }))[0];
    assert.equal(completed.status, "COMPLETED");
    assert.equal(completed.execution.steps[0].actions[0].result.result.verified, true);

    let releaseFinalPersist;
    let markFinalPersistBlocked;
    const finalPersistBlocked = new Promise(resolve => { markFinalPersistBlocked = resolve; });
    const finalPersistRelease = new Promise(resolve => { releaseFinalPersist = resolve; });
    let delayed = false;
    const delayedDbRun = async (sql, params = []) => {
        if (!delayed && String(sql).includes("INSERT INTO home_ai_agent_decisions") && params[6] === "WAITING_ACK") {
            delayed = true;
            markFinalPersistBlocked();
            await finalPersistRelease;
        }
        return dbRun(sql, params);
    };
    const racedPromise = orchestrate({
        intent: {
            type: "control",
            confidence: 0.99,
            room_id: "bedroom_01",
            device_id: "bedroom_01_fan",
            action: "turn_on"
        }
    }, { dbRun: delayedDbRun, dbAll });
    await Promise.race([
        finalPersistBlocked,
        new Promise((_, reject) => setTimeout(() => reject(new Error("final decision persist was not reached")), 2000))
    ]);
    const racedCommands = await listSmartHomeCommands(dbAll, { limit: 20 });
    const racedCommand = racedCommands.find(command => command.target_id === "bedroom_01_fan" && command.status === "queued");
    assert(racedCommand);
    const earlyAck = await ackSmartHomeCommand(dbRun, dbAll, racedCommand.command_id, {
        status: "succeeded",
        result: { applied: true, verified: true, execution_mode: "virtual" }
    });
    assert.equal(earlyAck.ok, true);
    assert.equal((await dbAll("SELECT COUNT(*) AS count FROM home_ai_agent_acks WHERE command_id=?", [racedCommand.command_id]))[0].count, 1);
    releaseFinalPersist();
    const raced = await racedPromise;
    assert.equal(raced.decision.status, "COMPLETED");
    assert.equal((await dbAll("SELECT COUNT(*) AS count FROM home_ai_agent_acks WHERE command_id=?", [racedCommand.command_id]))[0].count, 0);

    const orphanDecisionId = "decision_orphan_ack";
    const orphanCommand = await createSmartHomeCommand(dbRun, {
        target_id: "bedroom_01_fan",
        action: "turn_off",
        decision_id: orphanDecisionId
    });
    const orphanAck = await handleSmartHomeAck({ dbRun, dbAll }, {
        ...orphanCommand.command,
        status: "failed",
        result: { applied: false },
        error_message: "test failure"
    });
    assert.equal(orphanAck.ok, true);
    assert.equal(orphanAck.pending, true);

    const app = express();
    app.use(express.json());
    app.use(createHomeAiRouter({ dbRun, dbAll, logger: { error() {} } }));
    const server = await new Promise(resolve => {
        const listener = app.listen(0, "127.0.0.1", () => resolve(listener));
    });
    try {
        const response = await fetch(
            `http://127.0.0.1:${server.address().port}/api/home-ai/v1/overrides/web_keep_light_off`,
            { method: "DELETE" }
        );
        assert.equal(response.status, 200);
        const body = await response.json();
        assert.equal(body.data.override_id, "web_keep_light_off");
        assert.equal((await readRuleUpdateNotification(dbAll, 1, gatewayConfig.checksum)).config_update_available, true);

        const decisionListResponse = await fetch(
            `http://127.0.0.1:${server.address().port}/api/home-ai/v1/agent/decisions?limit=10`
        );
        assert.equal(decisionListResponse.status, 200);
        const decisionListBody = await decisionListResponse.json();
        assert(decisionListBody.data.decisions.some(decision => decision.decision_id === executed.decision.decision_id));
        const decisionResponse = await fetch(
            `http://127.0.0.1:${server.address().port}/api/home-ai/v1/agent/decisions/${encodeURIComponent(executed.decision.decision_id)}`
        );
        assert.equal(decisionResponse.status, 200);
        const decisionBody = await decisionResponse.json();
        assert.equal(decisionBody.data.status, "COMPLETED");

        const emergencyAckResponse = await fetch(
            `http://127.0.0.1:${server.address().port}/api/home-ai/v1/emergencies/${encodeURIComponent(emergencyEventId)}/acknowledge`,
            {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ source: "web_test" })
            }
        );
        assert.equal(emergencyAckResponse.status, 200);
        assert.equal((await emergencyAckResponse.json()).data.duplicate, true);

        const invalidLocationResponse = await fetch(
            `http://127.0.0.1:${server.address().port}/api/home-ai/v1/tools/settings/home-location`,
            {
                method: "PUT",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ city: "上海", latitude: 31.23, longitude: 121.47, timezone: "Mars/Base" })
            }
        );
        assert.equal(invalidLocationResponse.status, 400);
        const invalidLocationBody = await invalidLocationResponse.json();
        assert.equal(invalidLocationBody.error.code, "HOME_LOCATION_INVALID");
    } finally {
        await new Promise(resolve => server.close(resolve));
    }

    assert.equal((await recordHomeAiEvents(dbRun, {
        events: [{
            event_id: emergencyEventId,
            event_type: "emergency",
            room_id: "bedroom_01",
            priority: 1000,
            occurred_at_ms: emergencyOccurredAt + 1,
            schema_version: 1,
            payload: { state: "RESOLVED" }
        }]
    }, "sensair_s3_gateway_01", dbAll)).accepted, 1);
    assert.equal((await dbAll("SELECT COUNT(*) AS count FROM home_ai_emergency_acknowledgements WHERE event_id=?", [emergencyEventId]))[0].count, 0);

    await closeDatabase(db);
    process.stdout.write("home ai control-plane tests: PASS\n");
})().catch(error => {
    console.error(error);
    process.exitCode = 1;
});
