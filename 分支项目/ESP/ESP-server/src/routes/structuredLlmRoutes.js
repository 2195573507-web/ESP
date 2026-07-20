const express = require("express");
const {
    enqueueCommand
} = require("../commands/queue");
const {
    describeLlmError,
    getLlmResponseStatus,
    readLlmConfig,
    readLlmTextRequest,
    requestLlmText
} = require("../llm/textClient");
const {
    buildStructuredPrompt,
    parseStructuredLlmOutput
} = require("../llm/structuredOutput");
const {
    maskLogValue
} = require("../utils/logging");
const {
    buildLlmPrompt
} = require("../services/llmPromptContextService");

function createStructuredLlmRouter(options) {
    const router = express.Router();
    const dbRun = options.dbRun;
    const dbAll = options.dbAll;
    const logger = options.logger || console;

    router.post("/api/llm/structured", async (req, res) => {
        const llmRequest = readLlmTextRequest(req.body);
        if (llmRequest.error) {
            return res.status(400).json({
                ok: false,
                error: llmRequest.error
            });
        }

        const config = readLlmConfig(logger);
        const requestedTargetDeviceId = typeof req.body.target_device_id === "string"
            ? req.body.target_device_id.trim()
            : "";
        const targetDeviceId = requestedTargetDeviceId || llmRequest.deviceId;
        const promptContext = await buildLlmPrompt(dbAll, llmRequest.text, {
            deviceId: targetDeviceId || llmRequest.deviceId,
            mode: "structured"
        });
        const prompt = buildStructuredPrompt(promptContext.prompt);
        logger.log(
            `[llm-structured] request text_len=${llmRequest.text.length} device_id=${maskLogValue(llmRequest.deviceId)} target_device_id=${maskLogValue(targetDeviceId)} session_id=${maskLogValue(llmRequest.sessionId)} key_${config.keySummary} endpoint=${config.endpoint} model=${config.model}`
        );

        try {
            const llmResult = await requestLlmText(prompt, config);
            const parsed = parseStructuredLlmOutput(llmResult.text);
            const serverTimeMs = Date.now();
            const chatText = parsed.parsed ? parsed.chat_text : (parsed.chat_text || llmResult.text);
            const insertResult = await dbRun(
                "INSERT INTO llm_records(timestamp,prompt,response) VALUES(?,?,?)",
                [serverTimeMs, llmRequest.text, chatText]
            );

            const queuedCommands = [];
            const rejectedCommands = [];
            for (const command of parsed.commands) {
                const commandInput = targetDeviceId
                    ? {
                        ...command,
                        target_device_id: targetDeviceId
                    }
                    : command;
                const enqueueResult = await enqueueCommand(dbRun, dbAll, commandInput, {
                    source: "llm",
                    requestedBy: llmRequest.deviceId || "llm",
                    targetDeviceId,
                    relatedLlmRecordId: insertResult.lastID
                });
                if (enqueueResult.ok) {
                    queuedCommands.push(enqueueResult.command);
                } else {
                    rejectedCommands.push({
                        name: enqueueResult.name || commandInput?.name || "",
                        target_device_id: enqueueResult.target_device_id || commandInput?.target_device_id || targetDeviceId || "",
                        code: enqueueResult.code || "COMMAND_REJECTED",
                        error: enqueueResult.error || "command rejected"
                    });
                }
            }

            return res.json({
                ok: true,
                text: chatText,
                chat: {
                    text: chatText
                },
                commands: queuedCommands,
                rejected_commands: rejectedCommands,
                structured: {
                    parsed: parsed.parsed,
                    version: parsed.version,
                    error: parsed.error
                },
                id: insertResult.lastID,
                model: llmResult.model,
                server_time_ms: serverTimeMs
            });
        } catch (error) {
            logger.error(`[llm-structured] failed ${describeLlmError(error)}`);

            return res.status(getLlmResponseStatus(error)).json({
                ok: false,
                code: error?.code || "LLM_STRUCTURED_REQUEST_FAILED",
                error: "LLM structured request failed",
                upstream_status: typeof error?.status === "number" ? error.status : undefined
            });
        }
    });

    return router;
}

module.exports = {
    createStructuredLlmRouter
};
