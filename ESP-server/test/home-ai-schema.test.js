const assert = require("assert");
const {
    HOME_AI_MAX_RULES,
    HOME_AI_MAX_DEVICE_ID_LENGTH,
    HOME_AI_MAX_PROMPT_LENGTH,
    HOME_AI_SCHEMA_VERSION,
    defaultRooms,
    normalizeHomeAiEvents,
    normalizeRoomConfig,
    normalizeUserOverride,
    normalizeVirtualDeviceState,
    validateRulePackage
} = require("../src/homeAi/schema");

function baseRule(ruleId = "bedroom_light") {
    return {
        rule_id: ruleId,
        version: 1,
        rule_type: "basic_automation",
        source: "manual",
        room_id: "bedroom_01",
        enabled: true,
        priority: 500,
        conditions: [{
            field: "presence_state",
            operator: "eq",
            value: "occupied"
        }],
        actions: [{
            device_id: "bedroom_01_light",
            device_type: "light",
            action: "turn_on"
        }],
        cooldown_seconds: 120,
        offline_policy: "continue"
    };
}

function packageFor(rules) {
    return {
        schema_version: HOME_AI_SCHEMA_VERSION,
        version: 1,
        generated_at_ms: 1700000000000,
        rooms: defaultRooms(),
        rules
    };
}

const accepted = validateRulePackage(packageFor([baseRule()]));
assert.equal(accepted.ok, true);
assert.equal(accepted.package.checksum.length, 64);
assert.equal(accepted.resource.max_rules, HOME_AI_MAX_RULES);
assert.equal(accepted.package.rules[0].actions[0].action, "turn_on");

const pendingRebind = validateRulePackage(packageFor([{
    ...baseRule(),
    room_id: "removed_room",
    enabled: false,
    binding: {
        state: "PENDING_REBIND",
        reason: "room_config_migration",
        from_room_id: "removed_room",
        to_room_id: "",
        source_ids: ["s3_local"],
        config_version: 2,
        was_enabled: true
    }
}]));
assert.equal(pendingRebind.ok, true);
assert.equal(pendingRebind.package.rules[0].binding.state, "PENDING_REBIND");
assert.equal(validateRulePackage(packageFor([{
    ...baseRule(),
    room_id: "removed_room",
    binding: {
        state: "PENDING_REBIND",
        reason: "room_config_migration",
        from_room_id: "removed_room",
        to_room_id: "",
        source_ids: ["s3_local"]
    }
}])).code, "RULE_BINDING_INVALID");

const roomsWithTerminals = defaultRooms();
assert.equal(normalizeRoomConfig({ rooms: roomsWithTerminals }).ok, true);
assert.equal(normalizeRoomConfig({ rooms: roomsWithTerminals.map(room => ({
    ...room,
    voice_terminal_device_id: room.room_id === "bedroom_01" ? "unknown_terminal" : room.voice_terminal_device_id
})) }).code, "ROOM_VOICE_TERMINAL_UNSUPPORTED");
assert.equal(normalizeRoomConfig({ rooms: roomsWithTerminals.map(room => ({
    ...room,
    voice_terminal_device_id: room.room_id === "living_room" ? "sensair_shuttle_01" : room.voice_terminal_device_id
})) }).code, "ROOM_VOICE_TERMINAL_DUPLICATE");
assert.equal(normalizeRoomConfig({ rooms: roomsWithTerminals.map(room => ({
    ...room,
    voice_terminal_device_id: room.room_id === "bedroom_01" ? "BAD ID" : room.voice_terminal_device_id
})) }).code, "ROOM_VOICE_TERMINAL_INVALID");

const maxLengthDeviceId = "a".repeat(HOME_AI_MAX_DEVICE_ID_LENGTH);
const maxLengthPrompt = "x".repeat(HOME_AI_MAX_PROMPT_LENGTH);
const bounded = validateRulePackage(packageFor([{
    ...baseRule(),
    actions: [{
        device_id: maxLengthDeviceId,
        device_type: "light",
        action: "play_prompt",
        prompt: maxLengthPrompt
    }]
}]));
assert.equal(bounded.ok, true);
assert.equal(bounded.package.rules[0].actions[0].prompt.length, HOME_AI_MAX_PROMPT_LENGTH);
assert.equal(validateRulePackage(packageFor([{
    ...baseRule(),
    actions: [{
        device_id: `${maxLengthDeviceId}a`,
        device_type: "light",
        action: "turn_on"
    }]
}])).ok, false);

