const {
    fetchHomeWeather,
    readWeatherConfig
} = require("./weatherProvider");

const WEATHER_SCOPE = "home";
const WEATHER_CONTEXT_TTL_MS = 30 * 60 * 1000;
const REASONS = new Set(["link_stable", "ttl_due", "server_requested"]);

function parseForecast(value) {
    try {
        const parsed = JSON.parse(value || "[]");
        return Array.isArray(parsed) ? parsed.slice(0, 8) : [];
    } catch (_) {
        return [];
    }
}

function mapRow(row, nowMs = Date.now()) {
    if (!row) {
        return {
            status: "unavailable",
            available: false,
            observed_at_ms: null,
            fetched_at_ms: null,
            expires_at_ms: null,
            provider: "openweather",
            condition_code: null,
            condition_key: null,
            temperature_c: null,
            feels_like_c: null,
            humidity_percent: null,
            wind_speed_mps: null,
            precipitation_probability: null,
            sunrise_at_ms: null,
            sunset_at_ms: null,
            forecast: [],
            last_error_code: null
        };
    }
    const fresh = Boolean(row.available) && Number(row.expires_at_ms) > nowMs;
    return {
        status: fresh ? "fresh" : "stale",
        available: fresh,
        observed_at_ms: Number(row.observed_at_ms) || null,
        fetched_at_ms: Number(row.fetched_at_ms) || null,
        expires_at_ms: Number(row.expires_at_ms) || null,
        provider: row.provider || "openweather",
        condition_code: Number.isInteger(Number(row.condition_code)) ? Number(row.condition_code) : null,
        condition_key: row.condition_key || null,
        temperature_c: Number.isFinite(Number(row.temperature_c)) ? Number(row.temperature_c) : null,
        feels_like_c: Number.isFinite(Number(row.feels_like_c)) ? Number(row.feels_like_c) : null,
        humidity_percent: Number.isInteger(Number(row.humidity_percent)) ? Number(row.humidity_percent) : null,
        wind_speed_mps: Number.isFinite(Number(row.wind_speed_mps)) ? Number(row.wind_speed_mps) : null,
        precipitation_probability: Number.isFinite(Number(row.precipitation_probability)) ? Number(row.precipitation_probability) : null,
        sunrise_at_ms: Number(row.sunrise_at_ms) || null,
        sunset_at_ms: Number(row.sunset_at_ms) || null,
        forecast: parseForecast(row.forecast_json),
        last_error_code: row.last_error_code || null
    };
}

async function readWeatherContext(dbAll, nowMs = Date.now()) {
    const rows = await dbAll("SELECT * FROM weather_context WHERE scope_key=? LIMIT 1", [WEATHER_SCOPE]);
    return mapRow(rows[0], nowMs);
}

function normalizeRequest(input = {}) {
    if (Number(input.schema_version) !== 1) {
        return { ok: false, code: "SCHEMA_VERSION_INVALID", error: "schema_version must be 1" };
    }
    const requestId = typeof input.request_id === "string" ? input.request_id.trim().slice(0, 96) : "";
    const reason = typeof input.reason === "string" ? input.reason.trim() : "";
    if (!/^weather_[A-Za-z0-9_-]{4,88}$/.test(requestId)) return { ok: false, code: "REQUEST_ID_INVALID", error: "request_id is invalid" };
    if (!REASONS.has(reason)) return { ok: false, code: "REASON_INVALID", error: "reason is invalid" };
    return { ok: true, requestId, reason };
}

function responseFor(outcome, context, errorCode = null) {
    return {
        outcome,
        ...context,
        error_code: errorCode || context.last_error_code || null
    };
}

async function recordRequest(dbRun, request, gatewayId, receivedAtMs, result) {
    await dbRun(`
        INSERT OR REPLACE INTO weather_refresh_requests
            (request_id,gateway_id,reason,received_at_ms,outcome,context_updated_at_ms,error_code)
        VALUES(?,?,?,?,?,?,?)`,
        [request.requestId, gatewayId, request.reason, receivedAtMs, result.outcome, result.updated_at_ms || null, result.error_code || null]
    );
}

