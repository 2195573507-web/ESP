const assert = require("assert");
const fs = require("fs");
const http = require("http");
const os = require("os");
const path = require("path");
const { spawn } = require("child_process");
const sqlite3 = require("sqlite3").verbose();
const {
    normalizeTtsPcmBuffer
} = require("../src/voice/ttsAudio");
const {
    LLM_METADATA_MAX_CHARS,
    readLlmTextRequest
} = require("../src/llm/textClient");
const {
    listPendingCommands,
    upsertDeviceCapabilities
} = require("../src/commands/queue");
const {
    ensureCommandTables
} = require("../src/db/commands");
const {
    ensureAgentStateTables
} = require("../src/db/agentState");
const {
    ensureMemoryTables
} = require("../src/db/memory");
const {
    ensureUserDataDeletionTables
} = require("../src/db/userDataDeletion");
const {
    createDatabase,
    createDbHelpers
} = require("../src/db/sqlite");
const {
    runUpdateThenInsert
} = require("../src/db/upsert");
const {
    upsertEnvironmentProfile,
    upsertLcdStatus
} = require("../src/agent/stateStore");
const {
    upsertProfile
} = require("../src/memory/store");

const SERVER_START_TIMEOUT_MS = 15000;
const SERVER_STOP_TIMEOUT_MS = 5000;
const SMOKE_GATEWAY_ID = "sensair_s3_gateway_01";
const VERIFY_GATEWAY_ID = "verify-s3";
const SMOKE_GATEWAY_HEADERS = {
    "X-Gateway-Id": SMOKE_GATEWAY_ID
};
const VERIFY_GATEWAY_HEADERS = {
    "X-Gateway-Id": VERIFY_GATEWAY_ID
};
const USER_DATA_DELETE_TOKEN = "smoke-user-data-token";
const USER_DATA_HEADERS = {
    "X-Admin-Token": USER_DATA_DELETE_TOKEN
};

function makeTempDir() {
    return fs.mkdtempSync(path.join(os.tmpdir(), "esp-server-smoke-"));
}

function waitForServer(child) {
    return new Promise((resolve, reject) => {
        let output = "";
        const timer = setTimeout(() => {
            cleanup();
            reject(new Error(`server did not start within ${SERVER_START_TIMEOUT_MS}ms\n${output}`));
        }, SERVER_START_TIMEOUT_MS);

        function cleanup() {
            clearTimeout(timer);
            child.stdout.off("data", onData);
            child.stderr.off("data", onData);
            child.off("exit", onExit);
        }

        function onData(chunk) {
            output += chunk.toString();
            if (output.includes("Server running on port")) {
                cleanup();
                resolve(output);
            }
        }

        function onExit(code) {
            cleanup();
            reject(new Error(`server exited before startup code=${code}\n${output}`));
        }

        child.stdout.on("data", onData);
        child.stderr.on("data", onData);
        child.on("exit", onExit);
    });
}

function stopServer(child) {
    return new Promise(resolve => {
        if (child.exitCode !== null || child.signalCode !== null) {
            resolve({
                exited: true,
                code: child.exitCode,
                signal: child.signalCode,
                killed: false
            });
            return;
        }

        const timer = setTimeout(() => {
            child.kill("SIGKILL");
            resolve({
                exited: false,
                code: child.exitCode,
                signal: child.signalCode,
                killed: true
            });
        }, SERVER_STOP_TIMEOUT_MS);

        child.once("exit", (code, signal) => {
            clearTimeout(timer);
            resolve({
                exited: true,
                code,
                signal,
                killed: false
            });
        });

        child.kill("SIGTERM");
    });
}

function startMockLlmServer() {
    return new Promise((resolve, reject) => {
        const requests = [];
        const server = http.createServer((req, res) => {
            const chunks = [];
            req.on("data", chunk => chunks.push(chunk));
            req.on("end", () => {
                const body = Buffer.concat(chunks).toString("utf8");
                requests.push({
                    method: req.method,
                    url: req.url,
                    authorization: req.headers.authorization || "",
                    body
                });

                if (req.method !== "POST" || req.url.split("?")[0] !== "/v1/chat/completions") {
                    res.writeHead(404, {
                        "Content-Type": "application/json"
                    });
                    res.end(JSON.stringify({
                        error: "not found"
                    }));
                    return;
                }

                res.writeHead(200, {
                    "Content-Type": "application/json"
                });
                const contentText = (() => {
                    try {
                        const payload = JSON.parse(body);
                        return String(payload?.messages?.[0]?.content || "");
                    } catch (_) {
                        return "";
                    }
                })();
                const structuredContent = contentText.includes("生成非法结构化命令")
                    ? {
                        chat: {
                            text: "rejected command smoke reply"
                        },
                        commands: [
                            {
                                name: "unknown.command",
                                payload: {},
                                reason: "unknown command smoke"
                            },
                            {
                                name: "display.show_text",
                                payload: {},
                                reason: "invalid payload smoke"
                            }
                        ]
                    }
                    : {
                        chat: {
                            text: "structured smoke reply"
                        },
                        commands: [
                            {
                                name: "display.show_text",
                                target_device_id: "model-wrong-target",
                                payload: {
                                    text: "结构化烟雾",
                                    ttl_ms: 7000
                                },
                                reason: "smoke structured command"
                            }
                        ]
                    };
                res.end(JSON.stringify({
                    model: "smoke-structured-model",
                    choices: [
                        {
                            message: {
                                content: JSON.stringify(structuredContent)
                            }
                        }
                    ]
                }));
            });
        });

        server.once("error", reject);
        server.listen(0, "127.0.0.1", () => {
            server.off("error", reject);
            const address = server.address();
            resolve({
                server,
                requests,
                baseUrl: `http://127.0.0.1:${address.port}`
            });
        });
    });
}

function stopMockLlmServer(mockLlm) {
    return new Promise(resolve => {
        if (!mockLlm?.server?.listening) {
            resolve();
            return;
        }

        mockLlm.server.close(() => resolve());
    });
}

async function request(baseUrl, method, pathname, body, headers = {}) {
    const response = await fetch(`${baseUrl}${pathname}`, {
        method,
        headers: body
            ? {
                "Content-Type": "application/json",
                ...SMOKE_GATEWAY_HEADERS,
                ...headers
            }
            : {
                ...SMOKE_GATEWAY_HEADERS,
                ...headers
            },
        body: body ? JSON.stringify(body) : undefined
    });
    const contentType = response.headers.get("content-type") || "";
    if (contentType.includes("application/json")) {
        return {
            response,
            body: await response.json()
        };
    }

    return {
        response,
        body: Buffer.from(await response.arrayBuffer())
    };
}

async function requestRaw(baseUrl, method, pathname, body, headers = {}) {
    const response = await fetch(`${baseUrl}${pathname}`, {
        method,
        headers: {
            ...SMOKE_GATEWAY_HEADERS,
            ...headers
        },
        body
    });
    const contentType = response.headers.get("content-type") || "";
    if (contentType.includes("application/json")) {
        return {
            response,
            body: await response.json()
        };
    }

    return {
        response,
        body: Buffer.from(await response.arrayBuffer())
    };
}

function hasOwn(value, key) {
    return Object.prototype.hasOwnProperty.call(value, key);
}

function assertDashboardEnvelope(body, expectedOk = true) {
    assert.equal(typeof body.ok, "boolean");
    assert.equal(body.ok, expectedOk);
    assert.equal(typeof body.server_time_ms, "number");
    assert.equal(hasOwn(body, "data"), true);
    assert.equal(hasOwn(body, "error"), true);

    if (expectedOk) {
        assert.equal(body.error, null);
    } else {
        assert.ok(body.error);
        assert.equal(typeof body.error.code, "string");
    }
}

function dbRun(dbPath, sql, params = []) {
    return new Promise((resolve, reject) => {
        const db = new sqlite3.Database(dbPath);
        db.run(sql, params, function (err) {
            db.close();
            if (err) {
                reject(err);
                return;
            }

            resolve(this);
        });
    });
}

function dbAll(dbPath, sql, params = []) {
    return new Promise((resolve, reject) => {
        const db = new sqlite3.Database(dbPath);
        db.all(sql, params, (err, rows) => {
            db.close();
            if (err) {
                reject(err);
                return;
            }

            resolve(rows);
        });
    });
}

async function createLegacySchema(dbPath) {
    fs.mkdirSync(path.dirname(dbPath), { recursive: true });
    await dbRun(dbPath, `
        CREATE TABLE IF NOT EXISTS sensor_records (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp INTEGER
        )
    `);
    await dbRun(dbPath, `
        CREATE TABLE IF NOT EXISTS asr_records (
            id INTEGER PRIMARY KEY AUTOINCREMENT
        )
    `);
    await dbRun(dbPath, `
        CREATE TABLE IF NOT EXISTS llm_records (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp INTEGER
        )
    `);
    await dbRun(dbPath, `
        CREATE TABLE IF NOT EXISTS device_capabilities (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            device_id TEXT NOT NULL,
            capabilities_json TEXT NOT NULL DEFAULT '{}'
        )
    `);
    await dbRun(dbPath, `
        CREATE TABLE IF NOT EXISTS command_queue (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            command_id TEXT NOT NULL,
            device_id TEXT NOT NULL,
            name TEXT NOT NULL,
            payload_json TEXT NOT NULL DEFAULT '{}',
            status TEXT NOT NULL DEFAULT 'queued',
            created_at TEXT,
            updated_at TEXT
        )
    `);
    await dbRun(dbPath, `
        CREATE TABLE IF NOT EXISTS voice_turns (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            request_id TEXT,
            device_id TEXT,
            status TEXT NOT NULL,
            input_bytes INTEGER NOT NULL DEFAULT 0,
            total_ms INTEGER NOT NULL DEFAULT 0
        )
    `);
    await dbRun(dbPath, `
        CREATE TABLE IF NOT EXISTS conversation_turns (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            turn_id TEXT NOT NULL,
            device_id TEXT,
            input_text TEXT,
            response_text TEXT
        )
    `);
    await dbRun(dbPath, `
        CREATE TABLE IF NOT EXISTS daily_memory (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            memory_date TEXT NOT NULL,
            summary TEXT NOT NULL
        )
    `);
    await dbRun(dbPath, `
        CREATE TABLE IF NOT EXISTS long_term_profile (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            profile_key TEXT NOT NULL,
            profile_value TEXT NOT NULL
        )
    `);
    await dbRun(dbPath, `
        CREATE TABLE IF NOT EXISTS memory_corrections (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            correction_id TEXT NOT NULL,
            target_type TEXT NOT NULL,
            target_id TEXT NOT NULL,
            correction_text TEXT NOT NULL
        )
    `);
    await dbRun(dbPath, `
        CREATE TABLE IF NOT EXISTS memory_job_runs (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            job_id TEXT NOT NULL,
            job_name TEXT NOT NULL
        )
    `);
    await dbRun(dbPath, `
        CREATE TABLE IF NOT EXISTS environment_profile (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            profile_key TEXT NOT NULL,
            profile_value TEXT NOT NULL
        )
    `);
    await dbRun(dbPath, `
        CREATE TABLE IF NOT EXISTS experience_memory (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            experience_id TEXT NOT NULL,
            title TEXT NOT NULL
        )
    `);
    await dbRun(dbPath, `
        CREATE TABLE IF NOT EXISTS relation_memory (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            relation_id TEXT NOT NULL,
            subject TEXT NOT NULL,
            predicate TEXT NOT NULL,
            object TEXT NOT NULL
        )
    `);
    await dbRun(dbPath, `
        CREATE TABLE IF NOT EXISTS reminder_rules (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            reminder_id TEXT NOT NULL,
            title TEXT NOT NULL,
            message TEXT NOT NULL
        )
    `);
    await dbRun(dbPath, `
        CREATE TABLE IF NOT EXISTS reminder_records (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            reminder_event_id TEXT NOT NULL,
            message TEXT NOT NULL
        )
    `);
    await dbRun(dbPath, `
        CREATE TABLE IF NOT EXISTS emergency_events (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            event_id TEXT NOT NULL,
            event_type TEXT NOT NULL
        )
    `);
    await dbRun(dbPath, `
        CREATE TABLE IF NOT EXISTS csi_behavior_events (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            event_id TEXT NOT NULL,
            behavior_type TEXT NOT NULL
        )
    `);
    await dbRun(dbPath, `
        CREATE TABLE IF NOT EXISTS lcd_status (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            device_id TEXT NOT NULL
        )
    `);
}

async function hasUniqueIndex(dbPath, tableName, expectedColumns) {
    const indexes = await dbAll(dbPath, `PRAGMA index_list(${tableName})`);
    for (const index of indexes) {
        if (!Number(index.unique)) {
            continue;
        }

        const indexedColumns = await dbAll(dbPath, `PRAGMA index_info(${index.name})`);
        const names = indexedColumns
            .sort((a, b) => a.seqno - b.seqno)
            .map(column => column.name);
        if (names.length === expectedColumns.length &&
            names.every((name, columnIndex) => name === expectedColumns[columnIndex])) {
            return true;
        }
    }

    return false;
}

async function assertUniqueIndexes(dbPath) {
    const expectations = [
        ["device_capabilities", ["device_id"]],
        ["device_status", ["device_id"]],
        ["device_module_status", ["device_id", "module_type"]],
        ["command_queue", ["command_id"]],
        ["conversation_turns", ["turn_id"]],
        ["long_term_profile", ["profile_key"]],
        ["memory_corrections", ["correction_id"]],
        ["memory_job_runs", ["job_id"]],
        ["environment_profile", ["profile_key"]],
        ["experience_memory", ["experience_id"]],
        ["relation_memory", ["relation_id"]],
        ["reminder_rules", ["reminder_id"]],
        ["reminder_records", ["reminder_event_id"]],
        ["emergency_events", ["event_id"]],
        ["csi_behavior_events", ["event_id"]],
        ["lcd_status", ["device_id"]],
        ["data_deletion_runs", ["run_id"]],
        ["event_logs", ["event_id"]],
        ["smart_home_devices", ["device_id"]],
        ["smart_home_commands", ["command_id"]],
        ["natural_language_commands", ["command_id"]]
    ];

    for (const [tableName, columns] of expectations) {
        assert.equal(
            await hasUniqueIndex(dbPath, tableName, columns),
            true,
            `missing unique index on ${tableName}(${columns.join(",")})`
        );
    }
}

async function createDuplicateKeyLegacySchema(dbPath) {
    fs.mkdirSync(path.dirname(dbPath), { recursive: true });
    await dbRun(dbPath, `
        CREATE TABLE IF NOT EXISTS device_capabilities (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            device_id TEXT NOT NULL,
            capabilities_json TEXT NOT NULL DEFAULT '{}'
        )
    `);
    await dbRun(dbPath, "INSERT INTO device_capabilities(device_id,capabilities_json) VALUES(?,?)", ["dup-device", "{}"]);
    await dbRun(dbPath, "INSERT INTO device_capabilities(device_id,capabilities_json) VALUES(?,?)", ["dup-device", "{}"]);
    await dbRun(dbPath, `
        CREATE TABLE IF NOT EXISTS long_term_profile (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            profile_key TEXT NOT NULL,
            profile_value TEXT NOT NULL
        )
    `);
    await dbRun(dbPath, "INSERT INTO long_term_profile(profile_key,profile_value) VALUES(?,?)", ["dup.profile", "old-a"]);
    await dbRun(dbPath, "INSERT INTO long_term_profile(profile_key,profile_value) VALUES(?,?)", ["dup.profile", "old-b"]);
    await dbRun(dbPath, `
        CREATE TABLE IF NOT EXISTS environment_profile (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            profile_key TEXT NOT NULL,
            profile_value TEXT NOT NULL
        )
    `);
    await dbRun(dbPath, "INSERT INTO environment_profile(profile_key,profile_value) VALUES(?,?)", ["dup.env", "old-a"]);
    await dbRun(dbPath, "INSERT INTO environment_profile(profile_key,profile_value) VALUES(?,?)", ["dup.env", "old-b"]);
    await dbRun(dbPath, `
        CREATE TABLE IF NOT EXISTS lcd_status (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            device_id TEXT NOT NULL
        )
    `);
    await dbRun(dbPath, "INSERT INTO lcd_status(device_id) VALUES(?)", ["dup-lcd"]);
    await dbRun(dbPath, "INSERT INTO lcd_status(device_id) VALUES(?)", ["dup-lcd"]);
}

async function withTemporaryEnv(env, fn) {
    const previous = {};
    for (const [key, value] of Object.entries(env)) {
        previous[key] = process.env[key];
        process.env[key] = value;
    }

    try {
        return await fn();
    } finally {
        for (const key of Object.keys(env)) {
            if (previous[key] === undefined) {
                delete process.env[key];
            } else {
                process.env[key] = previous[key];
            }
        }
    }
}

