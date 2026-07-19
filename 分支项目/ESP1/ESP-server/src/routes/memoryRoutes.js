const express = require("express");
const {
    runDailySummaryJob,
    runWeeklyProfileJob
} = require("../jobs/memoryJobs");
const {
    applyMemoryCorrection,
    createConversationTurn,
    createDailyMemory,
    listConversationTurns,
    listDailyMemory,
    listMemoryJobRuns,
    listProfiles,
    upsertProfile
} = require("../memory/store");
const {
    isIsoDateString
} = require("../utils/date");

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

const MEMORY_DEVICE_ID_MAX_LENGTH = 128;
const PROFILE_STATUSES = new Set(["candidate", "active", "rejected", "archived"]);
const PROFILE_CATEGORY_MAX_LENGTH = 80;

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

function readConversationFilters(res, query) {
    const deviceFilter = readOptionalStringFilter(res, query, "device_id", MEMORY_DEVICE_ID_MAX_LENGTH, "DEVICE_ID_INVALID");
    if (!deviceFilter.ok) {
        return deviceFilter;
    }

    return readOptionalStringFilter(res, deviceFilter.query, "session_id", 128, "SESSION_ID_INVALID");
}

function readDailyMemoryFilters(res, query) {
    const rawDate = typeof query?.memory_date === "string" ? query.memory_date : query?.date;
    if (typeof rawDate !== "string") {
        return {
            ok: true,
            query
        };
    }

    const memoryDate = rawDate.trim();
    if (!memoryDate) {
        return {
            ok: true,
            query: {
                ...query,
                memory_date: "",
                date: ""
            }
        };
    }

    if (!isIsoDateString(memoryDate)) {
        res.status(400).json({
            ok: false,
            code: "DAILY_MEMORY_DATE_INVALID",
            error: "memory_date must use YYYY-MM-DD format"
        });
        return {
            ok: false
        };
    }

    return {
        ok: true,
        query: {
            ...query,
            memory_date: memoryDate,
            date: memoryDate
        }
    };
}

function readProfileFilters(res, query) {
    const statusFilter = readOptionalEnumFilter(res, query, "status", PROFILE_STATUSES, "PROFILE_STATUS_INVALID");
    if (!statusFilter.ok) {
        return statusFilter;
    }

    return readOptionalStringFilter(
        res,
        statusFilter.query,
        "category",
        PROFILE_CATEGORY_MAX_LENGTH,
        "PROFILE_CATEGORY_INVALID"
    );
}

function readMemoryJobFilters(res, query) {
    const jobNameFilter = readOptionalStringFilter(res, query, "job_name", 80, "MEMORY_JOB_NAME_INVALID");
    if (!jobNameFilter.ok) {
        return jobNameFilter;
    }

    const rawTargetDate = jobNameFilter.query?.target_date;
    if (typeof rawTargetDate !== "string") {
        return {
            ok: true,
            query: jobNameFilter.query
        };
    }

    const targetDate = rawTargetDate.trim();
    if (!targetDate) {
        return {
            ok: true,
            query: {
                ...jobNameFilter.query,
                target_date: ""
            }
        };
    }

    if (!isIsoDateString(targetDate)) {
        res.status(400).json({
            ok: false,
            code: "MEMORY_JOB_TARGET_DATE_INVALID",
            error: "target_date must use YYYY-MM-DD format"
        });
        return {
            ok: false
        };
    }

    return {
        ok: true,
        query: {
            ...jobNameFilter.query,
            target_date: targetDate
        }
    };
}

function createMemoryRouter(options) {
    const router = express.Router();
    const dbRun = options.dbRun;
    const dbAll = options.dbAll;

    router.post("/api/conversation/turns", async (req, res) => {
        const result = await createConversationTurn(dbRun, req.body);
        return sendResult(res, result);
    });

    router.get("/api/conversation/turns", async (req, res) => {
        const filter = readConversationFilters(res, req.query);
        if (!filter.ok) {
            return;
        }

        const turns = await listConversationTurns(dbAll, filter.query);
        return res.json({
            ok: true,
            turns
        });
    });

    router.post("/api/memory/daily", async (req, res) => {
        const result = await createDailyMemory(dbRun, req.body);
        return sendResult(res, result);
    });

    router.get("/api/memory/daily", async (req, res) => {
        const filter = readDailyMemoryFilters(res, req.query);
        if (!filter.ok) {
            return;
        }

        const memories = await listDailyMemory(dbAll, filter.query);
        return res.json({
            ok: true,
            memories
        });
    });

    router.post("/api/memory/profile", async (req, res) => {
        const result = await upsertProfile(dbRun, req.body);
        return sendResult(res, result, 200);
    });

    router.get("/api/memory/profile", async (req, res) => {
        const filter = readProfileFilters(res, req.query);
        if (!filter.ok) {
            return;
        }

        const profiles = await listProfiles(dbAll, filter.query);
        return res.json({
            ok: true,
            profiles
        });
    });

    router.post("/api/memory/corrections", async (req, res) => {
        const result = await applyMemoryCorrection(dbRun, req.body);
        return sendResult(res, result);
    });

    router.post("/api/jobs/daily-summary/run", async (req, res) => {
        const result = await runDailySummaryJob(dbRun, dbAll, req.body);
        return sendResult(res, result, 202);
    });

    router.post("/api/jobs/weekly-profile/run", async (req, res) => {
        const result = await runWeeklyProfileJob(dbRun, dbAll, req.body);
        return sendResult(res, result, 202);
    });

    router.get("/api/jobs/memory", async (req, res) => {
        const filter = readMemoryJobFilters(res, req.query);
        if (!filter.ok) {
            return;
        }

        const jobs = await listMemoryJobRuns(dbAll, filter.query);
        return res.json({
            ok: true,
            jobs
        });
    });

    return router;
}

module.exports = {
    createMemoryRouter
};
