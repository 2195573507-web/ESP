const assert = require("assert");
const fs = require("fs");
const os = require("os");
const path = require("path");
const test = require("node:test");
const { createDatabase, createDbHelpers } = require("../src/db/sqlite");
const { ensureHomeLocationTables } = require("../src/db/homeLocation");
const { ensureWeatherContextTables } = require("../src/db/weatherContext");
const { saveHomeLocation } = require("../src/services/homeLocationService");
const {
    WEATHER_CONTEXT_TTL_MS,
    readWeatherContext,
    refreshHomeWeather
} = require("../src/services/weatherContextService");

async function createTestDb() {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), "esp-weather-context-"));
    const db = createDatabase(root);
    const helpers = createDbHelpers(db);
    await ensureHomeLocationTables(helpers.dbRun);
    await ensureWeatherContextTables(helpers.dbRun);
    await saveHomeLocation(helpers.dbRun, helpers.dbAll, {
        city: "Shanghai",
        latitude: 31.2304,
        longitude: 121.4737,
        timezone: "Asia/Shanghai"
    });
    return {
        ...helpers,
        close: () => new Promise(resolve => db.close(resolve))
    };
}

function weatherFetcher() {
    return async url => ({
        ok: true,
        json: async () => String(url).includes("forecast")
            ? { list: [{ dt: 1001, main: { temp: 21, humidity: 60 }, weather: [{ id: 801 }], wind: { speed: 2 }, pop: 0.25 }] }
            : { dt: 1000, main: { temp: 20, feels_like: 19, humidity: 55 }, weather: [{ id: 800 }], wind: { speed: 1.5 }, sys: { sunrise: 900, sunset: 1900 }, coord: { lat: 31.2304, lon: 121.4737 } }
    });
}

const configuredWeather = { apiKey: "test-key", baseUrl: "https://weather.invalid", timeoutMs: 50 };

test("weather refresh persists a fresh bounded context and replays idempotently", async () => {
    const db = await createTestDb();
    try {
        const nowMs = 1_700_000_000_000;
        const input = { schema_version: 1, request_id: "weather_test_refresh", reason: "link_stable" };
        const result = await refreshHomeWeather({ ...db, input, gatewayId: "s3-test", nowMs, config: configuredWeather, fetcher: weatherFetcher() });
        assert.equal(result.ok, true);
        assert.equal(result.outcome, "refreshed");
        assert.equal(result.available, true);
        assert.equal(result.condition_key, "clear");
        assert.equal(result.forecast.length, 1);
        assert.equal(result.expires_at_ms, nowMs + WEATHER_CONTEXT_TTL_MS);

        const replay = await refreshHomeWeather({ ...db, input, gatewayId: "s3-test", nowMs: nowMs + 1, config: configuredWeather, fetcher: async () => { throw new Error("must not fetch"); } });
        assert.equal(replay.ok, true);
        assert.equal(replay.replayed, true);
        assert.equal(replay.outcome, "refreshed");
    } finally {
        await db.close();
    }
});

test("expired or failed weather context is unavailable and does not extend expiry", async () => {
    const db = await createTestDb();
    try {
        const nowMs = 1_700_000_000_000;
        await refreshHomeWeather({
            ...db,
            input: { schema_version: 1, request_id: "weather_test_seed", reason: "link_stable" },
            gatewayId: "s3-test",
            nowMs,
            config: configuredWeather,
            fetcher: weatherFetcher()
        });
        const expiredAt = nowMs + WEATHER_CONTEXT_TTL_MS + 1;
        assert.equal((await readWeatherContext(db.dbAll, expiredAt)).status, "stale");
        const failed = await refreshHomeWeather({
            ...db,
            input: { schema_version: 1, request_id: "weather_test_failed", reason: "ttl_due" },
            gatewayId: "s3-test",
            nowMs: expiredAt,
            config: configuredWeather,
            fetcher: async () => { throw new Error("network unreachable"); },
            logger: { warn() {} }
        });
        assert.equal(failed.outcome, "unavailable");
        assert.equal(failed.available, false);
        const context = await readWeatherContext(db.dbAll, expiredAt);
        assert.equal(context.expires_at_ms, nowMs + WEATHER_CONTEXT_TTL_MS);
        assert.equal(context.last_error_code, "NETWORK_ERROR");
    } finally {
        await db.close();
    }
});
