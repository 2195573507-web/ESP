const { buildLocationLabel, readHomeLocation } = require("../services/homeLocationService");
const { readWeatherContext } = require("../services/weatherContextService");
const {
    DEFAULT_WEATHER_TIMEOUT_MS,
    buildOpenWeatherUrl,
    fetchOpenWeather,
    mapForecast,
    readWeatherConfig
} = require("../services/weatherProvider");

function parseWeatherLocation(location) {
    if (typeof location !== "string") {
        return "";
    }
    return location.trim().slice(0, 256);
}

function weatherError(error) {
    return { success: false, error };
}

async function weatherQuery(args, context = {}) {
    if (!args?.location) {
        const cached = await readWeatherContext(context.dbAll);
        if (cached.status !== "fresh") {
            return weatherError("WEATHER_CONTEXT_UNAVAILABLE");
        }
        return {
            success: true,
            source: "weather_context",
            observed_at_ms: cached.observed_at_ms,
            expires_at_ms: cached.expires_at_ms,
            temperature: cached.temperature_c,
            feels_like: cached.feels_like_c,
            humidity: cached.humidity_percent,
            weather: cached.condition_key,
            wind_speed: cached.wind_speed_mps,
            forecast: cached.forecast
        };
    }
    const config = context.weatherConfig || readWeatherConfig(context.logger || console);
    if (!config.apiKey) {
        return weatherError("OpenWeather is not configured");
    }

    const requestedLocation = parseWeatherLocation(args?.location);
    if (args?.location !== undefined && !requestedLocation) {
        return weatherError("location must be a non-empty string when provided");
    }

    let homeLocation = null;
    let query;
    if (requestedLocation) {
        query = { q: requestedLocation, units: "metric", appid: config.apiKey };
    } else {
        homeLocation = await readHomeLocation(context.dbAll);
        if (homeLocation.latitude === null || homeLocation.longitude === null) {
            return weatherError("home location is not configured with latitude and longitude");
        }
        query = {
            lat: homeLocation.latitude,
            lon: homeLocation.longitude,
            units: "metric",
            appid: config.apiKey
        };
    }

    const currentResult = await fetchOpenWeather(
        buildOpenWeatherUrl(config.baseUrl, "/data/2.5/weather", query),
        config,
        context.fetcher
    );
    if (!currentResult.ok) {
        return weatherError(currentResult.error);
    }

    const current = currentResult.body;
    const forecastResult = await fetchOpenWeather(
        buildOpenWeatherUrl(config.baseUrl, "/data/2.5/forecast", {
            lat: current?.coord?.lat,
            lon: current?.coord?.lon,
            units: "metric",
            appid: config.apiKey
        }),
        config,
        context.fetcher
    );
    if (!forecastResult.ok) {
        return weatherError(forecastResult.error);
    }

    return {
        success: true,
        location: current?.name || requestedLocation || buildLocationLabel(homeLocation),
        temperature: Number.isFinite(Number(current?.main?.temp)) ? Number(current.main.temp) : null,
        humidity: Number.isFinite(Number(current?.main?.humidity)) ? Number(current.main.humidity) : null,
        weather: current?.weather?.[0]?.description || "",
        wind_speed: Number.isFinite(Number(current?.wind?.speed)) ? Number(current.wind.speed) : null,
        forecast: mapForecast(forecastResult.body?.list)
    };
}

module.exports = {
    DEFAULT_WEATHER_TIMEOUT_MS,
    buildOpenWeatherUrl,
    fetchOpenWeather,
    mapForecast,
    readWeatherConfig,
    weatherQuery
};
