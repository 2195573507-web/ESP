require("dotenv").config();

const express = require("express");
const path = require("path");
const {
    createTimeSyncRouter
} = require("./server-time-sync/timeSync");
const {
    createDatabase,
    createDbHelpers
} = require("./src/db/sqlite");
const {
    ensureRecordTables
} = require("./src/db/records");
const {
    ensureSensorTimingColumns
} = require("./src/db/sensorRecords");
const {
    ensureDeviceStatusTables
} = require("./src/db/deviceStatus");
const {
    ensureGatewayAuthTables
} = require("./src/db/gatewayAuth");
const {
    ensureDashboardSnapshotTables
} = require("./src/db/dashboardSnapshots");
const {
    ensureCsiMotionTables
} = require("./src/db/csiMotion");
const {
    ensureEventLogTables
} = require("./src/db/eventLogs");
const {
    ensureCommandTables
} = require("./src/db/commands");
const {
    ensureSmartHomeTables
} = require("./src/db/smartHome");
const {
    ensureAgentStateTables
} = require("./src/db/agentState");
const {
    ensureMemoryTables
} = require("./src/db/memory");
const {
    ensureUserDataDeletionTables
} = require("./src/db/userDataDeletion");
const {
    ensureVoiceTurnsTable
} = require("./src/db/voiceTurns");
const {
    ensureHomeLocationTables
} = require("./src/db/homeLocation");
const {
    ensureWeatherContextTables
} = require("./src/db/weatherContext");
const {
    ensureHabitRulesTables
} = require("./src/db/habitRules");
const {
    ensureHabitEventsTables
} = require("./src/db/habitEvents");
const {
    createCommandRouter
} = require("./src/routes/commandRoutes");
const {
    createDeviceRouter
} = require("./src/routes/deviceRoutes");
const {
    createDashboardRouter
} = require("./src/routes/dashboardRoutes");
const {
    createEventRouter
} = require("./src/routes/eventRoutes");
const {
    createSmartHomeRouter
} = require("./src/routes/smartHomeRoutes");
const {
    createAgentStateRouter
} = require("./src/routes/agentStateRoutes");
const {
    createLlmTextRouter
} = require("./src/routes/llmTextRoutes");
const {
    createMemoryRouter
} = require("./src/routes/memoryRoutes");
const {
    createRecordRouter
} = require("./src/routes/recordRoutes");
const {
    createSensorRouter
} = require("./src/routes/sensorRoutes");
const {
    createStructuredLlmRouter
} = require("./src/routes/structuredLlmRoutes");
const {
    createUserDataRouter
} = require("./src/routes/userDataRoutes");
const {
    createSettingsRouter
} = require("./src/routes/settingsRoutes");
const {
    createHabitRulesRouter
} = require("./src/routes/habitRulesRoutes");
const {
    createHabitEventsRouter
} = require("./src/routes/habitEventsRoutes");
const {
    createWeatherContextRouter
} = require("./src/routes/weatherContextRoutes");
const {
    ensureDefaultHabitRules
} = require("./src/services/habitRulesService");
const {
    readWeatherConfig
} = require("./src/agent/weatherQuery");
const {
    createVoiceBodyParserErrorHandler,
    createVoiceRouter
} = require("./src/routes/voiceRoutes");
const {
    recordEvent
} = require("./src/services/eventLogService");

const app = express();

// 数据库连接
const db = createDatabase(__dirname);
const { dbRun, dbAll } = createDbHelpers(db);

app.use(createVoiceRouter({ dbRun, dbAll }));
app.use(express.json());
app.use(createVoiceBodyParserErrorHandler({ dbRun }));
app.use((err, req, res, next) => {
    if (err instanceof SyntaxError && err.status === 400 && "body" in err) {
        return res.status(400).json({
            ok: false,
            error: "Invalid JSON body"
        });
    }

    return next(err);
});

// Static frontend routes. Backend work may read these routes but must not edit public/.
app.use(express.static(path.join(__dirname, "public")));

app.get("/", (req, res) => {
    res.redirect("/dashboard");
});

app.get("/dashboard", (req, res) => {
    res.sendFile(path.join(__dirname, "public", "index.html"));
});

app.use(createLlmTextRouter({ dbRun, dbAll }));
app.use(createStructuredLlmRouter({ dbRun, dbAll }));
app.use(createCommandRouter({ dbRun, dbAll }));
app.use(createDeviceRouter({ dbRun, dbAll }));
app.use("/api/dashboard/v1", createDashboardRouter({ dbRun, dbAll }));
app.use(createSmartHomeRouter({ dbRun, dbAll }));
app.use(createEventRouter({ dbRun, dbAll }));
app.use(createMemoryRouter({ dbRun, dbAll }));
app.use(createAgentStateRouter({ dbRun, dbAll }));
app.use(createUserDataRouter({ dbRun, dbAll }));
app.use(createSettingsRouter({ dbRun, dbAll }));
app.use(createHabitRulesRouter({ dbRun, dbAll }));
app.use(createHabitEventsRouter({ dbRun, dbAll }));
app.use(createWeatherContextRouter({ dbRun, dbAll }));
app.use(createRecordRouter({ db }));
app.use(createSensorRouter({ db, dbRun, dbAll }));

