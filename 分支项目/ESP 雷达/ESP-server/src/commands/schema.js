const COMMAND_DEVICE_ID_MAX_LENGTH = 128;
const COMMAND_REASON_MAX_LENGTH = 240;

const COMMAND_DEFINITIONS = [
    {
        name: "device.noop",
        description: "Connectivity test command with no device-side action.",
        payload: {}
    },
    {
        name: "voice.set_volume",
        description: "Request a bounded playback volume change.",
        payload: {
            volume: {
                type: "integer",
                min: 0,
                max: 100,
                required: true
            }
        }
    },
    {
        name: "sensor.set_upload_interval",
        description: "Request a sensor upload interval change in milliseconds.",
        payload: {
            interval_ms: {
                type: "integer",
                min: 1000,
                max: 3600000,
                required: true
            }
        }
    },
    {
        name: "display.show_text",
        description: "Reserved LCD/display protocol command; server queues it but does not implement LCD firmware.",
        payload: {
            text: {
                type: "string",
                maxLength: 120,
                required: true
            },
            ttl_ms: {
                type: "integer",
                min: 1000,
                max: 60000,
                default: 5000,
                required: false
            }
        }
    },
    {
        name: "alert.play_tone",
        description: "Request a local alert tone from a fixed whitelist.",
        payload: {
            tone: {
                type: "enum",
                values: ["confirm", "warning", "error"],
                required: true
            },
            duration_ms: {
                type: "integer",
                min: 100,
                max: 10000,
                required: false
            }
        }
    }
];

const COMMAND_DEFINITION_BY_NAME = new Map(
    COMMAND_DEFINITIONS.map(definition => [definition.name, definition])
);

function readBoundedString(value, maxLength) {
    if (typeof value !== "string") {
        return "";
    }

    const trimmed = value.trim();
    return trimmed.length > maxLength ? trimmed.slice(0, maxLength) : trimmed;
}

function normalizeCommandName(value) {
    return readBoundedString(value, 80);
}

function readTrimmedString(value) {
    if (typeof value !== "string") {
        return "";
    }

    return value.trim();
}

function normalizeInteger(value) {
    const numeric = Number(value);
    if (!Number.isInteger(numeric)) {
        return null;
    }

    return numeric;
}

function validatePayloadField(payload, output, key, schema) {
    const value = payload[key];

    if (value === undefined || value === null || value === "") {
        if (schema.required) {
            return `${key} is required`;
        }
        if (schema.default !== undefined) {
            output[key] = schema.default;
        }
        return null;
    }

    if (schema.type === "integer") {
        const numeric = normalizeInteger(value);
        if (numeric === null) {
            return `${key} must be an integer`;
        }
        if (typeof schema.min === "number" && numeric < schema.min) {
            return `${key} must be >= ${schema.min}`;
        }
        if (typeof schema.max === "number" && numeric > schema.max) {
            return `${key} must be <= ${schema.max}`;
        }
        output[key] = numeric;
        return null;
    }

    if (schema.type === "string") {
        if (typeof value !== "string") {
            return `${key} must be a string`;
        }
        const text = value.trim();
        if (schema.required && !text) {
            return `${key} is required`;
        }
        if (schema.maxLength && text.length > schema.maxLength) {
            return `${key} must be <= ${schema.maxLength} characters`;
        }
        output[key] = text;
        return null;
    }

    if (schema.type === "enum") {
        const text = readBoundedString(value, 80);
        if (!schema.values.includes(text)) {
            return `${key} must be one of ${schema.values.join(", ")}`;
        }
        output[key] = text;
        return null;
    }

    return `${key} has unsupported schema`;
}

function validateCommandPayload(definition, payload) {
    const sourcePayload = payload && typeof payload === "object" && !Array.isArray(payload)
        ? payload
        : {};
    const output = {};

    for (const [key, schema] of Object.entries(definition.payload || {})) {
        const error = validatePayloadField(sourcePayload, output, key, schema);
        if (error) {
            return {
                ok: false,
                error
            };
        }
    }

    return {
        ok: true,
        payload: output
    };
}

function normalizeCommand(input, defaults = {}) {
    if (!input || typeof input !== "object" || Array.isArray(input)) {
        return {
            ok: false,
            code: "COMMAND_INVALID",
            error: "command must be an object"
        };
    }

    const name = normalizeCommandName(input.name || input.command || input.type);
    const definition = COMMAND_DEFINITION_BY_NAME.get(name);
    if (!definition) {
        return {
            ok: false,
            code: "COMMAND_NOT_WHITELISTED",
            error: "command is not whitelisted",
            name: name || ""
        };
    }

    const deviceId = readTrimmedString(input.target_device_id || input.device_id || defaults.deviceId || defaults.targetDeviceId);
    if (!deviceId) {
        return {
            ok: false,
            code: "COMMAND_TARGET_REQUIRED",
            error: "target_device_id is required",
            name
        };
    }
    if (deviceId.length > COMMAND_DEVICE_ID_MAX_LENGTH) {
        return {
            ok: false,
            code: "COMMAND_TARGET_INVALID",
            error: `target_device_id must be <= ${COMMAND_DEVICE_ID_MAX_LENGTH} characters`,
            name,
            target_device_id: deviceId.slice(0, COMMAND_DEVICE_ID_MAX_LENGTH)
        };
    }

    const payloadResult = validateCommandPayload(definition, input.payload);
    if (!payloadResult.ok) {
        return {
            ok: false,
            code: "COMMAND_PAYLOAD_INVALID",
            error: payloadResult.error,
            name,
            target_device_id: deviceId
        };
    }

    const reason = readTrimmedString(input.reason);
    if (reason.length > COMMAND_REASON_MAX_LENGTH) {
        return {
            ok: false,
            code: "COMMAND_REASON_INVALID",
            error: `reason must be <= ${COMMAND_REASON_MAX_LENGTH} characters`,
            name,
            target_device_id: deviceId
        };
    }

    return {
        ok: true,
        command: {
            name,
            target_device_id: deviceId,
            payload: payloadResult.payload,
            reason
        }
    };
}

function listCommandDefinitions() {
    return COMMAND_DEFINITIONS.map(definition => ({
        name: definition.name,
        description: definition.description,
        payload: definition.payload
    }));
}

function isCommandWhitelisted(name) {
    return COMMAND_DEFINITION_BY_NAME.has(normalizeCommandName(name));
}

module.exports = {
    COMMAND_DEVICE_ID_MAX_LENGTH,
    COMMAND_DEFINITIONS,
    isCommandWhitelisted,
    listCommandDefinitions,
    normalizeCommandName,
    normalizeCommand
};
