function readPositiveInteger(value, fallback) {
    const numeric = Number.parseInt(value, 10);
    if (!Number.isFinite(numeric) || numeric <= 0) {
        return fallback;
    }

    return numeric;
}

function readTrimmedEnv(name, fallback = "") {
    const value = process.env[name];
    if (typeof value !== "string") {
        return fallback;
    }

    const trimmed = value.trim();
    return trimmed || fallback;
}

function readFirstTrimmedEnv(names, fallback = "") {
    for (const name of names) {
        const value = readTrimmedEnv(name);
        if (value) {
            return value;
        }
    }

    return fallback;
}

function readBooleanFlag(value) {
    const normalized = String(value || "").trim().toLowerCase();
    return normalized === "1" || normalized === "true" || normalized === "yes" || normalized === "on";
}

function normalizeBaseUrl(value, fallback, expectedProtocols) {
    const rawValue = readTrimmedEnv(value, fallback).replace(/\/+$/, "");
    let parsed;
    try {
        parsed = new URL(rawValue);
    } catch (_) {
        return rawValue;
    }

    if (expectedProtocols.includes(parsed.protocol)) {
        return rawValue;
    }

    return rawValue;
}

function normalizeGatewayPathValue(value) {
    const pathValue = String(value || "").trim();
    if (!pathValue) {
        return "";
    }

    if (/^https?:\/\//i.test(pathValue) || /^wss?:\/\//i.test(pathValue)) {
        return pathValue;
    }

    return pathValue.startsWith("/") ? pathValue : `/${pathValue}`;
}

function readGatewayPathEnv(name, fallback) {
    return normalizeGatewayPathValue(readTrimmedEnv(name, fallback));
}

function readFirstGatewayPathEnv(names, fallback = "") {
    return normalizeGatewayPathValue(readFirstTrimmedEnv(names, fallback));
}

function buildGatewayHttpUrl(baseUrl, pathValue) {
    const normalizedPath = normalizeGatewayPathValue(pathValue);
    if (/^https?:\/\//i.test(normalizedPath) || /^wss?:\/\//i.test(normalizedPath)) {
        return normalizedPath;
    }

    return `${baseUrl.replace(/\/+$/, "")}${normalizedPath || "/"}`;
}

function buildGatewayRealtimeUrl(baseUrl, pathValue, model) {
    const normalizedPath = normalizeGatewayPathValue(pathValue);
    const rawUrl = /^wss?:\/\//i.test(normalizedPath) || /^https?:\/\//i.test(normalizedPath)
        ? normalizedPath.replace(/^http/i, "ws")
        : buildGatewayHttpUrl(baseUrl.replace(/^http/i, "ws"), normalizedPath);
    const url = new URL(rawUrl);
    url.searchParams.set("model", model);
    return url.toString();
}

function buildGatewayTtsUrl(wsBaseUrl, httpBaseUrl, pathValue, model) {
    if (/^https?:\/\//i.test(pathValue) || /^wss?:\/\//i.test(pathValue)) {
        const url = new URL(pathValue);
        if (url.protocol === "ws:" || url.protocol === "wss:") {
            url.searchParams.set("model", model);
        }
        return url.toString();
    }

    if (pathValue.toLowerCase().includes("realtime")) {
        return buildGatewayRealtimeUrl(wsBaseUrl, pathValue, model);
    }

    return buildGatewayHttpUrl(httpBaseUrl, pathValue);
}

module.exports = {
    buildGatewayHttpUrl,
    buildGatewayRealtimeUrl,
    buildGatewayTtsUrl,
    normalizeBaseUrl,
    normalizeGatewayPathValue,
    readBooleanFlag,
    readFirstGatewayPathEnv,
    readFirstTrimmedEnv,
    readGatewayPathEnv,
    readPositiveInteger,
    readTrimmedEnv
};
