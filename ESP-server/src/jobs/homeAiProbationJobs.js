const { evaluateDueProbationRuns } = require("../homeAi/learningService");
const { rollbackRulePackage } = require("../services/homeAiService");

const DEFAULT_PROBATION_INTERVAL_MS = 5 * 60 * 1000;
const PROBATION_BATCH_LIMIT = 100;

async function runHomeAiProbationSweep(options = {}) {
    const { dbRun, dbAll, logger = console } = options;
    const evaluateImpl = options.evaluateImpl || evaluateDueProbationRuns;
    try {
        return await evaluateImpl(dbRun, dbAll, {
            now_ms: options.now_ms,
            limit: PROBATION_BATCH_LIMIT,
            rollbackFn: options.rollbackImpl || ((body) => rollbackRulePackage(dbRun, dbAll, body))
        });
    } catch (error) {
        if (typeof logger.error === "function") {
            logger.error(`[home-ai] probation sweep failed ${error?.message || error}`);
        }
        return {
            ok: false,
            evaluated: 0,
            updated: [],
            errors: [{ code: "PROBATION_SWEEP_FAILED", message: String(error?.message || error) }]
        };
    }
}

function createHomeAiProbationScheduler(options = {}) {
    const intervalMs = Math.max(60 * 1000, Number(options.interval_ms) || DEFAULT_PROBATION_INTERVAL_MS);
    let timer = null;
    let running = false;
    let inFlight = null;

    async function tick() {
        if (inFlight) return inFlight;
        inFlight = runHomeAiProbationSweep(options).finally(() => {
            inFlight = null;
        });
        return inFlight;
    }

    return {
        start() {
            if (timer) return;
            running = true;
            timer = setInterval(() => { void tick(); }, intervalMs);
            if (typeof timer.unref === "function") timer.unref();
            if (options.run_immediately) void tick();
        },
        async stop() {
            if (timer) clearInterval(timer);
            timer = null;
            running = false;
            if (inFlight) await inFlight;
        },
        tick,
        isRunning() { return running; }
    };
}

module.exports = {
    DEFAULT_PROBATION_INTERVAL_MS,
    PROBATION_BATCH_LIMIT,
    createHomeAiProbationScheduler,
    runHomeAiProbationSweep
};
