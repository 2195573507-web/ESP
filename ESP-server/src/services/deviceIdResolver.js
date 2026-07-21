const DEVICE_ID_MAX_LENGTH = 128;

const DEVICE_ID_ALIASES = Object.freeze({
    c51: "sensair_shuttle_01",
    c52: "sensair_shuttle_02",
    s3: "sensair_s3_gateway_01"
});

const C5_DEVICE_IDS = Object.freeze([
    "sensair_shuttle_01",
    "sensair_shuttle_02"
]);

function normalizeDeviceId(value) {
    if (value === undefined || value === null) {
        return "";
    }

    return String(value).trim().slice(0, DEVICE_ID_MAX_LENGTH);
}

function resolveDeviceId(value) {
    const normalized = normalizeDeviceId(value);
    if (!normalized) {
        return "";
    }

    return DEVICE_ID_ALIASES[normalized.toLowerCase()] || normalized;
}

function isSupportedC5DeviceId(value) {
    return C5_DEVICE_IDS.includes(resolveDeviceId(value));
}

module.exports = {
    DEVICE_ID_ALIASES,
    DEVICE_ID_MAX_LENGTH,
    C5_DEVICE_IDS,
    isSupportedC5DeviceId,
    normalizeDeviceId,
    resolveDeviceId
};
