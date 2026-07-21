const {
    createVoiceStageError
} = require("./errors");

function positiveLimit(value, fallback) {
    return Number.isSafeInteger(value) && value > 0 ? value : fallback;
}

class BoundedPcmQueue {
    constructor(options = {}) {
        this.maxQueuedBytes = positiveLimit(options.maxQueuedBytes, 128 * 1024);
        this.maxChunkBytes = positiveLimit(options.maxChunkBytes, 64 * 1024);
        this.items = [];
        this.waiters = [];
        this.queuedBytes = 0;
        this.peakQueuedBytes = 0;
        this.terminal = null;
        this.error = null;
    }

    push(chunk) {
        if (this.terminal) {
            throw this.error || createVoiceStageError("tts", "VOICE_TTS_FAILED", "TTS PCM queue is already complete", 502);
        }
        if (!Buffer.isBuffer(chunk) || chunk.length === 0 || chunk.length % 2 !== 0) {
            const error = createVoiceStageError("tts", "VOICE_TTS_FAILED", "TTS PCM queue accepts non-empty PCM16 chunks only", 502);
            this.fail(error);
            throw error;
        }
        if (chunk.length > this.maxChunkBytes || this.queuedBytes + chunk.length > this.maxQueuedBytes) {
            const error = createVoiceStageError("tts", "VOICE_TTS_QUEUE_OVERFLOW", "TTS PCM queue limit exceeded", 502, {
                bytes: chunk.length
            });
            this.fail(error);
            throw error;
        }

        const waiter = this.waiters.shift();
        if (waiter) {
            waiter.resolve({ value: chunk, done: false });
            return;
        }

        this.items.push(chunk);
        this.queuedBytes += chunk.length;
        this.peakQueuedBytes = Math.max(this.peakQueuedBytes, this.queuedBytes);
    }

    finish() {
        if (this.terminal) {
            return false;
        }

        this.terminal = "finished";
        for (const waiter of this.waiters.splice(0)) {
            waiter.resolve({ value: undefined, done: true });
        }
        return true;
    }

    fail(error) {
        if (this.terminal) {
            return false;
        }

        this.terminal = "failed";
        this.error = error?.code
            ? error
            : createVoiceStageError("tts", "VOICE_TTS_FAILED", error?.message || "TTS PCM queue failed", 502, {
                cause: error
            });
        this.items = [];
        this.queuedBytes = 0;
        for (const waiter of this.waiters.splice(0)) {
            waiter.reject(this.error);
        }
        return true;
    }

    abort(reason = "TTS PCM queue aborted") {
        return this.fail(createVoiceStageError("tts", "VOICE_TTS_ABORTED", reason, 502));
    }

    next() {
        if (this.items.length > 0) {
            const value = this.items.shift();
            this.queuedBytes -= value.length;
            return Promise.resolve({ value, done: false });
        }
        if (this.terminal === "finished") {
            return Promise.resolve({ value: undefined, done: true });
        }
        if (this.terminal === "failed") {
            return Promise.reject(this.error);
        }

        return new Promise((resolve, reject) => {
            this.waiters.push({ resolve, reject });
        });
    }

    [Symbol.asyncIterator]() {
        return this;
    }

    stats() {
        return {
            queuedBytes: this.queuedBytes,
            peakQueuedBytes: this.peakQueuedBytes,
            maxQueuedBytes: this.maxQueuedBytes,
            maxChunkBytes: this.maxChunkBytes
        };
    }
}

module.exports = {
    BoundedPcmQueue
};
