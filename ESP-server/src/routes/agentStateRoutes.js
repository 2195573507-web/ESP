const express = require("express");
const {
    enqueueCommand
} = require("../commands/queue");
const {
    createCsiBehaviorEvent,
    createEmergencyEvent,
    createExperienceMemory,
    createRelationMemory,
    createReminderEvent,
    createReminderRule,
    listCsiBehaviorEvents,
    listEmergencyEvents,
    listEnvironmentProfiles,
    listExperienceMemory,
    listLcdStatus,
    listRelationMemory,
    listReminderEvents,
    listReminderRules,
    upsertEnvironmentProfile,
    upsertLcdStatus
} = require("../agent/stateStore");
const {
    recordEvent
} = require("../services/eventLogService");

function sendResult(res, result, status = 201) {
    if (!result.ok) {
        const statusCode = Number.isInteger(result.statusCode) &&
            result.statusCode >= 400 &&
            result.statusCode < 600
            ? result.statusCode
            : 400;

        const {
            statusCode: _statusCode,
            ...body
        } = result;

        return res.status(statusCode).json(body);
    }

    return res.status(status).json(result);
}

const AGENT_DEVICE_ID_MAX_LENGTH = 128;
const PROFILE_STATUSES = new Set(["candidate", "active", "rejected", "archived"]);
const REMINDER_RULE_STATUSES = new Set(["active", "paused", "archived"]);
const REMINDER_EVENT_STATUSES = new Set(["pending", "triggered", "confirmed", "canceled", "suppressed"]);
const EMERGENCY_STATUSES = new Set(["received", "llm_pending", "forwarded", "resolved", "archived"]);
const CSI_BEHAVIOR_TYPE_MAX_LENGTH = 120;
const RELATION_TYPE_MAX_LENGTH = 80;

function readOptionalEnumFilter(res, query, field, allowed, code) {
    const value = query?.[field];
    if (typeof value !== "string") {
        return {
            ok: true,
            query
        };
    }

    const text = value.trim();
    if (!text) {
        return {
            ok: true,
            query: {
                ...query,
                [field]: ""
            }
        };
    }

    if (!allowed.has(text)) {
        res.status(400).json({
            ok: false,
            code,
            error: `${field} must be one of ${Array.from(allowed).join(", ")}`
        });
        return {
            ok: false
        };
    }

    return {
        ok: true,
        query: {
            ...query,
            [field]: text
        }
    };
}

function readOptionalDeviceIdFilter(res, query) {
    const value = query?.device_id;
    if (typeof value !== "string") {
        return {
            ok: true,
            query
        };
    }

    const deviceId = value.trim();
    if (!deviceId) {
        return {
            ok: true,
            query: {
                ...query,
                device_id: ""
            }
        };
    }

    if (deviceId.length > AGENT_DEVICE_ID_MAX_LENGTH) {
        res.status(400).json({
            ok: false,
            code: "DEVICE_ID_INVALID",
            error: `device_id must be <= ${AGENT_DEVICE_ID_MAX_LENGTH} characters`
        });
        return {
            ok: false
        };
    }

    return {
        ok: true,
        query: {
            ...query,
            device_id: deviceId
        }
    };
}

function readOptionalStringFilter(res, query, field, maxLength, code) {
    const value = query?.[field];
    if (typeof value !== "string") {
        return {
            ok: true,
            query
        };
    }

    const text = value.trim();
    if (!text) {
        return {
            ok: true,
            query: {
                ...query,
                [field]: ""
            }
        };
    }

    if (text.length > maxLength) {
        res.status(400).json({
            ok: false,
            code,
            error: `${field} must be <= ${maxLength} characters`
        });
        return {
            ok: false
        };
    }

    return {
        ok: true,
        query: {
            ...query,
            [field]: text
        }
    };
}

function readEnvironmentProfileFilters(res, query) {
    const deviceFilter = readOptionalDeviceIdFilter(res, query);
    if (!deviceFilter.ok) {
        return deviceFilter;
    }

    return readOptionalEnumFilter(res, deviceFilter.query, "status", PROFILE_STATUSES, "PROFILE_STATUS_INVALID");
}

function readProfileStatusFilter(res, query) {
    return readOptionalEnumFilter(res, query, "status", PROFILE_STATUSES, "MEMORY_STATUS_INVALID");
}

function readRelationMemoryFilters(res, query) {
    const statusFilter = readProfileStatusFilter(res, query);
    if (!statusFilter.ok) {
        return statusFilter;
    }

    return readOptionalStringFilter(
        res,
        statusFilter.query,
        "relation_type",
        RELATION_TYPE_MAX_LENGTH,
        "RELATION_TYPE_INVALID"
    );
}

function readReminderRuleFilters(res, query) {
    return readOptionalEnumFilter(res, query, "status", REMINDER_RULE_STATUSES, "REMINDER_RULE_STATUS_INVALID");
}

function readReminderEventFilters(res, query) {
    return readOptionalEnumFilter(res, query, "status", REMINDER_EVENT_STATUSES, "REMINDER_EVENT_STATUS_INVALID");
}

function readEmergencyEventFilters(res, query) {
    const deviceFilter = readOptionalDeviceIdFilter(res, query);
    if (!deviceFilter.ok) {
        return deviceFilter;
    }

    return readOptionalEnumFilter(res, deviceFilter.query, "status", EMERGENCY_STATUSES, "EMERGENCY_STATUS_INVALID");
}

