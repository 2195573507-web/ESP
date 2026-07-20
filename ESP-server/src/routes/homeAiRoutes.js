const express = require("express");
const {
    validateRulePackage
} = require("../homeAi/schema");
const {
    attachRuleCandidateProbation,
    createMemoryCandidate,
    createRuleCandidate,
    deleteMemoryCandidate,
    evaluateProbationRun,
    evaluateRuleCandidate,
    listFeedback,
    listHabits,
    listMemoryCandidates,
    listProbationRuns,
    listRuleCandidates,
    startProbationRuns,
    updateMemoryCandidate
} = require("../homeAi/learningService");
const {
    listAgentDecisions,
    listPromptProfiles,
    orchestrate,
    readAgentDecision
} = require("../homeAi/agentOrchestrator");
const {
    HomeAiToolError,
    executeTool,
    listTools,
    normalizeHomeLocation,
    readSetting,
    writeSetting
} = require("../homeAi/toolRegistry");
const {
    listMaintenanceRuns,
    readRetentionPolicy,
    validateRetentionPolicy,
    writeRetentionPolicy
} = require("../jobs/homeAiDataJobs");
const {
    acknowledgeEmergency,
    deleteUserOverride,
    listHomeAiEvents,
    listRuleDeployments,
    listRulePackages,
    publishRulePackage,
    rollbackRulePackage,
    readCurrentRulePackage,
    readCurrentRulePackageTransport,
    readGatewayConfigTransport,
    readRoomConfig,
    readRuleUpdateNotification,
    readVirtualDevices,
    listUserOverrides,
    recordFeedback,
    recordHomeAiEvents,
    recordRuleDeployment,
    upsertVirtualDeviceStates,
    writeUserOverride,
    writeRoomConfig
} = require("../services/homeAiService");
const {
    requireGatewayAuth
} = require("../services/gatewayAuthService");
const {
    apiEnvelope,
    apiError
} = require("../utils/apiEnvelope");

