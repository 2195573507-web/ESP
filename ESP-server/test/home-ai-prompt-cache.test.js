const assert = require("assert");
const fs = require("fs");
const os = require("os");
const path = require("path");

const tempDir = fs.mkdtempSync(path.join(os.tmpdir(), "home-ai-prompt-cache-"));
process.env.VOICE_PROMPT_CACHE_DIR = tempDir;
process.env.VOICE_DYNAMIC_PROMPT_CACHE_TTL_MS = "1000";
process.env.VOICE_DYNAMIC_PROMPT_CACHE_MAX_ENTRIES = "2";
process.env.VOICE_DYNAMIC_PROMPT_CACHE_MAX_BYTES = "65536";
process.env.VOICE_DYNAMIC_PROMPT_CACHE_MAX_ITEM_BYTES = "65536";

const {
    clearEphemeralPromptCache,
    getEphemeralPromptCacheStats,
    promptKeyForText,
    readPromptCache,
    sendPromptCachePcm,
    writePromptCache
} = require("../src/voice/promptCache");

function pcm(seed, bytes = 32000) {
    const value = seed & 0xff || 1;
    return Buffer.alloc(bytes, value);
}

function fakeResponse() {
    return {
        headers: {},
        statusCode: 0,
        body: null,
        status(code) {
            this.statusCode = code;
            return this;
        },
        set(headers) {
            this.headers = { ...headers };
            return this;
        },
        end(body) {
            this.body = body;
            return this;
        }
    };
}

try {
    clearEphemeralPromptCache();
    const startMs = 1_700_000_000_000;
    const firstKey = promptKeyForText("第一条动态播报");
    const secondKey = promptKeyForText("第二条动态播报");
    const thirdKey = promptKeyForText("第三条动态播报");

    const first = writePromptCache(firstKey, "第一条动态播报", pcm(1), {}, startMs);
    assert.equal(first.meta.ephemeral, true);
    assert.equal(first.meta.file_path, "");
    assert.equal(fs.readdirSync(tempDir).length, 0, "dynamic TTS must not be persisted");
    assert.equal(readPromptCache(firstKey, startMs + 1)?.promptKey, firstKey);

    writePromptCache(secondKey, "第二条动态播报", pcm(2), {}, startMs + 10);
    writePromptCache(thirdKey, "第三条动态播报", pcm(3), {}, startMs + 20);
    assert.equal(readPromptCache(firstKey, startMs + 21), null, "entry cap must evict LRU prompt");
    const bounded = getEphemeralPromptCacheStats(startMs + 21);
    assert.equal(bounded.entries, 2);
    assert.ok(bounded.bytes <= bounded.max_bytes);
    assert.ok(bounded.entries <= bounded.max_entries);

    assert.equal(readPromptCache(secondKey, startMs + 1011), null, "expired prompt must be removed");
    assert.equal(readPromptCache(thirdKey, startMs + 1021), null, "expired prompt must be removed");
    assert.equal(getEphemeralPromptCacheStats(startMs + 1021).entries, 0);

    const ephemeralResponse = fakeResponse();
    sendPromptCachePcm(ephemeralResponse, first, "miss", startMs);
    assert.equal(ephemeralResponse.statusCode, 200);
    assert.match(ephemeralResponse.headers["Cache-Control"], /no-store/);

    const staticRecord = writePromptCache("wake_prompt_test", "我在", pcm(4), {}, startMs);
    assert.equal(staticRecord.meta.ephemeral, false);
    assert.ok(fs.existsSync(path.join(tempDir, "wake_prompt_test_16000_s16le.pcm")));
    assert.ok(fs.existsSync(path.join(tempDir, "wake_prompt_test_16000_s16le.json")));
    clearEphemeralPromptCache();
    assert.equal(readPromptCache(firstKey, startMs + 1), null);
    assert.equal(readPromptCache("wake_prompt_test", startMs + 1)?.promptKey, "wake_prompt_test");

    const staticResponse = fakeResponse();
    sendPromptCachePcm(staticResponse, staticRecord, "hit", startMs);
    assert.equal(staticResponse.headers["Cache-Control"], "public, max-age=86400");

    process.stdout.write("home ai prompt-cache tests: PASS\n");
} finally {
    clearEphemeralPromptCache();
    fs.rmSync(tempDir, { recursive: true, force: true });
}
