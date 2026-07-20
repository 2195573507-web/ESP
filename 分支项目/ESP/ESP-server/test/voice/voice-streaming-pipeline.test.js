const assert = require("assert");
const { EventEmitter } = require("events");
const fs = require("fs");
const path = require("path");

const {
    sendVoiceTurnPcmStream
} = require("../../src/voice/http");
const {
    BoundedPcmQueue
} = require("../../src/voice/pcmQueue");
const {
    MinimalWebSocket
} = require("../../src/voice/realtimeSocket");
const {
    parseTtsRealtimeEvent
} = require("../../src/voice/realtimeEvents");
const {
    createVoiceTtsPcmStream
} = require("../../src/voice/ttsStream");
const {
    requestVoiceTts
} = require("../../src/voice/chain");
const {
    decodeBase64Buffer
} = require("../../src/voice/payloadUtils");
const {
    normalizeTtsPcmBuffer
} = require("../../src/voice/ttsAudio");
const {
    readVoiceTurnConfig
} = require("../../src/voice/turnConfig");

function readFixture() {
    const fixturePath = path.join(__dirname, "fixtures", "volc-tts-realtime-sanitized.json");
    return JSON.parse(fs.readFileSync(fixturePath, "utf8"));
}

async function collect(stream) {
    const chunks = [];
    for await (const chunk of stream) {
        chunks.push(chunk);
    }
    return Buffer.concat(chunks);
}

async function *fixtureEvents(fixture, pauseBeforeCompletion) {
    let accumulatedAudioBytes = 0;
    let pauseApplied = false;
    for (const entry of fixture.events) {
        const event = parseTtsRealtimeEvent(JSON.stringify(entry.payload));
        yield event;
        if (event.isAudioDelta && !pauseApplied) {
            const decoded = decodeBase64Buffer(event.delta);
            accumulatedAudioBytes += decoded?.length || 0;
            if (accumulatedAudioBytes >= 12) {
                pauseApplied = true;
                await pauseBeforeCompletion?.();
            }
        }
    }
}

function consumeBufferedFixture(fixture) {
    const chunks = [];
    let completed = false;
    for (const entry of fixture.events) {
        const event = parseTtsRealtimeEvent(JSON.stringify(entry.payload));
        if (event.isError) {
            throw new Error(event.errorMessage || "fixture returned TTS error");
        }
        if (event.isAudioDelta) {
            const decoded = decodeBase64Buffer(event.delta);
            assert.ok(decoded, "fixture audio delta must decode for buffered consumption");
            chunks.push(decoded);
        }
        if (event.isAudioDone) {
            assert.equal(completed, false, "fixture must have one completion event");
            completed = true;
        }
    }
    assert.equal(completed, true, "fixture must complete for buffered consumption");
    return normalizeTtsPcmBuffer(Buffer.concat(chunks));
}

class FakeResponse extends EventEmitter {
    constructor({ backpressureOnce = false } = {}) {
        super();
        this.headers = {};
        this.statusCode = null;
        this.chunks = [];
        this.writableEnded = false;
        this.destroyed = false;
        this.backpressureOnce = backpressureOnce;
    }

    status(value) {
        this.statusCode = value;
        return this;
    }

    set(values, value) {
        if (typeof values === "string") {
            this.headers[values] = value;
        } else {
            Object.assign(this.headers, values);
        }
        return this;
    }

    write(chunk) {
        this.chunks.push(Buffer.from(chunk));
        if (this.backpressureOnce) {
            this.backpressureOnce = false;
            queueMicrotask(() => this.emit("drain"));
            return false;
        }
        return true;
    }

    end(chunk) {
        if (chunk) {
            this.chunks.push(Buffer.from(chunk));
        }
        this.writableEnded = true;
    }
}

class FakeSocket extends EventEmitter {
    constructor() {
        super();
        this.destroyed = false;
    }

    write() {}

    end() {}

    destroy() {
        this.destroyed = true;
    }
}

class FakeRealtimeWebSocket {
    constructor(messages) {
        this.messages = [...messages];
        this.sent = [];
        this.closeCalls = 0;
        this.abortCalls = 0;
    }

    sendText(message) {
        this.sent.push(JSON.parse(message));
    }

    async nextMessage() {
        if (this.messages.length === 0) {
            const error = new Error("fixture WebSocket closed before completion");
            error.code = "VOICE_TTS_INCOMPLETE";
            throw error;
        }
        return this.messages.shift();
    }

    sendClose() {
        this.closeCalls += 1;
    }

    abort() {
        this.abortCalls += 1;
    }
}

