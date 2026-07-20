const express = require("express");
const {
    executeUserDataDelete,
    getUserDataSummary,
    listDeletionRuns,
    previewUserDataDelete,
    requireUserDataAdmin
} = require("../services/userDataService");

function sendResult(res, result, status = 200) {
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

function createUserDataRouter(options) {
    const router = express.Router();
    const dbRun = options.dbRun;
    const dbAll = options.dbAll;

    router.get("/api/user-data/summary", requireUserDataAdmin, async (req, res) => {
        return sendResult(res, await getUserDataSummary(dbAll));
    });

    router.post("/api/user-data/delete/preview", requireUserDataAdmin, async (req, res) => {
        return sendResult(res, await previewUserDataDelete(dbRun, dbAll, req.body));
    });

    router.post("/api/user-data/delete", requireUserDataAdmin, async (req, res) => {
        return sendResult(res, await executeUserDataDelete(dbRun, dbAll, req.body), 200);
    });

    router.get("/api/user-data/deletion-runs", requireUserDataAdmin, async (req, res) => {
        return sendResult(res, await listDeletionRuns(dbAll, req.query));
    });

    router.get("/api/user-data/export", requireUserDataAdmin, async (req, res) => {
        return res.status(501).json({
            ok: false,
            code: "USER_DATA_EXPORT_RESERVED",
            error: "user data export is reserved for a future backend phase"
        });
    });

    return router;
}

module.exports = {
    createUserDataRouter
};
