const assert = require("assert");
const { clearRawVoiceBody } = require("../../src/routes/voiceRoutes");

const request = { body: Buffer.from([0x01, 0x02, 0x03, 0x04]) };
clearRawVoiceBody(request);
assert.deepEqual([...request.body], [0, 0, 0, 0]);

const nonBuffer = { body: "not pcm" };
clearRawVoiceBody(nonBuffer);
assert.equal(nonBuffer.body, "not pcm");

process.stdout.write("raw audio lifecycle tests: PASS\n");
