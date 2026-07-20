const express = require("express");
const { saveHabitEvent } = require("../services/habitEventsService");
const { apiEnvelope, apiError } = require("../utils/apiEnvelope");

function createHabitEventsRouter(options) {
    const router = express.Router();
    const { dbRun } = options;
    const logger = options.logger || console;

    router.post("/api/habit-events", async (req, res) => {
        try {
            const result = await saveHabitEvent(dbRun, req.body);
            if (!result.ok) return res.status(400).json(apiError("HABIT_EVENT_INVALID", result.error));
            return res.status(result.duplicate ? 200 : 201).json(apiEnvelope({
                accepted: !result.duplicate,
                duplicate: result.duplicate,
                event_id: result.event.event_id
            }));
        } catch (error) {
            logger.error(`[habit-events] save failed ${error?.message || error}`);
            return res.status(500).json(apiError("HABIT_EVENT_SAVE_FAILED", "habit event could not be saved"));
        }
    });

    return router;
}

module.exports = { createHabitEventsRouter };
