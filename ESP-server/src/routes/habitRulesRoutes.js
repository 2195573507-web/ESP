const express = require("express");
const {
    createHabitRule,
    compileHabitRuleBundle,
    deleteHabitRule,
    getHabitRule,
    getHabitRulesSnapshotMetadata,
    listHabitRules,
    updateHabitRule
} = require("../services/habitRulesService");
const { apiEnvelope, apiError } = require("../utils/apiEnvelope");

function createHabitRulesRouter(options) {
    const router = express.Router();
    const { dbRun, dbAll } = options;
    const logger = options.logger || console;

    function validId(value) {
        return typeof value === "string" && /^[a-zA-Z0-9_-]{1,128}$/.test(value);
    }

    function sendServiceError(res, result) {
        if (result.code === "HABIT_RULE_NOT_FOUND") {
            return res.status(404).json(apiError(result.code, result.error));
        }
        return res.status(400).json(apiError(result.code || "HABIT_RULE_INVALID", result.error));
    }

    router.get("/api/habit-rules", async (req, res) => {
        try {
            return res.json(apiEnvelope({ rules: await listHabitRules(dbAll) }));
        } catch (error) {
            logger.error(`[habit-rules] list failed ${error?.message || error}`);
            return res.status(500).json(apiError("HABIT_RULE_LIST_FAILED", "habit rules could not be read"));
        }
    });

    router.post("/api/habit-rules", async (req, res) => {
        try {
            const result = await createHabitRule(dbRun, dbAll, req.body);
            return result.ok
                ? res.status(201).json(apiEnvelope({ rule: result.rule }))
                : sendServiceError(res, result);
        } catch (error) {
            logger.error(`[habit-rules] create failed ${error?.message || error}`);
            return res.status(500).json(apiError("HABIT_RULE_CREATE_FAILED", "habit rule could not be created"));
        }
    });

    router.get("/api/habit-rules/version", async (req, res) => {
        try {
            return res.json(apiEnvelope(await getHabitRulesSnapshotMetadata(dbAll)));
        } catch (error) {
            logger.error(`[habit-rules] version read failed ${error?.message || error}`);
            return res.status(500).json(apiError("HABIT_RULE_VERSION_READ_FAILED", "habit rule version could not be read"));
        }
    });

    router.get("/api/habit-rules/bundle", async (req, res) => {
        try {
            const result = compileHabitRuleBundle(await listHabitRules(dbAll));
            return result.ok
                ? res.json(apiEnvelope({ bundle: result.bundle }))
                : res.status(400).json(apiError("HABIT_RULE_BUNDLE_INVALID", result.error));
        } catch (error) {
            logger.error(`[habit-rules] bundle export failed ${error?.message || error}`);
            return res.status(500).json(apiError("HABIT_RULE_BUNDLE_EXPORT_FAILED", "habit rule bundle could not be compiled"));
        }
    });

    router.get("/api/habit-rules/:id", async (req, res) => {
        if (!validId(req.params.id)) return res.status(400).json(apiError("HABIT_RULE_ID_INVALID", "habit rule id is invalid"));
        try {
            const rule = await getHabitRule(dbAll, req.params.id);
            return rule
                ? res.json(apiEnvelope({ rule }))
                : res.status(404).json(apiError("HABIT_RULE_NOT_FOUND", "habit rule not found"));
        } catch (error) {
            logger.error(`[habit-rules] get failed ${error?.message || error}`);
            return res.status(500).json(apiError("HABIT_RULE_READ_FAILED", "habit rule could not be read"));
        }
    });

    router.put("/api/habit-rules/:id", async (req, res) => {
        if (!validId(req.params.id)) return res.status(400).json(apiError("HABIT_RULE_ID_INVALID", "habit rule id is invalid"));
        try {
            const result = await updateHabitRule(dbRun, dbAll, req.params.id, req.body);
            return result.ok
                ? res.json(apiEnvelope({ rule: result.rule }))
                : sendServiceError(res, result);
        } catch (error) {
            logger.error(`[habit-rules] update failed ${error?.message || error}`);
            return res.status(500).json(apiError("HABIT_RULE_UPDATE_FAILED", "habit rule could not be updated"));
        }
    });

    router.delete("/api/habit-rules/:id", async (req, res) => {
        if (!validId(req.params.id)) return res.status(400).json(apiError("HABIT_RULE_ID_INVALID", "habit rule id is invalid"));
        try {
            const result = await deleteHabitRule(dbRun, dbAll, req.params.id);
            return result.ok
                ? res.json(apiEnvelope({ rule: result.rule }))
                : sendServiceError(res, result);
        } catch (error) {
            logger.error(`[habit-rules] delete failed ${error?.message || error}`);
            return res.status(500).json(apiError("HABIT_RULE_DELETE_FAILED", "habit rule could not be deleted"));
        }
    });

    return router;
}

module.exports = {
    createHabitRulesRouter
};