async function assertDuplicateKeyUpserts() {
    const tempDir = makeTempDir();
    const dbPath = path.join(tempDir, "nested", "duplicate-key.sqlite");
    try {
        await createDuplicateKeyLegacySchema(dbPath);
        await withTemporaryEnv({
            ESP_SERVER_DB_PATH: dbPath
        }, async () => {
            const db = createDatabase(path.join(__dirname, ".."));
            const {
                dbRun: sqliteRun,
                dbAll: sqliteAll
            } = createDbHelpers(db);

            try {
                await ensureCommandTables(sqliteRun, sqliteAll);
                await ensureMemoryTables(sqliteRun, sqliteAll);
                await ensureAgentStateTables(sqliteRun, sqliteAll);
                await ensureUserDataDeletionTables(sqliteRun, sqliteAll);

                assert.equal(await hasUniqueIndex(dbPath, "device_capabilities", ["device_id"]), false);
                assert.equal(await hasUniqueIndex(dbPath, "long_term_profile", ["profile_key"]), false);
                assert.equal(await hasUniqueIndex(dbPath, "environment_profile", ["profile_key"]), false);
                assert.equal(await hasUniqueIndex(dbPath, "lcd_status", ["device_id"]), false);

                let result = await upsertDeviceCapabilities(sqliteRun, {
                    device_id: "dup-device",
                    protocol_version: "agent-command-v1",
                    capabilities: {
                        commands: ["device.noop"]
                    }
                });
                assert.equal(result.ok, true);

                result = await upsertProfile(sqliteRun, {
                    profile_key: "dup.profile",
                    profile_value: "updated profile"
                });
                assert.equal(result.ok, true);

                result = await upsertEnvironmentProfile(sqliteRun, {
                    profile_key: "dup.env",
                    profile_value: "updated env"
                });
                assert.equal(result.ok, true);

                result = await upsertLcdStatus(sqliteRun, {
                    device_id: "dup-lcd",
                    page: "message",
                    state: {
                        text: "duplicate-key smoke"
                    }
                });
                assert.equal(result.ok, true);
            } finally {
                db.close();
            }
        });
    } finally {
        fs.rmSync(tempDir, {
            recursive: true,
            force: true
        });
    }
}

async function assertUpsertRetryAfterInsertConflict() {
    const calls = [];
    const dbRun = async (sql, params) => {
        calls.push({
            sql,
            params
        });

        if (calls.length === 1) {
            return {
                changes: 0
            };
        }

        if (calls.length === 2) {
            const error = new Error("SQLITE_CONSTRAINT: UNIQUE constraint failed: smoke.key");
            error.code = "SQLITE_CONSTRAINT";
            throw error;
        }

        return {
            changes: 1
        };
    };

    const result = await runUpdateThenInsert(dbRun, {
        updateSql: "UPDATE smoke SET value=? WHERE key=?",
        updateParams: ["value", "key"],
        insertSql: "INSERT INTO smoke(key,value) VALUES(?,?)",
        insertParams: ["key", "value"]
    });

    assert.equal(result.changes, 1);
    assert.equal(calls.length, 3);
    assert.match(calls[2].sql, /^UPDATE smoke/);
}

async function assertPendingDispatchSkipsLostClaim() {
    const rows = [{
        command_id: "lost-claim-command",
        device_id: "race-device",
        name: "device.noop",
        payload_json: "{}",
        status: "queued",
        created_at: "2026-06-07T00:00:00.000Z",
        updated_at: "2026-06-07T00:00:00.000Z"
    }];
    const dbAll = async () => rows;
    const dbRun = async () => ({
        changes: 0
    });

    const commands = await listPendingCommands(dbRun, dbAll, "race-device", 1);
    assert.equal(commands.length, 0);
}

function assertTtsJsonPcmNormalization() {
    const pcm = Buffer.from([0x01, 0x00, 0x02, 0x00]);
    const jsonBuffer = Buffer.from(JSON.stringify({
        data: {
            audio_base64: pcm.toString("base64")
        }
    }));
    const normalized = normalizeTtsPcmBuffer(jsonBuffer, "application/json");
    assert.deepEqual(normalized, pcm);

    const inferredJson = normalizeTtsPcmBuffer(jsonBuffer);
    assert.deepEqual(inferredJson, pcm);
}

function assertLlmMetadataBounds() {
    const parsed = readLlmTextRequest({
        text: "metadata bounds",
        device_id: ` ${"d".repeat(LLM_METADATA_MAX_CHARS + 10)} `,
        session_id: ` ${"s".repeat(LLM_METADATA_MAX_CHARS + 10)} `
    });

    assert.equal(parsed.error, undefined);
    assert.equal(parsed.deviceId, "d".repeat(LLM_METADATA_MAX_CHARS));
    assert.equal(parsed.sessionId, "s".repeat(LLM_METADATA_MAX_CHARS));
}

