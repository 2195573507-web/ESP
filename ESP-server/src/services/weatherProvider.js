const {
    readHomeLocation
} = require("./homeLocationService");
const {
    readPositiveInteger,
    readTrimmedEnv
} = require("../utils/env");

const DEFAULT_WEATHER_TIMEOUT_MS = 8000;
const DEFAULT_OPENWEATHER_BASE_URL = "https://api.openweathermap.org";

function readWeatherConfig(logger = console) {
    const apiKey = readTrimmedEnv("OPENWEATHER_API_KEY");
    const baseUrl = readTrimmedEnv("OPENWEATHER_BASE_URL", DEFAULT_OPENWEATHER_BASE_URL).replace(/\/+$/, "");
    const timeoutMs = readPositiveInteger(process.env.OPENWEATHER_TIMEOUT_MS, DEFAULT_WEATHER_TIMEOUT_MS);
    if (!apiKey) {
        logger.warn("[weather] OPENWEATHER_API_KEY is not configured; weather refresh will fail closed");
    }
    return { apiKey, baseUrl, timeoutMs };
}

function buildOpenWeatherUrl(baseUrl, pathname, query) {
    const url = new URL(pathname, `${baseUrl}/`);
    for (const [key, value] of Object.entries(query)) {
        if (value !== null && value !== undefined && value !== "") {
            url.searchParams.set(key, String(value));
        }
    }
    return url.toString();
}

async function fetchOpenWeather(url, config, fetcher = fetch) {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), config.timeoutMs);
    try {
        const response = await fetcher(url, { signal: controller.signal });
        const body = await response.json().catch(() => null);
        if (!response.ok) {
            return { ok: false, error: body?.message || `OpenWeather request failed (${response.status})`, code: `HTTP_${response.status}` };
        }
        return { ok: true, body };
    } catch (error) {
        return {
            ok: false,
            error: error?.name === "AbortError" ? "OpenWeather request timed out" : "OpenWeather network request failed",
            code: error?.name === "AbortError" ? "TIMEOUT" : "NETWORK_ERROR"
        };
    } finally {
        clearTimeout(timer);
    }
}

function numberOrNull(value) {
    return Number.isFinite(Number(value)) ? Number(value) : null;
}

function integerOrNull(value) {
    return Number.isInteger(Number(value)) ? Number(value) : null;
}

function conditionKey(code) {
    const numeric = Number(code);
    if (numeric >= 200 && numeric < 300) return "thunderstorm";
    if (numeric >= 300 && numeric < 400) return "drizzle";
    if (numeric >= 500 && numeric < 600) return "rain";
    if (numeric >= 600 && numeric < 700) return "snow";
    if (numeric >= 700 && numeric < 800) return "atmosphere";
    if (numeric === 800) return "clear";
    if (numeric > 800 && numeric < 900) return "clouds";
    return null;
}

function mapForecast(list) {
    return Array.isArray(list) ? list.slice(0, 8).map(item => ({
        observed_at_ms: integerOrNull(Number(item?.dt) * 1000),
        temperature_c: numberOrNull(item?.main?.temp),
        humidity_percent: integerOrNull(item?.main?.humidity),
        condition_code: integerOrNull(item?.weather?.[0]?.id),
        condition_key: conditionKey(item?.weather?.[0]?.id),
        wind_speed_mps: numberOrNull(item?.wind?.speed),
        precipitation_probability: numberOrNull(item?.pop)
    })) : [];
}

function normalizeCurrent(body) {
    const conditionCode = integerOrNull(body?.weather?.[0]?.id);
    const temperature = numberOrNull(body?.main?.temp);
    if (conditionCode === null || temperature === null || body?.dt === undefined) {
        return null;
    }
    return {
        observed_at_ms: integerOrNull(Number(body.dt) * 1000),
        condition_code: conditionCode,
        condition_key: conditionKey(conditionCode),
        temperature_c: temperature,
        feels_like_c: numberOrNull(body?.main?.feels_like),
        humidity_percent: integerOrNull(body?.main?.humidity),
        wind_speed_mps: numberOrNull(body?.wind?.speed),
        sunrise_at_ms: integerOrNull(Number(body?.sys?.sunrise) * 1000),
        sunset_at_ms: integerOrNull(Number(body?.sys?.sunset) * 1000)
    };
}

async function fetchHomeWeather({ dbAll, config, fetcher = fetch }) {
    if (!config?.apiKey) return { ok: false, error: "OpenWeather is not configured", code: "NOT_CONFIGURED" };
    const homeLocation = await readHomeLocation(dbAll);
    if (homeLocation.latitude === null || homeLocation.longitude === null) {
        return { ok: false, error: "home location is not configured with latitude and longitude", code: "LOCATION_NOT_CONFIGURED" };
    }
    const query = {
        lat: homeLocation.latitude,
        lon: homeLocation.longitude,
        units: "metric",
        appid: config.apiKey
    };
    const currentResult = await fetchOpenWeather(
        buildOpenWeatherUrl(config.baseUrl, "/data/2.5/weather", query),
        config,
        fetcher
    );
    if (!currentResult.ok) return currentResult;

    const current = normalizeCurrent(currentResult.body);
    if (!current) return { ok: false, error: "OpenWeather current payload is incomplete", code: "MALFORMED_CURRENT" };

    const forecastResult = await fetchOpenWeather(
        buildOpenWeatherUrl(config.baseUrl, "/data/2.5/forecast", {
            ...query,
            lat: currentResult.body?.coord?.lat ?? homeLocation.latitude,
            lon: currentResult.body?.coord?.lon ?? homeLocation.longitude
        }),
        config,
        fetcher
    );
    if (!forecastResult.ok) return forecastResult;
    if (!Array.isArray(forecastResult.body?.list)) {
        return { ok: false, error: "OpenWeather forecast payload is incomplete", code: "MALFORMED_FORECAST" };
    }

    return {
        ok: true,
        location_updated_at: null,
        ...current,
        precipitation_probability: numberOrNull(forecastResult.body?.list?.[0]?.pop),
        forecast: mapForecast(forecastResult.body.list)
    };
}

module.exports = {
    DEFAULT_OPENWEATHER_BASE_URL,
    DEFAULT_WEATHER_TIMEOUT_MS,
    buildOpenWeatherUrl,
    conditionKey,
    fetchHomeWeather,
    fetchOpenWeather,
    mapForecast,
    readWeatherConfig
};
