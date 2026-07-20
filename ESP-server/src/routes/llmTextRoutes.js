const express = require("express");
const {
    describeLlmError,
    getLlmResponseStatus,
    readLlmConfig,
    readLlmTextRequest
} = require("../llm/textClient");
const {
    maskLogValue
} = require("../utils/logging");
const {
    createDefaultToolRegistry
} = require("../agent/defaultToolRegistry");
const {
    runAgentConversation
} = require("../agent/agentRunner");

function createLlmTextRouter(options) {
    const router = express.Router();
    const dbRun = options.dbRun;
    const dbAll = options.dbAll;
    const logger = options.logger || console;
    const toolRegistry = options.toolRegistry || createDefaultToolRegistry();

    router.post("/api/llm/text", async (req, res) => {
        const llmRequest = readLlmTextRequest(req.body);
        if (llmRequest.error) {
            return res.status(400).json({
                ok: false,
                error: llmRequest.error
            });
        }

        const config = readLlmConfig(logger);
        logger.log(
            `[llm-text] request text_len=${llmRequest.text.length} device_id=${maskLogValue(llmRequest.deviceId)} session_id=${maskLogValue(llmRequest.sessionId)} key_${config.keySummary} endpoint=${config.endpoint} model=${config.model}`
        );

        try {
            const llmResult = await runAgentConversation({
                dbAll,
                toolRegistry,
                userText: llmRequest.text,
                deviceId: llmRequest.deviceId,
                config,
                logger
            });
            const serverTimeMs = Date.now();
            const insertResult = await dbRun(
                "INSERT INTO llm_records(timestamp,prompt,response) VALUES(?,?,?)",
                [serverTimeMs, llmRequest.text, llmResult.text]
            );

            logger.log(
                `[llm-text] success id=${insertResult.lastID} reply_len=${llmResult.text.length} model=${llmResult.model}`
            );

            return res.json({
                ok: true,
                text: llmResult.text,
                id: insertResult.lastID,
                model: llmResult.model,
                server_time_ms: serverTimeMs
            });
        } catch (error) {
            logger.error(`[llm-text] failed ${describeLlmError(error)}`);

            const status = getLlmResponseStatus(error);
            const payload = {
                ok: false,
                code: error?.code || "LLM_REQUEST_FAILED",
                error: "LLM request failed"
            };

            if (typeof error?.status === "number") {
                payload.upstream_status = error.status;
            }

            return res.status(status).json(payload);
        }
    });

    return router;
}

module.exports = {
    createLlmTextRouter
};
