const assert = require("assert");
const fs = require("fs");
const path = require("path");
const {
    buildLlmPrompt,
    buildPromptWithContext,
    getEspHomeAgentSystemPrompt
} = require("../src/services/llmPromptContextService");

const promptPath = path.resolve(__dirname, "../prompts/esp_home_agent_system.md");

function createContext() {
    return {
        device: {
            device_id: "esp-test-s3",
            online: true,
            last_seen_age_ms: 50,
            avg_upload_delay_ms: 10,
            latest_upload_delay_ms: 8,
            delay_sample_count: 2,
            time_synced: true
        },
        environment: {
            available: false,
            fresh: false,
            age_ms: null
        },
        air_quality: {
            available: false
        },
        modules: {}
    };
}

async function run() {
    assert.equal(fs.existsSync(promptPath), true, "system prompt file must exist");

    const systemPrompt = getEspHomeAgentSystemPrompt();
    for (const requiredText of [
        "ESP Home AI Agent",
        "实时数据真实性",
        "weather_query",
        "home_state_query",
        "sensor_query",
        "device_status_query",
        "不得推断人员身份"
    ]) {
        assert.match(systemPrompt, new RegExp(requiredText));
    }

    assert.match(systemPrompt, /普通知识问题无需调用工具/);
    assert.match(systemPrompt, /当前天气.*必须调用 `weather_query`/);
    assert.match(systemPrompt, /家里有没有人.*调用 `home_state_query`/);

    const prompt = buildPromptWithContext("今天天气怎么样？", createContext());
    assert.match(prompt, /System Prompt/);
    assert.match(prompt, /Runtime Context/);
    assert.match(prompt, /User Message/);
    assert.match(prompt, /weather_query/);
    assert.match(prompt, /今天天气怎么样/);
    assert.doesNotMatch(prompt, /Tool Decision/);

    const homeStatePrompt = buildPromptWithContext("家里哪个房间有人？", createContext());
    assert.match(homeStatePrompt, /home_state_query/);
    assert.match(homeStatePrompt, /家里哪个房间有人/);

    const result = await buildLlmPrompt(async () => {
        throw new Error("context unavailable");
    }, "家里有没有人？");
    assert.equal(result.ok, false);
    assert.match(result.prompt, /System Prompt/);
    assert.match(result.prompt, /home_state_query/);

    process.stdout.write("ESP Home AI Agent prompt tests passed\n");
}

run().catch(error => {
    process.stderr.write(`${error.stack || error.message}\n`);
    process.exitCode = 1;
});
