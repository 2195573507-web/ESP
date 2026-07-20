const {
    createToolRegistry
} = require("./toolRegistry");
const {
    deviceStatusQuery,
    homeStateQuery,
    sensorQuery,
    DEVICE_ID_PARAMETERS,
    EMPTY_PARAMETERS
} = require("./homeTools");
const {
    weatherQuery
} = require("./weatherQuery");

function createDefaultToolRegistry() {
    return createToolRegistry([
        {
            name: "weather_query",
            description: "Use only for an explicit refresh request, a non-home location, or a complex weather question not answerable from fresh weather_context.",
            parameters: {
                type: "object",
                properties: {
                    location: {
                        type: "string",
                        description: "Optional city, district, or place. Omit to use the configured home location."
                    }
                },
                additionalProperties: false
            },
            handler: weatherQuery
        },
        {
            name: "home_state_query",
            description: "Get the current home and room state reported to ESP-server. It does not infer occupancy from online devices.",
            parameters: EMPTY_PARAMETERS,
            handler: homeStateQuery
        },
        {
            name: "sensor_query",
            description: "Get fresh BME690 environment and air-quality data from ESP-server.",
            parameters: DEVICE_ID_PARAMETERS,
            handler: sensorQuery
        },
        {
            name: "device_status_query",
            description: "Get current gateway, sub-device, and module online status from ESP-server.",
            parameters: DEVICE_ID_PARAMETERS,
            handler: deviceStatusQuery
        }
    ]);
}

module.exports = {
    createDefaultToolRegistry
};