async function assertFixtureByteEquivalenceAndFirstChunkTiming() {
    const fixture = readFixture();
    const bufferedPcm = consumeBufferedFixture(fixture);
    let releaseCompletion;
    const completionGate = new Promise(resolve => {
        releaseCompletion = resolve;
    });
    const streamResult = createVoiceTtsPcmStream({
        events: fixtureEvents(fixture, () => completionGate),
        limits: {
            maxQueuedBytes: 64,
            maxChunkBytes: 16,
            maxTotalBytes: 64
        }
    });
    const iterator = streamResult.stream[Symbol.asyncIterator]();
    const first = await iterator.next();
    assert.equal(first.done, false);
    assert.equal(first.value.length % 2, 0);
    assert.ok(first.value.length > 0);

    let completed = false;
    streamResult.completion.then(() => {
        completed = true;
    });
    await Promise.resolve();
    assert.equal(completed, false, "first PCM must be observable before provider completion");

    releaseCompletion();
    const rest = [];
    for await (const chunk of { [Symbol.asyncIterator]: () => iterator }) {
        rest.push(chunk);
    }
    const completion = await streamResult.completion;
    const pcm = Buffer.concat([first.value, ...rest]);
    assert.equal(completion.status, "completed");
    assert.equal(completion.bytesGenerated, pcm.length);
    assert.deepEqual(pcm, bufferedPcm);
    assert.deepEqual(pcm, Buffer.from(fixture.expected_pcm_base64, "base64"));
}

async function assertBufferedWebSocketFallback() {
    const fixture = readFixture();
    const socket = new FakeRealtimeWebSocket([
        JSON.stringify({ type: "tts_session.updated" }),
        ...fixture.events.slice(1).map(entry => JSON.stringify(entry.payload))
    ]);
    const config = {
        apiKey: "offline-test-key",
        tts: {
            url: "wss://fixture.invalid/realtime",
            model: "offline-model",
            voice: "offline-voice",
            sampleRate: 16000,
            speed: 1,
            pitch: 1,
            volume: 1,
            useResourceId: false,
            resourceId: ""
        }
    };
    const result = await requestVoiceTts("offline text", config, "offline-device", undefined, {
        openWebSocket: async () => socket
    });

    assert.deepEqual(result.pcm, consumeBufferedFixture(fixture));
    assert.deepEqual(socket.sent.map(message => message.type), [
        "tts_session.update",
        "input_text.append",
        "input_text.done"
    ]);
    assert.equal(socket.closeCalls, 1);
    assert.equal(socket.abortCalls, 0);
}

async function assertAlignerAndTerminalCases() {
    async function *splitSamples() {
        yield { isAudioDelta: true, delta: Buffer.from([0x01]).toString("base64") };
        yield { isAudioDelta: true, delta: Buffer.from([0x00, 0x02, 0x00]).toString("base64") };
        yield { isAudioDone: true };
    }
    const splitStream = createVoiceTtsPcmStream({ events: splitSamples() });
    assert.deepEqual(await collect(splitStream.stream), Buffer.from([0x01, 0x00, 0x02, 0x00]));
    assert.equal((await splitStream.completion).status, "completed");

    async function *oddTail() {
        yield { isAudioDelta: true, delta: Buffer.from([0x01]).toString("base64") };
        yield { isAudioDone: true };
    }
    const oddStream = createVoiceTtsPcmStream({ events: oddTail() });
    await assert.rejects(() => collect(oddStream.stream), error => error?.code === "VOICE_TTS_ODD_LENGTH");
    assert.equal((await oddStream.completion).failureReason, "VOICE_TTS_ODD_LENGTH");

    async function *emptyAudio() {
        yield { isAudioDone: true };
    }
    const emptyStream = createVoiceTtsPcmStream({ events: emptyAudio() });
    await assert.rejects(() => collect(emptyStream.stream), error => error?.code === "VOICE_TTS_EMPTY_AUDIO");

    assert.equal(parseTtsRealtimeEvent(JSON.stringify({ type: "tts_session.completed" })).isAudioDone, false);
    assert.equal(parseTtsRealtimeEvent(JSON.stringify({ type: "response.completed" })).isAudioDone, true);
    assert.equal(parseTtsRealtimeEvent(JSON.stringify({ type: "response.audio.completed" })).isAudioDone, true);
    assert.equal(parseTtsRealtimeEvent(JSON.stringify({ type: "audio.completed" })).isAudioDone, true);

    async function *wavContainer() {
        yield { isAudioDelta: true, delta: Buffer.from("RIFF0000WAVE", "ascii").toString("base64") };
        yield { isAudioDone: true };
    }
    const wavStream = createVoiceTtsPcmStream({ events: wavContainer() });
    await assert.rejects(() => collect(wavStream.stream), error => error?.code === "VOICE_TTS_UNSUPPORTED_FORMAT");
    assert.equal((await wavStream.completion).failureReason, "VOICE_TTS_UNSUPPORTED_FORMAT");
}

