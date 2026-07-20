function apiEnvelope(data = null, options = {}) {
    const serverTimeMs = Number.isFinite(options.serverTimeMs)
        ? Math.trunc(options.serverTimeMs)
        : Date.now();

    return {
        ok: options.ok !== false,
        server_time_ms: serverTimeMs,
        data,
        error: null
    };
}

function apiError(code, message, options = {}) {
    const serverTimeMs = Number.isFinite(options.serverTimeMs)
        ? Math.trunc(options.serverTimeMs)
        : Date.now();

    return {
        ok: false,
        server_time_ms: serverTimeMs,
        data: null,
        error: {
            code: String(code || "REQUEST_FAILED"),
            message: String(message || "Request failed")
        }
    };
}

module.exports = {
    apiEnvelope,
    apiError
};
