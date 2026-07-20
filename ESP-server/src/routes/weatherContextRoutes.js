const express = require("express");
const { apiEnvelope, apiError } = require("../utils/apiEnvelope");
const { requireGatewayAuth } = require("../services/gatewayAuthService");
const { readWeatherContext, refreshHomeWeather } = require("../services/weatherContextService");

function createWeatherContextRouter(options) {
    const router = express.Router();
    const dbRun = options.dbRun;
    const dbAll = options.dbAll;
    const logger = options.logger || console;
    const gatewayOnly = requireGatewayAuth({ dbRun, dbAll });

    router.post("/api/weather/v1/refresh", gatewayOnly, async (req, res) => {
        try {
            const result = await refreshHomeWeather({
                dbRun,
                dbAll,
                input: req.body,
                gatewayId: req.gatewayAuth?.gateway_id || "",
                logger
            });
            if (!result.ok) return res.status(400).json(apiError(result.code, result.error));
            return res.json(apiEnvelope({
                outcome: result.outcome,
                weather_context: result
            }));
        } catch (error) {
            logger.error(`[weather] refresh failed ${error?.message || error}`);
            return res.status(500).json(apiError("WEATHER_REFRESH_FAILED", "weather refresh failed"));
        }
    });

    router.get("/api/weather/v1/context", gatewayOnly, async (req, res) => {
        try {
            return res.json(apiEnvelope({ weather_context: await readWeatherContext(dbAll) }));
        } catch (error) {
            logger.error(`[weather] context read failed ${error?.message || error}`);
            return res.status(500).json(apiError("WEATHER_CONTEXT_READ_FAILED", "weather context read failed"));
        }
    });

    return router;
}

module.exports = { createWeatherContextRouter };
