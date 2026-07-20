function buildVolcGatewayHeaders(config, purpose) {
    const headers = {
        Authorization: `Bearer ${config.apiKey}`
    };

    if (purpose === "asr" && config.asr.useResourceId && config.asr.resourceId) {
        headers["X-Api-Resource-Id"] = config.asr.resourceId;
    }

    if (purpose === "tts" && config.tts.useResourceId && config.tts.resourceId) {
        headers["X-Api-Resource-Id"] = config.tts.resourceId;
    }

    return headers;
}

module.exports = {
    buildVolcGatewayHeaders
};