function readCsiBehaviorFilters(res, query) {
    const deviceFilter = readOptionalDeviceIdFilter(res, query);
    if (!deviceFilter.ok) {
        return deviceFilter;
    }

    return readOptionalStringFilter(
        res,
        deviceFilter.query,
        "behavior_type",
        CSI_BEHAVIOR_TYPE_MAX_LENGTH,
        "CSI_BEHAVIOR_TYPE_INVALID"
    );
}

function createAgentStateRouter(options) {
    const router = express.Router();
    const dbRun = options.dbRun;
    const dbAll = options.dbAll;

    router.post("/api/environment/profile", async (req, res) => {
        return sendResult(res, await upsertEnvironmentProfile(dbRun, req.body), 200);
    });

    router.get("/api/environment/profile", async (req, res) => {
        const filter = readEnvironmentProfileFilters(res, req.query);
        if (!filter.ok) {
            return;
        }

        res.json({
            ok: true,
            profiles: await listEnvironmentProfiles(dbAll, filter.query)
        });
    });

    router.post("/api/memory/experience", async (req, res) => {
        return sendResult(res, await createExperienceMemory(dbRun, req.body));
    });

    router.get("/api/memory/experience", async (req, res) => {
        const filter = readProfileStatusFilter(res, req.query);
        if (!filter.ok) {
            return;
        }

        res.json({
            ok: true,
            memories: await listExperienceMemory(dbAll, filter.query)
        });
    });

    router.post("/api/memory/relation", async (req, res) => {
        return sendResult(res, await createRelationMemory(dbRun, req.body));
    });

    router.get("/api/memory/relation", async (req, res) => {
        const filter = readRelationMemoryFilters(res, req.query);
        if (!filter.ok) {
            return;
        }

        res.json({
            ok: true,
            memories: await listRelationMemory(dbAll, filter.query)
        });
    });

    router.post("/api/reminders/rules", async (req, res) => {
        return sendResult(res, await createReminderRule(dbRun, req.body));
    });

    router.get("/api/reminders/rules", async (req, res) => {
        const filter = readReminderRuleFilters(res, req.query);
        if (!filter.ok) {
            return;
        }

        res.json({
            ok: true,
            rules: await listReminderRules(dbAll, filter.query)
        });
    });

    router.post("/api/reminders/events", async (req, res) => {
        return sendResult(res, await createReminderEvent(dbRun, req.body));
    });

    router.get("/api/reminders/events", async (req, res) => {
        const filter = readReminderEventFilters(res, req.query);
        if (!filter.ok) {
            return;
        }

        res.json({
            ok: true,
            events: await listReminderEvents(dbAll, filter.query)
        });
    });

    router.post("/api/emergency/events", async (req, res) => {
        const result = await createEmergencyEvent(dbRun, req.body);
        if (result.ok) {
            try {
                await recordEvent(dbRun, {
                    event_type: "alarm",
                    event_name: "alarm_created",
                    event_id: result.event_id,
                    device_id: req.body?.device_id || "",
                    severity: req.body?.severity || "warning",
                    message: req.body?.event_type || "emergency event",
                    payload: req.body || {},
                    source: "emergency_events",
                    server_recv_ms: Date.now()
                });
            } catch (_) {
                // Emergency event has already been persisted in its source table.
            }
        }
        return sendResult(res, result);
    });

    router.get("/api/emergency/events", async (req, res) => {
        const filter = readEmergencyEventFilters(res, req.query);
        if (!filter.ok) {
            return;
        }

        res.json({
            ok: true,
            events: await listEmergencyEvents(dbAll, filter.query)
        });
    });

    router.post("/api/csi/behavior", async (req, res) => {
        const result = await createCsiBehaviorEvent(dbRun, req.body);
        if (result.ok) {
            try {
                await recordEvent(dbRun, {
                    event_type: "csi",
                    event_name: "system_log_created",
                    event_id: result.event_id,
                    device_id: req.body?.device_id || "",
                    severity: "info",
                    message: req.body?.behavior_type || "csi behavior",
                    payload: req.body || {},
                    source: "csi_behavior",
                    server_recv_ms: Date.now()
                });
            } catch (_) {
                // CSI source row is already persisted.
            }
        }
        return sendResult(res, result);
    });

    router.get("/api/csi/behavior", async (req, res) => {
        const filter = readCsiBehaviorFilters(res, req.query);
        if (!filter.ok) {
            return;
        }

        res.json({
            ok: true,
            events: await listCsiBehaviorEvents(dbAll, filter.query)
        });
    });

    router.post("/api/lcd/status", async (req, res) => {
        return sendResult(res, await upsertLcdStatus(dbRun, req.body), 200);
    });

    router.get("/api/lcd/status", async (req, res) => {
        const filter = readOptionalDeviceIdFilter(res, req.query);
        if (!filter.ok) {
            return;
        }

        res.json({
            ok: true,
            devices: await listLcdStatus(dbAll, filter.query)
        });
    });

    router.post("/api/lcd/display", async (req, res) => {
        const result = await enqueueCommand(dbRun, dbAll, {
            name: "display.show_text",
            target_device_id: req.body?.device_id,
            payload: {
                text: req.body?.text,
                ttl_ms: req.body?.ttl_ms
            },
            reason: "lcd display request"
        }, {
            source: "lcd_api",
            requestedBy: "server"
        });

        if (!result.ok) {
            return res.status(400).json(result);
        }

        await upsertLcdStatus(dbRun, {
            device_id: result.command.device_id,
            page: "message",
            state: {
                text: result.command.payload.text,
                ttl_ms: result.command.payload.ttl_ms
            },
            last_command_id: result.command.command_id
        });

        return res.status(201).json({
            ok: true,
            command: result.command
        });
    });

    return router;
}

module.exports = {
    createAgentStateRouter
};
