const {
    dequeuePersistenceBatch,
    getPersistenceQueueStats,
    requeuePersistenceBatch
} = require("./persistenceQueue");

const DEFAULT_FLUSH_INTERVAL_MS = 500;
const DEFAULT_BATCH_SIZE = 100;
const CSI_PERSISTENCE_JOB_TYPE = "csi.motion";

function csiBatchSize(batch) {
    return batch.filter(job => job?.type === CSI_PERSISTENCE_JOB_TYPE).length;
}

function elapsedMs(startNs) {
    return Math.round(Number(process.hrtime.bigint() - startNs) / 1e6);
}

function logCsiDbWrite(logger, batchSize, startNs, failed) {
    if (batchSize === 0) {
        return;
    }
    logger.info(`[CSI_DB_WRITE] batch_size=${batchSize} duration_ms=${elapsedMs(startNs)} failed=${failed}`);
}

function logCsiQueue(logger, csi) {
    if (!csi || (csi.dropped === 0 && csi.coalesced === 0)) {
        return;
    }
    logger.warn(`[CSI_PERSIST_QUEUE] length=${csi.length} dropped=${csi.dropped} coalesced=${csi.coalesced}`);
}

function describeBatch(batch) {
    return batch.map(job => ({
        id: job?.id,
        type: job?.type || "unknown",
        attempts: Math.max(0, Number(job?.attempts) || 0),
        snapshot_id: job?.snapshot_id || undefined
    }));
}

function createPersistenceWorker(options = {}) {
    const dbRun = options.dbRun;
    const logger = options.logger || console;
    const flushIntervalMs = Math.max(50, Math.trunc(Number(options.flushIntervalMs) || DEFAULT_FLUSH_INTERVAL_MS));
    const batchSize = Math.max(1, Math.trunc(Number(options.batchSize) || DEFAULT_BATCH_SIZE));

    let timer = null;
    let flushing = false;
    let stopped = false;
    let immediateScheduled = false;
    let lastFailure = null;

    async function runBatch(batch) {
        if (batch.length === 0) {
            return {
                persisted: 0
            };
        }

        const csiJobs = csiBatchSize(batch);
        const startNs = process.hrtime.bigint();

        try {
            if (typeof dbRun === "function") {
                await dbRun("BEGIN IMMEDIATE TRANSACTION");
            }

            for (const job of batch) {
                job.attempts = Math.max(0, Number(job.attempts) || 0) + 1;
                await job.run();
            }

            if (typeof dbRun === "function") {
                await dbRun("COMMIT");
            }

            logger.info(`[persistence_worker] persisted=${batch.length} queue=${JSON.stringify(getPersistenceQueueStats())}`);
            logCsiDbWrite(logger, csiJobs, startNs, false);
            lastFailure = null;
            return {
                persisted: batch.length
            };
        } catch (error) {
            if (typeof dbRun === "function") {
                try {
                    await dbRun("ROLLBACK");
                } catch (rollbackError) {
                    logger.error(`[persistence_worker] rollback failed ${rollbackError?.message || rollbackError}`);
                }
            }

            const requeued = requeuePersistenceBatch(batch);
            logCsiQueue(logger, requeued.csi);
            logCsiDbWrite(logger, csiJobs, startNs, true);
            lastFailure = {
                at_ms: Date.now(),
                error: error?.message || String(error),
                jobs: describeBatch(batch)
            };
            logger.error(`[PERSISTENCE_JOB_FAILED] requeued=${batch.length} jobs=${JSON.stringify(lastFailure.jobs)} error=${lastFailure.error}`);
            return {
                persisted: 0,
                error
            };
        }
    }

    async function flushOnce() {
        if (flushing) {
            return {
                skipped: true
            };
        }

        flushing = true;
        try {
            const batch = dequeuePersistenceBatch(batchSize);
            return await runBatch(batch);
        } finally {
            flushing = false;
        }
    }

    function scheduleImmediateFlushIfNeeded() {
        if (stopped || immediateScheduled || flushing) {
            return;
        }

        const stats = getPersistenceQueueStats();
        if (stats.total < batchSize) {
            return;
        }

        immediateScheduled = true;
        setImmediate(() => {
            immediateScheduled = false;
            flushOnce().catch(error => {
                logger.error(`[persistence_worker] immediate flush failed ${error?.message || error}`);
            });
        });
    }

    function start() {
        if (timer) {
            return;
        }

        stopped = false;
        timer = setInterval(() => {
            flushOnce().catch(error => {
                logger.error(`[persistence_worker] interval flush failed ${error?.message || error}`);
            });
        }, flushIntervalMs);
        if (typeof timer.unref === "function") {
            timer.unref();
        }
        logger.info(`[persistence_worker] started interval_ms=${flushIntervalMs} batch_size=${batchSize}`);
    }

    async function stop({ drain = true } = {}) {
        stopped = true;
        if (timer) {
            clearInterval(timer);
            timer = null;
        }

        if (!drain) {
            return;
        }

        while (getPersistenceQueueStats().total > 0) {
            await flushOnce();
        }
        logger.info("[persistence_worker] stopped after drain");
    }

    async function waitForIdle(options = {}) {
        const maxBatches = Math.max(1, Math.trunc(Number(options.maxBatches) || 100));
        let batches = 0;
        while (getPersistenceQueueStats().total > 0) {
            if (batches >= maxBatches) {
                throw new Error(`[persistence_worker] waitForIdle exceeded max_batches=${maxBatches} queue=${JSON.stringify(getPersistenceQueueStats())}`);
            }
            const result = await flushOnce();
            batches += 1;
            if (result?.error) {
                throw new Error(`[persistence_worker] persistence failed while waiting for idle: ${result.error?.message || result.error}`);
            }
        }
        return {
            batches,
            queue: getPersistenceQueueStats()
        };
    }

    return {
        flushOnce,
        getLastFailure: () => lastFailure,
        scheduleImmediateFlushIfNeeded,
        start,
        stop,
        waitForIdle
    };
}

module.exports = {
    DEFAULT_BATCH_SIZE,
    DEFAULT_FLUSH_INTERVAL_MS,
    createPersistenceWorker
};
