const {
    getDeviceContext
} = require("../services/deviceContextService");
const {
    readDeviceStatuses,
    readModuleStatuses
} = require("../services/deviceStatusService");

const EMPTY_PARAMETERS = {
    type: "object",
    properties: {},
    additionalProperties: false
};

const DEVICE_ID_PARAMETERS = {
    type: "object",
    properties: {
        device_id: {
            type: "string",
            description: "Optional ESP-server device identifier. Omit for the most recent device."
        }
    },
    additionalProperties: false
};

function safeDeviceId(value) {
    return typeof value === "string" ? value.trim().slice(0, 128) : "";
}

async function homeStateQuery(args, context) {
    const devices = await readDeviceStatuses(context.dbAll, {}, Date.now());
    if (devices.length === 0) {
        return { success: false, error: "no device state is available" };
    }

    return {
        success: true,
        source: "ESP-server device_status",
        devices: devices.map(device => ({
            device_id: device.device_id,
            room_id: device.room_id || "",
            room_name: device.room_name || "",
            online: Boolean(device.online),
            last_seen_age_ms: device.last_seen_age_ms ?? null
        })),
        note: "Occupancy is unavailable unless a device reports it explicitly; online status is not proof that a person is present."
    };
}

async function sensorQuery(args, context) {
    const deviceId = safeDeviceId(args?.device_id || context.deviceId);
    const deviceContext = await getDeviceContext(context.dbAll, deviceId);
    if (!deviceContext.environment.available) {
        return { success: false, error: "no sensor data is available" };
    }
    if (!deviceContext.environment.fresh) {
        return { success: false, error: "latest sensor data is stale", data: deviceContext };
    }

    return {
        success: true,
        source: "ESP-server sensor_records",
        data: deviceContext
    };
}

async function deviceStatusQuery(args, context) {
    const deviceId = safeDeviceId(args?.device_id || context.deviceId);
    const devices = await readDeviceStatuses(context.dbAll, { device_id: deviceId }, Date.now());
    if (devices.length === 0) {
        return { success: false, error: "no device status is available" };
    }

    const modules = await readModuleStatuses(context.dbAll, deviceId, Date.now());
    return {
        success: true,
        source: "ESP-server device_status",
        devices,
        modules
    };
}

module.exports = {
    DEVICE_ID_PARAMETERS,
    EMPTY_PARAMETERS,
    deviceStatusQuery,
    homeStateQuery,
    sensorQuery
};