const duplicate = validateRulePackage(packageFor([baseRule(), baseRule()]));
assert.equal(duplicate.ok, false);
assert.equal(duplicate.code, "RULE_ID_DUPLICATE");

const oversized = validateRulePackage(packageFor(
    Array.from({ length: HOME_AI_MAX_RULES + 1 }, (_, index) => baseRule(`rule_${index}`))
));
assert.equal(oversized.ok, false);
assert.equal(oversized.code, "RULE_PACKAGE_LIMIT");

const unsupportedAction = validateRulePackage(packageFor([{
    ...baseRule(),
    actions: [{
        device_id: "bedroom_01_light",
        action: "set_color"
    }]
}]));
assert.equal(unsupportedAction.ok, false);
assert.equal(unsupportedAction.code, "RULE_ACTION_UNSUPPORTED");

const missingDeviceType = validateRulePackage(packageFor([{
    ...baseRule(),
    actions: [{
        device_id: "bedroom_01_light",
        action: "turn_on"
    }]
}]));
assert.equal(missingDeviceType.ok, false);
assert.equal(missingDeviceType.code, "RULE_ACTION_DEVICE_INVALID");

const missingPrompt = validateRulePackage(packageFor([{
    ...baseRule(),
    actions: [{ action: "play_prompt" }]
}]));
assert.equal(missingPrompt.ok, false);
assert.equal(missingPrompt.code, "RULE_ACTION_PROMPT_REQUIRED");

const invalidBooleanComparison = validateRulePackage(packageFor([{
    ...baseRule(),
    conditions: [{ field: "radar_fresh", operator: "gt", value: true }]
}]));
assert.equal(invalidBooleanComparison.ok, false);
assert.equal(invalidBooleanComparison.code, "RULE_CONDITION_OPERATOR_INVALID");

const invalidRange = validateRulePackage(packageFor([{
    ...baseRule(),
    conditions: [{ field: "temperature_c", operator: "range", value: [30, 20] }]
}]));
assert.equal(invalidRange.ok, false);
assert.equal(invalidRange.code, "RULE_CONDITION_VALUE_INVALID");

const wrongChecksum = validateRulePackage({
    ...packageFor([baseRule()]),
    checksum: "wrong"
}, {
    requireChecksum: true
});
assert.equal(wrongChecksum.ok, false);
assert.equal(wrongChecksum.code, "RULE_PACKAGE_CHECKSUM_INVALID");

const virtualDevice = normalizeVirtualDeviceState({
    device_id: "bedroom_01_light",
    room_id: "bedroom_01",
    device_type: "light",
    power: "on",
    execution_mode: "virtual",
    last_action: "turn_on",
    action_source: "presence_automation",
    decision_reason: "occupied_and_night",
    verified: true
});
assert.equal(virtualDevice.ok, true);
assert.equal(virtualDevice.device.execution_mode, "virtual");
assert.equal(virtualDevice.device.verified, true);

const invalidVirtualDevice = normalizeVirtualDeviceState({
    ...virtualDevice.device,
    execution_mode: "physical"
});
assert.equal(invalidVirtualDevice.ok, false);
assert.equal(invalidVirtualDevice.code, "VIRTUAL_DEVICE_MODE_INVALID");

const override = normalizeUserOverride({
    scope: { room_id: "bedroom_01", device_id: "bedroom_01_light" },
    action: "keep_off",
    source: "explicit_user_command",
    expires_at_ms: Date.now() + 60000,
    priority: 900,
    allow_safety_override: true
});
assert.equal(override.ok, true);
assert.equal(override.override.scope.device_id, "bedroom_01_light");

const events = normalizeHomeAiEvents({
    events: [{
        event_id: "decision_1",
        event_type: "decision",
        room_id: "bedroom_01",
        schema_version: HOME_AI_SCHEMA_VERSION,
        priority: 500,
        payload: { rule_id: "bedroom_light", result: "executed" }
    }, {
        event_id: "bad_event",
        event_type: "unknown",
        payload: {}
    }]
});
assert.equal(events.ok, true);
assert.equal(events.events.length, 1);
assert.equal(events.rejected[0].code, "HOME_AI_EVENT_INVALID");

const wrongEventSchema = normalizeHomeAiEvents({
    events: [{
        event_id: "bad_schema",
        event_type: "decision",
        schema_version: 2,
        payload: {}
    }]
});
assert.equal(wrongEventSchema.ok, false);

process.stdout.write("home ai schema tests: PASS\n");