async function assertQueueAbortAndBounds() {
    const queue = new BoundedPcmQueue({ maxQueuedBytes: 4, maxChunkBytes: 4 });
    queue.push(Buffer.from([0x01, 0x00]));
    queue.push(Buffer.from([0x02, 0x00]));
    assert.throws(
        () => queue.push(Buffer.from([0x03, 0x00])),
        error => error?.code === "VOICE_TTS_QUEUE_OVERFLOW"
    );
    await assert.rejects(() => queue.next(), error => error?.code === "VOICE_TTS_QUEUE_OVERFLOW");

    let releaseSource;
    const sourceGate = new Promise(resolve => {
        releaseSource = resolve;
    });
    let abortCalls = 0;
    async function *pendingSource() {
        await sourceGate;
        throw Object.assign(new Error("aborted"), { name: "AbortError" });
    }
    const pending = createVoiceTtsPcmStream({
        events: pendingSource(),
        abortUpstream: () => {
            abortCalls += 1;
            releaseSource();
        }
    });
    const pendingRead = pending.stream.next();
    assert.equal(pending.abort("test abort"), true);
    assert.equal(pending.abort("repeat abort"), false);
    await assert.rejects(() => pendingRead, error => error?.code === "VOICE_TTS_ABORTED");
    assert.equal((await pending.completion).status, "aborted");
    assert.equal(abortCalls, 1);
}

async function assertWritableBackpressureAndWebSocketBounds() {
    async function *pcm() {
        yield Buffer.from([0x01, 0x00]);
        yield Buffer.from([0x02, 0x00]);
    }
    const response = new FakeResponse({ backpressureOnce: true });
    const writeResult = await sendVoiceTurnPcmStream(response, pcm());
    assert.equal(writeResult.bytesWritten, 4);
    assert.equal(writeResult.chunksWritten, 2);
    assert.equal(response.statusCode, 200);
    assert.equal(response.headers["Content-Length"], undefined);
    assert.deepEqual(Buffer.concat(response.chunks), Buffer.from([0x01, 0x00, 0x02, 0x00]));

    const socket = new FakeSocket();
    const ws = new MinimalWebSocket(socket, "tts", {
        maxMessageBytes: 8,
        maxBufferedBytes: 8,
        maxBufferedMessages: 1
    });
    ws.pushMessage("one");
    ws.pushMessage("two");
    assert.equal(socket.destroyed, true);
    await assert.rejects(() => ws.nextMessage(), error => error?.code === "VOICE_TTS_BACKLOG_OVERFLOW");

    const abortSocket = new FakeSocket();
    const abortWs = new MinimalWebSocket(abortSocket, "tts");
    assert.equal(abortWs.abort(new Error("test abort")), true);
    assert.equal(abortWs.abort(new Error("repeat abort")), false);

    async function *partialPcm() {
        yield Buffer.from([0x01, 0x00]);
        const error = new Error("provider failed after first PCM");
        error.code = "VOICE_TTS_FAILED";
        throw error;
    }
    const partialResponse = new FakeResponse();
    await assert.rejects(
        () => sendVoiceTurnPcmStream(partialResponse, partialPcm()),
        error => error?.code === "VOICE_TTS_FAILED" &&
            error.voiceStreamWrite?.started === true &&
            error.voiceStreamWrite?.bytesWritten === 2 &&
            error.voiceStreamWrite?.chunksWritten === 1
    );
    assert.deepEqual(Buffer.concat(partialResponse.chunks), Buffer.from([0x01, 0x00]));
}

function assertStreamingFlagDefaultsOff() {
    const original = process.env.VOICE_TTS_STREAMING_ENABLED;
    try {
        delete process.env.VOICE_TTS_STREAMING_ENABLED;
        assert.equal(readVoiceTurnConfig().streamingEnabled, false);
        process.env.VOICE_TTS_STREAMING_ENABLED = "true";
        assert.equal(readVoiceTurnConfig().streamingEnabled, true);
        process.env.VOICE_TTS_STREAMING_ENABLED = "0";
        assert.equal(readVoiceTurnConfig().streamingEnabled, false);
    } finally {
        if (original === undefined) {
            delete process.env.VOICE_TTS_STREAMING_ENABLED;
        } else {
            process.env.VOICE_TTS_STREAMING_ENABLED = original;
        }
    }
}

async function run() {
    await assertFixtureByteEquivalenceAndFirstChunkTiming();
    await assertBufferedWebSocketFallback();
    await assertAlignerAndTerminalCases();
    await assertQueueAbortAndBounds();
    await assertWritableBackpressureAndWebSocketBounds();
    assertStreamingFlagDefaultsOff();
    process.stdout.write("voice streaming pipeline offline tests: PASS\n");
}

run().catch(error => {
    process.stderr.write(`${error.stack || error}\n`);
    process.exitCode = 1;
});
