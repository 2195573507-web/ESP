const assert = require("assert");
const sqlite3 = require("sqlite3").verbose();
const { ensureEventLogTables } = require("../src/db/eventLogs");
const { ensureSmartHomeTables } = require("../src/db/smartHome");
const { ensureHomeAiTables } = require("../src/db/homeAi");
const { writeRoomConfig } = require("../src/services/homeAiService");
const {
    buildPlan
} = require("../src/homeAi/agentOrchestrator");
const {
    requestHomeAiVoiceDecision,
    selectVoicePromptProfile
} = require("../src/homeAi/voiceAgentService");
const { defaultRooms } = require("../src/homeAi/schema");

function helpers(db) {
    return {
        dbRun(sql, params = []) {
            return new Promise((resolve, reject) => db.run(sql, params, function onRun(error) {
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

function close(db) {
    return new Promise((resolve, reject) => db.close(error => error ? reject(error) : resolve()));
}

(async () => {
    assert.equal(selectVoicePromptProfile("随便聊聊今天的事"), "conversation");
    assert.equal(selectVoicePromptProfile("打开卧室灯"), "plan");
    assert.equal(selectVoicePromptProfile("现在温度多少"), "tool_calling");

    const plan = buildPlan({
        type: "control",
        confidence: 0.99,
        room_id: "bedroom_01",
        raw: {
            steps: [
                { actions: [{ tool: "get_device_state", args: { device_id: "bedroom_01_light" } }] },
                {
                    precondition: { type: "previous_step_succeeded", step_index: 0 },
                    actions: [{ tool: "control_virtual_device", args: { device_id: "bedroom_01_light", action: "turn_on" } }]
                }
            ]
        }
    });
    assert.equal(plan.ok, true);
    assert.deepEqual(plan.plan.steps[1].precondition, { type: "previous_step_succeeded", step_index: 0 });
    const invalidPrecondition = buildPlan({
        type: "control",
        confidence: 0.99,
        raw: {
            steps: [{ actions: [{ tool: "get_device_state", args: { device_id: "bedroom_01_light" } }] }, {
                precondition: { type: "unsupported", step_index: 0 },
                actions: [{ tool: "get_device_state", args: { device_id: "bedroom_01_light" } }]
            }]
        }
    });
    assert.equal(invalidPrecondition.code, "AGENT_PRECONDITION_INVALID");

    const db = new sqlite3.Database(":memory:");
    const { dbRun, dbAll } = helpers(db);
    await ensureEventLogTables(dbRun, dbAll);
    await ensureSmartHomeTables(dbRun, dbAll);
    await ensureHomeAiTables(dbRun, dbAll);
    await writeRoomConfig(dbRun, dbAll, { rooms: defaultRooms() });

    const gatewayConfig = {
        apiKey: "test-key",
        chat: {
            endpoint: "https://llm.example.test/v1/chat/completions",
            baseUrl: "https://llm.example.test",
            path: "/v1/chat/completions",
            model: "test-model"
        }
    };
    let requestConfig;
    const decision = await requestHomeAiVoiceDecision(
        "打开卧室灯",
        gatewayConfig,
        new AbortController().signal,
        {
            dbRun,
            dbAll,
            deviceId: "sensair_shuttle_01",
            requestModel: async (prompt, config) => {
                requestConfig = { prompt, config };
                return {
                    model: "test-model",
                    text: JSON.stringify({
                        route: "home_ai",
                        intent: {
                            type: "control",
                            confidence: 0.99,
                            room_id: "bedroom_01",
                            device_id: "bedroom_01_light",
                            action: "turn_on"
                        }
                    })
                };
            }
        }
    );
    assert.equal(decision.handled, true);
    assert.equal(decision.decision_status, "WAITING_ACK");
    assert.equal(decision.text.includes("已完成"), false);
    assert.equal(requestConfig.config.jsonMode, true);
    assert.match(requestConfig.config.systemPrompt, /复杂命令计划层/);
    assert.match(requestConfig.prompt, /bedroom_01_light/);
    const commands = await dbAll("SELECT * FROM smart_home_commands WHERE decision_id=?", [decision.decision_id]);
    assert.equal(commands.length, 1);
    assert.equal(commands[0].target_id, "bedroom_01_light");

    const malformed = await requestHomeAiVoiceDecision(
        "打开卧室灯",
        gatewayConfig,
        new AbortController().signal,
        {
            dbRun,
            dbAll,
            deviceId: "sensair_shuttle_01",
            requestModel: async () => ({ text: "```json {bad}```", model: "test-model" })
        }
    );
    assert.equal(malformed.handled, true);
    assert.equal(malformed.code, "AGENT_JSON_INVALID");
    assert.match(malformed.text, /安全解析/);

    const ordinary = await requestHomeAiVoiceDecision(
        "讲个故事",
        gatewayConfig,
        new AbortController().signal,
        { dbRun, dbAll, deviceId: "sensair_shuttle_01" }
    );
    assert.equal(ordinary.handled, false);
    assert.equal(ordinary.profile, "conversation");

    await close(db);
    process.stdout.write("home ai voice-agent tests: PASS\n");
})().catch(error => {
    console.error(error);
    process.exitCode = 1;
});
