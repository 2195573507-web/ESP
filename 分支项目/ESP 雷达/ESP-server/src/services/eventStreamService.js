const SSE_HEARTBEAT_MS = 15000;

const clients = new Set();

async function recordSseSystemEvent(dbRun, eventName, message) {
    if (typeof dbRun !== "function") {
        return;
    }

    try {
        const {
            recordEvent
        } = require("./eventLogService");
        await recordEvent(dbRun, {
            event_type: "system",
            event_name: "system_log_created",
            severity: "info",
            message,
            payload: {
                event: eventName,
                client_count: clients.size
            },
            source: "sse",
            server_recv_ms: Date.now()
        }, {
            broadcast: false
        });
    } catch (_) {
        // SSE must remain available even if log persistence is temporarily unavailable.
    }
}

function writeSse(res, eventName, data) {
    res.write(`event: ${eventName}\n`);
    res.write(`data: ${JSON.stringify(data)}\n\n`);
}

function subscribeSse(req, res, options = {}) {
    res.status(200).set({
        "Content-Type": "text/event-stream",
        "Cache-Control": "no-cache, no-transform",
        "Connection": "keep-alive",
        "X-Accel-Buffering": "no"
    });
    res.flushHeaders?.();

    const client = {
        res,
        heartbeat: setInterval(() => {
            writeSse(res, "ping", {
                server_time_ms: Date.now()
            });
        }, SSE_HEARTBEAT_MS)
    };

    clients.add(client);
    writeSse(res, "connected", {
        server_time_ms: Date.now()
    });
    recordSseSystemEvent(options.dbRun, "sse_client_connect", "SSE client connected");

    req.on("close", () => {
        clearInterval(client.heartbeat);
        clients.delete(client);
        recordSseSystemEvent(options.dbRun, "sse_client_disconnect", "SSE client disconnected");
    });
}

function broadcastEvent(eventName, payload) {
    const data = {
        server_time_ms: Date.now(),
        event: eventName,
        data: payload || null
    };

    for (const client of clients) {
        try {
            writeSse(client.res, eventName, data);
        } catch (_) {
            clearInterval(client.heartbeat);
            clients.delete(client);
        }
    }
}

module.exports = {
    SSE_HEARTBEAT_MS,
    broadcastEvent,
    subscribeSse
};
