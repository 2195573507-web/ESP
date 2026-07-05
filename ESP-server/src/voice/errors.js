const {
    extractErrorMessageFromBody,
    maskUrlForLog,
    normalizeLogPreview
} = require("../utils/logging");

function createVoiceError(code, message, status) {
    const error = new Error(message || code);
    error.code = code;
    error.status = status;
    return error;
}

function createVoiceStageError(stage, code, message, status, details = {}) {
    const error = createVoiceError(code, message, status);
    error.stage = stage;
    Object.assign(error, details);
    return error;
}

function createUpstreamVoiceStageError(stage, code, response, responseBody, fallbackMessage) {
    const upstreamMessage = extractErrorMessageFromBody(responseBody);
    return createVoiceStageError(
        stage,
        code,
        upstreamMessage || fallbackMessage,
        502,
        {
            upstreamStatus: response.status,
            bodyLength: responseBody.length,
            bodyPreview: normalizeLogPreview(responseBody)
        }
    );
}

function describeVoiceError(error) {
    const parts = [
        `name=${error?.name || "Error"}`
    ];

    if (error?.stage) {
        parts.push(`stage=${error.stage}`);
    }

    if (error?.code) {
        parts.push(`code=${error.code}`);
    }

    if (typeof error?.status === "number") {
        parts.push(`status=${error.status}`);
    }

    if (typeof error?.upstreamStatus === "number") {
        parts.push(`upstream_status=${error.upstreamStatus}`);
    }

    if (typeof error?.bytes === "number") {
        parts.push(`bytes=${error.bytes}`);
    }

    if (typeof error?.bodyLength === "number") {
        parts.push(`body_len=${error.bodyLength}`);
    }

    if (error?.endpoint) {
        parts.push(`endpoint=${maskUrlForLog(error.endpoint)}`);
    }

    if (error?.model) {
        parts.push(`model=${error.model}`);
    }

    if (error?.bodyPreview) {
        parts.push(`body=${JSON.stringify(error.bodyPreview)}`);
    }

    return parts.join(" ");
}

module.exports = {
    createUpstreamVoiceStageError,
    createVoiceStageError,
    describeVoiceError
};
