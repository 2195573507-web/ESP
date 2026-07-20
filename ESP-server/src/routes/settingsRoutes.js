const express = require("express");
const {
    readHomeLocation,
    saveHomeLocation
} = require("../services/homeLocationService");
const {
    apiEnvelope,
    apiError
} = require("../utils/apiEnvelope");

function createSettingsRouter(options) {
    const router = express.Router();
    const dbRun = options.dbRun;
    const dbAll = options.dbAll;
    const logger = options.logger || console;

    router.get("/api/settings/home-location", async (req, res) => {
        try {
            return res.json(apiEnvelope({
                home_location: await readHomeLocation(dbAll)
            }));
        } catch (error) {
            logger.error(`[settings] home location read failed ${error?.message || error}`);
            return res.status(500).json(apiError("HOME_LOCATION_READ_FAILED", "home location read failed"));
        }
    });

    router.post("/api/settings/home-location", async (req, res) => {
        try {
            const result = await saveHomeLocation(dbRun, dbAll, req.body);
            if (!result.ok) {
                return res.status(400).json(apiError("HOME_LOCATION_INVALID", result.error));
            }

            return res.json(apiEnvelope({
                home_location: result.location
            }));
        } catch (error) {
            logger.error(`[settings] home location save failed ${error?.message || error}`);
            return res.status(500).json(apiError("HOME_LOCATION_SAVE_FAILED", "home location save failed"));
        }
    });

    return router;
}

module.exports = {
    createSettingsRouter
};
