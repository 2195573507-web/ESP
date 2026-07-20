const assert = require("assert");
const fs = require("fs");
const path = require("path");

const {
    decodeBase64Buffer
} = require("../../src/voice/payloadUtils");
const {
    parseTtsRealtimeEvent
} = require("../../src/voice/realtimeEvents");
const {
    normalizeTtsPcmBuffer
} = require("../../src/voice/ttsAudio");

function loadFixture() {
    const fixturePath = path.join(__dirname, "fixtures", "volc-tts-realtime-sanitized.json");
    return JSON.parse(fs.readFileSync(fixturePath, "utf8"));
}

function consumeBufferedFixture(fixture) {
    const chunks = [];
    let sessionUpdatedAt = null;
    let firstAudioAt = null;
    let completedAt = null;

    for (const fixtureEvent of fixture.events) {
        const event = parseTtsRealtimeEvent(JSON.stringify(fixtureEvent.payload));
        if (event.isError) {
            throw new Error(`fixture returned TTS error: ${event.errorMessage || "unknown"}`);
        }

        if (event.isSessionUpdated) {
            sessionUpdatedAt = fixtureEvent.at_ms;
        }

        if (event.isAudioDelta) {
            const chunk = decodeBase64Buffer(event.delta);
            assert.ok(chunk, `audio delta at ${fixtureEvent.at_ms}ms must decode`);
            chunks.push(chunk);
            if (firstAudioAt === null) {
                firstAudioAt = fixtureEvent.at_ms;
            }
        }

        if (event.isAudioDone) {
            assert.equal(completedAt, null, "fixture must have one completion event");
            completedAt = fixtureEvent.at_ms;
        }
    }

    assert.notEqual(sessionUpdatedAt, null, "fixture must include session acknowledgement");
    assert.notEqual(firstAudioAt, null, "fixture must include audio before completion");
    assert.notEqual(completedAt, null, "fixture must include completion");
    assert.ok(firstAudioAt < completedAt, "first audio must arrive before completion");

    return {
        firstAudioAt,
        completedAt,
        pcm: normalizeTtsPcmBuffer(Buffer.concat(chunks))
    };
}

function assertTtsParserAndBufferedReference() {
    const fixture = loadFixture();
    const result = consumeBufferedFixture(fixture);

    assert.deepEqual(result.pcm, Buffer.from(fixture.expected_pcm_base64, "base64"));
    assert.equal(result.firstAudioAt, 8);
    assert.equal(result.completedAt, 40);
    assert.throws(
        () => normalizeTtsPcmBuffer(Buffer.from([0x00])),
        error => error?.code === "VOICE_TTS_FAILED"
    );
    assert.throws(
        () => normalizeTtsPcmBuffer(Buffer.from("ID3x")),
        error => error?.code === "VOICE_TTS_UNSUPPORTED_FORMAT"
    );
}

assertTtsParserAndBufferedReference();
process.stdout.write("voice streaming offline fixture: PASS\n");
