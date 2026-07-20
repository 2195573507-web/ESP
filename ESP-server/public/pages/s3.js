(function () {
    const EMPTY_TEXT = "暂无数据";
    const ERROR_TEXT = "接口请求失败";
    const UNAVAILABLE_TEXT = "无法获取数据";
    const OFFLINE_TEXT = "离线";
    const UNKNOWN_TEXT = "未知";
    const DISCONNECTED_TEXT = "未连接";
    const SENSOR_EMPTY_TEXT = "--";
    const TARGET_DEVICE_IDS = ["C51", "C52"];
    const DEVICE_DISPLAY_NAMES = {
        C51: "卧室（C51）",
        C52: "客厅（C52）"
    };
    const DEVICE_ROOM_NAMES = {
        C51: "卧室",
        C52: "客厅"
    };

    const applianceSlots = [
        { key: "air_conditioner", label: "空调", icon: "❄️" },
        { key: "fan", label: "风扇", icon: "🌀" },
        { key: "light", label: "灯", icon: "💡" },
        { key: "tv", label: "TV", icon: "▣" },
        { key: "curtain", label: "窗帘", icon: "▥" },
        { key: "humidifier", label: "加湿器", icon: "💧" },
        { key: "air_purifier", label: "空气净化器", icon: "◌" }
    ];

    function escapeHtml(value) {
        return String(value ?? "")
            .replace(/&/g, "&amp;")
            .replace(/</g, "&lt;")
            .replace(/>/g, "&gt;")
            .replace(/"/g, "&quot;")
            .replace(/'/g, "&#39;");
    }

    function isPlainObject(value) {
        return Boolean(value) && typeof value === "object" && !Array.isArray(value);
    }

    function toNumberOrNull(value) {
        const numeric = Number(value);
        return Number.isFinite(numeric) ? numeric : null;
    }

    function formatNumber(value, digits = 1) {
        const numeric = Number(value);
        if (!Number.isFinite(numeric)) return DISCONNECTED_TEXT;
        return Number(numeric.toFixed(digits)).toString();
    }

    function formatTime(value) {
        if (value === undefined || value === null || value === "") return EMPTY_TEXT;
        const numeric = Number(value);
        const date = Number.isFinite(numeric)
            ? new Date(numeric < 10000000000 ? numeric * 1000 : numeric)
            : new Date(value);
        if (Number.isNaN(date.getTime())) return String(value);
        return date.toLocaleString("zh-CN", {
            hour12: false,
            month: "2-digit",
            day: "2-digit",
            hour: "2-digit",
            minute: "2-digit",
            second: "2-digit"
        });
    }

    function unwrapEnvelope(payload) {
        if (payload && typeof payload === "object" && "data" in payload && "ok" in payload) {
            return payload.data;
        }
        return payload;
    }

    function buildUrl(path, params = {}) {
        const url = new URL(path, window.location.origin);
        Object.entries(params).forEach(([key, value]) => {
            if (value !== undefined && value !== null && value !== "") {
                url.searchParams.set(key, value);
            }
        });
        return `${url.pathname}${url.search}`;
    }

    function realtime() {
        return window.DashboardRealtime || {};
    }

    function parseTimestamp(value) {
        if (realtime().parseTimestamp) return realtime().parseTimestamp(value);
        if (value === undefined || value === null || value === "") return null;
        const numeric = Number(value);
        const date = Number.isFinite(numeric)
            ? new Date(numeric < 10000000000 ? numeric * 1000 : numeric)
            : new Date(value);
        return Number.isNaN(date.getTime()) ? null : date;
    }

    async function fetchOverview(deviceId = "") {
        const response = await fetch(buildUrl("/api/dashboard/v1/overview", { device_id: deviceId }), { cache: "no-store" });
        if (!response.ok) {
            throw new Error(`${response.status}`);
        }
        return unwrapEnvelope(await response.json());
    }

    async function fetchModulesStatus() {
        const response = await fetch("/api/dashboard/v1/modules/status", { cache: "no-store" });
        if (!response.ok) {
            throw new Error(`${response.status}`);
        }
        const data = unwrapEnvelope(await response.json());
        return Array.isArray(data?.modules) ? data.modules : [];
    }

    async function fetchDeviceStatus(deviceId = "") {
        const response = await fetch(buildUrl("/api/dashboard/v1/device/status", { device_id: deviceId }), { cache: "no-store" });
        if (!response.ok) {
            throw new Error(`${response.status}`);
        }
        const data = unwrapEnvelope(await response.json());
        return data || null;
    }

    async function fetchCommandHistory() {
        const response = await fetch("/api/commands/history?limit=20", { cache: "no-store" });
        if (!response.ok) {
            throw new Error(`${response.status}`);
        }
        const data = unwrapEnvelope(await response.json());
        return Array.isArray(data?.commands) ? data.commands : [];
    }

    async function fetchAlarmLogs() {
        const response = await fetch("/api/emergency/events?limit=20", { cache: "no-store" });
        if (!response.ok) {
            throw new Error(`${response.status}`);
        }
        const data = unwrapEnvelope(await response.json());
        return Array.isArray(data?.events) ? data.events : [];
    }

    async function fetchListEndpoint(path, keys = []) {
        const response = await fetch(path, { cache: "no-store" });
        if (!response.ok) {
            throw new Error(`${response.status}`);
        }
        const data = unwrapEnvelope(await response.json());
        if (Array.isArray(data)) return data;
        if (!isPlainObject(data)) return [];
        for (const key of keys) {
            if (Array.isArray(data[key])) return data[key];
        }
        return [];
    }

    async function fetchSystemEvents() {
        const results = await Promise.allSettled([
            fetchListEndpoint("/api/logs/v1/system?limit=20", ["logs", "system_logs", "events", "records"]),
            fetchListEndpoint("/api/voice/v1/events?limit=20", ["events", "logs", "records"]),
            fetchCommandHistory(),
            fetchListEndpoint("/api/logs/v1/alarms?limit=20", ["alarms", "logs", "events"]),
            fetchAlarmLogs()
        ]);
        return results.flatMap(result => result.status === "fulfilled" ? result.value : []);
    }

    function getStatusClass(status) {
        const text = String(status ?? "").toLowerCase();
        if (status === true || ["running", "completed", "resolved", "archived", "info", "normal"].includes(text)) return "normal";
        if (status === false || ["stopped", "failed", "disconnected", "critical", "danger"].includes(text)) return "danger";
        if (["pending", "standby", "queued", "dispatched", "received", "llm_pending", "forwarded", "warning"].includes(text)) return "warning";
        return "unknown";
    }

    function getStatusText(status) {
        const textMap = {
            running: "运行中",
            standby: "待命",
            stopped: DISCONNECTED_TEXT,
            disconnected: DISCONNECTED_TEXT,
            completed: "已完成",
            pending: "处理中",
            queued: "待执行",
            dispatched: "已下发",
            failed: "失败",
            received: "已接收",
            llm_pending: "AI 处理中",
            forwarded: "已转发",
            resolved: "已解决",
            archived: "已归档",
            info: "信息",
            warning: "警告",
            critical: "严重"
        };
        if (status === true) return "在线";
        if (status === false) return OFFLINE_TEXT;
        return textMap[status] || cleanDisplayText(status, UNKNOWN_TEXT);
    }

    const INTERNAL_DISPLAY_PATTERN = /\b(?:command|sensor|gateway|voice|llm|mqtt|module|system)\.[\w.-]+|\btopic\b|module_type|event_type|device_id|dashboard_snapshot|bme690|rssi|heap|cpu|memory|firmware|version|[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}|(?:[0-9a-f]{2}:){5}[0-9a-f]{2}/i;

    function looksInternalText(value) {
        const text = String(value ?? "").trim();
        return Boolean(text) && (INTERNAL_DISPLAY_PATTERN.test(text) || /^\/?api\//i.test(text));
    }

    function cleanDisplayText(value, fallback = "") {
        const text = String(value ?? "").trim();
        if (!text) return fallback;
        return looksInternalText(text) ? fallback : text;
    }

    function getDeviceDisplayName(deviceId) {
        const key = normalizeDeviceId(deviceId);
        return DEVICE_DISPLAY_NAMES[key] || "";
    }

    function getDeviceRoomName(deviceId) {
        const key = normalizeDeviceId(deviceId);
        return DEVICE_ROOM_NAMES[key] || "";
    }

    function friendlyDeviceName(value) {
        const text = String(value || "").trim().toUpperCase();
        const roomName = getDeviceRoomName(text);
        if (roomName) return roomName;
        return text && !looksInternalText(text) ? text : "设备";
    }

    function humanizeAlarmType(type, message = "") {
        const text = `${type || ""} ${message || ""}`.toLowerCase();
        if (/air|aqi|quality|空气/.test(text)) return "空气质量报警";
        if (/temp|temperature|温度|hot|heat/.test(text)) return "温度过高";
        if (/humid|humidity|湿度/.test(text)) return "湿度异常";
        if (/pressure|气压/.test(text)) return "气压异常";
        if (/offline|disconnect|离线|设备/.test(text)) return "设备离线";
        return "报警";
    }

    function getCommandActionText(command, status) {
        const text = String(command || "").toLowerCase();
        const statusText = String(status || "").toLowerCase();
        if (["failed", "error", "danger", "unavailable"].includes(statusText)) return "命令执行失败";
        if (/turn_on|\.on|open|enable/.test(text)) return "已开启";
        if (/turn_off|\.off|close|disable/.test(text)) return "已关闭";
        if (/set_temperature/.test(text)) return "温度已调整";
        if (["completed", "success", "resolved"].includes(statusText)) return "命令执行成功";
        if (["queued", "pending", "dispatched", "received"].includes(statusText)) return "命令处理中";
        return "命令记录已更新";
    }

    function getCommandTargetLabel(command, record, payload) {
        const commandText = String(command || "").toLowerCase();
        const targetText = String(record.target || record.device_name || payload.target || payload.device_name || "").trim();
        if (targetText && !looksInternalText(targetText)) return targetText;
        const match = applianceSlots.find(slot => commandText.includes(slot.key) || commandText.includes(slot.key.replace("_", "")));
        return match?.label || "设备";
    }

    function humanizeCommandEvent(command, record, payload) {
        const action = getCommandActionText(command, record.status || payload.status);
        if (action === "命令执行成功" || action === "命令执行失败" || action === "命令处理中" || action === "命令记录已更新") {
            return action;
        }
        return `${getCommandTargetLabel(command, record, payload)}${action}`;
    }

    function getRecordTime(record) {
        if (!isPlainObject(record)) return null;
        const payload = isPlainObject(record.payload) ? record.payload : {};
        return parseTimestamp(
            record.created_at ||
            record.updated_at ||
            record.timestamp ||
            record.completed_at ||
            record.time ||
            payload.created_at ||
            payload.timestamp
        );
    }

    function normalizeSystemEvent(record) {
        const item = isPlainObject(record) ? record : {};
        const payload = isPlainObject(item.payload) ? item.payload : {};
        const command = item.command || item.name || item.command_id || payload.command;
        const eventType = item.event_type || item.type || payload.event_type || payload.type || "";
        const message = item.message ||
            item.content ||
            item.text ||
            item.local_action ||
            payload.message ||
            payload.summary ||
            payload.description ||
            payload.text ||
            "";
        const time = getRecordTime(item);
        const combined = `${eventType} ${item.source || ""} ${item.module || ""} ${payload.source || ""} ${payload.module || ""} ${message}`.toLowerCase();
        const deviceName = friendlyDeviceName(item.device_id || payload.device_id || "");
        let event = null;
        if (command) {
            event = {
                icon: "💡",
                type: "设备控制",
                description: humanizeCommandEvent(command, item, payload)
            };
        } else if (/voice|asr|语音/.test(combined) || item.asr_text || payload.asr_text) {
            const speechText = cleanDisplayText(item.asr_text || payload.asr_text || message, "");
            event = {
                icon: "🎤",
                type: "语音命令",
                description: speechText ? `收到语音命令：${speechText}` : "收到语音命令"
            };
        } else if (/llm|ai|response|回复|分析/.test(combined) || item.response || payload.response) {
            event = {
                icon: "🤖",
                type: "AI 分析",
                description: "AI 已完成分析"
            };
        } else if (/alarm|warning|critical|报警/.test(combined)) {
            event = {
                icon: "⚠",
                type: "报警",
                description: humanizeAlarmType(eventType, message)
            };
        } else if (/wifi|sta_connected|network/.test(combined)) {
            event = {
                icon: "📶",
                type: "网络状态",
                description: "WiFi 已重新连接"
            };
        } else if (/mqtt|cloud|server_available/.test(combined)) {
            event = {
                icon: "☁",
                type: "云端连接",
                description: /disconnect|offline|failed|未连接|异常/.test(combined) ? "云端连接异常" : "云端连接恢复"
            };
        } else if (/gateway|dashboard_snapshot|网关/.test(combined)) {
            event = {
                icon: "📡",
                type: "网关状态",
                description: /disconnect|offline|failed|离线/.test(combined) ? "设备离线" : "网关重新连接"
            };
        } else if (/sensor|bme|temperature|humidity|pressure|air_quality|环境/.test(combined) || item.device_id || payload.device_id) {
            event = {
                icon: "🌡️",
                type: "环境数据",
                description: `${deviceName.replace(/\s+/g, "")}环境数据更新`
            };
        }
        if (!event) return null;
        return {
            timestamp: time,
            icon: event.icon,
            type: event.type,
            description: event.description,
            detail: event.description
        };
    }

    function normalizeSystemEvents(records) {
        return (Array.isArray(records) ? records : [])
            .map(normalizeSystemEvent)
            .filter(event => event && (event.timestamp || event.description))
            .sort((a, b) => {
                const aTime = a.timestamp ? a.timestamp.getTime() : 0;
                const bTime = b.timestamp ? b.timestamp.getTime() : 0;
                return bTime - aTime;
            })
            .slice(0, 20);
    }

    function normalizeGateway(rawGateway, deviceStatus = null) {
        const gateway = isPlainObject(rawGateway) ? rawGateway : {};
        const hasDeviceStatus = Boolean(deviceStatus?._hasData);
        const hasGatewaySnapshot = gateway.last_error !== "no_gateway_snapshot";
        const online = hasGatewaySnapshot && typeof gateway.online === "boolean" ? gateway.online : null;
        const cloudConnected = hasGatewaySnapshot && typeof gateway.cloud_connected === "boolean"
            ? gateway.cloud_connected
            : (hasGatewaySnapshot && typeof gateway.server_available === "boolean" ? gateway.server_available : null);
        const latency = Number.isFinite(Number(gateway.latency_ms))
            ? Number(gateway.latency_ms)
            : (hasDeviceStatus && Number.isFinite(Number(deviceStatus?.latest_upload_delay_ms)) ? Number(deviceStatus.latest_upload_delay_ms) : null);
        const localDegraded = hasGatewaySnapshot && typeof gateway.local_degraded === "boolean"
            ? gateway.local_degraded
            : (hasGatewaySnapshot && typeof gateway.server_available === "boolean"
                ? !gateway.server_available
                : (hasDeviceStatus && typeof deviceStatus?.time_synced === "boolean" ? !deviceStatus.time_synced : null));
        return {
            name: gateway.name || gateway.gateway_id || "网关",
            online,
            cloud_connected: cloudConnected,
            latency_ms: latency,
            local_degraded: localDegraded,
            softap_ready: gateway.softap_ready,
            sta_connected: gateway.sta_connected,
            voice_busy: gateway.voice_busy,
            last_error: gateway.last_error || "",
            timestamp: gateway.timestamp,
            modules: []
        };
    }

    function normalizeDeviceStatus(rawStatus) {
        const status = isPlainObject(rawStatus) ? rawStatus : {};
        const latestUploadDelay = toNumberOrNull(status.latest_upload_delay_ms);
        const lastSeenMs = toNumberOrNull(status.last_seen_ms);
        const lastSeenAgeMs = toNumberOrNull(status.last_seen_age_ms);
        const hasData = Boolean(
            lastSeenMs !== null ||
            latestUploadDelay !== null ||
            status.last_seen_iso ||
            Number(status.delay_sample_count) > 0
        );
        return {
            device_id: status.device_id || null,
            online: typeof status.online === "boolean"
                ? status.online
                : (typeof status.device_online === "boolean" ? status.device_online : null),
            device_online: typeof status.device_online === "boolean" ? status.device_online : null,
            latest_upload_delay_ms: latestUploadDelay,
            last_seen_ms: lastSeenMs,
            last_seen_age_ms: lastSeenAgeMs,
            time_synced: typeof status.time_synced === "boolean" ? status.time_synced : null,
            _hasData: hasData
        };
    }

    function normalizeSensors(rawSensors) {
        const sensors = isPlainObject(rawSensors) ? rawSensors : {};
        const airQualityObject = isPlainObject(sensors.air_quality) ? sensors.air_quality : {};
        const airQualityScore = toNumberOrNull(sensors.air_quality_score ?? airQualityObject.air_quality_score);
        const airQualityLevel = sensors.air_quality_level ??
            sensors.air_quality_label ??
            airQualityObject.air_quality_level ??
            airQualityObject.level ??
            "";
        return {
            temperature: toNumberOrNull(sensors.temperature ?? sensors.temperature_c),
            humidity: toNumberOrNull(sensors.humidity ?? sensors.humidity_percent),
            pressure: toNumberOrNull(sensors.pressure ?? sensors.pressure_hpa),
            air_quality_score: airQualityScore,
            air_quality_level: airQualityLevel ? String(airQualityLevel) : ""
        };
    }

    function hasSensorValues(sensors) {
        return ["temperature", "humidity", "pressure", "air_quality_score"]
            .some(key => sensors?.[key] !== null && sensors?.[key] !== undefined);
    }

    function isMockAppliance(appliance) {
        return isPlainObject(appliance) && (appliance.mock === true || appliance.source === "mock");
    }

    function normalizeAppliances(rawAppliances) {
        if (!isPlainObject(rawAppliances)) {
            return {};
        }

        return Object.entries(rawAppliances).reduce((result, [key, appliance]) => {
            if (isPlainObject(appliance) && !isMockAppliance(appliance)) {
                result[key] = appliance;
            }
            return result;
        }, {});
    }

    function normalizeDevice(rawDevice) {
        const device = isPlainObject(rawDevice) ? rawDevice : {};
        const deviceId = device.device_id || device.id || "";
        const displayName = getDeviceDisplayName(deviceId);
        const roomName = getDeviceRoomName(deviceId);
        const sensors = normalizeSensors(device.sensors);
        const appliances = normalizeAppliances(device.appliances);
        const occupancy = isPlainObject(device.occupancy) ? device.occupancy : null;
        const rawRoom = device.room_name || device.room || "";
        const hasIdentityData = Boolean(
            device.name ||
            device.alias ||
            device.local_id !== undefined && device.local_id !== null ||
            (rawRoom && rawRoom !== "unassigned")
        );
        const hasOccupancyData = Boolean(occupancy?.available === true || occupancy?.updated_at);
        const hasDeviceEvidence = hasIdentityData ||
            hasSensorValues(sensors) ||
            Object.keys(appliances).length > 0 ||
            hasOccupancyData ||
            toNumberOrNull(device.wifi_rssi) !== null;
        return {
            id: deviceId,
            name: displayName || cleanDisplayText(device.name || deviceId, UNKNOWN_TEXT),
            room: roomName || (rawRoom && rawRoom !== "unassigned" ? cleanDisplayText(rawRoom, "未分配") : "未分配"),
            online: hasDeviceEvidence && typeof device.online === "boolean" ? device.online : null,
            timestamp: hasDeviceEvidence ? device.timestamp : null,
            sensors,
            occupancy: hasOccupancyData ? occupancy : null,
            appliances
        };
    }

    function normalizeDeviceId(value) {
        return String(value || "").trim().toUpperCase();
    }

    function mergeDevice(existing, next) {
        if (!existing) return next;

        const sensors = { ...existing.sensors };
        Object.entries(next.sensors || {}).forEach(([key, value]) => {
            if (value !== null && value !== undefined && value !== "") {
                sensors[key] = value;
            }
        });

        return {
            ...existing,
            ...next,
            name: next.name && next.name !== UNKNOWN_TEXT ? next.name : existing.name,
            room: next.room && next.room !== "未分配" ? next.room : existing.room,
            online: typeof next.online === "boolean" ? next.online : existing.online,
            timestamp: next.timestamp ?? existing.timestamp,
            sensors,
            occupancy: next.occupancy || existing.occupancy,
            appliances: {
                ...existing.appliances,
                ...next.appliances
            }
        };
    }

    function createEmptyDevice(deviceId) {
        return {
            id: deviceId,
            name: getDeviceDisplayName(deviceId) || deviceId,
            room: getDeviceRoomName(deviceId) || "未分配",
            online: null,
            timestamp: null,
            sensors: {
                temperature: null,
                humidity: null,
                pressure: null,
                air_quality_score: null,
                air_quality_level: ""
            },
            occupancy: null,
            appliances: {}
        };
    }

    function mergeOverviewDevices(overviews) {
        const byId = new Map();
        overviews.forEach(item => {
            if (!isPlainObject(item) || !Array.isArray(item.devices)) return;
            item.devices
                .map(normalizeDevice)
                .forEach(device => {
                    const id = normalizeDeviceId(device.id);
                    if (!id) return;
                    byId.set(id, mergeDevice(byId.get(id), device));
                });
        });

        const orderedTargets = TARGET_DEVICE_IDS.map(deviceId => {
            const key = normalizeDeviceId(deviceId);
            return byId.get(key) || createEmptyDevice(deviceId);
        });
        return orderedTargets;
    }

    function normalizeAlarm(rawAlarm) {
        const alarm = isPlainObject(rawAlarm) ? rawAlarm : {};
        const payload = isPlainObject(alarm.payload) ? alarm.payload : {};
        const rawMessage = alarm.local_action ||
            payload.message ||
            payload.summary ||
            payload.description ||
            payload.reason ||
            "";
        const eventType = humanizeAlarmType(alarm.event_type || payload.event_type, rawMessage);
        return {
            event_id: alarm.event_id || "",
            device_id: alarm.device_id || "",
            event_type: eventType,
            severity: alarm.severity || payload.severity || "info",
            message: cleanDisplayText(rawMessage, eventType),
            status: alarm.status || UNKNOWN_TEXT,
            created_at: alarm.created_at || payload.created_at || "",
            updated_at: alarm.updated_at || "",
            resolved_at: alarm.resolved_at || ""
        };
    }

    function applyDeviceStatuses(devices, rawStatuses) {
        const statuses = Array.isArray(rawStatuses)
            ? rawStatuses.map(normalizeDeviceStatus).filter(status => status._hasData && status.device_id)
            : [];
        if (!statuses.length) {
            return devices;
        }

        const byId = new Map(statuses.map(status => [normalizeDeviceId(status.device_id), status]));
        return devices.map(device => {
            const status = byId.get(normalizeDeviceId(device.id));
            if (!status) return device;
            return {
                ...device,
                online: typeof status.online === "boolean" ? status.online : device.online,
                timestamp: status.last_seen_ms ?? device.timestamp
            };
        });
    }

    function normalizeOverview(data, modules = [], alarms = [], deviceStatus = null, states = {}, relatedOverviews = [], deviceStatuses = [], systemEvents = [], requestMeta = {}) {
        const overview = isPlainObject(data) ? data : {};
        const devices = applyDeviceStatuses(mergeOverviewDevices([overview, ...relatedOverviews]), deviceStatuses);
        const normalizedDeviceStatus = normalizeDeviceStatus(deviceStatus);
        return {
            gateway: {
                ...normalizeGateway(overview.gateway, normalizedDeviceStatus),
                modules
            },
            devices,
            home_summary: buildHomeSummary(devices),
            recent_alarms: Array.isArray(alarms) ? alarms.map(normalizeAlarm) : [],
            system_events: normalizeSystemEvents(systemEvents),
            device_status_error: Boolean(states.deviceStatusError),
            module_error: Boolean(states.moduleError),
            alarm_error: Boolean(states.alarmError),
            event_error: Boolean(states.eventError),
            request_meta: requestMeta
        };
    }

    function buildHomeSummary(devices) {
        if (!devices.length) {
            return {
                online_device_count: null,
                offline_device_count: null,
                avg_temperature: SENSOR_EMPTY_TEXT,
                avg_humidity: SENSOR_EMPTY_TEXT,
                avg_air_quality: SENSOR_EMPTY_TEXT
            };
        }
        const onlineDevices = devices.filter(device => device.online === true).length;
        const offlineDevices = devices.filter(device => device.online === false).length;
        const average = (reader, digits = 1) => {
            const values = devices
                .filter(device => device.online === true)
                .map(reader)
                .map(Number)
                .filter(Number.isFinite);
            if (!values.length) return SENSOR_EMPTY_TEXT;
            return formatNumber(values.reduce((sum, value) => sum + value, 0) / values.length, digits);
        };
        return {
            online_device_count: onlineDevices,
            offline_device_count: offlineDevices,
            avg_temperature: average(device => device.sensors.temperature),
            avg_humidity: average(device => device.sensors.humidity),
            avg_air_quality: average(device => device.sensors.air_quality_score, 0)
        };
    }

    function getLatestDataTimestamp(data) {
        const candidates = [
            data?.request_meta?.last_sync_at,
            data?.gateway?.timestamp,
            ...(Array.isArray(data?.devices) ? data.devices.map(device => device.timestamp) : []),
            ...(Array.isArray(data?.gateway?.modules) ? data.gateway.modules.map(module => module.last_seen_ms || module.updated_at || module.timestamp) : [])
        ];
        for (const candidate of candidates) {
            const date = parseTimestamp(candidate);
            if (date) return date;
        }
        return null;
    }

    function formatSensorValue(value, unit, digits = 1) {
        const numeric = Number(value);
        if (!Number.isFinite(numeric)) return SENSOR_EMPTY_TEXT;
        return `${formatNumber(numeric, digits)}${unit}`;
    }

    function formatDeviceSensorValue(device, key, unit, digits = 1) {
        if (device?.online !== true) return SENSOR_EMPTY_TEXT;
        return formatSensorValue(device?.sensors?.[key], unit, digits);
    }

    function getDeviceAirQualityState(device) {
        const score = Number(device?.sensors?.air_quality_score);
        if (device?.online !== true || !Number.isFinite(score)) {
            return { label: "", className: "unknown" };
        }
        return realtime().getAirQualityState
            ? realtime().getAirQualityState(score)
            : { label: device.sensors.air_quality_level || "", className: "unknown" };
    }

    function formatDeviceAirQuality(device) {
        const score = Number(device?.sensors?.air_quality_score);
        if (device?.online !== true || !Number.isFinite(score)) return SENSOR_EMPTY_TEXT;
        const airState = getDeviceAirQualityState(device);
        const label = airState.label ? ` · ${airState.label}` : "";
        return `${formatNumber(score, 0)} 分${label}`;
    }

    function getApplianceStatus(appliances, slot, online) {
        if (online === false) {
            return {
                label: slot.label,
                icon: slot.icon,
                state: DISCONNECTED_TEXT,
                isActive: false
            };
        }

        const appliance = appliances?.[slot.key];
        if (!isPlainObject(appliance) || isMockAppliance(appliance)) {
            return {
                label: slot.label,
                icon: slot.icon,
                state: "",
                isActive: false
            };
        }
        if (appliance.online === false) {
            return {
                label: appliance.name || slot.label,
                icon: slot.icon,
                state: DISCONNECTED_TEXT,
                isActive: false
            };
        }

        const rawState = String(appliance.state ?? appliance.status ?? "").trim();
        const stateText = cleanDisplayText(rawState, "");
        const explicitOn = appliance.on ?? appliance.enabled ?? appliance.power ?? appliance.online;
        const openPercent = toNumberOrNull(appliance.open_percent);
        const speed = toNumberOrNull(appliance.speed);
        const isActive = typeof explicitOn === "boolean"
            ? explicitOn
            : (openPercent !== null ? openPercent > 0 : false);
        const stateParts = [];
        if (stateText) {
            const lower = stateText.toLowerCase();
            if (["on", "open", "opened", "enabled", "running", "true", "1"].includes(lower)) {
                stateParts.push("开启");
            } else if (["off", "closed", "disabled", "stopped", "false", "0"].includes(lower)) {
                stateParts.push("关闭");
            } else {
                stateParts.push(stateText);
            }
        } else if (openPercent !== null) {
            stateParts.push(`${Math.round(openPercent)}%`);
        } else {
            stateParts.push(isActive ? "开启" : "关闭");
        }
        if (speed !== null && speed > 0) {
            stateParts.push(`${Math.round(speed)}档`);
        }
        return {
            label: appliance.name || slot.label,
            icon: slot.icon,
            state: stateParts.join(" · "),
            isActive
        };
    }

    function getConnectedAppliances(data) {
        const devices = Array.isArray(data?.devices) ? data.devices : [];
        return devices.flatMap(device => {
            const deviceKey = normalizeDeviceId(device.id || device.name);
            return applianceSlots.flatMap(slot => {
                const rawAppliance = device.appliances?.[slot.key];
                if (!isPlainObject(rawAppliance) || isMockAppliance(rawAppliance)) return [];
                const appliance = getApplianceStatus(device.appliances, slot, device.online);
                if (!appliance.state) return [];
                return [{
                    ...appliance,
                    key: `${deviceKey}-${slot.key}`,
                    room: device.room || getDeviceRoomName(deviceKey) || ""
                }];
            });
        });
    }

    function renderSmartHomeOverview(data) {
        const appliances = getConnectedAppliances(data);
        const content = appliances.length
            ? `<div class="s3-appliance-grid" data-smart-home-grid>${appliances.map(appliance => `
                <div class="s3-appliance ${appliance.isActive ? "is-on" : "is-off"}" data-smart-home-appliance="${escapeHtml(appliance.key)}">
                    <span class="s3-appliance-icon" aria-hidden="true">${appliance.icon}</span>
                    <strong data-appliance-label>${escapeHtml(appliance.label)}</strong>
                    <small data-appliance-state>${escapeHtml(appliance.state)}</small>
                </div>
            `).join("")}</div>`
            : '<div class="system-log empty">暂无智能家居设备。</div>';
        return `
            <section class="s3-section" data-s3-panel="smart-home">
                <div class="s3-section-heading">
                    <h2>🏡 智能家居</h2>
                </div>
                ${content}
            </section>
        `;
    }

    function updateSmartHomeOverview(root, data) {
        const panel = root.querySelector('[data-s3-panel="smart-home"]');
        if (!panel) return;
        const nextHtml = renderSmartHomeOverview(data);
        if (panel.dataset.signature !== nextHtml) {
            panel.outerHTML = nextHtml;
            const nextPanel = root.querySelector('[data-s3-panel="smart-home"]');
            if (nextPanel) nextPanel.dataset.signature = nextHtml;
        }
    }

    function getHomeSummaryItems(summary) {
        return [
            { key: "temperature", label: "平均温度", value: summary.avg_temperature === SENSOR_EMPTY_TEXT ? SENSOR_EMPTY_TEXT : `${summary.avg_temperature}°C`, accent: "blue" },
            { key: "humidity", label: "平均湿度", value: summary.avg_humidity === SENSOR_EMPTY_TEXT ? SENSOR_EMPTY_TEXT : `${summary.avg_humidity}%`, accent: "green" },
            { key: "air", label: "空气质量", value: summary.avg_air_quality === SENSOR_EMPTY_TEXT ? SENSOR_EMPTY_TEXT : `${summary.avg_air_quality} 分`, accent: "purple" },
            {
                key: "online",
                label: "在线设备数",
                value: summary.online_device_count === null ||
                    summary.offline_device_count === null
                    ? EMPTY_TEXT
                    : `${summary.online_device_count} 台`,
                accent: "orange"
            }
        ];
    }

    function renderHomeSummary(summary) {
        const items = getHomeSummaryItems(summary);

        return `
            <article class="panel s3-summary-panel">
                <div class="panel-header">
                    <h2>🏠 全屋状态</h2>
                </div>
                <div class="s3-summary-grid">
                    ${items.map(item => `
                        <div class="s3-summary-tile ${item.accent}" data-summary-key="${escapeHtml(item.key)}">
                            <span>${escapeHtml(item.label)}</span>
                            <strong data-summary-value>${escapeHtml(item.value)}</strong>
                        </div>
                    `).join("")}
                </div>
            </article>
        `;
    }

    function renderSensorMetricRealtime(label, value, meta = {}) {
        const statusClass = meta.statusClass ? ` ${meta.statusClass}` : "";
        const tooltip = meta.tooltip ? ` title="${escapeHtml(meta.tooltip)}"` : "";
        return `
            <div class="s3-sensor-metric${statusClass}"${tooltip} ${meta.key ? `data-device-metric="${escapeHtml(meta.key)}"` : ""}>
                <span>${escapeHtml(label)}</span>
                <strong data-device-metric-value>${escapeHtml(value)}</strong>
            </div>
        `;
    }

    function renderDeviceCardRealtime(device) {
        const updatedAt = parseTimestamp(device.timestamp);
        const deviceKey = normalizeDeviceId(device.id || device.name);
        const airState = getDeviceAirQualityState(device);
        return `
            <article class="panel s3-device-card" data-device-card="${escapeHtml(deviceKey)}">
                <div class="panel-header">
                    <div>
                        <h2 data-device-name>${escapeHtml(device.name)}</h2>
                    </div>
                    <span class="state-badge state-${getStatusClass(device.online)}" data-device-status>${getStatusText(device.online)}</span>
                </div>
                <div class="s3-sensor-grid">
                    ${renderSensorMetricRealtime("温度", formatDeviceSensorValue(device, "temperature", "°C"), { key: "temperature" })}
                    ${renderSensorMetricRealtime("湿度", formatDeviceSensorValue(device, "humidity", "%"), { key: "humidity" })}
                    ${renderSensorMetricRealtime("气压", formatDeviceSensorValue(device, "pressure", " hPa"), { key: "pressure" })}
                </div>
                <div class="s3-sensor-grid">
                    ${renderSensorMetricRealtime("空气质量", formatDeviceAirQuality(device), { statusClass: `aqi-${airState.className}`, key: "aqi" })}
                    ${renderSensorMetricRealtime("最近上报", updatedAt ? formatTime(updatedAt) : (device.online === false ? "设备离线" : "暂无数据"), { key: "lastReported" })}
                </div>
            </article>
        `;
    }

    function renderDeviceOverview(devices) {
        const content = devices.length
            ? devices.map(renderDeviceCardRealtime).join("")
            : '<div class="system-log empty">暂无数据</div>';
        return `
            <section class="s3-section">
                <div class="s3-section-heading">
                    <h2>📦 设备总览</h2>
                </div>
                <div class="s3-device-grid">
                    ${content}
                </div>
            </section>
        `;
    }

    function isActiveAlarm(alarm) {
        const status = String(alarm?.status || "").toLowerCase();
        if (alarm?.resolved_at) return false;
        return !["resolved", "archived", "completed", "normal", "recovered", "已恢复", "已解决", "已处理"].includes(status);
    }

    function getActiveAlarms(data) {
        return (Array.isArray(data.recent_alarms) ? data.recent_alarms : [])
            .filter(isActiveAlarm)
            .slice(0, 20);
    }

    function renderAlarmRows(data) {
        const alarms = getActiveAlarms(data);
        return alarms.length
            ? alarms.map(alarm => `
                <tr>
                    <td>${escapeHtml(formatTime(alarm.created_at))}</td>
                    <td>${escapeHtml(friendlyDeviceName(alarm.device_id))}</td>
                    <td>${escapeHtml(alarm.event_type)}</td>
                    <td>${escapeHtml(alarm.message || EMPTY_TEXT)}</td>
                    <td><span class="level-badge level-${getStatusClass(alarm.severity)}">${getStatusText(alarm.severity)}</span></td>
                </tr>
            `).join("")
            : `<tr><td colspan="5" class="table-empty">${data.alarm_error ? ERROR_TEXT : "当前无报警。"}</td></tr>`;
    }

    function renderAlarmPanel(data) {
        const rows = renderAlarmRows(data);
        return `
            <article class="panel" data-s3-panel="alarms">
                <div class="panel-header">
                    <h2>⚠️ 当前报警</h2>
                </div>
                <div class="table-wrap">
                    <table class="s3-table">
                        <thead>
                            <tr>
                                <th>时间</th>
                                <th>设备</th>
                                <th>类型</th>
                                <th>内容</th>
                                <th>级别</th>
                            </tr>
                        </thead>
                        <tbody data-alarm-rows>${rows}</tbody>
                    </table>
                </div>
            </article>
        `;
    }

    function renderSystemEventsPanel(data) {
        const events = Array.isArray(data.system_events) ? data.system_events : [];
        const timeline = realtime().EventTimeline
            ? realtime().EventTimeline(events)
            : '<div class="system-log empty">暂无系统事件。</div>';
        return `
            <article class="panel s3-events-panel" data-s3-panel="events">
                <div class="panel-header">
                    <h2>📝 最近系统事件</h2>
                </div>
                <div data-event-timeline>${timeline}</div>
            </article>
        `;
    }

    function setStableText(element, value) {
        if (!element) return;
        const text = String(value ?? "");
        if (element.textContent !== text) {
            element.textContent = text;
        }
    }

    function setStableClass(element, className) {
        if (element && element.className !== className) {
            element.className = className;
        }
    }

    function setStableTitle(element, title) {
        if (element && element.title !== title) {
            element.title = title;
        }
    }

    function updateDeviceCards(root, devices) {
        const grid = root.querySelector(".s3-device-grid");
        if (!grid) return;
        const cards = Array.from(grid.querySelectorAll("[data-device-card]"));
        if (!Array.isArray(devices) || cards.length !== devices.length) {
            const html = devices?.length ? devices.map(renderDeviceCardRealtime).join("") : '<div class="system-log empty">暂无数据</div>';
            if (grid.dataset.signature !== html) {
                grid.innerHTML = html;
                grid.dataset.signature = html;
            }
            return;
        }

        devices.forEach(device => {
            const key = normalizeDeviceId(device.id || device.name);
            const card = cards.find(item => item.dataset.deviceCard === key);
            if (!card) return;
            const updatedAt = parseTimestamp(device.timestamp);
            const airState = getDeviceAirQualityState(device);
            const values = {
                temperature: formatDeviceSensorValue(device, "temperature", "°C"),
                humidity: formatDeviceSensorValue(device, "humidity", "%"),
                pressure: formatDeviceSensorValue(device, "pressure", " hPa"),
                aqi: formatDeviceAirQuality(device),
                lastReported: updatedAt ? formatTime(updatedAt) : (device.online === false ? "设备离线" : "暂无数据")
            };

            setStableTitle(card, "");
            setStableText(card.querySelector("[data-device-name]"), device.name);
            const status = card.querySelector("[data-device-status]");
            setStableClass(status, `state-badge state-${getStatusClass(device.online)}`);
            setStableText(status, getStatusText(device.online));
            Object.entries(values).forEach(([metric, value]) => {
                const metricNode = card.querySelector(`[data-device-metric="${metric}"]`);
                if (!metricNode) return;
                if (metric === "aqi") {
                    setStableClass(metricNode, `s3-sensor-metric aqi-${airState.className}`);
                }
                setStableText(metricNode.querySelector("[data-device-metric-value]"), value);
            });
        });
    }

    function updateStableDashboard(container, data, summary) {
        const root = container.querySelector(".s3-dashboard");
        if (!root) return false;

        const header = root.querySelector(".s3-page-header");
        setStableText(header?.querySelector("p"), "家庭环境与设备状态");

        getHomeSummaryItems(summary).forEach(item => {
            const node = root.querySelector(`[data-summary-key="${item.key}"]`);
            setStableText(node?.querySelector("[data-summary-value]"), item.value);
        });

        updateDeviceCards(root, data.devices || []);
        updateSmartHomeOverview(root, data);

        const alarmRows = renderAlarmRows(data);
        const alarmBody = root.querySelector("[data-alarm-rows]");
        if (alarmBody && alarmBody.dataset.signature !== alarmRows) {
            alarmBody.innerHTML = alarmRows;
            alarmBody.dataset.signature = alarmRows;
        }

        const events = Array.isArray(data.system_events) ? data.system_events : [];
        const timeline = realtime().EventTimeline
            ? realtime().EventTimeline(events)
            : '<div class="system-log empty">暂无系统事件。</div>';
        const timelineBody = root.querySelector("[data-event-timeline]");
        if (timelineBody && timelineBody.dataset.signature !== timeline) {
            timelineBody.innerHTML = timeline;
            timelineBody.dataset.signature = timeline;
        }

        return true;
    }

    function renderReady(container, data, summary) {
        container.innerHTML = `
            <div class="s3-dashboard">
                <div class="s3-page-header">
                    <div>
                        <h1>家庭环境总览</h1>
                        <p>家庭环境与设备状态</p>
                    </div>
                </div>
                ${renderHomeSummary(summary)}
                ${renderDeviceOverview(data.devices || [])}
                ${renderSmartHomeOverview(data)}
                ${renderAlarmPanel(data)}
                ${renderSystemEventsPanel(data)}
            </div>
        `;
        container.dataset.s3DashboardReady = "true";
    }

    function renderLoading(container) {
        const data = normalizeOverview(null, [], [], null, {});
        const summary = buildHomeSummary(data.devices || []);
        container.dataset.s3DashboardReady = "false";
        container.innerHTML = `
            <div class="s3-dashboard">
                <div class="s3-page-header">
                    <div>
                        <h1>家庭环境总览</h1>
                        <p>Loading...</p>
                    </div>
                </div>
                ${renderHomeSummary(summary)}
                ${renderDeviceOverview(data.devices || [])}
                ${renderSmartHomeOverview(data)}
                ${renderAlarmPanel(data)}
                ${renderSystemEventsPanel(data)}
            </div>
        `;
    }

    function renderError(container) {
        const data = normalizeOverview(null, [], [], null, {
            deviceStatusError: true,
            moduleError: true,
            alarmError: true
        });
        const summary = buildHomeSummary(data.devices || []);
        container.dataset.s3DashboardReady = "false";
        container.innerHTML = `
            <div class="s3-dashboard">
                <div class="s3-page-header">
                    <div>
                        <h1>家庭环境总览</h1>
                        <p>${UNAVAILABLE_TEXT}</p>
                    </div>
                </div>
                ${renderHomeSummary(summary)}
                ${renderDeviceOverview(data.devices || [])}
                ${renderSmartHomeOverview(data)}
                ${renderAlarmPanel(data)}
                ${renderSystemEventsPanel(data)}
            </div>
        `;
    }

    async function render(container) {
        if (!container) return;
        if (!container.querySelector(".s3-dashboard")) {
            renderLoading(container);
        }
        const requestStart = performance.now();
        try {
            const [
                overviewRaw,
                deviceStatusRaw,
                modulesResult,
                alarmsResult,
                eventsResult,
                ...targetResults
            ] = await Promise.allSettled([
                fetchOverview(),
                fetchDeviceStatus(),
                fetchModulesStatus(),
                fetchAlarmLogs(),
                fetchSystemEvents(),
                ...TARGET_DEVICE_IDS.map(deviceId => fetchOverview(deviceId)),
                ...TARGET_DEVICE_IDS.map(deviceId => fetchDeviceStatus(deviceId))
            ]);
            const targetOverviewResults = targetResults.slice(0, TARGET_DEVICE_IDS.length);
            const targetDeviceStatusResults = targetResults.slice(TARGET_DEVICE_IDS.length);

            if (overviewRaw.status !== "fulfilled") {
                throw overviewRaw.reason;
            }

            const data = normalizeOverview(
                overviewRaw.value,
                modulesResult.status === "fulfilled" ? modulesResult.value : [],
                alarmsResult.status === "fulfilled" ? alarmsResult.value : [],
                deviceStatusRaw.status === "fulfilled" ? deviceStatusRaw.value : null,
                {
                    deviceStatusError: deviceStatusRaw.status === "rejected",
                    moduleError: modulesResult.status === "rejected",
                    alarmError: alarmsResult.status === "rejected",
                    eventError: eventsResult.status === "rejected"
                },
                targetOverviewResults
                    .filter(result => result.status === "fulfilled")
                    .map(result => result.value),
                targetDeviceStatusResults
                    .filter(result => result.status === "fulfilled")
                    .map(result => result.value),
                eventsResult.status === "fulfilled" ? eventsResult.value : [],
                {
                    api_ok: true,
                    api_latency_ms: performance.now() - requestStart,
                    last_sync_at: Date.now()
                }
            );
            realtime().markSuccess?.({
                syncAt: Date.now(),
                dataAt: getLatestDataTimestamp(data) || Date.now(),
                apiOk: true,
                apiLatencyMs: performance.now() - requestStart,
                page: "s3"
            });
            const summary = data.home_summary || buildHomeSummary(data.devices || []);
            if (container.dataset.s3DashboardReady === "true") {
                updateStableDashboard(container, data, summary);
            } else {
                renderReady(container, data, summary);
            }
        } catch (error) {
            console.warn("[S3Dashboard] overview request failed", error.message);
            if (container.dataset.s3DashboardReady === "true") {
                const message = container.querySelector(".s3-page-header p");
                setStableText(message, UNAVAILABLE_TEXT);
            } else {
                renderError(container);
            }
        }
    }

    window.S3Dashboard = {
        render
    };
})();