// Health/debug API
app.use("/api/time", createTimeSyncRouter({ dbRun, dbAll }));

function isMachineApiPath(pathname) {
    return pathname === "/api" ||
        pathname.startsWith("/api/") ||
        pathname === "/sensor" ||
        pathname.startsWith("/sensor/") ||
        pathname === "/asr" ||
        pathname.startsWith("/asr/") ||
        pathname === "/llm" ||
        pathname.startsWith("/llm/");
}

function getDashboardHashPage(pathname) {
    let decodedPathname;
    try {
        decodedPathname = decodeURIComponent(pathname);
    } catch (_) {
        return null;
    }

    const page = decodedPathname.replace(/^\/#?/, "");
    return ["s3", "c51", "c52", "settings", "habit-rules"].includes(page)
        ? page
        : null;
}

app.use((req, res, next) => {
    if (!isMachineApiPath(req.path)) {
        return next();
    }

    return res.status(404).json({
        ok: false,
        error: "Not found"
    });
});

// The dashboard is hash-routed. Normalize direct or proxy-encoded hash paths to
// the canonical fragment URL, then serve the shell for other SPA paths.
app.use((req, res, next) => {
    if (req.method !== "GET" && req.method !== "HEAD") {
        return next();
    }

    const page = getDashboardHashPage(req.path);
    if (page) {
        return res.redirect(`/#${page}`);
    }

    return res.sendFile(path.join(__dirname, "public", "index.html"));
});

app.use((err, req, res, next) => {
    if (res.headersSent) {
        return next(err);
    }

    console.error("[server] unhandled route error", err);
    const status = Number.isInteger(err?.status) && err.status >= 400 && err.status < 600
        ? err.status
        : 500;

    return res.status(status).json({
        ok: false,
        error: status >= 500 ? "Internal server error" : (err?.message || "Request failed")
    });
});

const PORT = process.env.PORT || 3000;
let httpServer = null;
let shuttingDown = false;

function closeDatabase() {
    return new Promise(resolve => {
        db.close(error => {
            if (error) {
                console.error("[server] failed to close database", error);
            }

            resolve();
        });
    });
}

function closeHttpServer() {
    return new Promise(resolve => {
        if (!httpServer) {
            resolve();
            return;
        }

        httpServer.close(error => {
            if (error) {
                console.error("[server] failed to close http server", error);
            }

            resolve();
        });
    });
}

async function shutdown(signal) {
    if (shuttingDown) {
        return;
    }

    shuttingDown = true;
    console.log(`[server] shutting down signal=${signal}`);
    await closeHttpServer();
    await closeDatabase();
    process.exit(0);
}

async function startServer() {
    await ensureRecordTables(dbRun, dbAll);
    await ensureSensorTimingColumns(dbRun, dbAll);
    await ensureDeviceStatusTables(dbRun, dbAll);
    await ensureGatewayAuthTables(dbRun, dbAll);
    await ensureDashboardSnapshotTables(dbRun, dbAll);
    await ensureCsiMotionTables(dbRun, dbAll);
    await ensureEventLogTables(dbRun, dbAll);
    await ensureVoiceTurnsTable(dbRun, dbAll);
    await ensureCommandTables(dbRun, dbAll);
    await ensureSmartHomeTables(dbRun, dbAll);
    await ensureMemoryTables(dbRun, dbAll);
    await ensureAgentStateTables(dbRun, dbAll);
    await ensureUserDataDeletionTables(dbRun, dbAll);
    await ensureHomeLocationTables(dbRun, dbAll);
    await ensureWeatherContextTables(dbRun);
    await ensureHabitRulesTables(dbRun);
    await ensureHabitEventsTables(dbRun);
    await ensureDefaultHabitRules(dbRun, dbAll);
    readWeatherConfig(console);
    await recordEvent(dbRun, {
        event_type: "system",
        event_name: "system_log_created",
        severity: "info",
        message: "server started and database migrations ensured",
        payload: {
            event: "server_start",
            migrations: [
                "records",
                "sensor_timing",
                "device_status",
                "dashboard_snapshots",
                "csi_motion_events",
                "event_logs",
                "voice_turns",
                "commands",
                "smart_home",
                "memory",
                "agent_state",
                "user_data_deletion",
                "home_location",
                "weather_context",
                "habit_rules",
                "habit_events"
            ]
        },
        source: "server_startup",
        server_recv_ms: Date.now()
    });

    httpServer = app.listen(PORT, () => {
        console.log(`Server running on port ${PORT}`);
    });
}

process.on("SIGTERM", () => {
    shutdown("SIGTERM").catch(error => {
        console.error("[server] shutdown failed", error);
        process.exit(1);
    });
});

process.on("SIGINT", () => {
    shutdown("SIGINT").catch(error => {
        console.error("[server] shutdown failed", error);
        process.exit(1);
    });
});

startServer().catch(error => {
    console.error("[server] failed to start", error);
    process.exit(1);
});