async function run() {
    assertTtsJsonPcmNormalization();
    assertLlmMetadataBounds();
    await assertUpsertRetryAfterInsertConflict();
    await assertPendingDispatchSkipsLostClaim();
    await assertDuplicateKeyUpserts();

    const tempDir = makeTempDir();
    const dbPath = path.join(tempDir, "nested", "smoke.sqlite");
    const promptCacheDir = path.join(tempDir, "voice_prompts");
    const promptConfigPath = path.join(tempDir, "voice_prompts", "wake_prompt_config.json");
    const port = String(44000 + Math.floor(Math.random() * 1000));
    const baseUrl = `http://127.0.0.1:${port}`;
    const mockLlm = await startMockLlmServer();
    await createLegacySchema(dbPath);

    const child = spawn(process.execPath, ["server.js"], {
        cwd: path.join(__dirname, ".."),
        env: {
            ...process.env,
            PORT: port,
            ESP_SERVER_DB_PATH: dbPath,
            VOICE_TURN_MOCK: "1",
            VOICE_TURN_MAX_BYTES: "4096",
            VOICE_PROMPT_CACHE_DIR: promptCacheDir,
            VOICE_PROMPT_CONFIG_PATH: promptConfigPath,
            LLM_API_KEY: "smoke-llm-key",
            LLM_BASE_URL: mockLlm.baseUrl,
            LLM_CHAT_PATH: "/v1/chat/completions",
            USER_DATA_DELETE_TOKEN,
            GATEWAY_AUTH_TOKEN: "",
            GATEWAY_AUTH_TOKENS: "",
            VOLC_GATEWAY_API_KEY: ""
        },
        stdio: ["ignore", "pipe", "pipe"]
    });

    try {
        await waitForServer(child);
        await assertUniqueIndexes(dbPath);

        const deviceId = "esp smoke+c5&测试";

        let result = await request(baseUrl, "GET", "/api/commands/whitelist");
        assert.equal(result.response.status, 200);
        assert.equal(result.body.ok, true);
        assert.ok(result.body.commands.some(command => command.name === "display.show_text"));

        result = await request(baseUrl, "POST", "/api/devices/capabilities", {
            device_id: deviceId,
            protocol_version: "agent-command-v1",
            capabilities: {
                commands: [
                    "device.noop",
                    " display.show_text ",
                    "unknown.command",
                    "x".repeat(200),
                    "device.noop"
                ]
            }
        });
        assert.equal(result.response.status, 200);
        assert.equal(result.body.device_id, deviceId);
        assert.deepEqual(result.body.capabilities.commands, ["device.noop", "display.show_text"]);

        result = await request(baseUrl, "GET", "/api/devices/" + encodeURIComponent(` ${deviceId} `) + "/capabilities");
        assert.equal(result.response.status, 200);
        assert.equal(result.body.ok, true);
        assert.equal(result.body.device_id, deviceId);
        assert.deepEqual(result.body.capabilities.commands, ["device.noop", "display.show_text"]);

        result = await request(baseUrl, "POST", "/api/devices/capabilities", {
            device_id: "d".repeat(129),
            protocol_version: "agent-command-v1",
            capabilities: {
                commands: ["device.noop"]
            }
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "DEVICE_ID_INVALID");

        result = await request(baseUrl, "POST", "/api/devices/capabilities", {
            device_id: "protocol-version-too-long-device",
            protocol_version: "p".repeat(41),
            capabilities: {
                commands: ["device.noop"]
            }
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "PROTOCOL_VERSION_INVALID");
        assert.equal(result.body.error, "protocol_version must be <= 40 characters");

        result = await request(baseUrl, "GET", "/api/devices/" + "d".repeat(129) + "/capabilities");
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "DEVICE_ID_INVALID");

        result = await request(baseUrl, "POST", "/api/llm/structured", {
            text: "请在屏幕显示结构化烟雾",
            device_id: "esp-smoke-requester",
            target_device_id: deviceId
        });
        assert.equal(result.response.status, 200);
        assert.equal(result.body.ok, true);
        assert.equal(result.body.text, "structured smoke reply");
        assert.equal(result.body.structured.parsed, true);
        assert.equal(result.body.model, "smoke-structured-model");
        assert.equal(result.body.commands.length, 1);
        assert.equal(result.body.commands[0].device_id, deviceId);
        assert.equal(result.body.commands[0].name, "display.show_text");
        assert.equal(result.body.commands[0].payload.text, "结构化烟雾");
        assert.equal(result.body.rejected_commands.length, 0);
        assert.equal(mockLlm.requests.length, 1);
        assert.equal(mockLlm.requests[0].authorization, "Bearer smoke-llm-key");
        assert.match(mockLlm.requests[0].body, /请在屏幕显示结构化烟雾/);
        const structuredCommandId = result.body.commands[0].command_id;

        result = await request(baseUrl, "GET", `/api/commands/pending?${new URLSearchParams({
            device_id: deviceId,
            limit: "1"
        }).toString()}`);
        assert.equal(result.response.status, 200);
        assert.equal(result.body.commands.length, 1);
        assert.equal(result.body.commands[0].command_id, structuredCommandId);
        assert.equal(result.body.commands[0].device_id, deviceId);
        assert.equal(result.body.commands[0].status, "dispatched");
        assert.ok(result.body.commands[0].dispatched_at);

        result = await request(baseUrl, "POST", `/api/commands/${structuredCommandId}/ack`, {
            status: "completed",
            result: {
                applied: true,
                source: "structured-smoke"
            }
        });
        assert.equal(result.response.status, 200);
        assert.equal(result.body.status, "succeeded");

        result = await request(baseUrl, "POST", "/api/llm/structured", {
            text: "目标为空白时回退到请求设备",
            device_id: deviceId,
            target_device_id: "   "
        });
        assert.equal(result.response.status, 200);
        assert.equal(result.body.ok, true);
        assert.equal(result.body.commands.length, 1);
        assert.equal(result.body.commands[0].device_id, deviceId);
        assert.equal(result.body.rejected_commands.length, 0);
        assert.equal(mockLlm.requests.length, 2);
        assert.match(mockLlm.requests[1].body, /目标为空白时回退到请求设备/);
        const fallbackStructuredCommandId = result.body.commands[0].command_id;

        result = await request(baseUrl, "POST", `/api/commands/${fallbackStructuredCommandId}/ack`, {
            status: "completed",
            result: {
                applied: true,
                source: "structured-fallback-smoke"
            }
        });
        assert.equal(result.response.status, 200);
        assert.equal(result.body.status, "succeeded");

        result = await request(baseUrl, "POST", "/api/llm/structured", {
            text: "目标设备过长时拒绝结构化命令",
            device_id: deviceId,
            target_device_id: "s".repeat(129)
        });
        assert.equal(result.response.status, 200);
        assert.equal(result.body.ok, true);
        assert.equal(result.body.commands.length, 0);
        assert.equal(result.body.rejected_commands.length, 1);
        assert.equal(result.body.rejected_commands[0].code, "COMMAND_TARGET_INVALID");
        assert.equal(result.body.rejected_commands[0].name, "display.show_text");
        assert.match(mockLlm.requests[2].body, /目标设备过长时拒绝结构化命令/);

        result = await request(baseUrl, "POST", "/api/llm/structured", {
            text: "生成非法结构化命令",
            device_id: deviceId,
            target_device_id: deviceId
        });
        assert.equal(result.response.status, 200);
        assert.equal(result.body.ok, true);
        assert.equal(result.body.text, "rejected command smoke reply");
        assert.equal(result.body.commands.length, 0);
        assert.equal(result.body.rejected_commands.length, 2);
        assert.equal(result.body.rejected_commands[0].code, "COMMAND_NOT_WHITELISTED");
        assert.equal(result.body.rejected_commands[0].name, "unknown.command");
        assert.equal(result.body.rejected_commands[1].code, "COMMAND_PAYLOAD_INVALID");
        assert.equal(result.body.rejected_commands[1].name, "display.show_text");
        assert.equal(result.body.rejected_commands[1].target_device_id, deviceId);
        assert.match(mockLlm.requests[3].body, /生成非法结构化命令/);

        const longRequesterId = "r".repeat(LLM_METADATA_MAX_CHARS + 10);
        const longSessionId = "s".repeat(LLM_METADATA_MAX_CHARS + 10);
        result = await request(baseUrl, "POST", "/api/llm/structured", {
            text: "元数据字段过长时仍可入队",
            device_id: ` ${longRequesterId} `,
            target_device_id: deviceId,
            session_id: ` ${longSessionId} `
        });
        assert.equal(result.response.status, 200);
        assert.equal(result.body.ok, true);
        assert.equal(result.body.commands.length, 1);
        assert.equal(result.body.commands[0].device_id, deviceId);
        assert.match(mockLlm.requests[4].body, /元数据字段过长时仍可入队/);
        const metadataBoundedCommandId = result.body.commands[0].command_id;
        const metadataRows = await dbAll(dbPath, "SELECT requested_by FROM command_queue WHERE command_id=? LIMIT 1", [metadataBoundedCommandId]);
        assert.equal(metadataRows.length, 1);
        assert.equal(metadataRows[0].requested_by, "r".repeat(LLM_METADATA_MAX_CHARS));

        result = await request(baseUrl, "POST", `/api/commands/${metadataBoundedCommandId}/ack`, {
            status: "completed",
            result: {
                applied: true,
                source: "metadata-bounds-smoke"
            }
        });
        assert.equal(result.response.status, 200);
        assert.equal(result.body.status, "succeeded");

        result = await request(baseUrl, "POST", "/api/llm/text", {
            text: "   "
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.error, "text is required");

        result = await request(baseUrl, "POST", "/api/llm/structured", {
            text: ""
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.error, "text is required");

        result = await request(baseUrl, "POST", "/api/commands", {
            name: "unknown.command",
            target_device_id: deviceId,
            payload: {}
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "COMMAND_NOT_WHITELISTED");

        result = await request(baseUrl, "POST", "/api/commands", {
            name: "voice.set_volume",
            target_device_id: deviceId,
            payload: {
                volume: 35
            }
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "DEVICE_COMMAND_UNSUPPORTED");

        result = await request(baseUrl, "GET", "/api/commands/pending");
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "DEVICE_ID_REQUIRED");
        assert.equal(result.body.error, "device_id is required");

        result = await request(baseUrl, "GET", `/api/commands/pending?${new URLSearchParams({
            device_id: "p".repeat(129)
        }).toString()}`);
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "DEVICE_ID_INVALID");

        result = await request(baseUrl, "POST", "/api/commands", {
            name: "display.show_text",
            target_device_id: "t".repeat(129),
            payload: {
                text: "目标过长"
            }
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "COMMAND_TARGET_INVALID");

        result = await request(baseUrl, "POST", "/api/commands", {
            name: "display.show_text",
            target_device_id: "unregistered-command-device",
            payload: {
                text: "未注册设备"
            }
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "DEVICE_CAPABILITIES_REQUIRED");

        result = await request(baseUrl, "POST", "/api/commands", {
            name: "display.show_text",
            target_device_id: deviceId,
            payload: {}
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "COMMAND_PAYLOAD_INVALID");
        assert.equal(result.body.error, "text is required");

        result = await request(baseUrl, "POST", "/api/commands", {
            name: "display.show_text",
            target_device_id: deviceId,
            payload: {
                text: "ttl 超范围",
                ttl_ms: 999999
            }
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "COMMAND_PAYLOAD_INVALID");
        assert.equal(result.body.error, "ttl_ms must be <= 60000");

        result = await request(baseUrl, "POST", "/api/commands", {
            name: "display.show_text",
            target_device_id: deviceId,
            payload: {
                text: "显".repeat(121)
            }
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "COMMAND_PAYLOAD_INVALID");
        assert.equal(result.body.error, "text must be <= 120 characters");

        result = await request(baseUrl, "POST", "/api/commands", {
            name: "display.show_text",
            target_device_id: deviceId,
            payload: {
                text: "reason too long"
            },
            reason: "r".repeat(241)
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "COMMAND_REASON_INVALID");
        assert.equal(result.body.error, "reason must be <= 240 characters");

        result = await request(baseUrl, "POST", "/api/commands", {
            name: "display.show_text",
            target_device_id: deviceId,
            payload: {
                text: "烟雾测试",
                ttl_ms: 5000
            }
        });
        assert.equal(result.response.status, 201);
        assert.equal(result.body.ok, true);
        const commandId = result.body.command.command_id;

        const pendingPath = `/api/commands/pending?${new URLSearchParams({
            device_id: deviceId,
            limit: "1"
        }).toString()}`;
        result = await request(baseUrl, "GET", pendingPath);
        assert.equal(result.response.status, 200);
        assert.equal(result.body.commands.length, 1);
        assert.equal(result.body.commands[0].command_id, commandId);
        assert.equal(result.body.commands[0].status, "dispatched");

        result = await request(baseUrl, "POST", `/api/commands/${commandId}/ack`, {
            status: "done",
            result: {
                applied: true
            }
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "COMMAND_ACK_STATUS_INVALID");

        result = await request(baseUrl, "GET", `/api/commands/history?${new URLSearchParams({
            device_id: ` ${deviceId} `,
            limit: "1"
        }).toString()}`);
        assert.equal(result.response.status, 200);
        assert.equal(result.body.commands.length, 1);
        assert.equal(result.body.commands[0].command_id, commandId);
        assert.equal(result.body.commands[0].status, "dispatched");

        result = await request(baseUrl, "GET", `/api/commands/history?${new URLSearchParams({
            device_id: "h".repeat(129)
        }).toString()}`);
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "DEVICE_ID_INVALID");

        result = await request(baseUrl, "POST", `/api/commands/${commandId}/ack`, {
            status: "completed",
            result: {
                applied: true
            }
        });
        assert.equal(result.response.status, 200);
        assert.equal(result.body.status, "succeeded");

        result = await request(baseUrl, "POST", `/api/commands/${commandId}/ack`, {
            status: "completed",
            result: {
                applied: true,
                duplicate: true
            }
        });
        assert.equal(result.response.status, 200);
        assert.equal(result.body.ok, true);
        assert.equal(result.body.idempotent, true);
        assert.equal(result.body.status, "succeeded");
        assert.equal(result.body.command_id, commandId);

        result = await request(baseUrl, "POST", "/api/conversation/turns", {
            turn_id: "smoke-conversation-turn",
            session_id: " smoke-session ",
            device_id: deviceId,
            input_text: "今天空气怎么样",
            response_text: "空气状态正常",
            memory_level: " important ",
            source: "smoke"
        });
        assert.equal(result.response.status, 201);
        assert.equal(result.body.ok, true);
        assert.equal(result.body.turn_id, "smoke-conversation-turn");

        result = await request(baseUrl, "POST", "/api/conversation/turns", {
            turn_id: "smoke-conversation-turn",
            device_id: deviceId,
            input_text: "重复 turn id"
        });
        assert.equal(result.response.status, 409);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "CONVERSATION_TURN_ID_DUPLICATE");
        assert.equal(result.body.turn_id, "smoke-conversation-turn");

        result = await request(baseUrl, "POST", "/api/conversation/turns", {
            turn_id: "t".repeat(81),
            device_id: deviceId,
            input_text: "超长 turn id"
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "CONVERSATION_TURN_ID_INVALID");
        assert.equal(result.body.error, "turn_id must be <= 80 characters");

        result = await request(baseUrl, "POST", "/api/conversation/turns", {
            role: "r".repeat(41),
            input_text: "超长 role"
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "CONVERSATION_ROLE_INVALID");
        assert.equal(result.body.error, "role must be <= 40 characters");

        result = await request(baseUrl, "POST", "/api/conversation/turns", {
            source: "s".repeat(81),
            input_text: "超长 source"
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "CONVERSATION_SOURCE_INVALID");
        assert.equal(result.body.error, "source must be <= 80 characters");

        result = await request(baseUrl, "POST", "/api/conversation/turns", {
            device_id: "d".repeat(129),
            input_text: "超长 conversation device id"
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "DEVICE_ID_INVALID");
        assert.equal(result.body.error, "device_id must be <= 128 characters");

        result = await request(baseUrl, "POST", "/api/conversation/turns", {
            session_id: "s".repeat(129),
            input_text: "超长 conversation session id"
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "SESSION_ID_INVALID");
        assert.equal(result.body.error, "session_id must be <= 128 characters");

        result = await request(baseUrl, "POST", "/api/conversation/turns", {
            device_id: deviceId,
            input_text: "自动生成 turn id"
        });
        assert.equal(result.response.status, 201);
        assert.equal(result.body.ok, true);
        assert.match(result.body.turn_id, /^turn_/);

        result = await request(baseUrl, "GET", "/api/conversation/turns?device_id=" + encodeURIComponent(deviceId) + "&limit=2");
        assert.equal(result.response.status, 200);
        assert.equal(result.body.turns.length, 2);
        assert.match(result.body.turns[0].turn_id, /^turn_/);
        assert.equal(result.body.turns[1].memory_level, "important");

        result = await request(baseUrl, "GET", "/api/conversation/turns?" + new URLSearchParams({
            device_id: ` ${deviceId} `,
            limit: "2"
        }).toString());
        assert.equal(result.response.status, 200);
        assert.equal(result.body.turns.length, 2);
        assert.equal(result.body.turns[0].device_id, deviceId);

        result = await request(baseUrl, "GET", "/api/conversation/turns?" + new URLSearchParams({
            session_id: " smoke-session ",
            limit: "1"
        }).toString());
        assert.equal(result.response.status, 200);
        assert.equal(result.body.turns.length, 1);
        assert.equal(result.body.turns[0].turn_id, "smoke-conversation-turn");

        result = await request(baseUrl, "GET", "/api/conversation/turns?" + new URLSearchParams({
            session_id: "s".repeat(129)
        }).toString());
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "SESSION_ID_INVALID");

        result = await request(baseUrl, "GET", "/api/conversation/turns?" + new URLSearchParams({
            device_id: "d".repeat(129)
        }).toString());
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "DEVICE_ID_INVALID");

        result = await request(baseUrl, "POST", "/api/memory/daily", {
            memory_date: "2026-06-07",
            summary: "smoke summary",
            status: "invalid-status"
        });
        assert.equal(result.response.status, 201);
        assert.equal(result.body.ok, true);

        result = await request(baseUrl, "GET", "/api/memory/daily?date=2026-06-07&limit=1");
        assert.equal(result.response.status, 200);
        assert.equal(result.body.memories.length, 1);
        assert.equal(result.body.memories[0].status, "candidate");

        result = await request(baseUrl, "POST", "/api/memory/daily", {
            memory_date: "2026-06-08",
            summary: "smoke trimmed status summary",
            status: " active "
        });
        assert.equal(result.response.status, 201);
        assert.equal(result.body.ok, true);

        result = await request(baseUrl, "GET", "/api/memory/daily?date=2026-06-08&limit=1");
        assert.equal(result.response.status, 200);
        assert.equal(result.body.memories.length, 1);
        assert.equal(result.body.memories[0].status, "active");

        result = await request(baseUrl, "GET", "/api/memory/daily?" + new URLSearchParams({
            date: " 2026-06-08 ",
            limit: "1"
        }).toString());
        assert.equal(result.response.status, 200);
        assert.equal(result.body.memories.length, 1);
        assert.equal(result.body.memories[0].memory_date, "2026-06-08");

        result = await request(baseUrl, "GET", "/api/memory/daily?" + new URLSearchParams({
            date: "2026/06/08"
        }).toString());
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "DAILY_MEMORY_DATE_INVALID");
        assert.equal(result.body.error, "memory_date must use YYYY-MM-DD format");

        result = await request(baseUrl, "GET", "/api/memory/daily?" + new URLSearchParams({
            date: "2026-02-31"
        }).toString());
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "DAILY_MEMORY_DATE_INVALID");
        assert.equal(result.body.error, "memory_date must use YYYY-MM-DD format");

        result = await request(baseUrl, "POST", "/api/memory/daily", {
            memory_date: "2026/06/09",
            summary: "invalid date format"
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "DAILY_MEMORY_DATE_INVALID");
        assert.equal(result.body.error, "memory_date must use YYYY-MM-DD format");

        result = await request(baseUrl, "POST", "/api/memory/daily", {
            memory_date: "2026-02-31",
            summary: "invalid calendar date"
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "DAILY_MEMORY_DATE_INVALID");
        assert.equal(result.body.error, "memory_date must use YYYY-MM-DD format");

        result = await request(baseUrl, "POST", "/api/memory/profile", {
            profile_key: "smoke.prefers_quiet",
            profile_value: "用户偏好安静提示音",
            category: "user",
            status: " active ",
            confidence: 0.7,
            evidence: [{
                source: "smoke"
            }]
        });
        assert.equal(result.response.status, 200);
        assert.equal(result.body.ok, true);
        assert.equal(result.body.status, "active");

        result = await request(baseUrl, "POST", "/api/memory/profile", {
            profile_key: "p".repeat(121),
            profile_value: "profile key too long"
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "PROFILE_KEY_INVALID");
        assert.equal(result.body.error, "profile_key must be <= 120 characters");

        result = await request(baseUrl, "POST", "/api/memory/profile", {
            profile_key: "smoke.category_too_long",
            profile_value: "profile category too long",
            category: "c".repeat(81)
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "PROFILE_CATEGORY_INVALID");
        assert.equal(result.body.error, "category must be <= 80 characters");

        result = await request(baseUrl, "POST", "/api/memory/corrections", {
            target_type: "profile",
            target_id: "smoke.prefers_quiet",
            correction_text: "smoke correction",
            corrected_value: "用户只在夜间偏好安静提示音",
            device_id: deviceId,
            status: "invalid-status"
        });
        assert.equal(result.response.status, 201);
        assert.equal(result.body.ok, true);

        result = await request(baseUrl, "POST", "/api/memory/corrections", {
            target_type: "profile",
            target_id: "k".repeat(121),
            correction_text: "target id too long"
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "MEMORY_CORRECTION_TARGET_ID_INVALID");
        assert.equal(result.body.error, "target_id must be <= 120 characters");

        result = await request(baseUrl, "POST", "/api/memory/corrections", {
            target_type: "t".repeat(41),
            target_id: "smoke.prefers_quiet",
            correction_text: "correction target type too long"
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "MEMORY_CORRECTION_TARGET_TYPE_INVALID");
        assert.equal(result.body.error, "target_type must be <= 40 characters");

        result = await request(baseUrl, "POST", "/api/memory/corrections", {
            target_type: "profile",
            target_id: "smoke.prefers_quiet",
            correction_text: "correction device id too long",
            device_id: "d".repeat(129)
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "DEVICE_ID_INVALID");
        assert.equal(result.body.error, "device_id must be <= 128 characters");

        result = await request(baseUrl, "POST", "/api/memory/corrections", {
            target_type: "profile",
            target_id: "smoke.prefers_quiet",
            correction_text: "correction session id too long",
            session_id: "s".repeat(129)
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "SESSION_ID_INVALID");
        assert.equal(result.body.error, "session_id must be <= 128 characters");

        result = await request(baseUrl, "GET", "/api/memory/profile?category=user&limit=1");
        assert.equal(result.response.status, 200);
        assert.equal(result.body.profiles.length, 1);
        assert.equal(result.body.profiles[0].profile_key, "smoke.prefers_quiet");
        assert.equal(result.body.profiles[0].status, "candidate");
        assert.equal(result.body.profiles[0].correction_count, 1);

        result = await request(baseUrl, "GET", "/api/memory/profile?" + new URLSearchParams({
            category: " user ",
            status: " candidate ",
            limit: "1"
        }).toString());
        assert.equal(result.response.status, 200);
        assert.equal(result.body.profiles.length, 1);
        assert.equal(result.body.profiles[0].profile_key, "smoke.prefers_quiet");

        result = await request(baseUrl, "GET", "/api/memory/profile?" + new URLSearchParams({
            status: "invalid-status"
        }).toString());
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "PROFILE_STATUS_INVALID");

        result = await request(baseUrl, "GET", "/api/memory/profile?" + new URLSearchParams({
            category: "c".repeat(81)
        }).toString());
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "PROFILE_CATEGORY_INVALID");
        assert.equal(result.body.error, "category must be <= 80 characters");

        result = await request(baseUrl, "POST", "/api/jobs/daily-summary/run", {
            date: "2026-06-07",
            summary: "smoke daily job summary",
            force: true
        });
        assert.equal(result.response.status, 202);
        assert.equal(result.body.ok, true);
        assert.equal(result.body.status, "completed");
        assert.equal(result.body.memory_type, "daily_summary");

        result = await request(baseUrl, "POST", "/api/jobs/daily-summary/run", {
            date: "2026/06/07"
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "DAILY_SUMMARY_DATE_INVALID");
        assert.equal(result.body.error, "date must use YYYY-MM-DD format");

        result = await request(baseUrl, "POST", "/api/jobs/daily-summary/run", {
            date: "2026-02-31"
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "DAILY_SUMMARY_DATE_INVALID");
        assert.equal(result.body.error, "date must use YYYY-MM-DD format");

        result = await request(baseUrl, "POST", "/api/jobs/weekly-profile/run", {
            week_start: "2026-06-01"
        });
        assert.equal(result.response.status, 202);
        assert.equal(result.body.ok, true);
        assert.equal(result.body.status, "completed");
        assert.equal(result.body.memory_type, "weekly_summary");

        result = await request(baseUrl, "POST", "/api/jobs/weekly-profile/run", {
            week_start: "2026/06/01"
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "WEEKLY_PROFILE_DATE_INVALID");
        assert.equal(result.body.error, "week_start must use YYYY-MM-DD format");

        result = await request(baseUrl, "POST", "/api/jobs/weekly-profile/run", {
            week_start: "2026-02-31"
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "WEEKLY_PROFILE_DATE_INVALID");
        assert.equal(result.body.error, "week_start must use YYYY-MM-DD format");

        result = await request(baseUrl, "GET", "/api/jobs/memory?limit=2");
        assert.equal(result.response.status, 200);
        assert.equal(result.body.jobs.length, 2);
        assert.ok(result.body.jobs.every(job => job.status === "completed"));
        assert.ok(result.body.jobs.every(job => job.completed_at));

        result = await request(baseUrl, "GET", "/api/jobs/memory?" + new URLSearchParams({
            job_name: " daily_summary ",
            target_date: " 2026-06-07 ",
            limit: "1"
        }).toString());
        assert.equal(result.response.status, 200);
        assert.equal(result.body.jobs.length, 1);
        assert.equal(result.body.jobs[0].job_name, "daily_summary");

        result = await request(baseUrl, "GET", "/api/jobs/memory?" + new URLSearchParams({
            target_date: "2026/06/07"
        }).toString());
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "MEMORY_JOB_TARGET_DATE_INVALID");
        assert.equal(result.body.error, "target_date must use YYYY-MM-DD format");

        result = await request(baseUrl, "GET", "/api/jobs/memory?" + new URLSearchParams({
            target_date: "2026-02-31"
        }).toString());
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "MEMORY_JOB_TARGET_DATE_INVALID");
        assert.equal(result.body.error, "target_date must use YYYY-MM-DD format");

        result = await request(baseUrl, "GET", "/api/jobs/memory?" + new URLSearchParams({
            job_name: "j".repeat(81)
        }).toString());
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "MEMORY_JOB_NAME_INVALID");
        assert.equal(result.body.error, "job_name must be <= 80 characters");

        result = await request(baseUrl, "POST", "/api/environment/profile", {
            profile_key: "smoke.room_air",
            profile_value: "空气状态正常",
            device_id: deviceId,
            status: " active ",
            confidence: 0.8,
            evidence: [{
                source: "smoke"
            }]
        });
        assert.equal(result.response.status, 200);
        assert.equal(result.body.ok, true);

        result = await request(baseUrl, "POST", "/api/environment/profile", {
            profile_key: "e".repeat(121),
            profile_value: "environment profile key too long"
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "ENVIRONMENT_PROFILE_KEY_INVALID");
        assert.equal(result.body.error, "profile_key must be <= 120 characters");

        result = await request(baseUrl, "POST", "/api/environment/profile", {
            profile_key: "smoke.room_device_too_long",
            profile_value: "device id too long",
            device_id: "d".repeat(129)
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "DEVICE_ID_INVALID");
        assert.equal(result.body.error, "device_id must be <= 128 characters");

        result = await request(baseUrl, "GET", "/api/environment/profile?device_id=" + encodeURIComponent(deviceId));
        assert.equal(result.response.status, 200);
        assert.equal(result.body.profiles.length, 1);
        assert.equal(result.body.profiles[0].profile_key, "smoke.room_air");
        assert.equal(result.body.profiles[0].status, "active");

        result = await request(baseUrl, "GET", "/api/environment/profile?" + new URLSearchParams({
            device_id: ` ${deviceId} `,
            status: " active ",
            limit: "1"
        }).toString());
        assert.equal(result.response.status, 200);
        assert.equal(result.body.profiles.length, 1);
        assert.equal(result.body.profiles[0].profile_key, "smoke.room_air");

        result = await request(baseUrl, "GET", "/api/environment/profile?" + new URLSearchParams({
            status: "invalid-status"
        }).toString());
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "PROFILE_STATUS_INVALID");

        result = await request(baseUrl, "GET", "/api/environment/profile?" + new URLSearchParams({
            device_id: "q".repeat(129)
        }).toString());
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "DEVICE_ID_INVALID");

        result = await request(baseUrl, "POST", "/api/memory/experience", {
            experience_id: "smoke-experience-night-volume",
            title: "smoke experience",
            situation: "sensor smoke test",
            action: "verify backend API",
            outcome: "passed",
            status: " active ",
            confidence: 0.9,
            evidence: [{
                source: "smoke"
            }]
        });
        assert.equal(result.response.status, 201);
        assert.equal(result.body.ok, true);
        assert.equal(result.body.experience_id, "smoke-experience-night-volume");

        result = await request(baseUrl, "POST", "/api/memory/experience", {
            experience_id: "smoke-experience-night-volume",
            title: "smoke experience duplicate"
        });
        assert.equal(result.response.status, 409);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "EXPERIENCE_ID_DUPLICATE");
        assert.equal(result.body.experience_id, "smoke-experience-night-volume");

        result = await request(baseUrl, "POST", "/api/memory/experience", {
            experience_id: "x".repeat(121),
            title: "smoke experience too long"
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "EXPERIENCE_ID_INVALID");
        assert.equal(result.body.error, "experience_id must be <= 120 characters");

        result = await request(baseUrl, "POST", "/api/memory/experience", {
            title: "t".repeat(201)
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "EXPERIENCE_TITLE_INVALID");
        assert.equal(result.body.error, "title must be <= 200 characters");

        result = await request(baseUrl, "GET", "/api/memory/experience?status=active&limit=1");
        assert.equal(result.response.status, 200);
        assert.equal(result.body.memories.length, 1);
        assert.equal(result.body.memories[0].title, "smoke experience");

        result = await request(baseUrl, "GET", "/api/memory/experience?" + new URLSearchParams({
            status: "invalid-status"
        }).toString());
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "MEMORY_STATUS_INVALID");

        result = await request(baseUrl, "GET", "/api/memory/experience?" + new URLSearchParams({
            status: " active ",
            limit: "1"
        }).toString());
        assert.equal(result.response.status, 200);
        assert.equal(result.body.memories.length, 1);
        assert.equal(result.body.memories[0].title, "smoke experience");

        result = await request(baseUrl, "POST", "/api/memory/relation", {
            relation_id: "smoke-relation-user-device",
            subject: "user",
            predicate: "uses",
            object: "esp smoke test",
            relation_type: "test",
            status: " active ",
            confidence: 0.8
        });
        assert.equal(result.response.status, 201);
        assert.equal(result.body.ok, true);
        assert.equal(result.body.relation_id, "smoke-relation-user-device");

        result = await request(baseUrl, "POST", "/api/memory/relation", {
            relation_id: "smoke-relation-user-device",
            subject: "user",
            predicate: "uses",
            object: "esp smoke test duplicate"
        });
        assert.equal(result.response.status, 409);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "RELATION_ID_DUPLICATE");
        assert.equal(result.body.relation_id, "smoke-relation-user-device");

        result = await request(baseUrl, "POST", "/api/memory/relation", {
            relation_id: "r".repeat(121),
            subject: "user",
            predicate: "uses",
            object: "too long relation"
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "RELATION_ID_INVALID");
        assert.equal(result.body.error, "relation_id must be <= 120 characters");

        result = await request(baseUrl, "POST", "/api/memory/relation", {
            subject: "user",
            predicate: "uses",
            object: "too long relation type",
            relation_type: "r".repeat(81)
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "RELATION_TYPE_INVALID");
        assert.equal(result.body.error, "relation_type must be <= 80 characters");

        result = await request(baseUrl, "POST", "/api/memory/relation", {
            subject: "s".repeat(201),
            predicate: "uses",
            object: "too long relation subject"
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "RELATION_SUBJECT_INVALID");
        assert.equal(result.body.error, "subject must be <= 200 characters");

        result = await request(baseUrl, "POST", "/api/memory/relation", {
            subject: "user",
            predicate: "p".repeat(121),
            object: "too long relation predicate"
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "RELATION_PREDICATE_INVALID");
        assert.equal(result.body.error, "predicate must be <= 120 characters");

        result = await request(baseUrl, "POST", "/api/memory/relation", {
            subject: "user",
            predicate: "uses",
            object: "o".repeat(201)
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "RELATION_OBJECT_INVALID");
        assert.equal(result.body.error, "object must be <= 200 characters");

        result = await request(baseUrl, "GET", "/api/memory/relation?relation_type=test&limit=1");
        assert.equal(result.response.status, 200);
        assert.equal(result.body.memories.length, 1);
        assert.equal(result.body.memories[0].object, "esp smoke test");
        assert.equal(result.body.memories[0].status, "active");

        result = await request(baseUrl, "GET", "/api/memory/relation?" + new URLSearchParams({
            relation_type: " test ",
            status: " active ",
            limit: "1"
        }).toString());
        assert.equal(result.response.status, 200);
        assert.equal(result.body.memories.length, 1);
        assert.equal(result.body.memories[0].object, "esp smoke test");

        result = await request(baseUrl, "GET", "/api/memory/relation?" + new URLSearchParams({
            status: "invalid-status"
        }).toString());
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "MEMORY_STATUS_INVALID");

        result = await request(baseUrl, "GET", "/api/memory/relation?" + new URLSearchParams({
            relation_type: "r".repeat(81)
        }).toString());
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "RELATION_TYPE_INVALID");
        assert.equal(result.body.error, "relation_type must be <= 80 characters");

        result = await request(baseUrl, "POST", "/api/reminders/rules", {
            reminder_id: "smoke-reminder-rule",
            title: "smoke reminder",
            message: "smoke reminder message",
            rule: {
                type: "manual"
            },
            status: " active ",
            source: "smoke"
        });
        assert.equal(result.response.status, 201);
        assert.equal(result.body.ok, true);
        const reminderId = result.body.reminder_id;
        assert.equal(reminderId, "smoke-reminder-rule");

        result = await request(baseUrl, "POST", "/api/reminders/rules", {
            reminder_id: "smoke-reminder-rule",
            title: "smoke reminder duplicate",
            message: "smoke reminder duplicate message"
        });
        assert.equal(result.response.status, 409);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "REMINDER_ID_DUPLICATE");
        assert.equal(result.body.reminder_id, "smoke-reminder-rule");

        result = await request(baseUrl, "POST", "/api/reminders/rules", {
            reminder_id: "m".repeat(121),
            title: "smoke reminder too long",
            message: "smoke reminder too long message"
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "REMINDER_ID_INVALID");
        assert.equal(result.body.error, "reminder_id must be <= 120 characters");

        result = await request(baseUrl, "POST", "/api/reminders/rules", {
            title: "t".repeat(201),
            message: "smoke reminder title too long"
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "REMINDER_TITLE_INVALID");
        assert.equal(result.body.error, "title must be <= 200 characters");

        result = await request(baseUrl, "POST", "/api/reminders/rules", {
            title: "smoke reminder message too long",
            message: "m".repeat(1001)
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "REMINDER_MESSAGE_INVALID");
        assert.equal(result.body.error, "message must be <= 1000 characters");

        result = await request(baseUrl, "POST", "/api/reminders/rules", {
            title: "smoke reminder channel too long",
            message: "smoke reminder channel too long message",
            channel: "c".repeat(41)
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "REMINDER_CHANNEL_INVALID");
        assert.equal(result.body.error, "channel must be <= 40 characters");

        result = await request(baseUrl, "POST", "/api/reminders/rules", {
            title: "smoke reminder next run too long",
            message: "smoke reminder next run too long message",
            next_run_at: "n".repeat(81)
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "REMINDER_NEXT_RUN_AT_INVALID");
        assert.equal(result.body.error, "next_run_at must be <= 80 characters");

        result = await request(baseUrl, "POST", "/api/reminders/rules", {
            title: "smoke reminder suppress until too long",
            message: "smoke reminder suppress until too long message",
            suppress_until: "s".repeat(81)
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "REMINDER_SUPPRESS_UNTIL_INVALID");
        assert.equal(result.body.error, "suppress_until must be <= 80 characters");

        result = await request(baseUrl, "GET", "/api/reminders/rules?status=active&limit=1");
        assert.equal(result.response.status, 200);
        assert.equal(result.body.rules.length, 1);
        assert.equal(result.body.rules[0].reminder_id, reminderId);

        result = await request(baseUrl, "GET", "/api/reminders/rules?" + new URLSearchParams({
            status: "invalid-status"
        }).toString());
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "REMINDER_RULE_STATUS_INVALID");

        result = await request(baseUrl, "GET", "/api/reminders/rules?" + new URLSearchParams({
            status: " active ",
            limit: "1"
        }).toString());
        assert.equal(result.response.status, 200);
        assert.equal(result.body.rules.length, 1);
        assert.equal(result.body.rules[0].reminder_id, reminderId);

        result = await request(baseUrl, "POST", "/api/reminders/events", {
            reminder_id: reminderId,
            message: "smoke reminder event",
            status: "pending"
        });
        assert.equal(result.response.status, 201);
        assert.equal(result.body.ok, true);

        result = await request(baseUrl, "POST", "/api/reminders/events", {
            reminder_id: reminderId,
            message: "m".repeat(1001)
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "REMINDER_EVENT_MESSAGE_INVALID");
        assert.equal(result.body.error, "message must be <= 1000 characters");

        result = await request(baseUrl, "GET", "/api/reminders/events?status=pending&limit=1");
        assert.equal(result.response.status, 200);
        assert.equal(result.body.events.length, 1);
        assert.equal(result.body.events[0].message, "smoke reminder event");
        assert.equal(result.body.events[0].completed_at, "");

        result = await request(baseUrl, "POST", "/api/reminders/events", {
            reminder_id: reminderId,
            message: "smoke invalid reminder event",
            status: "invalid-status"
        });
        assert.equal(result.response.status, 201);
        assert.equal(result.body.ok, true);

        result = await request(baseUrl, "GET", "/api/reminders/events?limit=1");
        assert.equal(result.response.status, 200);
        assert.equal(result.body.events.length, 1);
        assert.equal(result.body.events[0].message, "smoke invalid reminder event");
        assert.equal(result.body.events[0].status, "pending");
        assert.equal(result.body.events[0].completed_at, "");

        result = await request(baseUrl, "POST", "/api/reminders/events", {
            reminder_id: "m".repeat(121),
            message: "smoke reminder event reference too long"
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "REMINDER_ID_INVALID");
        assert.equal(result.body.error, "reminder_id must be <= 120 characters");

        result = await request(baseUrl, "POST", "/api/reminders/events", {
            reminder_id: reminderId,
            message: "smoke reminder event due at too long",
            due_at: "d".repeat(81)
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "REMINDER_DUE_AT_INVALID");
        assert.equal(result.body.error, "due_at must be <= 80 characters");

        result = await request(baseUrl, "POST", "/api/reminders/events", {
            reminder_event_id: "smoke-reminder-confirmed",
            reminder_id: reminderId,
            message: "smoke confirmed reminder event",
            status: " confirmed "
        });
        assert.equal(result.response.status, 201);
        assert.equal(result.body.ok, true);

        result = await request(baseUrl, "POST", "/api/reminders/events", {
            reminder_event_id: "smoke-reminder-confirmed",
            reminder_id: reminderId,
            message: "smoke duplicate reminder event",
            status: "confirmed"
        });
        assert.equal(result.response.status, 409);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "REMINDER_EVENT_ID_DUPLICATE");
        assert.equal(result.body.reminder_event_id, "smoke-reminder-confirmed");

        result = await request(baseUrl, "POST", "/api/reminders/events", {
            reminder_event_id: "v".repeat(121),
            reminder_id: reminderId,
            message: "smoke reminder event too long"
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "REMINDER_EVENT_ID_INVALID");
        assert.equal(result.body.error, "reminder_event_id must be <= 120 characters");

        result = await request(baseUrl, "GET", "/api/reminders/events?status=confirmed&limit=1");
        assert.equal(result.response.status, 200);
        assert.equal(result.body.events.length, 1);
        assert.equal(result.body.events[0].message, "smoke confirmed reminder event");
        assert.equal(result.body.events[0].status, "confirmed");
        assert.equal(result.body.events[0].completed_at.length > 0, true);

        result = await request(baseUrl, "GET", "/api/reminders/events?" + new URLSearchParams({
            status: " confirmed ",
            limit: "1"
        }).toString());
        assert.equal(result.response.status, 200);
        assert.equal(result.body.events.length, 1);
        assert.equal(result.body.events[0].message, "smoke confirmed reminder event");

        result = await request(baseUrl, "GET", "/api/reminders/events?" + new URLSearchParams({
            status: "invalid-status"
        }).toString());
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "REMINDER_EVENT_STATUS_INVALID");

        result = await request(baseUrl, "POST", "/api/emergency/events", {
            device_id: deviceId,
            event_type: "smoke_local_action_too_long",
            local_action: "l".repeat(501)
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "EMERGENCY_LOCAL_ACTION_INVALID");
        assert.equal(result.body.error, "local_action must be <= 500 characters");

        result = await request(baseUrl, "POST", "/api/emergency/events", {
            event_id: "smoke-emergency-resolved",
            device_id: deviceId,
            event_type: "smoke_resolved_test",
            severity: " warning ",
            status: " resolved ",
            payload: {
                source: "smoke"
            }
        });
        assert.equal(result.response.status, 201);
        assert.equal(result.body.ok, true);

        result = await request(baseUrl, "POST", "/api/emergency/events", {
            event_id: "smoke-emergency-resolved",
            device_id: deviceId,
            event_type: "smoke_resolved_test_duplicate",
            severity: "warning",
            status: "resolved"
        });
        assert.equal(result.response.status, 409);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "EMERGENCY_EVENT_ID_DUPLICATE");
        assert.equal(result.body.event_id, "smoke-emergency-resolved");

        result = await request(baseUrl, "POST", "/api/emergency/events", {
            event_id: "e".repeat(121),
            device_id: deviceId,
            event_type: "smoke_event_id_too_long"
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "EMERGENCY_EVENT_ID_INVALID");
        assert.equal(result.body.error, "event_id must be <= 120 characters");

        result = await request(baseUrl, "POST", "/api/emergency/events", {
            device_id: "d".repeat(129),
            event_type: "smoke_device_id_too_long"
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "DEVICE_ID_INVALID");
        assert.equal(result.body.error, "device_id must be <= 128 characters");

        result = await request(baseUrl, "POST", "/api/emergency/events", {
            event_type: "e".repeat(121)
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "EMERGENCY_EVENT_TYPE_INVALID");
        assert.equal(result.body.error, "event_type must be <= 120 characters");

        result = await request(baseUrl, "GET", "/api/emergency/events?status=resolved&limit=1");
        assert.equal(result.response.status, 200);
        assert.equal(result.body.events.length, 1);
        assert.equal(result.body.events[0].event_type, "smoke_resolved_test");
        assert.equal(result.body.events[0].severity, "warning");
        assert.equal(result.body.events[0].status, "resolved");
        assert.equal(result.body.events[0].resolved_at.length > 0, true);

        result = await request(baseUrl, "GET", "/api/emergency/events?" + new URLSearchParams({
            status: " resolved ",
            device_id: ` ${deviceId} `,
            limit: "1"
        }).toString());
        assert.equal(result.response.status, 200);
        assert.equal(result.body.events.length, 1);
        assert.equal(result.body.events[0].event_type, "smoke_resolved_test");

        result = await request(baseUrl, "GET", "/api/emergency/events?" + new URLSearchParams({
            status: "invalid-status"
        }).toString());
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "EMERGENCY_STATUS_INVALID");

        result = await request(baseUrl, "GET", "/api/emergency/events?" + new URLSearchParams({
            device_id: "z".repeat(129)
        }).toString());
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "DEVICE_ID_INVALID");

        result = await request(baseUrl, "POST", "/api/emergency/events", {
            device_id: deviceId,
            event_type: "smoke_test",
            severity: "invalid-severity",
            status: "invalid-status",
            payload: {
                source: "smoke"
            }
        });
        assert.equal(result.response.status, 201);
        assert.equal(result.body.ok, true);

        result = await request(baseUrl, "GET", "/api/emergency/events?device_id=" + encodeURIComponent(deviceId) + "&limit=1");
        assert.equal(result.response.status, 200);
        assert.equal(result.body.events.length, 1);
        assert.equal(result.body.events[0].event_type, "smoke_test");
        assert.equal(result.body.events[0].severity, "info");
        assert.equal(result.body.events[0].status, "received");

        result = await request(baseUrl, "POST", "/api/csi/behavior", {
            event_id: "smoke-csi-presence",
            device_id: "csi-smoke-node",
            behavior_type: "presence",
            confidence: 0.7,
            features: {
                motion_score: 0.5
            },
            summary: "smoke behavior placeholder"
        });
        assert.equal(result.response.status, 201);
        assert.equal(result.body.ok, true);

        result = await request(baseUrl, "POST", "/api/csi/behavior", {
            event_id: "smoke-csi-presence",
            device_id: "csi-smoke-node",
            behavior_type: "presence",
            confidence: 0.7
        });
        assert.equal(result.response.status, 409);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "CSI_EVENT_ID_DUPLICATE");
        assert.equal(result.body.event_id, "smoke-csi-presence");

        result = await request(baseUrl, "POST", "/api/csi/behavior", {
            event_id: "c".repeat(121),
            device_id: "csi-smoke-node",
            behavior_type: "presence"
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "CSI_EVENT_ID_INVALID");
        assert.equal(result.body.error, "event_id must be <= 120 characters");

        result = await request(baseUrl, "POST", "/api/csi/behavior", {
            device_id: "d".repeat(129),
            behavior_type: "device_id_too_long"
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "DEVICE_ID_INVALID");
        assert.equal(result.body.error, "device_id must be <= 128 characters");

        result = await request(baseUrl, "POST", "/api/csi/behavior", {
            behavior_type: "b".repeat(121)
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "CSI_BEHAVIOR_TYPE_INVALID");
        assert.equal(result.body.error, "behavior_type must be <= 120 characters");

        result = await request(baseUrl, "POST", "/api/csi/behavior", {
            behavior_type: "occurred_at_too_long",
            occurred_at: "o".repeat(81)
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "CSI_OCCURRED_AT_INVALID");
        assert.equal(result.body.error, "occurred_at must be <= 80 characters");

        result = await request(baseUrl, "POST", "/api/csi/behavior", {
            device_id: "csi-smoke-node",
            behavior_type: "presence",
            summary: "s".repeat(1001)
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "CSI_SUMMARY_INVALID");
        assert.equal(result.body.error, "summary must be <= 1000 characters");

        result = await request(baseUrl, "POST", "/api/csi/behavior", {
            device_id: "csi-smoke-node",
            behavior_type: "auto_event_id"
        });
        assert.equal(result.response.status, 201);
        assert.equal(result.body.ok, true);
        assert.match(result.body.event_id, /^csi_/);

        result = await request(baseUrl, "GET", "/api/csi/behavior?device_id=csi-smoke-node&limit=1");
        assert.equal(result.response.status, 200);
        assert.equal(result.body.events.length, 1);
        assert.equal(result.body.events[0].behavior_type, "auto_event_id");

        result = await request(baseUrl, "GET", "/api/csi/behavior?" + new URLSearchParams({
            device_id: " csi-smoke-node ",
            behavior_type: " presence ",
            limit: "1"
        }).toString());
        assert.equal(result.response.status, 200);
        assert.equal(result.body.events.length, 1);
        assert.equal(result.body.events[0].behavior_type, "presence");

        result = await request(baseUrl, "GET", "/api/csi/behavior?" + new URLSearchParams({
            behavior_type: "b".repeat(121)
        }).toString());
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "CSI_BEHAVIOR_TYPE_INVALID");
        assert.equal(result.body.error, "behavior_type must be <= 120 characters");

        result = await request(baseUrl, "GET", "/api/csi/behavior?" + new URLSearchParams({
            device_id: "c".repeat(129)
        }).toString());
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "DEVICE_ID_INVALID");

        result = await request(baseUrl, "POST", "/api/lcd/display", {
            text: "缺少设备"
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "COMMAND_TARGET_REQUIRED");

        result = await request(baseUrl, "POST", "/api/lcd/display", {
            device_id: "unregistered-lcd-device",
            text: "未注册显示设备"
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "DEVICE_CAPABILITIES_REQUIRED");

        result = await request(baseUrl, "POST", "/api/lcd/display", {
            device_id: deviceId
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "COMMAND_PAYLOAD_INVALID");
        assert.equal(result.body.error, "text is required");

        result = await request(baseUrl, "POST", "/api/lcd/display", {
            device_id: deviceId,
            text: "你好",
            ttl_ms: 5000
        });
        assert.equal(result.response.status, 201);
        assert.equal(result.body.command.name, "display.show_text");
        assert.equal(result.body.command.payload.ttl_ms, 5000);

        result = await request(baseUrl, "GET", "/api/lcd/status?device_id=" + encodeURIComponent(deviceId));
        assert.equal(result.response.status, 200);
        assert.equal(result.body.devices.length, 1);
        assert.equal(result.body.devices[0].page, "message");
        assert.equal(result.body.devices[0].state.text, "你好");
        assert.equal(result.body.devices[0].state.ttl_ms, 5000);

        result = await request(baseUrl, "GET", "/api/lcd/status?" + new URLSearchParams({
            device_id: ` ${deviceId} `,
            limit: "1"
        }).toString());
        assert.equal(result.response.status, 200);
        assert.equal(result.body.devices.length, 1);
        assert.equal(result.body.devices[0].page, "message");

        result = await request(baseUrl, "GET", "/api/lcd/status?" + new URLSearchParams({
            device_id: "l".repeat(129)
        }).toString());
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "DEVICE_ID_INVALID");

        result = await request(baseUrl, "POST", "/api/lcd/status", {
            device_id: "d".repeat(129),
            page: "idle"
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "DEVICE_ID_INVALID");
        assert.equal(result.body.error, "device_id must be <= 128 characters");

        result = await request(baseUrl, "POST", "/api/lcd/status", {
            device_id: deviceId,
            page: "p".repeat(81)
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "LCD_PAGE_INVALID");
        assert.equal(result.body.error, "page must be <= 80 characters");

        result = await request(baseUrl, "POST", "/api/lcd/status", {
            device_id: deviceId,
            last_command_id: "l".repeat(121)
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "LCD_LAST_COMMAND_ID_INVALID");
        assert.equal(result.body.error, "last_command_id must be <= 120 characters");

        result = await request(baseUrl, "POST", "/api/lcd/display", {
            device_id: ` ${deviceId} `,
            text: "  归一化显示  ",
            ttl_ms: "6000"
        });
        assert.equal(result.response.status, 201);
        assert.equal(result.body.command.device_id, deviceId);
        assert.equal(result.body.command.payload.text, "归一化显示");
        assert.equal(result.body.command.payload.ttl_ms, 6000);

        result = await request(baseUrl, "GET", "/api/lcd/status?device_id=" + encodeURIComponent(deviceId));
        assert.equal(result.response.status, 200);
        assert.equal(result.body.devices.length, 1);
        assert.equal(result.body.devices[0].device_id, deviceId);
        assert.equal(result.body.devices[0].state.text, "归一化显示");
        assert.equal(result.body.devices[0].state.ttl_ms, 6000);

        const bmeDeviceId = "esp32-c5-whole-001";
        const validEspTimeMs = Date.now() - 120;
        const bmeEnvelope = {
            schema_version: 1,
            device_id: bmeDeviceId,
            device_type: "esp32c5_env_voice_node",
            firmware_version: "0.1.0-smoke",
            request_seq: 101,
            esp_uptime_ms: 987654,
            esp_time_ms: validEspTimeMs,
            time_synced: true,
            server_recv_ms: 1,
            upload_delay_ms: 999999,
            payload_type: "sensor.bme690",
            payload: {
                sensor_id: "bme690_01",
                temperature_c: 29.57,
                humidity_percent: 30.29,
                pressure_hpa: 986.26,
                gas_resistance_ohm: 35164,
                air_quality_score: 72,
                air_quality_level: "moderate",
                air_quality_confidence: "low",
                air_quality_algo_version: "esp-bme690-relative-v1",
                air_quality_source: "esp",
                gas_baseline_ohm: 82000,
                gas_ratio: 0.43,
                gas_score: 43,
                humidity_score: 87,
                baseline_ready: false,
                warmup_done: false,
                sample_count: 12
            }
        };

        result = await request(baseUrl, "POST", "/api/device/v1/ingest", bmeEnvelope);
        assert.equal(result.response.status, 201);
        assert.equal(result.body.ok, true);
        assert.equal(result.body.data.device_id, bmeDeviceId);
        assert.equal(result.body.data.payload_type, "sensor.bme690");
        assert.equal(result.body.data.sensor_id, "bme690_01");
        assert.equal(result.body.data.air_quality.air_quality_score, 72);
        assert.equal(result.body.data.air_quality.air_quality_source, "esp");
        assert.ok(result.body.data.upload_delay_ms >= 0);
        assert.ok(result.body.data.upload_delay_ms < 60000);
        assert.notEqual(result.body.server_recv_ms, bmeEnvelope.server_recv_ms);
        assert.notEqual(result.body.data.upload_delay_ms, bmeEnvelope.upload_delay_ms);

        let sensorRows = await dbAll(dbPath, "SELECT * FROM sensor_records WHERE id=? LIMIT 1", [result.body.data.id]);
        assert.equal(sensorRows.length, 1);
        assert.equal(sensorRows[0].device_id, bmeDeviceId);
        assert.equal(sensorRows[0].temperature, 29.57);
        assert.equal(sensorRows[0].humidity, 30.29);
        assert.equal(sensorRows[0].pressure, 986.26);
        assert.equal(sensorRows[0].gas_resistance, 35164);
        assert.equal(sensorRows[0].payload_type, "sensor.bme690");
        assert.equal(sensorRows[0].sensor_id, "bme690_01");
        assert.equal(sensorRows[0].air_quality_score, 72);
        assert.equal(sensorRows[0].air_quality_level, "moderate");
        assert.equal(sensorRows[0].air_quality_confidence, "low");
        assert.equal(sensorRows[0].air_quality_source, "esp");
        assert.ok(sensorRows[0].raw_json.includes("\"sensor.bme690\""));
        assert.ok(sensorRows[0].metadata_json.includes("\"time_synced\":true"));

        result = await request(baseUrl, "POST", "/api/device/v1/ingest", {
            ...bmeEnvelope,
            request_seq: 102,
            payload: {
                ...bmeEnvelope.payload,
                temperature_c: undefined
            }
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.error.code, "INVALID_PAYLOAD");

        result = await request(baseUrl, "POST", "/api/device/v1/ingest", {
            ...bmeEnvelope,
            request_seq: 103,
            esp_time_ms: Date.now(),
            time_synced: false,
            payload: {
                ...bmeEnvelope.payload,
                air_quality_score: 999,
                air_quality_level: "bad-level"
            }
        });
        assert.equal(result.response.status, 201);
        assert.equal(result.body.ok, true);
        assert.equal(result.body.data.upload_delay_ms, null);
        assert.equal(result.body.data.air_quality.air_quality_source, "server_fallback");

        result = await request(baseUrl, "POST", "/api/device/v1/ingest", {
            ...bmeEnvelope,
            request_seq: 104,
            esp_time_ms: Date.now() + 5000,
            time_synced: true
        });
        assert.equal(result.response.status, 201);
        assert.equal(result.body.data.upload_delay_ms, null);

        result = await request(baseUrl, "POST", "/api/device/v1/ingest", {
            ...bmeEnvelope,
            request_seq: 105,
            esp_time_ms: Date.now() - 120000,
            time_synced: true
        });
        assert.equal(result.response.status, 201);
        assert.equal(result.body.data.upload_delay_ms, null);

        result = await request(baseUrl, "GET", `/api/device/v1/status?${new URLSearchParams({
            device_id: bmeDeviceId
        }).toString()}`);
        assert.equal(result.response.status, 200);
        assert.equal(result.body.ok, true);
        assert.equal(result.body.status.device_id, bmeDeviceId);
        assert.equal(result.body.status.online, true);
        assert.equal(result.body.status.delay_sample_count, 1);
        assert.ok(result.body.status.avg_upload_delay_ms >= 0);

        result = await request(baseUrl, "GET", `/api/device/v1/modules/status?${new URLSearchParams({
            device_id: bmeDeviceId
        }).toString()}`);
        assert.equal(result.response.status, 200);
        assert.equal(result.body.ok, true);
        const bmeModule = result.body.modules.find(module => module.module_type === "sensor.bme690");
        assert.ok(bmeModule);
        assert.equal(bmeModule.online, true);
        assert.equal(bmeModule.delay_sample_count, 1);

        result = await request(baseUrl, "GET", `/api/device/v1/context?${new URLSearchParams({
            device_id: bmeDeviceId
        }).toString()}`);
        assert.equal(result.response.status, 200);
        assert.equal(result.body.ok, true);
        assert.equal(result.body.context.device.device_id, bmeDeviceId);
        assert.equal(result.body.context.environment.available, true);
        assert.equal(result.body.context.air_quality.available, true);
        assert.equal(result.body.context.air_quality.score, 72);
        assert.match(result.body.context.air_quality.note, /not national AQI/);
        assert.equal(result.body.context.modules["csi.motion"].available, false);
        assert.equal(result.body.context.modules["lcd.status"].available, false);

        result = await request(baseUrl, "GET", `/api/dashboard/v1/overview?${new URLSearchParams({
            device_id: bmeDeviceId
        }).toString()}`);
        assert.equal(result.response.status, 200);
        assertDashboardEnvelope(result.body, true);
        assert.equal(result.body.data.devices.length, 1);
        assert.equal(result.body.data.devices[0].csi.state, "IDLE");
        assert.equal(result.body.data.devices[0].csi.available, false);
        assert.equal(result.body.data.csi.available, false);

        const csiUpdatedAt = Date.now() - 5;
        const csiMotionEnvelope = {
            schema_version: 1,
            device_id: "sensair_s3_gateway_01",
            device_type: "S3",
            request_seq: 201,
            esp_uptime_ms: 988001,
            payload_type: "csi.motion",
            room_id: "living_room",
            payload: {
                device_id: "sensair_s3_gateway_01",
                link_id: "fused",
                state: "MOTION",
                frame_energy: 12.75,
                variance: 0.0182,
                rssi: -58,
                motion_score: 0.73,
                timestamp: csiUpdatedAt
            }
        };
        result = await request(baseUrl, "POST", "/api/device/v1/ingest", csiMotionEnvelope);
        assert.equal(result.response.status, 202);
        assert.equal(result.body.ok, true);
        assert.equal(result.body.data.device_id, "sensair_s3_gateway_01");
        assert.equal(result.body.data.payload_type, "csi.motion");
        assert.equal(result.body.data.link_id, "fused");
        assert.equal(result.body.data.state, "MOTION");
        assert.equal(result.body.data.frame_energy, 12.75);
        assert.equal(result.body.data.variance, 0.0182);
        assert.equal(result.body.data.motion_score, 0.73);

        result = await request(baseUrl, "POST", "/api/device/v1/ingest", {
            ...csiMotionEnvelope,
            request_seq: 202,
            payload: {
                ...csiMotionEnvelope.payload,
                state: "HOLD",
                frame_energy: 5.25,
                variance: 0.0021,
                rssi: -62,
                motion_score: 0.11,
                timestamp: csiUpdatedAt + 1
            }
        });
        assert.equal(result.response.status, 202);
        assert.equal(result.body.ok, true);
        assert.equal(result.body.data.device_id, "sensair_s3_gateway_01");
        assert.equal(result.body.data.state, "HOLD");

        sensorRows = await dbAll(dbPath, "SELECT * FROM sensor_records WHERE payload_type='csi.motion'");
        assert.equal(sensorRows.length, 0);

        let csiRows = await dbAll(dbPath, "SELECT * FROM csi_motion_events ORDER BY timestamp ASC, id ASC");
        assert.equal(csiRows.length, 2);
        assert.equal(csiRows[0].state, "MOTION");
        assert.equal(csiRows[0].link_id, "fused");
        assert.equal(csiRows[0].frame_energy, 12.75);
        assert.equal(csiRows[1].state, "HOLD");
        assert.ok(csiRows[0].raw_json.includes("\"csi.motion\""));

        result = await request(baseUrl, "GET", `/api/device/v1/modules/status?${new URLSearchParams({
            device_id: "sensair_s3_gateway_01"
        }).toString()}`);
        const csiModule = result.body.modules.find(module => module.module_type === "csi.motion");
        assert.ok(csiModule);
        assert.equal(csiModule.online, true);

        result = await request(baseUrl, "GET", `/api/device/v1/context?${new URLSearchParams({
            device_id: "sensair_s3_gateway_01"
        }).toString()}`);
        assert.equal(result.response.status, 200);
        assert.equal(result.body.context.modules["csi.motion"].available, true);

        result = await request(baseUrl, "GET", "/api/dashboard/v1/overview");
        assert.equal(result.response.status, 200);
        assertDashboardEnvelope(result.body, true);
        assert.equal(result.body.data.csi.state, "HOLD");
        assert.equal(result.body.data.csi.available, true);
        assert.equal(result.body.data.csi.motion_score, 0.11);
        assert.equal(result.body.data.csi.frame_energy, 5.25);

        result = await request(baseUrl, "GET", "/api/dashboard/v1/csi/history?limit=5");
        assert.equal(result.response.status, 200);
        assertDashboardEnvelope(result.body, true);
        assert.equal(result.body.data.events.length, 2);
        assert.equal(result.body.data.events[0].state, "MOTION");
        assert.equal(result.body.data.events[1].state, "HOLD");
        assert.equal(result.body.data.events[1].motion_score, 0.11);

        result = await request(baseUrl, "POST", "/api/device/v1/ingest", {
            ...csiMotionEnvelope,
            request_seq: 203,
            payload: {
                ...csiMotionEnvelope.payload,
                occupancy: {
                    state: "occupied"
                }
            }
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.error.code, "LEGACY_CSI_MODEL_NOT_ACCEPTED");

        result = await request(baseUrl, "POST", "/api/device/v1/ingest", {
            ...csiMotionEnvelope,
            request_seq: 204,
            payload: {
                ...csiMotionEnvelope.payload,
                raw_csi: [1, 2, 3]
            }
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.error.code, "RAW_CSI_NOT_ACCEPTED");

        result = await request(baseUrl, "POST", "/api/device/v1/ingest", {
            ...csiMotionEnvelope,
            request_seq: 205,
            payload: {
                ...csiMotionEnvelope.payload,
                state: "occupied"
            }
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.error.code, "INVALID_CSI_STATE");

        result = await request(baseUrl, "GET", `/api/device/v1/sensors/latest?${new URLSearchParams({
            device_id: bmeDeviceId
        }).toString()}`);
        assert.equal(result.response.status, 200);
        assert.equal(result.body.ok, true);
        assert.equal(result.body.sensor.device_id, bmeDeviceId);
        assert.equal(result.body.sensor.air_quality_score, 72);

        await dbRun(dbPath, "UPDATE device_module_status SET last_seen_ms=? WHERE device_id=? AND module_type='sensor.bme690'", [
            Date.now() - 120000,
            bmeDeviceId
        ]);
        result = await request(baseUrl, "GET", `/api/device/v1/status?${new URLSearchParams({
            device_id: bmeDeviceId
        }).toString()}`);
        assert.equal(result.body.status.online, true);
        result = await request(baseUrl, "GET", `/api/device/v1/modules/status?${new URLSearchParams({
            device_id: bmeDeviceId
        }).toString()}`);
        const staleBmeModule = result.body.modules.find(module => module.module_type === "sensor.bme690");
        assert.equal(staleBmeModule.online, false);

        result = await request(baseUrl, "POST", "/api/llm/text", {
            text: "现在房间环境怎么样",
            device_id: bmeDeviceId
        });
        assert.equal(result.response.status, 200);
        assert.equal(result.body.ok, true);
        assert.match(mockLlm.requests[mockLlm.requests.length - 1].body, /设备上下文/);
        assert.match(mockLlm.requests[mockLlm.requests.length - 1].body, /BME690/);
        assert.match(mockLlm.requests[mockLlm.requests.length - 1].body, /不是国标 AQI/);
        assert.match(mockLlm.requests[mockLlm.requests.length - 1].body, /已过期|历史参考|not recent|offline/);

        result = await request(baseUrl, "POST", "/sensor", {
            temperature: 25.5,
            humidity: 57.2,
            pressure: 1012.3,
            gas_resistance: 123.4,
            device_id: deviceId
        });
        assert.equal(result.response.status, 200);
        assert.equal(result.body.success, true);

        result = await request(baseUrl, "GET", "/api/time/now");
        assert.equal(result.response.status, 200);
        assert.equal(result.body.ok, true);
        assert.equal(typeof result.body.server_time_ms, "number");
        assert.match(result.body.server_time_iso, /^\d{4}-\d{2}-\d{2}T/);

        const espSendMs = Date.now() - 25;
        result = await request(baseUrl, "POST", "/api/time/ping", {
            device_id: deviceId,
            esp_send_ms: espSendMs,
            esp_uptime_ms: 123456
        });
        assert.equal(result.response.status, 200);
        assert.equal(result.body.ok, true);
        assert.equal(result.body.device_id, deviceId);
        assert.equal(result.body.esp_send_ms, espSendMs);
        assert.equal(typeof result.body.estimated_one_way_delay_ms, "number");

        result = await request(baseUrl, "GET", "/api/time/status");
        assert.equal(result.response.status, 200);
        assert.equal(result.body.ok, true);
        assert.equal(result.body.latest_ping.device_id, deviceId);

        result = await request(baseUrl, "POST", "/api/time/ping", {
            device_id: ` ${"t".repeat(140)} `,
            esp_send_ms: "not-a-number",
            esp_uptime_ms: "456"
        });
        assert.equal(result.response.status, 200);
        assert.equal(result.body.ok, true);
        assert.equal(result.body.device_id, "t".repeat(128));
        assert.equal(result.body.esp_send_ms, null);
        assert.equal(result.body.esp_uptime_ms, 456);
        assert.equal(result.body.estimated_one_way_delay_ms, null);

        result = await request(baseUrl, "GET", "/api/time/status");
        assert.equal(result.response.status, 200);
        assert.equal(result.body.ok, true);
        assert.equal(result.body.latest_ping.device_id, "t".repeat(128));

        result = await request(baseUrl, "GET", "/sensor/latest");
        assert.equal(result.response.status, 200);
        assert.equal(result.body.device_id, deviceId);
        assert.equal(result.body.time_sync.latest_ping.device_id, "t".repeat(128));

        result = await request(baseUrl, "GET", "/sensor/history?limit=1");
        assert.equal(result.response.status, 200);
        assert.equal(result.body.length, 1);
        assert.equal(result.body[0].device_id, deviceId);

        result = await request(baseUrl, "POST", "/sensor", {
            temperature: "26.5",
            humidity: "bad-humidity",
            pressure: "1010.25",
            gas_resistance: {
                invalid: true
            },
            device_id: ` ${"s".repeat(140)} `
        });
        assert.equal(result.response.status, 200);
        assert.equal(result.body.success, true);
        assert.equal(result.body.device_id, "s".repeat(128));

        result = await request(baseUrl, "GET", "/sensor/latest");
        assert.equal(result.response.status, 200);
        assert.equal(result.body.device_id, "s".repeat(128));
        assert.equal(result.body.temperature, 26.5);
        assert.equal(result.body.humidity, null);
        assert.equal(result.body.pressure, 1010.25);
        assert.equal(result.body.gas_resistance, null);

        result = await request(baseUrl, "POST", "/asr", {
            text: "烟雾 ASR"
        });
        assert.equal(result.response.status, 200);
        assert.equal(result.body.ok, true);
        assert.equal(result.body.success, true);

        result = await request(baseUrl, "GET", "/asr/latest");
        assert.equal(result.response.status, 200);
        assert.equal(result.body.text, "烟雾 ASR");

        result = await request(baseUrl, "POST", "/asr", {
            text: ` ${"a".repeat(4001)} `
        });
        assert.equal(result.response.status, 200);
        assert.equal(result.body.ok, true);
        assert.equal(result.body.success, true);

        result = await request(baseUrl, "GET", "/asr/latest");
        assert.equal(result.response.status, 200);
        assert.equal(result.body.text.length, 4000);
        assert.equal(result.body.text, "a".repeat(4000));

        result = await request(baseUrl, "POST", "/llm", {
            prompt: "烟雾 prompt",
            response: "烟雾 response"
        });
        assert.equal(result.response.status, 200);
        assert.equal(result.body.ok, true);
        assert.equal(result.body.success, true);

        result = await request(baseUrl, "GET", "/llm/latest");
        assert.equal(result.response.status, 200);
        assert.equal(result.body.prompt, "烟雾 prompt");
        assert.equal(result.body.response, "烟雾 response");

        result = await request(baseUrl, "POST", "/llm", {
            prompt: ` ${"p".repeat(4001)} `,
            response: ` ${"r".repeat(4001)} `
        });
        assert.equal(result.response.status, 200);
        assert.equal(result.body.ok, true);
        assert.equal(result.body.success, true);

        result = await request(baseUrl, "GET", "/llm/latest");
        assert.equal(result.response.status, 200);
        assert.equal(result.body.prompt, "p".repeat(4000));
        assert.equal(result.body.response, "r".repeat(4000));

        const dashboardDeviceQuery = new URLSearchParams({
            device_id: bmeDeviceId
        }).toString();
        const dashboardHistoryQuery = new URLSearchParams({
            device_id: bmeDeviceId,
            limit: "5"
        }).toString();

        const dashboardSnapshot = {
            schema_version: 2,
            payload_type: "gateway.dashboard_snapshot",
            source: "s3_gateway",
            gateway: {
                gateway_id: "sensair_s3_gateway_01",
                online: true,
                softap_ready: true,
                sta_connected: true,
                server_available: true,
                voice_busy: false,
                last_error: "",
                timestamp: Date.now()
            },
            csi: {
                device_id: "sensair_s3_gateway_01",
                link_id: "fused",
                state: "IDLE",
                available: true,
                frame_energy: 3.5,
                variance: 0.004,
                rssi: -61,
                motion_score: 0.21,
                timestamp: Date.now() + 1000
            },
            devices: [{
                device_id: bmeDeviceId,
                local_id: 1,
                name: "SensaiShuttle",
                room_name: "living_room",
                online: true,
                wifi_rssi: -58,
                timestamp: Date.now(),
                sensors: {
                    temperature: 29.57,
                    humidity: 30.29,
                    pressure: 986.26,
                    gas_resistance: 35164,
                    air_quality_score: 72,
                    air_quality_level: "moderate",
                    air_quality_source: "s3_mapped"
                },
                appliances: {
                    air_conditioner: {
                        power: false,
                        mode: "cool",
                        target_temperature: 26,
                        source: "mock",
                        mock: true
                    }
                }
            }],
            home_summary: {
                online_device_count: 1,
                offline_device_count: 0,
                avg_temperature: 29.57,
                avg_humidity: 30.29,
                avg_air_quality: 72
            },
            history: [{
                device_id: bmeDeviceId,
                sensor_type: "bme690",
                timestamp: Date.now(),
                temperature: 29.57,
                humidity: 30.29,
                pressure: 986.26,
                gas_resistance: 35164,
                air_quality_score: 72,
                air_quality_level: "moderate"
            }],
            recent_voice_events: [{
                device_id: bmeDeviceId,
                event: "voice_turn_completed",
                timestamp: Date.now(),
                duration_ms: 2000,
                source: "s3_gateway"
            }],
            recent_commands: [{
                command_id: "cmd-smoke-001",
                device_id: bmeDeviceId,
                command_code: 2,
                status: "completed",
                timestamp: Date.now(),
                source: "s3_gateway"
            }]
        };

        result = await request(baseUrl, "POST", "/api/device/v1/gateway-state", dashboardSnapshot);
        assert.equal(result.response.status, 202);
        assert.equal(result.body.ok, true);
        assert.equal(result.body.data.payload_type, "gateway.dashboard_snapshot");
        assert.equal(result.body.data.gateway_id, "sensair_s3_gateway_01");
        assert.equal(result.body.data.device_count, 1);
        const persistedSnapshotRows = await dbAll(dbPath, "SELECT payload_json FROM dashboard_snapshots WHERE snapshot_id=? LIMIT 1", [result.body.data.snapshot_id]);
        assert.equal(persistedSnapshotRows.length, 1);
        const persistedSnapshot = JSON.parse(persistedSnapshotRows[0].payload_json);
        assert.equal(persistedSnapshot.mock_persistence, "stripped");
        assert.deepEqual(persistedSnapshot.devices[0].appliances, {});

        const dashboardEndpoints = [
            `/api/dashboard/v1/overview?${dashboardDeviceQuery}`,
            `/api/dashboard/v1/sensors/latest?${dashboardDeviceQuery}`,
            `/api/dashboard/v1/sensors/history?${dashboardHistoryQuery}`,
            `/api/dashboard/v1/devices/${encodeURIComponent(bmeDeviceId)}/history?limit=5`,
            "/api/dashboard/v1/asr/latest",
            "/api/dashboard/v1/llm/latest",
            "/api/dashboard/v1/csi/history?limit=5",
            "/api/dashboard/v1/time/status",
            `/api/dashboard/v1/device/status?${dashboardDeviceQuery}`,
            `/api/dashboard/v1/modules/status?${dashboardDeviceQuery}`
        ];

        for (const endpoint of dashboardEndpoints) {
            result = await request(baseUrl, "GET", endpoint);
            assert.equal(result.response.status, 200, `${endpoint} should be reachable`);
            assertDashboardEnvelope(result.body, true);
        }

        result = await request(baseUrl, "GET", `/api/dashboard/v1/sensors/latest?${dashboardDeviceQuery}`);
        assert.equal(result.response.status, 200);
        assertDashboardEnvelope(result.body, true);
        assert.equal(result.body.data.device_id, bmeDeviceId);
        assert.equal(result.body.data.temperature, 29.57);
        assert.equal(result.body.data.humidity, 30.29);
        assert.equal(result.body.data.pressure, 986.26);
        assert.equal(result.body.data.gas_resistance, 35164);
        assert.equal(result.body.data.air_quality_score, 72);
        assert.equal(result.body.data.air_quality_level, "moderate");
        assert.equal(result.body.data.air_quality_confidence, "low");
        assert.equal(result.body.data.air_quality_source, "esp");
        assert.equal(result.body.data.air_quality.air_quality_score, 72);
        assert.notStrictEqual(result.body.data.gas_resistance, result.body.data.air_quality_score);
        assert.equal(typeof result.body.data.online, "boolean");
        assert.equal(typeof result.body.data.device_online, "boolean");
        assert.equal(typeof result.body.data.sensor_online, "boolean");
        assert.equal(hasOwn(result.body.data, "latest_upload_delay_ms"), true);
        assert.equal(hasOwn(result.body.data, "avg_upload_delay_ms"), true);
        assert.equal(hasOwn(result.body.data, "delay_sample_count"), true);
        assert.ok(result.body.data.time_sync);

        result = await request(baseUrl, "GET", `/api/dashboard/v1/sensors/history?${dashboardHistoryQuery}`);
        assert.equal(result.response.status, 200);
        assertDashboardEnvelope(result.body, true);
        assert.ok(Array.isArray(result.body.data));
        assert.ok(result.body.data.length >= 1);
        const dashboardHistoryBme = result.body.data.find(row => row.gas_resistance === 35164 && row.air_quality_score === 72);
        assert.ok(dashboardHistoryBme);
        assert.equal(dashboardHistoryBme.air_quality_level, "moderate");
        assert.equal(dashboardHistoryBme.air_quality_confidence, "low");
        assert.equal(dashboardHistoryBme.air_quality_source, "esp");

        result = await request(baseUrl, "GET", "/api/dashboard/v1/sensors/history?limit=bad");
        assert.equal(result.response.status, 400);
        assertDashboardEnvelope(result.body, false);
        assert.equal(result.body.data, null);
        assert.equal(result.body.error.code, "DASHBOARD_BAD_LIMIT");

        result = await request(baseUrl, "GET", `/api/dashboard/v1/overview?${dashboardDeviceQuery}`);
        assert.equal(result.response.status, 200);
        assertDashboardEnvelope(result.body, true);
        assert.equal(result.body.data.gateway.gateway_id, "sensair_s3_gateway_01");
        assert.equal(result.body.data.gateway.online, true);
        assert.equal(result.body.data.devices.length, 1);
        assert.equal(result.body.data.devices[0].device_id, bmeDeviceId);
        assert.equal(result.body.data.devices[0].sensors.gas_resistance, 35164);
        assert.equal(result.body.data.devices[0].sensors.air_quality_score, 72);
        assert.equal(result.body.data.csi.state, "HOLD");
        assert.equal(result.body.data.csi.available, true);
        assert.equal(result.body.data.csi.motion_score, 0.11);
        assert.equal(result.body.data.csi.frame_energy, 5.25);
        assert.equal(result.body.data.devices[0].appliances.air_conditioner.source, "mock");
        assert.equal(result.body.data.devices[0].appliances.fan.mock, true);
        assert.equal(result.body.data.home_summary.online_device_count, 1);
        assert.equal(result.body.data.home_summary.avg_air_quality, 72);
        assert.equal(result.body.data.history[0].device_id, bmeDeviceId);
        assert.equal(result.body.data.history[0].air_quality_level, "moderate");
        assert.equal(result.body.data.recent_voice_events[0].event, "voice_turn_completed");
        assert.equal(result.body.data.recent_commands[0].command_id, "cmd-smoke-001");

        result = await request(baseUrl, "GET", `/api/dashboard/v1/devices/${encodeURIComponent(bmeDeviceId)}/history?limit=5`);
        assert.equal(result.response.status, 200);
        assertDashboardEnvelope(result.body, true);
        assert.ok(Array.isArray(result.body.data));
        assert.ok(result.body.data.some(row => row.device_id === bmeDeviceId));

        result = await request(baseUrl, "GET", "/sensor/latest");
        assert.equal(result.response.status, 200);
        assert.equal(hasOwn(result.body, "data"), false);
        assert.equal(hasOwn(result.body, "error"), false);
        assert.equal(hasOwn(result.body, "server_time_ms"), false);

        result = await request(baseUrl, "GET", "/sensor/history?limit=1");
        assert.equal(result.response.status, 200);
        assert.ok(Array.isArray(result.body));

        result = await request(baseUrl, "GET", "/asr/latest");
        assert.equal(result.response.status, 200);
        assert.equal(result.body.text, "a".repeat(4000));
        assert.equal(hasOwn(result.body, "data"), false);

        result = await request(baseUrl, "GET", "/llm/latest");
        assert.equal(result.response.status, 200);
        assert.equal(result.body.prompt, "p".repeat(4000));
        assert.equal(result.body.response, "r".repeat(4000));
        assert.equal(hasOwn(result.body, "data"), false);

        result = await request(baseUrl, "GET", "/api/time/status");
        assert.equal(result.response.status, 200);
        assert.equal(result.body.ok, true);
        assert.equal(hasOwn(result.body, "data"), false);

        result = await request(baseUrl, "GET", `/api/device/v1/status?${dashboardDeviceQuery}`);
        assert.equal(result.response.status, 200);
        assert.equal(result.body.ok, true);
        assert.equal(hasOwn(result.body, "status"), true);
        assertDashboardEnvelope(result.body, true);
        assert.ok(Array.isArray(result.body.data.devices));

        result = await request(baseUrl, "GET", "/api/not-found-for-smoke");
        assert.equal(result.response.status, 404);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.error, "Not found");

        result = await request(baseUrl, "GET", "/sensor/not-found-for-smoke");
        assert.equal(result.response.status, 404);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.error, "Not found");

        result = await request(baseUrl, "GET", "/dashboard");
        assert.equal(result.response.status, 200);
        assert.match(result.response.headers.get("content-type") || "", /text\/html/);
        assert.ok(Buffer.isBuffer(result.body));
        assert.ok(result.body.length > 0);

        result = await request(baseUrl, "GET", "/api/smart-home/v1/status");
        assert.equal(result.response.status, 200);
        assertDashboardEnvelope(result.body);
        assert.equal(result.body.data.configured, false);
        assert.equal(result.body.data.available, false);
        assert.equal(result.body.data.provider, "none");
        assert.deepEqual(result.body.data.devices, []);

        result = await request(baseUrl, "POST", "/api/logs/v1/system", {
            level: "info",
            source: "server",
            message: "manual system log test",
            payload: {
                from: "smoke"
            }
        });
        assert.equal(result.response.status, 201);
        assertDashboardEnvelope(result.body);
        assert.equal(result.body.data.log.message, "manual system log test");

        result = await request(baseUrl, "GET", "/api/logs/v1/system?limit=10");
        assert.equal(result.response.status, 200);
        assertDashboardEnvelope(result.body);
        assert.ok(result.body.data.logs.some(log => log.message === "manual system log test"));

        result = await request(baseUrl, "POST", "/api/logs/v1/alarms", {
            level: "warning",
            source: "device",
            device_id: "verify-c5",
            room_id: "bedroom",
            room_name: "卧室",
            title: "测试报警",
            message: "curl alarm test",
            payload: {
                from: "smoke"
            }
        });
        assert.equal(result.response.status, 201);
        assertDashboardEnvelope(result.body);
        assert.equal(result.body.data.alarm.title, "测试报警");
        assert.equal(result.body.data.alarm.acknowledged, false);

        result = await request(baseUrl, "GET", "/api/logs/v1/alarms?limit=10");
        assert.equal(result.response.status, 200);
        assertDashboardEnvelope(result.body);
        assert.ok(result.body.data.alarms.some(alarm => alarm.message === "curl alarm test"));

        result = await request(baseUrl, "POST", "/api/smart-home/v1/state", {
            provider: "s3_gateway",
            gateway_id: "verify-s3",
            devices: [{
                id: "ac_living_room",
                type: "air_conditioner",
                name: "客厅空调",
                room_id: "living_room",
                room_name: "客厅",
                online: true,
                state: {
                    power: "off",
                    temperature: 26
                }
            }]
        }, VERIFY_GATEWAY_HEADERS);
        assert.equal(result.response.status, 202);
        assertDashboardEnvelope(result.body);
        assert.equal(result.body.data.provider, "s3_gateway");
        assert.equal(result.body.data.devices.length, 1);

        result = await request(baseUrl, "GET", "/api/smart-home/v1/status");
        assert.equal(result.response.status, 200);
        assertDashboardEnvelope(result.body);
        assert.equal(result.body.data.configured, true);
        assert.equal(result.body.data.available, true);
        assert.equal(result.body.data.provider, "s3_gateway");
        assert.equal(result.body.data.devices[0].id, "ac_living_room");
        assert.equal(result.body.data.devices[0].state.power, "off");

        result = await request(baseUrl, "POST", "/api/smart-home/v1/control", {
            target_id: "ac_living_room",
            room_id: "living_room",
            room_name: "客厅",
            action: "set_power",
            params: {
                power: "on"
            },
            source: "dashboard",
            requested_by: "user"
        });
        assert.equal(result.response.status, 202);
        assertDashboardEnvelope(result.body);
        assert.equal(result.body.data.command.status, "queued");
        assert.match(result.body.data.message, /waiting for gateway pull/);
        const smartHomeCommandId = result.body.data.command.command_id;

        result = await request(baseUrl, "GET", "/api/smart-home/v1/commands?limit=10");
        assert.equal(result.response.status, 200);
        assertDashboardEnvelope(result.body);
        assert.ok(result.body.data.commands.some(command => command.command_id === smartHomeCommandId));

        result = await request(baseUrl, "GET", "/api/smart-home/v1/commands/pending?gateway_id=verify-s3&limit=10", null, VERIFY_GATEWAY_HEADERS);
        assert.equal(result.response.status, 200);
        assertDashboardEnvelope(result.body);
        assert.equal(result.body.data.commands.length, 1);
        assert.equal(result.body.data.commands[0].command_id, smartHomeCommandId);
        assert.equal(result.body.data.commands[0].status, "dispatched");

        result = await request(baseUrl, "POST", `/api/smart-home/v1/commands/${smartHomeCommandId}/ack`, {
            status: "succeeded",
            result: {
                applied: true
            },
            executed_at_ms: Date.now()
        }, VERIFY_GATEWAY_HEADERS);
        assert.equal(result.response.status, 200);
        assertDashboardEnvelope(result.body);
        assert.equal(result.body.data.command.status, "succeeded");

        result = await request(baseUrl, "POST", "/api/commands/v1/natural-language", {
            text: "把客厅空调打开到 26 度",
            source: "dashboard",
            room_id: "living_room",
            device_id: "verify-s3"
        });
        assert.equal(result.response.status, 202);
        assertDashboardEnvelope(result.body);
        assert.equal(result.body.data.command.type, "natural_language");
        assert.equal(result.body.data.command.status, "queued");
        assert.equal(result.body.data.command.parsed_intent, null);
        const naturalLanguageCommandId = result.body.data.command.command_id;

        result = await request(baseUrl, "GET", "/api/commands/v1/recent?limit=10");
        assert.equal(result.response.status, 200);
        assertDashboardEnvelope(result.body);
        assert.ok(result.body.data.commands.some(command => command.command_id === naturalLanguageCommandId));

        result = await request(baseUrl, "POST", "/api/logs/v1/cleanup", {
            types: ["system", "alarm", "command"],
            older_than_ms: 604800000,
            dry_run: true
        });
        assert.equal(result.response.status, 200);
        assertDashboardEnvelope(result.body);
        assert.equal(result.body.data.dry_run, true);
        assert.equal(typeof result.body.data.deleted.system, "number");

        result = await request(baseUrl, "GET", "/api/voice/prompt/config");
        assert.equal(result.response.status, 200);
        assert.equal(result.body.ok, true);
        assert.equal(result.body.config.wake_prompt_text, "我在，你说");
        assert.equal(result.body.config.sample_rate, 16000);
        assert.equal(result.body.config.format, "s16le");
        assert.equal(result.body.config.channels, 1);
        assert.ok(result.body.config.voice_config_hash);
        const initialPromptHash = result.body.config.voice_config_hash;

        result = await request(baseUrl, "PUT", "/api/voice/prompt/config", {
            wake_prompt_text: "你好，我在",
            voice_id: "smoke_voice_v2",
            speed: 1.05,
            pitch: 1,
            volume: 1,
            sample_rate: 16000,
            format: "s16le",
            channels: 1
        });
        assert.equal(result.response.status, 200);
        assert.equal(result.body.ok, true);
        assert.equal(result.body.config.wake_prompt_text, "你好，我在");
        assert.equal(result.body.config.voice_id, "smoke_voice_v2");
        assert.notEqual(result.body.config.voice_config_hash, initialPromptHash);
        const updatedPromptHash = result.body.config.voice_config_hash;
        const updatedPromptVersion = result.body.config.prompt_version;

        const promptPath = `/api/voice/prompt-cache?${new URLSearchParams({
            prompt_key: "smoke_wake",
            device_id: deviceId
        }).toString()}`;
        result = await request(baseUrl, "GET", promptPath);
        assert.equal(result.response.status, 200);
        assert.equal(result.response.headers.get("x-audio-format"), "pcm_s16le_mono_16k");
        assert.equal(result.response.headers.get("x-audio-sample-rate"), "16000");
        assert.equal(result.response.headers.get("x-audio-channels"), "1");
        assert.equal(result.response.headers.get("x-voice-config-hash"), updatedPromptHash);
        assert.equal(result.response.headers.get("x-audio-version"), updatedPromptVersion);
        assert.equal(result.response.headers.get("x-prompt-cache"), "miss");
        assert.equal(result.body.length, 32000);
        const promptCacheFiles = fs.readdirSync(promptCacheDir).filter(file => file.includes("smoke_wake"));
        assert.ok(promptCacheFiles.some(file => file.endsWith(".pcm")));
        assert.ok(promptCacheFiles.some(file => file.endsWith(".json")));

        result = await request(baseUrl, "GET", promptPath);
        assert.equal(result.response.status, 200);
        assert.equal(result.response.headers.get("x-prompt-cache"), "hit");
        assert.equal(result.response.headers.get("x-voice-config-hash"), updatedPromptHash);
        assert.equal(result.body.length, 32000);

        result = await request(baseUrl, "GET", `/api/voice/prompt?${new URLSearchParams({
            wake: "1",
            prompt_key: "smoke_wake",
            device_id: deviceId
        }).toString()}`);
        assert.equal(result.response.status, 200);
        assert.equal(result.response.headers.get("x-prompt-cache"), "hit");

        const staleDbPath = path.join(tempDir, "nested", "stale-smoke.sqlite");
        const stalePort = String(Number(port) + 1000);
        const staleBaseUrl = `http://127.0.0.1:${stalePort}`;
        const staleChild = spawn(process.execPath, ["server.js"], {
            cwd: path.join(__dirname, ".."),
            env: {
                ...process.env,
                PORT: stalePort,
                ESP_SERVER_DB_PATH: staleDbPath,
                VOICE_TURN_MOCK: "0",
                VOICE_PROMPT_CACHE_DIR: promptCacheDir,
                VOICE_PROMPT_CONFIG_PATH: promptConfigPath,
                VOLC_GATEWAY_API_KEY: "",
                LLM_API_KEY: "smoke-llm-key",
                LLM_BASE_URL: mockLlm.baseUrl,
                LLM_CHAT_PATH: "/v1/chat/completions"
            },
            stdio: ["ignore", "pipe", "pipe"]
        });
        try {
            await waitForServer(staleChild);
            result = await request(staleBaseUrl, "GET", `/api/voice/prompt-cache?${new URLSearchParams({
                prompt_key: "smoke_wake",
                device_id: deviceId,
                refresh: "1"
            }).toString()}`);
            assert.equal(result.response.status, 200);
            assert.equal(result.response.headers.get("x-prompt-cache"), "stale");
            assert.equal(result.body.length, 32000);
        } finally {
            const staleStop = await stopServer(staleChild);
            assert.equal(staleStop.killed, false);
            assert.equal(staleStop.exited, true);
        }

        const voiceTurnPcm = Buffer.alloc(3200);
        for (let i = 0; i < voiceTurnPcm.length; i += 2) {
            voiceTurnPcm.writeInt16LE(i % 64 === 0 ? 12000 : 0, i);
        }
        result = await requestRaw(baseUrl, "POST", "/api/voice/turn", voiceTurnPcm, {
            "Content-Type": "audio/L16; rate=16000; channels=1",
            "X-Audio-Format": "pcm_s16le_mono_16k",
            "X-Device-Id": "esp-smoke-c5",
            "X-Voice-Turn-Id": "smoke-turn-001"
        });
        assert.equal(result.response.status, 200);
        assert.equal(result.response.headers.get("x-audio-format"), "pcm_s16le_mono_16k");
        assert.equal(result.body.length, voiceTurnPcm.length);
        let rows = await dbAll(dbPath, "SELECT * FROM voice_turns WHERE request_id=? LIMIT 1", ["smoke-turn-001"]);
        assert.equal(rows.length, 1);
        assert.equal(rows[0].device_id, "esp-smoke-c5");
        assert.equal(rows[0].mode, "mock");
        assert.equal(rows[0].status, "success");
        assert.equal(rows[0].status_code, 200);
        assert.equal(rows[0].input_bytes, voiceTurnPcm.length);
        assert.equal(rows[0].response_bytes, voiceTurnPcm.length);

        result = await requestRaw(baseUrl, "POST", "/api/voice/turn", voiceTurnPcm, {
            "Content-Type": "audio/L16; rate=16000; channels=1",
            "X-Device-Id": "esp-smoke-c5",
            "X-Voice-Turn-Id": "smoke-turn-missing-format"
        });
        assert.equal(result.response.status, 415);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "VOICE_UNSUPPORTED_AUDIO_FORMAT");
        assert.match(result.body.error, /pcm_s16le_mono_16k/);
        rows = await dbAll(dbPath, "SELECT * FROM voice_turns WHERE request_id=? LIMIT 1", ["smoke-turn-missing-format"]);
        assert.equal(rows.length, 1);
        assert.equal(rows[0].device_id, "esp-smoke-c5");
        assert.equal(rows[0].status, "rejected");
        assert.equal(rows[0].status_code, 415);
        assert.equal(rows[0].error_code, "VOICE_UNSUPPORTED_AUDIO_FORMAT");
        assert.equal(rows[0].input_bytes, voiceTurnPcm.length);

        result = await requestRaw(baseUrl, "POST", "/api/voice/turn", voiceTurnPcm, {
            "Content-Type": "application/octet-stream",
            "X-Audio-Format": "pcm_s16le_mono_16k",
            "X-Device-Id": "esp-smoke-c5",
            "X-Voice-Turn-Id": "smoke-turn-bad-content-type"
        });
        assert.equal(result.response.status, 415);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "VOICE_UNSUPPORTED_CONTENT_TYPE");
        assert.match(result.body.error, /audio\/L16/);
        rows = await dbAll(dbPath, "SELECT * FROM voice_turns WHERE request_id=? LIMIT 1", ["smoke-turn-bad-content-type"]);
        assert.equal(rows.length, 1);
        assert.equal(rows[0].device_id, "esp-smoke-c5");
        assert.equal(rows[0].status, "rejected");
        assert.equal(rows[0].status_code, 415);
        assert.equal(rows[0].error_code, "VOICE_UNSUPPORTED_CONTENT_TYPE");
        assert.equal(rows[0].input_bytes, voiceTurnPcm.length);

        const oversizedVoiceTurnPcm = Buffer.alloc(5000);
        result = await requestRaw(baseUrl, "POST", "/api/voice/turn", oversizedVoiceTurnPcm, {
            "Content-Type": "audio/L16; rate=16000; channels=1",
            "X-Audio-Format": "pcm_s16le_mono_16k",
            "X-Device-Id": "esp-smoke-c5",
            "X-Voice-Turn-Id": "smoke-turn-too-large"
        });
        assert.equal(result.response.status, 413);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "VOICE_BODY_TOO_LARGE");
        rows = await dbAll(dbPath, "SELECT * FROM voice_turns WHERE request_id=? LIMIT 1", ["smoke-turn-too-large"]);
        assert.equal(rows.length, 1);
        assert.equal(rows[0].device_id, "esp-smoke-c5");
        assert.equal(rows[0].status, "failed");
        assert.equal(rows[0].status_code, 413);
        assert.equal(rows[0].error_code, "VOICE_BODY_TOO_LARGE");
        assert.ok(rows[0].input_bytes >= 4096);

        result = await requestRaw(baseUrl, "POST", "/api/voice/turn", Buffer.alloc(0), {
            "Content-Type": "audio/L16; rate=16000; channels=1",
            "X-Audio-Format": "pcm_s16le_mono_16k",
            "X-Device-Id": "esp-smoke-c5",
            "X-Voice-Turn-Id": "smoke-turn-empty-body"
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "VOICE_BODY_EMPTY");
        rows = await dbAll(dbPath, "SELECT * FROM voice_turns WHERE request_id=? LIMIT 1", ["smoke-turn-empty-body"]);
        assert.equal(rows.length, 1);
        assert.equal(rows[0].status, "rejected");
        assert.equal(rows[0].status_code, 400);
        assert.equal(rows[0].error_code, "VOICE_BODY_EMPTY");
        assert.equal(rows[0].input_bytes, 0);

        result = await requestRaw(baseUrl, "POST", "/api/voice/turn", Buffer.from([0x01]), {
            "Content-Type": "audio/L16; rate=16000; channels=1",
            "X-Audio-Format": "pcm_s16le_mono_16k",
            "X-Device-Id": "esp-smoke-c5",
            "X-Voice-Turn-Id": "smoke-turn-odd-body"
        });
        assert.equal(result.response.status, 400);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "VOICE_PCM_ALIGNMENT_INVALID");
        rows = await dbAll(dbPath, "SELECT * FROM voice_turns WHERE request_id=? LIMIT 1", ["smoke-turn-odd-body"]);
        assert.equal(rows.length, 1);
        assert.equal(rows[0].status, "rejected");
        assert.equal(rows[0].status_code, 400);
        assert.equal(rows[0].error_code, "VOICE_PCM_ALIGNMENT_INVALID");
        assert.equal(rows[0].input_bytes, 1);

        const smokeToday = new Date().toISOString().slice(0, 10);
        const archivedTurnId = "smoke-archived-turn-delete";
        result = await request(baseUrl, "POST", "/api/conversation/turns", {
            turn_id: archivedTurnId,
            input_text: "archived conversation must still be deleted",
            memory_level: "archived"
        });
        assert.equal(result.response.status, 201);
        const archivedProfileKey = "smoke.archived.profile.delete";
        result = await request(baseUrl, "POST", "/api/memory/profile", {
            profile_key: archivedProfileKey,
            profile_value: "archived profile must still be deleted",
            status: "archived"
        });
        assert.equal(result.response.status, 200);
        const archivedDailyMemoryDate = smokeToday;
        result = await request(baseUrl, "POST", "/api/memory/daily", {
            memory_date: archivedDailyMemoryDate,
            summary: "archived summary must still be deleted",
            status: "archived"
        });
        assert.equal(result.response.status, 201);

        result = await request(baseUrl, "GET", "/api/user-data/summary");
        assert.equal(result.response.status, 401);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "USER_DATA_ADMIN_REQUIRED");

        result = await request(baseUrl, "GET", "/api/user-data/summary", null, USER_DATA_HEADERS);
        assert.equal(result.response.status, 200);
        assert.equal(result.body.ok, true);
        const scopeSummary = scope => result.body.scopes.find(item => item.scope === scope);
        assert.ok(scopeSummary("summaries").count >= 3);
        assert.ok(scopeSummary("profiles").count >= 4);
        assert.ok(scopeSummary("conversations").count >= 3);
        assert.ok(scopeSummary("device_history").count >= 5);
        assert.ok(scopeSummary("jobs").count >= 2);
        assert.ok(scopeSummary("all_user_data").count >= scopeSummary("device_history").count);

        const activeConversationRowsBefore = await dbAll(dbPath, "SELECT COUNT(*) AS count FROM conversation_turns WHERE deleted_at IS NULL");
        const activeAsrRowsBefore = await dbAll(dbPath, "SELECT COUNT(*) AS count FROM asr_records WHERE deleted_at IS NULL");
        const activeLlmRowsBefore = await dbAll(dbPath, "SELECT COUNT(*) AS count FROM llm_records WHERE deleted_at IS NULL");
        result = await request(baseUrl, "POST", "/api/user-data/delete/preview", {
            scope: "conversations",
            mode: "soft_delete"
        }, USER_DATA_HEADERS);
        assert.equal(result.response.status, 200);
        assert.equal(result.body.ok, true);
        assert.ok(result.body.run_id);
        assert.equal(result.body.request_type, "preview");
        assert.equal(result.body.required_confirm, "DELETE");
        assert.equal(result.body.affected_counts.conversation_turns, activeConversationRowsBefore[0].count);
        assert.equal(result.body.affected_counts.asr_records, activeAsrRowsBefore[0].count);
        assert.equal(result.body.affected_counts.llm_records, activeLlmRowsBefore[0].count);
        const previewRunId = result.body.run_id;
        result = await request(baseUrl, "GET", `/api/user-data/deletion-runs?request_type=preview&limit=1`, null, USER_DATA_HEADERS);
        assert.equal(result.response.status, 200);
        assert.equal(result.body.runs[0].run_id, previewRunId);
        assert.equal(result.body.runs[0].request_type, "preview");
        assert.equal(result.body.runs[0].status, "completed");
        assert.ok(result.body.runs[0].created_at);
        let afterPreviewRows = await dbAll(dbPath, "SELECT COUNT(*) AS count FROM conversation_turns WHERE deleted_at IS NULL");
        assert.equal(afterPreviewRows[0].count, activeConversationRowsBefore[0].count);

        const rollbackDeviceId = "rollback-smoke-device";
        result = await request(baseUrl, "POST", "/api/devices/capabilities", {
            device_id: rollbackDeviceId,
            protocol_version: "agent-command-v1",
            capabilities: {
                commands: ["display.show_text"]
            }
        });
        assert.equal(result.response.status, 200);
        result = await request(baseUrl, "POST", "/api/commands", {
            name: "display.show_text",
            target_device_id: rollbackDeviceId,
            payload: {
                text: "rollback smoke"
            }
        });
        assert.equal(result.response.status, 201);
        const rollbackCommandId = result.body.command.command_id;
        result = await request(baseUrl, "POST", "/sensor", {
            temperature: 23.5,
            humidity: 45.2,
            pressure: 1008.1,
            gas_resistance: 321.4,
            device_id: rollbackDeviceId
        });
        assert.equal(result.response.status, 200);
        const rollbackSensorId = result.body.id;
        await dbRun(dbPath, `
            CREATE TRIGGER fail_command_queue_soft_delete
            BEFORE UPDATE OF deleted_at ON command_queue
            WHEN NEW.delete_reason='rollback_smoke'
            BEGIN
                SELECT RAISE(ABORT, 'forced rollback smoke');
            END
        `);
        result = await request(baseUrl, "POST", "/api/user-data/delete", {
            scope: "device_history",
            mode: "soft_delete",
            confirm: "DELETE",
            reason: "rollback_smoke",
            requested_by: "smoke"
        }, USER_DATA_HEADERS);
        assert.equal(result.response.status, 500);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.code, "USER_DATA_DELETE_FAILED");
        await dbRun(dbPath, "DROP TRIGGER fail_command_queue_soft_delete");
        rows = await dbAll(dbPath, "SELECT deleted_at FROM sensor_records WHERE id=? LIMIT 1", [rollbackSensorId]);
        assert.equal(rows.length, 1);
        assert.equal(rows[0].deleted_at, null);
        rows = await dbAll(dbPath, "SELECT deleted_at FROM command_queue WHERE command_id=? LIMIT 1", [rollbackCommandId]);
        assert.equal(rows.length, 1);
        assert.equal(rows[0].deleted_at, null);
        result = await request(baseUrl, "GET", "/api/user-data/deletion-runs?status=failed&limit=1", null, USER_DATA_HEADERS);
        assert.equal(result.response.status, 200);
        assert.equal(result.body.runs.length, 1);
        assert.equal(result.body.runs[0].status, "failed");
        assert.equal(result.body.runs[0].scope, "device_history");
        assert.ok(result.body.runs[0].preview_counts.sensor_records >= 1);

        result = await request(baseUrl, "POST", "/api/jobs/daily-summary/run", {
            date: smokeToday,
            force: true,
            dry_run: true
        });
        assert.equal(result.response.status, 202);
        assert.equal(result.body.status, "dry_run");
        assert.ok(result.body.stats.sensor_records.count > 0);
        assert.ok(result.body.stats.command_queue.count > 0);
        assert.ok(result.body.stats.conversation_turns.count > 0);

        result = await request(baseUrl, "POST", "/api/user-data/delete", {
            scope: "conversations",
            mode: "soft_delete",
            confirm: "DELETE",
            reason: "smoke_conversations",
            requested_by: "smoke"
        }, USER_DATA_HEADERS);
        assert.equal(result.response.status, 200);
        assert.equal(result.body.status, "completed");
        assert.ok(result.body.affected_counts.conversation_turns >= activeConversationRowsBefore[0].count);
        rows = await dbAll(dbPath, "SELECT memory_level, deleted_at FROM conversation_turns WHERE turn_id=? LIMIT 1", [archivedTurnId]);
        assert.equal(rows.length, 1);
        assert.equal(rows[0].memory_level, "archived");
        assert.ok(rows[0].deleted_at);
        result = await request(baseUrl, "GET", "/api/user-data/summary", null, USER_DATA_HEADERS);
        assert.equal(result.response.status, 200);
        assert.ok(result.body.scopes.find(item => item.scope === "conversations").last_deleted_at);
        result = await request(baseUrl, "GET", "/api/conversation/turns?limit=10");
        assert.equal(result.response.status, 200);
        assert.equal(result.body.turns.length, 0);
        result = await request(baseUrl, "GET", "/asr/latest");
        assert.equal(result.response.status, 200);
        assert.deepEqual(result.body, {});
        result = await request(baseUrl, "GET", "/llm/latest");
        assert.equal(result.response.status, 200);
        assert.deepEqual(result.body, {});

        result = await request(baseUrl, "POST", "/api/user-data/delete", {
            scope: "device_history",
            mode: "soft_delete",
            confirm: "DELETE",
            reason: "smoke_device_history",
            requested_by: "smoke"
        }, USER_DATA_HEADERS);
        assert.equal(result.response.status, 200);
        assert.equal(result.body.status, "completed");
        assert.ok(result.body.affected_counts.sensor_records > 0);
        assert.equal(result.body.affected_counts.device_capabilities, undefined);
        rows = await dbAll(dbPath, "SELECT status, deleted_at FROM command_queue WHERE command_id=? LIMIT 1", [rollbackCommandId]);
        assert.equal(rows.length, 1);
        assert.equal(rows[0].status, "queued");
        assert.ok(rows[0].deleted_at);
        rows = await dbAll(dbPath, "SELECT status, deleted_at FROM voice_turns WHERE request_id=? LIMIT 1", ["smoke-turn-empty-body"]);
        assert.equal(rows.length, 1);
        assert.equal(rows[0].status, "rejected");
        assert.ok(rows[0].deleted_at);

        result = await request(baseUrl, "POST", "/api/llm/text", {
            text: "删除后现在房间环境怎么样",
            device_id: bmeDeviceId
        });
        assert.equal(result.response.status, 200);
        const postDeletePromptBody = mockLlm.requests[mockLlm.requests.length - 1].body;
        assert.match(postDeletePromptBody, /当前没有可用的实时 BME690|当前没有可靠的 ESP 本地空气状态估算/);
        assert.doesNotMatch(postDeletePromptBody, /moderate|72\/100|29\.57/);

        result = await request(baseUrl, "POST", "/api/user-data/delete", {
            scope: "summaries",
            mode: "soft_delete",
            confirm: "DELETE",
            reason: "smoke_summaries",
            requested_by: "smoke"
        }, USER_DATA_HEADERS);
        assert.equal(result.response.status, 200);
        assert.equal(result.body.status, "completed");
        rows = await dbAll(dbPath, "SELECT status, deleted_at FROM daily_memory WHERE memory_date=? AND summary=? LIMIT 1", [archivedDailyMemoryDate, "archived summary must still be deleted"]);
        assert.equal(rows.length, 1);
        assert.equal(rows[0].status, "archived");
        assert.ok(rows[0].deleted_at);
        result = await request(baseUrl, "GET", "/api/memory/daily?limit=10");
        assert.equal(result.response.status, 200);
        assert.equal(result.body.memories.length, 0);

        result = await request(baseUrl, "POST", "/api/user-data/delete", {
            scope: "profiles",
            mode: "soft_delete",
            confirm: "DELETE",
            reason: "smoke_profiles",
            requested_by: "smoke"
        }, USER_DATA_HEADERS);
        assert.equal(result.response.status, 200);
        assert.equal(result.body.status, "completed");
        rows = await dbAll(dbPath, "SELECT status, deleted_at FROM long_term_profile WHERE profile_key=? LIMIT 1", [archivedProfileKey]);
        assert.equal(rows.length, 1);
        assert.equal(rows[0].status, "archived");
        assert.ok(rows[0].deleted_at);
        result = await request(baseUrl, "GET", "/api/memory/profile?status=archived&limit=10");
        assert.equal(result.response.status, 200);
        assert.equal(result.body.profiles.some(profile => profile.profile_key === archivedProfileKey), false);

        result = await request(baseUrl, "POST", "/api/jobs/daily-summary/run", {
            date: smokeToday,
            force: true,
            dry_run: true
        });
        assert.equal(result.response.status, 202);
        assert.equal(result.body.status, "dry_run");
        assert.equal(result.body.stats.sensor_records.count, 0);
        assert.equal(result.body.stats.command_queue.count, 0);
        assert.equal(result.body.stats.conversation_turns.count, 0);

        result = await request(baseUrl, "POST", "/api/jobs/weekly-profile/run", {
            week_end: smokeToday,
            force: true,
            dry_run: true
        });
        assert.equal(result.response.status, 202);
        assert.equal(result.body.status, "dry_run");
        assert.equal(result.body.stats.daily_memory.count, 0);
        assert.equal(result.body.stats.sensor_records.count, 0);
        assert.equal(result.body.stats.conversation_turns.count, 0);

        result = await request(baseUrl, "GET", "/api/user-data/deletion-runs?request_type=delete&limit=1", null, USER_DATA_HEADERS);
        assert.equal(result.response.status, 200);
        const hiddenAuditRunId = result.body.runs[0].run_id;
        result = await request(baseUrl, "POST", "/api/user-data/delete", {
            scope: "jobs",
            mode: "soft_delete",
            confirm: "DELETE",
            reason: "smoke_audit_cleanup",
            requested_by: "smoke",
            include_audit_logs: true
        }, USER_DATA_HEADERS);
        assert.equal(result.response.status, 200);
        assert.equal(result.body.status, "completed");
        const auditCleanupRunId = result.body.run_id;
        result = await request(baseUrl, "GET", "/api/user-data/deletion-runs?limit=50", null, USER_DATA_HEADERS);
        assert.equal(result.response.status, 200);
        assert.ok(result.body.runs.some(run => run.run_id === auditCleanupRunId));
        assert.equal(result.body.runs.some(run => run.run_id === hiddenAuditRunId), false);
        result = await request(baseUrl, "GET", "/api/user-data/deletion-runs?include_deleted=true&limit=50", null, USER_DATA_HEADERS);
        assert.equal(result.response.status, 200);
        assert.ok(result.body.runs.some(run => run.run_id === hiddenAuditRunId && run.deleted_at));

        const capabilitiesBeforeDeleteRows = await dbAll(dbPath, "SELECT COUNT(*) AS count FROM device_capabilities");
        assert.ok(capabilitiesBeforeDeleteRows[0].count > 0);
        result = await request(baseUrl, "POST", "/api/user-data/delete", {
            scope: "all_user_data",
            mode: "hard_delete",
            confirm: "DELETE",
            reason: "smoke_hard_delete",
            requested_by: "smoke"
        }, USER_DATA_HEADERS);
        assert.equal(result.response.status, 200);
        assert.equal(result.body.status, "completed");
        assert.ok(result.body.affected_counts.sensor_records > 0);
        assert.equal(result.body.affected_counts.device_capabilities, undefined);
        for (const tableName of ["sensor_records", "conversation_turns", "daily_memory", "command_queue", "voice_turns", "memory_job_runs"]) {
            rows = await dbAll(dbPath, `SELECT COUNT(*) AS count FROM ${tableName}`);
            assert.equal(rows[0].count, 0, `${tableName} should be hard deleted`);
        }
        const capabilitiesAfterDeleteRows = await dbAll(dbPath, "SELECT COUNT(*) AS count FROM device_capabilities");
        assert.equal(capabilitiesAfterDeleteRows[0].count, capabilitiesBeforeDeleteRows[0].count);
        rows = await dbAll(dbPath, "SELECT name FROM sqlite_master WHERE type='table' AND name IN ('sensor_records','device_capabilities','data_deletion_runs') ORDER BY name");
        assert.deepEqual(rows.map(row => row.name), ["data_deletion_runs", "device_capabilities", "sensor_records"]);
        result = await request(baseUrl, "GET", "/api/user-data/deletion-runs?limit=5", null, USER_DATA_HEADERS);
        assert.equal(result.response.status, 200);
        assert.ok(result.body.runs.length >= 1);
        assert.equal(result.body.runs[0].status, "completed");
        assert.equal(result.body.runs[0].scope, "all_user_data");
        assert.equal(result.body.runs[0].request_type, "delete");
        assert.ok(result.body.runs[0].affected_counts.sensor_records > 0);

        await dbRun(dbPath, "DROP TABLE long_term_profile");
        result = await request(baseUrl, "GET", "/api/memory/profile?limit=1");
        assert.equal(result.response.status, 500);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.error, "Internal server error");

        await dbRun(dbPath, "DROP TABLE sensor_records");
        result = await request(baseUrl, "GET", "/sensor/latest");
        assert.equal(result.response.status, 500);
        assert.equal(result.body.ok, false);
        assert.match(result.body.error, /sensor_records/);

        await dbRun(dbPath, "DROP TABLE command_queue");
        result = await request(baseUrl, "GET", "/api/commands/history");
        assert.equal(result.response.status, 500);
        assert.equal(result.body.ok, false);
        assert.equal(result.body.error, "Internal server error");

        const sqliteBytes = fs.statSync(dbPath).size;
        assert.ok(sqliteBytes > 0);
        console.log(`smoke regression passed port=${port} db=${dbPath} bytes=${sqliteBytes}`);
    } finally {
        const stopResult = await stopServer(child);
        assert.equal(stopResult.killed, false);
        assert.equal(stopResult.exited, true);
        await stopMockLlmServer(mockLlm);
        fs.rmSync(tempDir, {
            recursive: true,
            force: true
        });
    }
}

run().catch(error => {
    console.error(error);
    process.exit(1);
});
