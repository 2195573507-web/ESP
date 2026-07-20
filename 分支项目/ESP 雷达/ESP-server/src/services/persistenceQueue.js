const PRIORITY_HIGH = "high";
const PRIORITY_MEDIUM = "medium";
const PRIORITY_LOW = "low";
const PRIORITIES = [PRIORITY_HIGH, PRIORITY_MEDIUM, PRIORITY_LOW];
const CSI_PERSISTENCE_JOB_TYPE = "csi.motion";
const CSI_PERSISTENCE_QUEUE_MAX_LENGTH = 1000;

const queues = {
    [PRIORITY_HIGH]: [],
    [PRIORITY_MEDIUM]: [],
    [PRIORITY_LOW]: []
};

let nextJobId = 1;

function normalizePriority(priority) {
    return PRIORITIES.includes(priority) ? priority : PRIORITY_MEDIUM;
}

function totalSize() {
    return PRIORITIES.reduce((sum, priority) => sum + queues[priority].length, 0);
}

function isCsiPersistenceJob(job) {
    return job?.type === CSI_PERSISTENCE_JOB_TYPE;
}

function csiQueueSize() {
    return PRIORITIES.reduce((sum, priority) => (
        sum + queues[priority].filter(isCsiPersistenceJob).length
    ), 0);
}

function csiQueueResult(dropped = 0, coalesced = 0) {
    return {
        length: csiQueueSize(),
        dropped,
        coalesced
    };
}

function removeQueuedCsiJobs() {
    const removed = [];
    for (const priority of PRIORITIES) {
        const retained = [];
        for (const job of queues[priority]) {
            if (isCsiPersistenceJob(job)) {
                removed.push(job);
            } else {
                retained.push(job);
            }
        }
        queues[priority] = retained;
    }
    return removed;
}

function latestCsiJob(jobs = []) {
    return jobs.reduce((latest, job) => {
        if (!latest) {
            return job;
        }

        const latestQueuedAt = Number(latest.queued_at_ms) || 0;
        const queuedAt = Number(job.queued_at_ms) || 0;
        if (queuedAt > latestQueuedAt || (queuedAt === latestQueuedAt && job.id > latest.id)) {
            return job;
        }
        return latest;
    }, null);
}

function enqueuePersistenceJob(job = {}) {
    if (typeof job.run !== "function") {
        throw new Error("persistence job requires run()");
    }

    const priority = normalizePriority(job.priority);
    const queuedJob = {
        id: nextJobId++,
        queued_at_ms: Date.now(),
        attempts: 0,
        ...job,
        priority
    };

    if (isCsiPersistenceJob(queuedJob) && csiQueueSize() >= CSI_PERSISTENCE_QUEUE_MAX_LENGTH) {
        const dropped = removeQueuedCsiJobs().length;
        queues[priority].push(queuedJob);

        return {
            job_id: queuedJob.id,
            priority,
            size: totalSize(),
            csi: csiQueueResult(dropped, dropped)
        };
    }

    queues[priority].push(queuedJob);

    return {
        job_id: queuedJob.id,
        priority,
        size: totalSize(),
        csi: isCsiPersistenceJob(queuedJob) ? csiQueueResult() : null
    };
}

function dequeuePersistenceBatch(limit = 100) {
    const max = Math.max(1, Math.trunc(Number(limit) || 100));
    const batch = [];

    for (const priority of PRIORITIES) {
        while (queues[priority].length > 0 && batch.length < max) {
            batch.push(queues[priority].shift());
        }
        if (batch.length >= max) {
            break;
        }
    }

    return batch;
}

function requeuePersistenceBatch(batch = []) {
    const csiBatch = batch.filter(isCsiPersistenceJob);
    if (csiBatch.length > 0 && csiQueueSize() + csiBatch.length > CSI_PERSISTENCE_QUEUE_MAX_LENGTH) {
        const queuedCsi = removeQueuedCsiJobs();
        const latest = latestCsiJob([...queuedCsi, ...csiBatch]);

        for (let index = batch.length - 1; index >= 0; index--) {
            const job = batch[index];
            if (isCsiPersistenceJob(job)) {
                continue;
            }
            const priority = normalizePriority(job?.priority);
            queues[priority].unshift({
                ...job,
                priority
            });
        }

        if (latest) {
            const priority = normalizePriority(latest.priority);
            queues[priority].unshift({
                ...latest,
                priority
            });
        }

        const dropped = queuedCsi.length + csiBatch.length - (latest ? 1 : 0);
        return {
            csi: csiQueueResult(dropped, dropped)
        };
    }

    for (let index = batch.length - 1; index >= 0; index--) {
        const job = batch[index];
        const priority = normalizePriority(job?.priority);
        queues[priority].unshift({
            ...job,
            priority
        });
    }

    return {
        csi: csiBatch.length > 0 ? csiQueueResult() : null
    };
}

function getPersistenceQueueStats() {
    return {
        high: queues[PRIORITY_HIGH].length,
        medium: queues[PRIORITY_MEDIUM].length,
        low: queues[PRIORITY_LOW].length,
        csi: csiQueueSize(),
        total: totalSize()
    };
}

function clearPersistenceQueue() {
    for (const priority of PRIORITIES) {
        queues[priority].length = 0;
    }
}

module.exports = {
    CSI_PERSISTENCE_JOB_TYPE,
    CSI_PERSISTENCE_QUEUE_MAX_LENGTH,
    PRIORITY_HIGH,
    PRIORITY_LOW,
    PRIORITY_MEDIUM,
    clearPersistenceQueue,
    dequeuePersistenceBatch,
    enqueuePersistenceJob,
    getPersistenceQueueStats,
    requeuePersistenceBatch
};
