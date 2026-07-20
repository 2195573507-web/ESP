const assert = require("assert");
const express = require("express");
const sqlite3 = require("sqlite3").verbose();

const { ensureHomeAiTables } = require("../src/db/homeAi");
const {
    publishRulePackage,
    readCurrentRulePackage
} = require("../src/services/homeAiService");
const { createHomeAiRouter } = require("../src/routes/homeAiRoutes");
const {
    createMemoryCandidate,
    createRuleCandidate,
    listHabits,
    startProbationRuns,
    updateMemoryCandidate
} = require("../src/homeAi/learningService");
const {
    createHomeAiProbationScheduler,
    runHomeAiProbationSweep
} = require("../src/jobs/homeAiProbationJobs");
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

function closeDatabase(db) {
    return new Promise((resolve, reject) => db.close(error => error ? reject(error) : resolve()));
}

function rulePackage(version) {
    return {
        schema_version: 1,
        version,
        generated_at_ms: Date.now(),
        rooms: defaultRooms(),
        rules: [{
            rule_id: "probation_light",
            version: 1,
            rule_type: "basic_automation",
            source: "manual",
            room_id: "bedroom_01",
            enabled: true,
            priority: 500,
            conditions: [{ field: "presence_state", operator: "eq", value: "occupied" }],
            actions: [{ device_id: "bedroom_01_light", device_type: "light", action: "turn_on" }],
            cooldown_seconds: 1,
            offline_policy: "continue"
        }]
    };
}

async function createFixture() {
    const db = new sqlite3.Database(":memory:");
    const { dbRun, dbAll } = helpers(db);
    await ensureHomeAiTables(dbRun, dbAll);
    assert.equal((await publishRulePackage(dbRun, dbAll, rulePackage(1))).ok, true);
    assert.equal((await publishRulePackage(dbRun, dbAll, rulePackage(2))).ok, true);
    return { db, dbRun, dbAll };
}

async function testAutonomousExpiryAndRollbackBinding() {
    const { db, dbRun, dbAll } = await createFixture();
    try {
        const current = await readCurrentRulePackage(dbAll);
        const runs = await startProbationRuns(dbRun, current, { duration_days: 3 });
        assert.equal(runs.length, 1);
        const nowMs = Date.now();
        await dbRun(
            `UPDATE home_ai_rule_probation_runs
             SET ends_at_ms=?,trigger_count=3,failure_count=1,metrics_json=? WHERE run_id=?`,
            [nowMs - 1, JSON.stringify({ trigger_count: 3, failure_count: 1 }), runs[0].run_id]
        );
        const sweep = await runHomeAiProbationSweep({ dbRun, dbAll, now_ms: nowMs });
        assert.equal(sweep.ok, true);
        assert.equal(sweep.evaluated, 1);
        assert.equal(sweep.updated[0].status, "ROLLED_BACK");
        assert.equal(sweep.updated[0].rollback.control.restored_from_version, 1);
        assert.equal((await readCurrentRulePackage(dbAll)).version, 3);

        const secondCurrent = await readCurrentRulePackage(dbAll);
        const secondRuns = await startProbationRuns(dbRun, secondCurrent, { duration_days: 3 });
        await dbRun(
            `UPDATE home_ai_rule_probation_runs
             SET ends_at_ms=?,trigger_count=3,failure_count=1,metrics_json=? WHERE run_id=?`,
            [nowMs - 1, JSON.stringify({ trigger_count: 3, failure_count: 1 }), secondRuns[0].run_id]
        );
        assert.equal((await publishRulePackage(dbRun, dbAll, rulePackage(4))).ok, true);
        const guarded = await runHomeAiProbationSweep({ dbRun, dbAll, now_ms: nowMs + 1 });
        assert.equal(guarded.evaluated, 1);
        assert.equal(guarded.updated[0].status, "FAILED");
        assert.equal(guarded.updated[0].rollback.code, "RULE_ROLLBACK_ACTIVE_VERSION_CHANGED");
        assert.equal((await readCurrentRulePackage(dbAll)).version, 4);
    } finally {
        await closeDatabase(db);
    }
}

