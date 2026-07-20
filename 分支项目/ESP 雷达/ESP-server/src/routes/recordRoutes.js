const express = require("express");

const LEGACY_RECORD_TEXT_MAX_LENGTH = 4000;

function normalizeLegacyRecordText(value) {
    if (value === undefined || value === null) {
        return "";
    }

    return String(value).trim().slice(0, LEGACY_RECORD_TEXT_MAX_LENGTH);
}

function createRecordRouter(options) {
    const router = express.Router();
    const db = options.db;

    function sendRecordDbError(res, err, includeSuccess = false) {
        return res.status(500).json({
            ok: false,
            ...(includeSuccess ? { success: false } : {}),
            error: err.message
        });
    }

    router.post("/asr", (req, res) => {
        const text = normalizeLegacyRecordText(req.body?.text);

        db.run(
            "INSERT INTO asr_records(timestamp,text) VALUES(?,?)",
            [Date.now(), text],
            function (err) {
                if (err) {
                    return sendRecordDbError(res, err, true);
                }

                res.json({
                    ok: true,
                    success: true,
                    id: this.lastID
                });
            }
        );
    });

    router.post("/llm", (req, res) => {
        const prompt = normalizeLegacyRecordText(req.body?.prompt);
        const response = normalizeLegacyRecordText(req.body?.response);

        db.run(
            "INSERT INTO llm_records(timestamp,prompt,response) VALUES(?,?,?)",
            [Date.now(), prompt, response],
            function (err) {
                if (err) {
                    return sendRecordDbError(res, err, true);
                }

                res.json({
                    ok: true,
                    success: true,
                    id: this.lastID
                });
            }
        );
    });

    router.get("/asr/latest", (req, res) => {
        db.get(
            "SELECT * FROM asr_records WHERE deleted_at IS NULL ORDER BY id DESC LIMIT 1",
            [],
            (err, row) => {
                if (err) {
                    return sendRecordDbError(res, err);
                }

                res.json(row || {});
            }
        );
    });

    router.get("/llm/latest", (req, res) => {
        db.get(
            "SELECT * FROM llm_records WHERE deleted_at IS NULL ORDER BY id DESC LIMIT 1",
            [],
            (err, row) => {
                if (err) {
                    return sendRecordDbError(res, err);
                }

                res.json(row || {});
            }
        );
    });

    return router;
}

module.exports = {
    createRecordRouter
};