function createHomeAiRouter(options) {
    const router = express.Router();
    const dbRun = options.dbRun;
    const dbAll = options.dbAll;
    const logger = options.logger || console;
    const gatewayOnly = requireGatewayAuth({ dbRun, dbAll });

    router.get("/api/home-ai/v1/rooms", async (req, res) => {
        try {
            return res.json(apiEnvelope(await readRoomConfig(dbAll)));
        } catch (error) {
            logger.error(`[home-ai] read rooms failed ${error?.message || error}`);
            return res.status(500).json(apiError("HOME_AI_ROOMS_READ_FAILED", "room configuration read failed"));
        }
    });

    router.put("/api/home-ai/v1/rooms", async (req, res) => {
        try {
            const result = await writeRoomConfig(dbRun, dbAll, req.body);
            if (!result.ok) {
                return res.status(400).json(apiError(result.code, result.error));
            }
            return res.json(apiEnvelope(result.config));
        } catch (error) {
            logger.error(`[home-ai] write rooms failed ${error?.message || error}`);
            return res.status(500).json(apiError("HOME_AI_ROOMS_WRITE_FAILED", "room configuration write failed"));
        }
    });

    router.get("/api/home-ai/v1/config", gatewayOnly, async (req, res) => {
        try {
            return res.json(apiEnvelope(await readGatewayConfigTransport(dbAll)));
        } catch (error) {
            logger.error(`[home-ai] gateway config failed ${error?.message || error}`);
            if (["HOME_AI_CONFIG_OVERRIDE_LIMIT", "HOME_AI_CONFIG_TOO_LARGE"].includes(error?.code)) {
                return res.status(409).json(apiError(error.code, error.message));
            }
            return res.status(500).json(apiError("HOME_AI_CONFIG_READ_FAILED", "gateway configuration read failed"));
        }
    });

    router.post("/api/home-ai/v1/rules/validate", async (req, res) => {
        const result = validateRulePackage(req.body, { requireChecksum: false });
        if (!result.ok) {
            return res.status(400).json(apiError(result.code, result.error));
        }
        return res.json(apiEnvelope({ package: result.package, resource: result.resource }));
    });

    router.post("/api/home-ai/v1/rules/publish", async (req, res) => {
        try {
            const result = await publishRulePackage(dbRun, dbAll, req.body);
            if (!result.ok) {
                return res.status(400).json(apiError(result.code, result.error));
            }
            return res.status(201).json(apiEnvelope(result.rule_package));
        } catch (error) {
            logger.error(`[home-ai] publish rules failed ${error?.message || error}`);
            return res.status(500).json(apiError("HOME_AI_RULE_PUBLISH_FAILED", "rule publish failed"));
        }
    });

    router.get("/api/home-ai/v1/rules", async (req, res) => {
        try {
            return res.json(apiEnvelope({ packages: await listRulePackages(dbAll, req.query) }));
        } catch (error) {
            logger.error(`[home-ai] list rules failed ${error?.message || error}`);
            return res.status(500).json(apiError("HOME_AI_RULE_LIST_FAILED", "rule list failed"));
        }
    });

    router.get("/api/home-ai/v1/rules/current", async (req, res) => {
        try {
            return res.json(apiEnvelope({ rule_package: await readCurrentRulePackage(dbAll) }));
        } catch (error) {
            logger.error(`[home-ai] current rules failed ${error?.message || error}`);
            return res.status(500).json(apiError("HOME_AI_RULE_CURRENT_FAILED", "current rule read failed"));
        }
    });

    router.get("/api/home-ai/v1/rules/package", gatewayOnly, async (req, res) => {
        try {
            return res.json(apiEnvelope({ rule_package: await readCurrentRulePackageTransport(dbAll) }));
        } catch (error) {
            logger.error(`[home-ai] gateway package failed ${error?.message || error}`);
            return res.status(500).json(apiError("HOME_AI_RULE_PACKAGE_READ_FAILED", "rule package read failed"));
        }
    });

    router.get("/api/home-ai/v1/rules/notification", gatewayOnly, async (req, res) => {
        try {
            return res.json(apiEnvelope(await readRuleUpdateNotification(
                dbAll,
                req.query.known_version,
                req.query.known_config_checksum
            )));
        } catch (error) {
            logger.error(`[home-ai] rule notification failed ${error?.message || error}`);
            return res.status(500).json(apiError("HOME_AI_RULE_NOTIFICATION_READ_FAILED", "rule notification read failed"));
        }
    });

    router.post("/api/home-ai/v1/rules/rollback", async (req, res) => {
        try {
            const result = await rollbackRulePackage(dbRun, dbAll, req.body);
            if (!result.ok) {
                return res.status(result.code === "RULE_ROLLBACK_TARGET_NOT_FOUND" ? 404 : 409)
                    .json(apiError(result.code, result.error));
            }
            return res.status(201).json(apiEnvelope({
                rule_package: result.rule_package,
                control: result.control
            }));
        } catch (error) {
            logger.error(`[home-ai] rule rollback failed ${error?.message || error}`);
            return res.status(500).json(apiError("HOME_AI_RULE_ROLLBACK_FAILED", "rule rollback failed"));
        }
    });

    router.post("/api/home-ai/v1/rules/deployments", gatewayOnly, async (req, res) => {
        try {
            const result = await recordRuleDeployment(dbRun, req.body, req.gatewayAuth?.gateway_id);
            if (!result.ok) {
                return res.status(400).json(apiError(result.code, result.error));
            }
            return res.status(202).json(apiEnvelope(result.deployment));
        } catch (error) {
            logger.error(`[home-ai] deployment failed ${error?.message || error}`);
            return res.status(500).json(apiError("HOME_AI_RULE_DEPLOYMENT_WRITE_FAILED", "rule deployment write failed"));
        }
    });

    router.get("/api/home-ai/v1/rules/deployments", async (req, res) => {
        try {
            return res.json(apiEnvelope({ deployments: await listRuleDeployments(dbAll, req.query) }));
        } catch (error) {
            logger.error(`[home-ai] list deployments failed ${error?.message || error}`);
            return res.status(500).json(apiError("HOME_AI_RULE_DEPLOYMENT_LIST_FAILED", "rule deployment list failed"));
        }
    });

    router.post("/api/home-ai/v1/events", gatewayOnly, async (req, res) => {
        try {
            const result = await recordHomeAiEvents(dbRun, req.body, req.gatewayAuth?.gateway_id, dbAll);
            if (!result.ok) {
                return res.status(400).json(apiError(result.code, result.error));
            }
            return res.status(202).json(apiEnvelope({ accepted: result.accepted, rejected: result.rejected, learning_errors: result.learning_errors }));
        } catch (error) {
            logger.error(`[home-ai] event ingest failed ${error?.message || error}`);
            return res.status(500).json(apiError("HOME_AI_EVENT_WRITE_FAILED", "event ingest failed"));
        }
    });

    router.post("/api/home-ai/v1/history/replay", gatewayOnly, async (req, res) => {
        try {
            const result = await recordHomeAiEvents(dbRun, req.body, req.gatewayAuth?.gateway_id, dbAll);
            if (!result.ok) {
                return res.status(400).json(apiError(result.code, result.error));
            }
            return res.json(apiEnvelope({ accepted: result.accepted, rejected: result.rejected, learning_errors: result.learning_errors }));
        } catch (error) {
            logger.error(`[home-ai] history replay failed ${error?.message || error}`);
            return res.status(500).json(apiError("HOME_AI_HISTORY_REPLAY_FAILED", "history replay failed"));
        }
    });

    router.get("/api/home-ai/v1/events", async (req, res) => {
        try {
            return res.json(apiEnvelope({ events: await listHomeAiEvents(dbAll, req.query) }));
        } catch (error) {
            logger.error(`[home-ai] list events failed ${error?.message || error}`);
            return res.status(500).json(apiError("HOME_AI_EVENT_LIST_FAILED", "event list failed"));
        }
    });

    router.post("/api/home-ai/v1/emergencies/:event_id/acknowledge", async (req, res) => {
        try {
            const result = await acknowledgeEmergency(dbRun, dbAll, req.params.event_id, req.body);
            if (!result.ok) {
                const status = result.code === "HOME_AI_EMERGENCY_NOT_FOUND" ? 404 :
                    result.code === "HOME_AI_EMERGENCY_NOT_ACTIVE" ? 409 : 400;
                return res.status(status).json(apiError(result.code, result.error));
            }
            return res.json(apiEnvelope(result.acknowledgement));
        } catch (error) {
            logger.error(`[home-ai] emergency acknowledgement failed ${error?.message || error}`);
            return res.status(500).json(apiError("HOME_AI_EMERGENCY_ACK_FAILED", "emergency acknowledgement failed"));
        }
    });

    router.post("/api/home-ai/v1/virtual-devices/state", gatewayOnly, async (req, res) => {
        try {
            const result = await upsertVirtualDeviceStates(dbRun, req.body);
            if (!result.ok) {
                return res.status(400).json(apiError(result.code, result.error));
            }
            return res.status(202).json(apiEnvelope({ devices: result.devices }));
        } catch (error) {
            logger.error(`[home-ai] virtual state failed ${error?.message || error}`);
            return res.status(500).json(apiError("HOME_AI_VIRTUAL_DEVICE_WRITE_FAILED", "virtual device write failed"));
        }
    });

    router.get("/api/home-ai/v1/virtual-devices", async (req, res) => {
        try {
            return res.json(apiEnvelope({ devices: await readVirtualDevices(dbAll) }));
        } catch (error) {
            logger.error(`[home-ai] list virtual devices failed ${error?.message || error}`);
            return res.status(500).json(apiError("HOME_AI_VIRTUAL_DEVICE_LIST_FAILED", "virtual device list failed"));
        }
    });

    router.get("/api/home-ai/v1/overrides", async (req, res) => {
        try {
            return res.json(apiEnvelope({ overrides: await listUserOverrides(dbAll, req.query) }));
        } catch (error) {
            logger.error(`[home-ai] override list failed ${error?.message || error}`);
            return res.status(500).json(apiError("HOME_AI_OVERRIDE_LIST_FAILED", "override list failed"));
        }
    });

    router.post("/api/home-ai/v1/overrides", async (req, res) => {
        try {
            const result = await writeUserOverride(dbRun, req.body, dbAll);
            if (!result.ok) {
                return res.status(400).json(apiError(result.code, result.error));
            }
            return res.status(201).json(apiEnvelope(result.override));
        } catch (error) {
            logger.error(`[home-ai] override write failed ${error?.message || error}`);
            return res.status(500).json(apiError("HOME_AI_OVERRIDE_WRITE_FAILED", "override write failed"));
        }
    });

    router.delete("/api/home-ai/v1/overrides/:override_id", async (req, res) => {
        try {
            const result = await deleteUserOverride(dbRun, req.params.override_id);
            if (!result.ok) {
                return res.status(result.code === "HOME_AI_OVERRIDE_NOT_FOUND" ? 404 : 400)
                    .json(apiError(result.code, result.error));
            }
            return res.json(apiEnvelope(result.removed));
        } catch (error) {
            logger.error(`[home-ai] override delete failed ${error?.message || error}`);
            return res.status(500).json(apiError("HOME_AI_OVERRIDE_DELETE_FAILED", "override delete failed"));
        }
    });

    router.post("/api/home-ai/v1/feedback", async (req, res) => {
        try {
            const result = await recordFeedback(dbRun, req.body, dbAll);
            if (!result.ok) {
                return res.status(400).json(apiError(result.code, result.error));
            }
            return res.status(201).json(apiEnvelope({ ...result.feedback, learning: result.learning }));
        } catch (error) {
            logger.error(`[home-ai] feedback failed ${error?.message || error}`);
            return res.status(500).json(apiError("HOME_AI_FEEDBACK_WRITE_FAILED", "feedback write failed"));
        }
    });

    router.get("/api/home-ai/v1/feedback", async (req, res) => {
        try {
            return res.json(apiEnvelope({ feedback: await listFeedback(dbAll, req.query) }));
        } catch (error) {
            logger.error(`[home-ai] feedback list failed ${error?.message || error}`);
            return res.status(500).json(apiError("HOME_AI_FEEDBACK_READ_FAILED", "feedback read failed"));
        }
    });

    router.get("/api/home-ai/v1/memory-candidates", async (req, res) => {
        try {
            return res.json(apiEnvelope({ candidates: await listMemoryCandidates(dbAll, req.query) }));
        } catch (error) {
            logger.error(`[home-ai] memory list failed ${error?.message || error}`);
            return res.status(500).json(apiError("HOME_AI_MEMORY_READ_FAILED", "memory candidates read failed"));
        }
    });

    router.post("/api/home-ai/v1/memory-candidates", async (req, res) => {
        try {
            const result = await createMemoryCandidate(dbRun, req.body);
            if (!result.ok) return res.status(400).json(apiError(result.code, result.error));
            return res.status(201).json(apiEnvelope(result.candidate));
        } catch (error) {
            logger.error(`[home-ai] memory create failed ${error?.message || error}`);
            return res.status(500).json(apiError("HOME_AI_MEMORY_WRITE_FAILED", "memory candidate write failed"));
        }
    });

    router.patch("/api/home-ai/v1/memory-candidates/:candidate_id", async (req, res) => {
        try {
            const result = await updateMemoryCandidate(dbRun, dbAll, req.params.candidate_id, req.body);
            if (!result.ok) return res.status(404).json(apiError(result.code, result.error));
            return res.json(apiEnvelope(result.candidate));
        } catch (error) {
            logger.error(`[home-ai] memory update failed ${error?.message || error}`);
            return res.status(500).json(apiError("HOME_AI_MEMORY_UPDATE_FAILED", "memory candidate update failed"));
        }
    });

    router.delete("/api/home-ai/v1/memory-candidates/:candidate_id", async (req, res) => {
        try {
            const result = await deleteMemoryCandidate(dbRun, req.params.candidate_id);
            if (!result.ok) return res.status(404).json(apiError(result.code, result.error));
            return res.json(apiEnvelope(result));
        } catch (error) {
            logger.error(`[home-ai] memory delete failed ${error?.message || error}`);
            return res.status(500).json(apiError("HOME_AI_MEMORY_DELETE_FAILED", "memory candidate delete failed"));
        }
    });

    router.get("/api/home-ai/v1/habits", async (req, res) => {
        try {
            return res.json(apiEnvelope({ habits: await listHabits(dbAll, req.query) }));
        } catch (error) {
            logger.error(`[home-ai] habits failed ${error?.message || error}`);
            return res.status(500).json(apiError("HOME_AI_HABIT_READ_FAILED", "habits read failed"));
        }
    });

    router.get("/api/home-ai/v1/rule-candidates", async (req, res) => {
        try {
            return res.json(apiEnvelope({ candidates: await listRuleCandidates(dbAll, req.query) }));
        } catch (error) {
            logger.error(`[home-ai] rule candidate list failed ${error?.message || error}`);
            return res.status(500).json(apiError("HOME_AI_RULE_CANDIDATE_READ_FAILED", "rule candidates read failed"));
        }
    });

    router.post("/api/home-ai/v1/rule-candidates", async (req, res) => {
        try {
            const result = await createRuleCandidate(dbRun, req.body);
            if (!result.ok) return res.status(400).json(apiError(result.code, result.error));
            return res.status(201).json(apiEnvelope(result.candidate));
        } catch (error) {
            logger.error(`[home-ai] rule candidate create failed ${error?.message || error}`);
            return res.status(500).json(apiError("HOME_AI_RULE_CANDIDATE_WRITE_FAILED", "rule candidate write failed"));
        }
    });

    router.post("/api/home-ai/v1/rule-candidates/:candidate_id/evaluate", async (req, res) => {
        try {
            const request = req.body && typeof req.body === "object" && !Array.isArray(req.body)
                ? req.body
                : {};
            const result = await evaluateRuleCandidate(
                dbRun,
                dbAll,
                req.params.candidate_id,
                { auto_publish: request.auto_publish === true },
                async candidatePackage => {
                    const current = await readCurrentRulePackage(dbAll);
                    const nextVersion = Math.max(Number(candidatePackage.version) || 1,
                        (Number(current?.version) || 0) + 1);
                    const published = await publishRulePackage(dbRun, dbAll, {
                        ...candidatePackage,
                        version: nextVersion,
                        generated_at_ms: Date.now()
                    });
                    return published;
                }
            );
            if (!result.ok) return res.status(404).json(apiError(result.code, result.error));
            if (result.status === "PUBLISHED" && result.published) {
                result.probation_runs = await startProbationRuns(dbRun, result.published, {
                    duration_days: request.duration_days,
                    candidate_id: req.params.candidate_id,
                    rule_ids: result.target_rule_ids
                });
                result.status = await attachRuleCandidateProbation(dbRun, req.params.candidate_id, result.probation_runs);
            }
            return res.json(apiEnvelope(result));
        } catch (error) {
            logger.error(`[home-ai] rule candidate evaluate failed ${error?.message || error}`);
            return res.status(500).json(apiError("HOME_AI_RULE_CANDIDATE_EVALUATE_FAILED", "rule candidate evaluation failed"));
        }
    });

    router.get("/api/home-ai/v1/probation", async (req, res) => {
        try {
            return res.json(apiEnvelope({ runs: await listProbationRuns(dbAll, req.query) }));
        } catch (error) {
            logger.error(`[home-ai] probation list failed ${error?.message || error}`);
            return res.status(500).json(apiError("HOME_AI_PROBATION_READ_FAILED", "probation runs read failed"));
        }
    });

    router.post("/api/home-ai/v1/probation/:run_id/evaluate", async (req, res) => {
        try {
            const result = await evaluateProbationRun(
                dbRun,
                dbAll,
                req.params.run_id,
                {},
                rollbackBody => rollbackRulePackage(dbRun, dbAll, rollbackBody)
            );
            if (!result.ok) return res.status(404).json(apiError(result.code, result.error));
            return res.json(apiEnvelope(result));
        } catch (error) {
            logger.error(`[home-ai] probation evaluate failed ${error?.message || error}`);
            return res.status(500).json(apiError("HOME_AI_PROBATION_EVALUATE_FAILED", "probation evaluation failed"));
        }
    });

    router.get("/api/home-ai/v1/tools", (req, res) => {
        void req;
        return res.json(apiEnvelope({ tools: listTools() }));
    });

    router.post("/api/home-ai/v1/tools/:tool_name/execute", async (req, res) => {
        try {
            const result = await executeTool(req.params.tool_name, req.body || {}, {
                dbRun,
                dbAll,
                requestedBy: req.get("X-Requested-By") || "api"
            });
            return res.json(apiEnvelope(result));
        } catch (error) {
            const status = error instanceof HomeAiToolError ? error.status : 500;
            return res.status(status).json(apiError(error?.code || "TOOL_EXECUTION_FAILED", error?.message || "tool execution failed"));
        }
    });

    router.get("/api/home-ai/v1/tools/settings", async (req, res) => {
        void req;
        try {
            const homeLocation = await readSetting(dbAll, "home_location", null);
            const newsProvider = await readSetting(dbAll, "news_provider", null);
            const weatherContext = await readSetting(dbAll, "weather_context", null);
            return res.json(apiEnvelope({
                home_location: homeLocation,
                weather_context: weatherContext ? {
                    available: weatherContext.available === true,
                    dark: weatherContext.dark === true,
                    reason: weatherContext.reason || "",
                    observed_at_ms: weatherContext.observed_at_ms || null,
                    expires_at_ms: weatherContext.expires_at_ms || null
                } : null,
                news_provider: newsProvider ? {
                    endpoint: newsProvider.endpoint || "",
                    configured: Boolean(newsProvider.endpoint),
                    api_key_configured: Boolean(newsProvider.api_key)
                } : null
            }));
        } catch (error) {
            logger.error(`[home-ai] tool settings read failed ${error?.message || error}`);
            return res.status(500).json(apiError("TOOL_SETTINGS_READ_FAILED", "tool settings read failed"));
        }
    });

    router.put("/api/home-ai/v1/tools/settings/home-location", async (req, res) => {
        try {
            const normalized = normalizeHomeLocation(req.body);
            if (!normalized.ok) return res.status(400).json(apiError(normalized.code, normalized.error));
            const setting = await writeSetting(dbRun, "home_location", normalized.location);
            return res.json(apiEnvelope(setting));
        } catch (error) {
            logger.error(`[home-ai] location write failed ${error?.message || error}`);
            return res.status(500).json(apiError("HOME_LOCATION_WRITE_FAILED", "home location write failed"));
        }
    });

    router.put("/api/home-ai/v1/tools/settings/news", async (req, res) => {
        try {
            const endpoint = typeof req.body?.endpoint === "string" ? req.body.endpoint.trim().slice(0, 500) : "";
            if (endpoint) {
                const parsed = new URL(endpoint);
                if (!['https:'].includes(parsed.protocol)) {
                    return res.status(400).json(apiError("NEWS_PROVIDER_INVALID", "news endpoint must use HTTPS"));
                }
            }
            const current = await readSetting(dbAll, "news_provider", null);
            const submittedApiKey = typeof req.body?.api_key === "string" ? req.body.api_key.trim().slice(0, 300) : "";
            const apiKey = req.body?.clear_api_key === true ? "" : (submittedApiKey || current?.api_key || "");
            const setting = await writeSetting(dbRun, "news_provider", { endpoint, api_key: apiKey });
            return res.json(apiEnvelope({
                key: setting.key,
                value: { endpoint, configured: Boolean(endpoint), api_key_configured: Boolean(apiKey) },
                updated_at_ms: setting.updated_at_ms
            }));
        } catch (error) {
            if (error instanceof TypeError) {
                return res.status(400).json(apiError("NEWS_PROVIDER_INVALID", "news endpoint is invalid"));
            }
            logger.error(`[home-ai] news settings write failed ${error?.message || error}`);
            return res.status(500).json(apiError("NEWS_PROVIDER_WRITE_FAILED", "news provider write failed"));
        }
    });

    router.get("/api/home-ai/v1/data/retention", async (req, res) => {
        void req;
        try {
            return res.json(apiEnvelope({ policy: await readRetentionPolicy(dbAll) }));
        } catch (error) {
            logger.error(`[home-ai] retention policy read failed ${error?.message || error}`);
            return res.status(500).json(apiError("HOME_AI_RETENTION_READ_FAILED", "retention policy read failed"));
        }
    });

    router.put("/api/home-ai/v1/data/retention", async (req, res) => {
        try {
            const validation = validateRetentionPolicy(req.body || {});
            if (!validation.ok) return res.status(400).json(apiError(validation.code, validation.error));
            return res.json(apiEnvelope(await writeRetentionPolicy(dbRun, validation.policy)));
        } catch (error) {
            logger.error(`[home-ai] retention policy write failed ${error?.message || error}`);
            return res.status(500).json(apiError("HOME_AI_RETENTION_WRITE_FAILED", "retention policy write failed"));
        }
    });

    router.get("/api/home-ai/v1/data/maintenance", async (req, res) => {
        try {
            return res.json(apiEnvelope({ runs: await listMaintenanceRuns(dbAll, req.query) }));
        } catch (error) {
            logger.error(`[home-ai] maintenance history read failed ${error?.message || error}`);
            return res.status(500).json(apiError("HOME_AI_MAINTENANCE_READ_FAILED", "maintenance history read failed"));
        }
    });

    router.get("/api/home-ai/v1/agent/prompt-profiles", (req, res) => {
        void req;
        return res.json(apiEnvelope({ profiles: listPromptProfiles() }));
    });

    router.get("/api/home-ai/v1/agent/decisions", async (req, res) => {
        try {
            return res.json(apiEnvelope({ decisions: await listAgentDecisions(dbAll, req.query) }));
        } catch (error) {
            logger.error(`[home-ai] agent decision list failed ${error?.message || error}`);
            return res.status(500).json(apiError("HOME_AI_AGENT_DECISION_READ_FAILED", "agent decisions read failed"));
        }
    });

    router.get("/api/home-ai/v1/agent/decisions/:decision_id", async (req, res) => {
        try {
            const decision = await readAgentDecision(dbAll, req.params.decision_id);
            if (!decision) return res.status(404).json(apiError("HOME_AI_AGENT_DECISION_NOT_FOUND", "agent decision was not found"));
            return res.json(apiEnvelope(decision));
        } catch (error) {
            logger.error(`[home-ai] agent decision read failed ${error?.message || error}`);
            return res.status(500).json(apiError("HOME_AI_AGENT_DECISION_READ_FAILED", "agent decision read failed"));
        }
    });

    router.post("/api/home-ai/v1/agent/orchestrate", async (req, res) => {
        try {
            const result = await orchestrate(req.body || {}, {
                dbRun,
                dbAll,
                requestedBy: req.get("X-Requested-By") || "api"
            });
            if (!result.ok) return res.status(400).json(apiError(result.code, result.error));
            return res.status(result.decision.status === "PLANNED" ? 200 : 202).json(apiEnvelope(result.decision));
        } catch (error) {
            logger.error(`[home-ai] agent orchestration failed ${error?.message || error}`);
            return res.status(500).json(apiError("HOME_AI_AGENT_FAILED", "agent orchestration failed"));
        }
    });

    return router;
}

module.exports = {
    createHomeAiRouter
};