async function refreshHomeWeather(options) {
    const dbRun = options.dbRun;
    const dbAll = options.dbAll;
    const logger = options.logger || console;
    const receivedAtMs = Number.isFinite(options.nowMs) ? options.nowMs : Date.now();
    const request = normalizeRequest(options.input);
    if (!request.ok) return request;

    const existingRequest = await dbAll("SELECT * FROM weather_refresh_requests WHERE request_id=? LIMIT 1", [request.requestId]);
    if (existingRequest[0]) {
        const context = await readWeatherContext(dbAll, receivedAtMs);
        return { ok: true, replayed: true, ...responseFor(existingRequest[0].outcome, context, existingRequest[0].error_code) };
    }

    const current = await readWeatherContext(dbAll, receivedAtMs);
    if (current.status === "fresh") {
        const result = responseFor("fresh_cache", current);
        await recordRequest(dbRun, request, options.gatewayId || "", receivedAtMs, result);
        return { ok: true, ...result };
    }

    const config = options.config || readWeatherConfig(logger);
    const providerResult = await fetchHomeWeather({ dbAll, config, fetcher: options.fetcher || fetch });
    if (!providerResult.ok) {
        const errorCode = providerResult.code || "PROVIDER_UNAVAILABLE";
        await dbRun("UPDATE weather_context SET last_error_code=?,last_error_at_ms=?,updated_at_ms=? WHERE scope_key=?",
            [errorCode, receivedAtMs, receivedAtMs, WEATHER_SCOPE]);
        const context = await readWeatherContext(dbAll, receivedAtMs);
        const result = responseFor("unavailable", context, errorCode);
        await recordRequest(dbRun, request, options.gatewayId || "", receivedAtMs, result);
        logger.warn(`[weather] refresh unavailable code=${errorCode}`);
        return { ok: true, ...result };
    }

    const expiresAtMs = receivedAtMs + WEATHER_CONTEXT_TTL_MS;
    await dbRun(`
        INSERT INTO weather_context
            (scope_key,location_updated_at,provider,available,observed_at_ms,fetched_at_ms,expires_at_ms,
             condition_code,condition_key,temperature_c,feels_like_c,humidity_percent,wind_speed_mps,
             precipitation_probability,sunrise_at_ms,sunset_at_ms,forecast_json,last_error_code,last_error_at_ms,updated_at_ms)
        VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,NULL,NULL,?)
        ON CONFLICT(scope_key) DO UPDATE SET
            location_updated_at=excluded.location_updated_at,
            provider=excluded.provider,
            available=excluded.available,
            observed_at_ms=excluded.observed_at_ms,
            fetched_at_ms=excluded.fetched_at_ms,
            expires_at_ms=excluded.expires_at_ms,
            condition_code=excluded.condition_code,
            condition_key=excluded.condition_key,
            temperature_c=excluded.temperature_c,
            feels_like_c=excluded.feels_like_c,
            humidity_percent=excluded.humidity_percent,
            wind_speed_mps=excluded.wind_speed_mps,
            precipitation_probability=excluded.precipitation_probability,
            sunrise_at_ms=excluded.sunrise_at_ms,
            sunset_at_ms=excluded.sunset_at_ms,
            forecast_json=excluded.forecast_json,
            last_error_code=NULL,
            last_error_at_ms=NULL,
            updated_at_ms=excluded.updated_at_ms`,
        [WEATHER_SCOPE, providerResult.location_updated_at, "openweather", 1, providerResult.observed_at_ms,
            receivedAtMs, expiresAtMs, providerResult.condition_code, providerResult.condition_key,
            providerResult.temperature_c, providerResult.feels_like_c, providerResult.humidity_percent,
            providerResult.wind_speed_mps, providerResult.precipitation_probability, providerResult.sunrise_at_ms,
            providerResult.sunset_at_ms, JSON.stringify(providerResult.forecast || []), receivedAtMs]
    );
    const context = await readWeatherContext(dbAll, receivedAtMs);
    const result = responseFor("refreshed", context);
    result.updated_at_ms = receivedAtMs;
    await recordRequest(dbRun, request, options.gatewayId || "", receivedAtMs, result);
    return { ok: true, ...result };
}

module.exports = {
    WEATHER_CONTEXT_TTL_MS,
    mapRow,
    normalizeRequest,
    readWeatherContext,
    refreshHomeWeather
};