async function testPublicEvaluateIgnoresClientMetricsAndForce() {
    const { db, dbRun, dbAll } = await createFixture();
    const app = express();
    app.use(express.json());
    app.use(createHomeAiRouter({ dbRun, dbAll, logger: { error() {} } }));
    const server = await new Promise(resolve => {
        const listener = app.listen(0, "127.0.0.1", () => resolve(listener));
    });
    try {
        const current = await readCurrentRulePackage(dbAll);
        const runs = await startProbationRuns(dbRun, current, { duration_days: 3 });
        await dbRun(
            `UPDATE home_ai_rule_probation_runs SET ends_at_ms=? WHERE run_id=?`,
            [Date.now() - 1, runs[0].run_id]
        );
        const response = await fetch(
            `http://127.0.0.1:${server.address().port}/api/home-ai/v1/probation/${runs[0].run_id}/evaluate`,
            {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({
                    force: true,
                    metrics: { trigger_count: 3, failure_count: 3, override_count: 3 }
                })
            }
        );
        assert.equal(response.status, 200);
        const body = await response.json();
        assert.equal(body.data.status, "EXPIRED");
        const row = (await dbAll("SELECT * FROM home_ai_rule_probation_runs WHERE run_id=?", [runs[0].run_id]))[0];
        assert.equal(row.trigger_count, 0);
        assert.equal(row.failure_count, 0);

        const candidate = await createRuleCandidate(dbRun, {
            candidate_id: "terminal_candidate",
            rule_package: rulePackage(8),
            confidence: 1,
            sample_count: 100,
            source: { generated_from_habit: false }
        });
        assert.equal(candidate.ok, true);
        await dbRun("UPDATE home_ai_rule_candidates SET status='SUSPENDED' WHERE candidate_id=?", [candidate.candidate.candidate_id]);
        const candidateResponse = await fetch(
            `http://127.0.0.1:${server.address().port}/api/home-ai/v1/rule-candidates/${candidate.candidate.candidate_id}/evaluate`,
            {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ force: true, auto_publish: true, gates: { schema: true } })
            }
        );
        assert.equal(candidateResponse.status, 200);
        const candidateBody = await candidateResponse.json();
        assert.equal(candidateBody.data.status, "SUSPENDED");
        assert.equal(candidateBody.data.published, null);
    } finally {
        await new Promise(resolve => server.close(resolve));
        await closeDatabase(db);
    }
}

async function testMemoryDefaultAndSilenceLearningBoundary() {
    const { db, dbRun, dbAll } = await createFixture();
    try {
        const memory = await createMemoryCandidate(dbRun, {
            room_id: "bedroom_01",
            category: "preference",
            content: "夜间保持安静",
            confidence: 0.9
        });
        const confirmed = await updateMemoryCandidate(
            dbRun,
            dbAll,
            memory.candidate.candidate_id,
            { status: "CONFIRMED" }
        );
        assert.equal(confirmed.candidate.automation_allowed, false);
        const stored = (await dbAll(
            "SELECT automation_allowed FROM home_ai_confirmed_memories WHERE candidate_id=?",
            [memory.candidate.candidate_id]
        ))[0];
        assert.equal(stored.automation_allowed, 0);

        await dbRun(
            `INSERT INTO home_ai_events
             (event_id,gateway_id,room_id,event_type,priority,occurred_at_ms,received_at_ms,payload_json)
             VALUES(?,?,?,?,?,?,?,?)`,
            ["silent_decision", "gateway_01", "bedroom_01", "decision", 500, Date.now(), Date.now(), "{}"]
        );
        assert.equal((await listHabits(dbAll, {})).length, 0);
    } finally {
        await closeDatabase(db);
    }
}

async function testSchedulerSingleFlightAndBoundedSweep() {
    let calls = 0;
    let release;
    const pending = new Promise(resolve => { release = resolve; });
    const scheduler = createHomeAiProbationScheduler({
        interval_ms: 60000,
        evaluateImpl: async () => {
            calls += 1;
            await pending;
            return { ok: true, evaluated: 0, updated: [], errors: [] };
        }
    });
    const first = scheduler.tick();
    const second = scheduler.tick();
    assert.equal(calls, 1);
    release();
    await Promise.all([first, second]);
    scheduler.start();
    assert.equal(scheduler.isRunning(), true);
    await scheduler.stop();
    assert.equal(scheduler.isRunning(), false);
}

(async () => {
    await testAutonomousExpiryAndRollbackBinding();
    await testPublicEvaluateIgnoresClientMetricsAndForce();
    await testMemoryDefaultAndSilenceLearningBoundary();
    await testSchedulerSingleFlightAndBoundedSweep();
    process.stdout.write("home ai probation jobs tests: PASS\n");
})().catch(error => {
    console.error(error);
    process.exitCode = 1;
});
