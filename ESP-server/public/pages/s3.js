(function () {
    const EMPTY_TEXT = "暂无数据";
    const ERROR_TEXT = "接口请求失败";
    const UNAVAILABLE_TEXT = "无法获取数据";
    const OFFLINE_TEXT = "离线";
    const UNKNOWN_TEXT = "未知";
    const DISCONNECTED_TEXT = "未连接";
    const TARGET_DEVICE_IDS = ["C51", "C52"];
    const previousRealtimeValues = new Map();

    const applianceSlots = [
        { key: "air_conditioner", label: "空调", icon: "❄️" },
        { key: "fan", label: "风扇", icon: "🌀" },
        { key: "light", label: "灯", icon: "💡" },
        { key: "tv", label: "TV", icon: "▣" },
        { key: "curtain", label: "窗帘", icon: "▥" },
        { key: "humidifier", label: "加湿器", icon: "💧" },
        { key: "air_purifier", label: "空气净化器", icon: "◌" }
    ];

    const commandDisplayMap = {
        "light.turn_on": "打开灯",
        "light.turn_off": "关闭灯",
        "air_conditioner.set_temperature": "设置空调温度",
        "air_conditioner.turn_on": "打开空调",
        "air_conditioner.turn_off": "关闭空调",
        "fan.turn_on": "打开风扇",
        "fan.turn_off": "关闭风扇",
        "tv.turn_on": "打开电视",
        "tv.turn_off": "关闭电视",
        "curtain.open": "打开窗帘",
        "curtain.close": "关闭窗帘",
        "humidifier.turn_on": "打开加湿器",
        "humidifier.turn_off": "关闭加湿器",
        "air_purifier.turn_on": "打开空气净化器",
        "air_purifier.turn_off": "关闭空气净化器",
        "air_quality.read": "读取空气质量",
        "temperature.read": "读取温度",
        "humidity.read": "读取湿度"
    };

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

    function formatInteger(value) {
        const numeric = Number(value);
        if (!Number.isFinite(numeric)) return DISCONNECTED_TEXT;
        return Math.round(numeric).toLocaleString("zh-CN");
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

    function relativeTime(value) {
        return realtime().formatRelativeTime ? realtime().formatRelativeTime(value) : "";
    }

    function updateTime(value, source, api) {
        return realtime().UpdateTime
            ? realtime().UpdateTime(value, source, api)
            : `<span class="update-time">${escapeHtml(formatTime(value))}</span>`;
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
            llm_pending: "LLM 处理中",
            forwarded: "已转发",
            resolved: "已解决",
            archived: "已归档",
            info: "信息",
            warning: "警告",
            critical: "严重"
        };
        if (status === true) return "在线";
        if (status === false) return OFFLINE_TEXT;
        return textMap[status] || status || UNKNOWN_TEXT;
    }

    function boolText(value, trueText, falseText) {
        if (value === true) return trueText;
        if (value === false) return falseText;
        return DISCONNECTED_TEXT;
    }

    function localizeCommandText(value) {
        const text = String(value ?? "");
        return commandDisplayMap[text] || text || EMPTY_TEXT;
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
        let icon = "📡";
        let type = eventType || "系统事件";
        let description = message || EMPTY_TEXT;
        if (command) {
            icon = "💡";
            type = "命令事件";
            description = `${localizeCommandText(command)} ${item.status ? getStatusText(item.status) : ""}`.trim();
        } else if (String(eventType).toLowerCase().includes("voice") || item.asr_text || payload.asr_text) {
            icon = "🎤";
            type = "语音命令";
            description = message || item.asr_text || payload.asr_text || "收到语音命令";
        } else if (String(eventType).toLowerCase().includes("llm") || item.response || payload.response) {
            icon = "🤖";
            type = "AI 回复";
            description = message || "AI 回复完成";
        } else if (item.device_id || payload.device_id) {
            icon = "📡";
            type = item.device_id || payload.device_id;
            description = message || `${type} 上传新的传感器数据`;
        }
        return {
            timestamp: time,
            icon,
            type,
            description,
            detail: [
                item.device_id ? `设备：${item.device_id}` : "",
                eventType ? `类型：${eventType}` : "",
                item.status ? `状态：${item.status}` : ""
            ].filter(Boolean).join("\n")
        };
    }

    function normalizeSystemEvents(records) {
        return (Array.isArray(records) ? records : [])
            .map(normalizeSystemEvent)
            .filter(event => event.timestamp || event.description)
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
            name: gateway.name || gateway.gateway_id || "S3 Gateway",
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
            id: device.device_id || device.id || "",
            name: device.name || device.device_id || device.id || UNKNOWN_TEXT,
            room: rawRoom && rawRoom !== "unassigned" ? rawRoom : "未分配",
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
            name: deviceId,
            room: "未分配",
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
        const extras = Array.from(byId.entries())
            .filter(([id]) => !TARGET_DEVICE_IDS.includes(id))
            .map(([, device]) => device);
        return [...orderedTargets, ...extras];
    }

    function normalizeAlarm(rawAlarm) {
        const alarm = isPlainObject(rawAlarm) ? rawAlarm : {};
        const payload = isPlainObject(alarm.payload) ? alarm.payload : {};
        return {
            event_id: alarm.event_id || "",
            device_id: alarm.device_id || "",
            event_type: alarm.event_type || payload.event_type || UNKNOWN_TEXT,
            severity: alarm.severity || payload.severity || "info",
            message: alarm.local_action ||
                payload.message ||
                payload.summary ||
                payload.description ||
                payload.reason ||
                "",
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

    function normalizeOverview(data, modules = [], commands = [], alarms = [], deviceStatus = null, states = {}, relatedOverviews = [], deviceStatuses = [], systemEvents = [], requestMeta = {}) {
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
            recent_commands: commands,
            recent_alarms: Array.isArray(alarms) ? alarms.map(normalizeAlarm) : [],
            system_events: normalizeSystemEvents(systemEvents),
            device_status_error: Boolean(states.deviceStatusError),
            module_error: Boolean(states.moduleError),
            command_error: Boolean(states.commandError),
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
                avg_temperature: DISCONNECTED_TEXT,
                avg_humidity: DISCONNECTED_TEXT,
                avg_air_quality: DISCONNECTED_TEXT
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
            if (!values.length) return DISCONNECTED_TEXT;
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

    function renderStatusTile(label, value, status) {
        return `
            <div class="s3-status-tile">
                <span>${escapeHtml(label)}</span>
                <strong>${escapeHtml(value)}</strong>
                <i class="s3-status-line ${escapeHtml(status)}"></i>
            </div>
        `;
    }

    function renderSystemHealthBar(data) {
        const gateway = data.gateway || {};
        const meta = data.request_meta || {};
        const modules = Array.isArray(gateway.modules) ? gateway.modules : [];
        const mqttModule = modules.find(module => /mqtt/i.test(String(module.module_type || module.name || module.id || "")));
        const mqttOnline = mqttModule
            ? (mqttModule.online === true || mqttModule.module_online === true)
            : (gateway.cloud_connected === true ? true : null);
        const dataTimestamp = getLatestDataTimestamp(data);
        const streamElapsed = dataTimestamp ? Date.now() - dataTimestamp.getTime() : Infinity;
        const streamStatus = streamElapsed <= 5000 ? "normal" : (streamElapsed <= 30000 ? "warning" : "danger");
        const apiOk = meta.api_ok !== false;
        const apiLatency = Number(meta.api_latency_ms);
        const items = [
            {
                label: "ESP32 Gateway",
                value: gateway.online === true ? "在线" : (gateway.online === false ? "离线" : UNKNOWN_TEXT),
                status: gateway.online === true ? "normal" : (gateway.online === false ? "danger" : "warning"),
                detail: `来源：/api/dashboard/v1/overview\n更新时间：${formatTime(dataTimestamp)}`
            },
            {
                label: "MQTT",
                value: mqttOnline === true ? "已连接" : (mqttOnline === false ? "未连接" : "重连中"),
                status: mqttOnline === true ? "normal" : (mqttOnline === false ? "danger" : "warning"),
                detail: `来源：/api/dashboard/v1/modules/status\n模块：${mqttModule?.module_type || "gateway/cloud"}`
            },
            {
                label: "API",
                value: apiOk ? `正常${Number.isFinite(apiLatency) ? ` · ${Math.round(apiLatency)} ms` : ""}` : "异常",
                status: apiOk ? "normal" : "danger",
                detail: `来源：Dashboard 请求\n响应时间：${Number.isFinite(apiLatency) ? `${Math.round(apiLatency)} ms` : UNKNOWN_TEXT}`
            },
            {
                label: "Data Stream",
                value: streamStatus === "normal" ? "实时更新" : (streamStatus === "warning" ? "等待数据" : "停止更新"),
                status: streamStatus,
                detail: dataTimestamp ? `最近数据：${formatTime(dataTimestamp)}\n${relativeTime(dataTimestamp)}` : "尚未收到数据"
            },
            {
                label: "Last Sync",
                value: meta.last_sync_at ? relativeTime(meta.last_sync_at) : UNKNOWN_TEXT,
                status: meta.last_sync_at ? "normal" : "warning",
                detail: meta.last_sync_at ? `同步时间：${formatTime(meta.last_sync_at)}` : "尚未同步"
            }
        ];
        return `<section class="s3-health-bar" aria-label="系统实时状态">
            ${items.map(item => `
                <div class="s3-health-item health-${item.status}" title="${escapeHtml(item.detail)}">
                    <span><i aria-hidden="true"></i>${escapeHtml(item.label)}</span>
                    <strong>${escapeHtml(item.value)}</strong>
                </div>
            `).join("")}
        </section>`;
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

    function formatAirQuality(sensors) {
        const score = Number(sensors?.air_quality_score);
        if (!Number.isFinite(score)) return DISCONNECTED_TEXT;
        const level = sensors.air_quality_level ? ` · ${sensors.air_quality_level}` : "";
        return `${formatNumber(score, 0)} 分${level}`;
    }

    function formatSensorValue(value, unit, digits = 1) {
        const numeric = Number(value);
        if (!Number.isFinite(numeric)) return DISCONNECTED_TEXT;
        return `${formatNumber(numeric, digits)}${unit}`;
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
                state: "未接入",
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
        const explicitOn = appliance.on ?? appliance.enabled ?? appliance.power ?? appliance.online;
        const openPercent = toNumberOrNull(appliance.open_percent);
        const speed = toNumberOrNull(appliance.speed);
        const isActive = typeof explicitOn === "boolean"
            ? explicitOn
            : (openPercent !== null ? openPercent > 0 : false);
        const stateParts = [];
        if (rawState) {
            stateParts.push(rawState);
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

    function formatModuleName(moduleType) {
        const text = String(moduleType || "").trim();
        return text || "未命名模块";
    }

    function renderModuleRows(data) {
        const modules = Array.isArray(data.gateway?.modules) ? data.gateway.modules : [];
        if (data.module_error) {
            return `<div class="system-log empty">${ERROR_TEXT}</div>`;
        }
        if (!modules.length) {
            return '<div class="system-log empty">暂无数据</div>';
        }

        return modules.map(module => {
            const online = module.online === true || module.module_online === true;
            const delay = Number(module.latest_upload_delay_ms);
            const age = Number(module.last_seen_age_ms);
            const detailParts = [];
            if (Number.isFinite(delay)) {
                detailParts.push(`延迟 ${formatInteger(delay)} ms`);
            }
            if (Number.isFinite(age)) {
                detailParts.push(`最近 ${formatInteger(age)} ms 前`);
            }
            return `
                <div class="s3-module-row">
                    <span class="status-dot ${online ? "online" : ""}"></span>
                    <div>
                        <strong>${escapeHtml(formatModuleName(module.module_type))}</strong>
                        <small>${escapeHtml(detailParts.join(" · ") || "暂无数据")}</small>
                    </div>
                    <span class="level-badge level-${getStatusClass(online)}">${getStatusText(online)}</span>
                </div>
            `;
        }).join("");
    }

    function renderSystemStatus(data) {
        const gateway = data.gateway || {};
        const gatewayOnline = gateway.online === true;
        const cloudConnected = gateway.cloud_connected === true;
        const latency = Number(gateway.latency_ms);
        const latencyStatus = Number.isFinite(latency)
            ? (latency <= 80 ? "normal" : "warning")
            : "danger";
        const localDegraded = gateway.local_degraded === true;
        const gatewayTiles = [
            {
                label: "S3 在线状态",
                value: gateway.online === null ? UNKNOWN_TEXT : boolText(gateway.online, "在线", "离线"),
                status: getStatusClass(gateway.online)
            },
            {
                label: "云端连接状态",
                value: gateway.cloud_connected === null ? DISCONNECTED_TEXT : boolText(gateway.cloud_connected, "已连接", "未连接"),
                status: getStatusClass(gateway.cloud_connected)
            },
            {
                label: "延迟",
                value: Number.isFinite(latency) ? `${formatInteger(latency)} ms` : DISCONNECTED_TEXT,
                status: latencyStatus
            },
            {
                label: "本地降级状态",
                value: gateway.local_degraded === null ? DISCONNECTED_TEXT : boolText(gateway.local_degraded, "已启用", "未启用"),
                status: localDegraded ? "warning" : (gatewayOnline || cloudConnected ? "normal" : "danger")
            }
        ];
        const rows = renderModuleRowsRealtime(data);

        return `
            <article class="panel s3-gateway-panel">
                <div class="panel-header">
                    <h2>系统状态</h2>
                </div>
                <div class="s3-status-grid" aria-label="Gateway 状态">
                    ${gatewayTiles.map(tile => renderStatusTile(tile.label, tile.value, tile.status)).join("")}
                </div>
                <div class="s3-module-list" aria-label="系统状态">${rows}</div>
            </article>
        `;
    }

    function renderModuleRowsRealtime(data) {
        const modules = Array.isArray(data.gateway?.modules) ? data.gateway.modules : [];
        if (data.module_error) {
            return `<div class="system-log empty">${ERROR_TEXT}</div>`;
        }
        if (!modules.length) {
            return '<div class="system-log empty">暂无数据</div>';
        }
        return modules.map(module => {
            const online = module.online === true || module.module_online === true;
            const moduleTime = parseTimestamp(module.last_seen_ms || module.updated_at || module.timestamp || data.request_meta?.last_sync_at);
            const delay = Number(module.latest_upload_delay_ms);
            const age = Number(module.last_seen_age_ms);
            const detailParts = [];
            if (Number.isFinite(delay)) detailParts.push(`延迟 ${formatInteger(delay)} ms`);
            if (Number.isFinite(age)) detailParts.push(`最近 ${formatInteger(age)} ms 前`);
            return `
                <div class="s3-module-row" title="${escapeHtml(`来源：/api/dashboard/v1/modules/status\n更新时间：${moduleTime ? formatTime(moduleTime) : EMPTY_TEXT}`)}">
                    <span class="status-dot ${online ? "online" : ""}"></span>
                    <div>
                        <strong>${escapeHtml(formatModuleName(module.module_type))}</strong>
                        <small>${escapeHtml(detailParts.join(" · ") || "暂无数据")}</small>
                        <small>${updateTime(moduleTime, "Gateway Module", "/api/dashboard/v1/modules/status")}</small>
                    </div>
                    <span class="level-badge level-${getStatusClass(online)}">${getStatusText(online)}</span>
                </div>
            `;
        }).join("");
    }

    function renderSystemStatusRealtime(data) {
        const gateway = data.gateway || {};
        const gatewayOnline = gateway.online === true;
        const cloudConnected = gateway.cloud_connected === true;
        const latency = Number(gateway.latency_ms);
        const latencyStatus = Number.isFinite(latency)
            ? (latency <= 80 ? "normal" : "warning")
            : "danger";
        const localDegraded = gateway.local_degraded === true;
        const gatewayTiles = [
            {
                label: "S3 在线状态",
                value: gateway.online === null ? UNKNOWN_TEXT : boolText(gateway.online, "在线", "离线"),
                status: getStatusClass(gateway.online)
            },
            {
                label: "云端连接状态",
                value: gateway.cloud_connected === null ? DISCONNECTED_TEXT : boolText(gateway.cloud_connected, "已连接", "未连接"),
                status: getStatusClass(gateway.cloud_connected)
            },
            {
                label: "延迟",
                value: Number.isFinite(latency) ? `${formatInteger(latency)} ms` : DISCONNECTED_TEXT,
                status: latencyStatus
            },
            {
                label: "本地降级状态",
                value: gateway.local_degraded === null ? DISCONNECTED_TEXT : boolText(gateway.local_degraded, "已启用", "未启用"),
                status: localDegraded ? "warning" : (gatewayOnline || cloudConnected ? "normal" : "danger")
            }
        ];
        return `
            <article class="panel s3-gateway-panel" title="${escapeHtml(`来源：/api/dashboard/v1/overview\n更新时间：${formatTime(getLatestDataTimestamp(data))}`)}">
                <div class="panel-header">
                    <h2>系统状态</h2>
                    ${updateTime(getLatestDataTimestamp(data), "S3 Gateway", "/api/dashboard/v1/overview")}
                </div>
                <div class="s3-status-grid" aria-label="Gateway 状态">
                    ${gatewayTiles.map(tile => renderStatusTile(tile.label, tile.value, tile.status)).join("")}
                </div>
                <div class="s3-module-list" aria-label="系统模块状态">${renderModuleRowsRealtime(data)}</div>
            </article>
        `;
    }

    function renderHomeSummary(summary) {
        const items = [
            { label: "全屋平均温度", value: summary.avg_temperature === DISCONNECTED_TEXT ? DISCONNECTED_TEXT : `${summary.avg_temperature}°C`, accent: "blue" },
            { label: "全屋平均湿度", value: summary.avg_humidity === DISCONNECTED_TEXT ? DISCONNECTED_TEXT : `${summary.avg_humidity}%`, accent: "green" },
            { label: "平均空气质量", value: summary.avg_air_quality === DISCONNECTED_TEXT ? DISCONNECTED_TEXT : `${summary.avg_air_quality} 分`, accent: "purple" },
            {
                label: "在线 / 离线设备",
                value: summary.online_device_count === null ||
                    summary.offline_device_count === null
                    ? EMPTY_TEXT
                    : `${summary.online_device_count} / ${summary.offline_device_count}`,
                accent: "orange"
            }
        ];

        return `
            <article class="panel s3-summary-panel">
                <div class="panel-header">
                    <h2>全屋概览</h2>
                </div>
                <div class="s3-summary-grid">
                    ${items.map(item => `
                        <div class="s3-summary-tile ${item.accent}">
                            <span>${escapeHtml(item.label)}</span>
                            <strong>${escapeHtml(item.value)}</strong>
                        </div>
                    `).join("")}
                </div>
            </article>
        `;
    }

    function renderSensorMetric(label, value) {
        return `
            <div class="s3-sensor-metric">
                <span>${escapeHtml(label)}</span>
                <strong>${escapeHtml(value)}</strong>
            </div>
        `;
    }

    function renderDeviceCard(device) {
        const sensors = device.sensors;
        const online = device.online === true;
        return `
            <article class="panel s3-device-card">
                <div class="panel-header">
                    <h2>${escapeHtml(device.name)}</h2>
                    <span class="state-badge state-${getStatusClass(device.online)}">${getStatusText(device.online)}</span>
                </div>
                <div class="s3-sensor-grid">
                    ${renderSensorMetric("温度", formatSensorValue(sensors.temperature, "°C"))}
                    ${renderSensorMetric("湿度", formatSensorValue(sensors.humidity, "%"))}
                    ${renderSensorMetric("气压", formatSensorValue(sensors.pressure, " hPa"))}
                </div>
                <div class="s3-sensor-grid">
                    ${renderSensorMetric("空气质量", formatAirQuality(sensors))}
                    ${renderSensorMetric("最近上报", device.timestamp ? formatTime(device.timestamp) : DISCONNECTED_TEXT)}
                    ${renderSensorMetric("占用状态", online && device.occupancy?.available ? getStatusText(device.occupancy.state) : DISCONNECTED_TEXT)}
                </div>
                <div class="s3-appliance-grid" aria-label="${escapeHtml(device.room)}设备状态">
                    ${applianceSlots.map(slot => {
                        const appliance = getApplianceStatus(device.appliances, slot, online);
                        return `
                            <div class="s3-appliance ${appliance.isActive ? "is-on" : "is-off"}">
                                <span class="s3-appliance-icon" aria-hidden="true">${appliance.icon}</span>
                                <strong>${escapeHtml(appliance.label)}</strong>
                                <small>${escapeHtml(appliance.state)}</small>
                            </div>
                        `;
                    }).join("")}
                </div>
            </article>
        `;
    }

    function renderSensorMetricRealtime(label, value, meta = {}) {
        const statusClass = meta.statusClass ? ` ${meta.statusClass}` : "";
        const tooltip = meta.tooltip ? ` title="${escapeHtml(meta.tooltip)}"` : "";
        return `
            <div class="s3-sensor-metric${statusClass}"${tooltip}>
                <span>${escapeHtml(label)}</span>
                <strong data-realtime-value="${escapeHtml(meta.valueKey || label)}">${escapeHtml(value)}</strong>
                ${meta.update ? `<small>${meta.update}</small>` : ""}
            </div>
        `;
    }

    function renderDeviceCardRealtime(device) {
        const sensors = device.sensors;
        const online = device.online === true;
        const updatedAt = parseTimestamp(device.timestamp);
        const api = `/api/dashboard/v1/overview?device_id=${encodeURIComponent(device.id || "")}`;
        const airState = realtime().getAirQualityState
            ? realtime().getAirQualityState(sensors.air_quality_score)
            : { label: sensors.air_quality_level || UNKNOWN_TEXT, className: "unknown" };
        const tooltip = field => [
            `来源：ESP32 ${device.id || device.name}`,
            updatedAt ? `更新时间：${formatTime(updatedAt)}` : "",
            `API：${api}`,
            `字段：${field}`
        ].filter(Boolean).join("\n");
        return `
            <article class="panel s3-device-card" title="${escapeHtml(`来源：ESP32 ${device.id || device.name}\n更新时间：${updatedAt ? formatTime(updatedAt) : EMPTY_TEXT}\nAPI：${api}`)}">
                <div class="panel-header">
                    <div>
                        <h2>${escapeHtml(device.name)}</h2>
                        ${updateTime(updatedAt, `ESP32 ${device.id || device.name}`, api)}
                    </div>
                    <span class="state-badge state-${getStatusClass(device.online)}">${getStatusText(device.online)}</span>
                </div>
                <div class="s3-sensor-grid">
                    ${renderSensorMetricRealtime("温度", formatSensorValue(sensors.temperature, "°C"), { tooltip: tooltip("temperature"), update: updateTime(updatedAt, device.id, api), valueKey: `${device.id}-temperature` })}
                    ${renderSensorMetricRealtime("湿度", formatSensorValue(sensors.humidity, "%"), { tooltip: tooltip("humidity"), update: updateTime(updatedAt, device.id, api), valueKey: `${device.id}-humidity` })}
                    ${renderSensorMetricRealtime("气压", formatSensorValue(sensors.pressure, " hPa"), { tooltip: tooltip("pressure"), update: updateTime(updatedAt, device.id, api), valueKey: `${device.id}-pressure` })}
                </div>
                <div class="s3-sensor-grid">
                    ${renderSensorMetricRealtime("AQI", `${formatAirQuality(sensors)} · ${airState.label}`, { tooltip: tooltip("air_quality_score"), update: updateTime(updatedAt, device.id, api), statusClass: `aqi-${airState.className}`, valueKey: `${device.id}-aqi` })}
                    ${renderSensorMetricRealtime("最近上报", updatedAt ? formatTime(updatedAt) : DISCONNECTED_TEXT, { update: updateTime(updatedAt, device.id, api) })}
                    ${renderSensorMetricRealtime("占用状态", online && device.occupancy?.available ? getStatusText(device.occupancy.state) : DISCONNECTED_TEXT, { tooltip: tooltip("occupancy") })}
                </div>
                <div class="s3-appliance-grid" aria-label="${escapeHtml(device.room)}设备状态">
                    ${applianceSlots.map(slot => {
                        const appliance = getApplianceStatus(device.appliances, slot, online);
                        return `
                            <div class="s3-appliance ${appliance.isActive ? "is-on" : "is-off"}" title="${escapeHtml(`来源：智能家居状态\n更新时间：${updatedAt ? formatTime(updatedAt) : EMPTY_TEXT}\nAPI：${api}`)}">
                                <span class="s3-appliance-icon" aria-hidden="true">${appliance.icon}</span>
                                <strong>${escapeHtml(appliance.label)}</strong>
                                <small>${escapeHtml(appliance.state)}</small>
                                <small>${updateTime(updatedAt, appliance.label, api)}</small>
                            </div>
                        `;
                    }).join("")}
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
                    <h2>设备总览</h2>
                </div>
                <div class="s3-device-grid">
                    ${content}
                </div>
            </section>
        `;
    }

    function renderRecentCommandsPanel(data) {
        const commands = data.recent_commands;
        const rows = commands.length
            ? commands.map(command => `
                <tr>
                    <td>${escapeHtml(localizeCommandText(command.command || command.name || command.command_id))}</td>
                    <td>${escapeHtml(command.target || command.device_id || EMPTY_TEXT)}</td>
                    <td><span class="level-badge level-${getStatusClass(command.status)}">${getStatusText(command.status)}</span></td>
                    <td>${escapeHtml(formatTime(command.created_at || command.timestamp))}</td>
                    <td>${escapeHtml(formatTime(command.completed_at || command.updated_at))}</td>
                </tr>
            `).join("")
            : `<tr><td colspan="5" class="table-empty">${data.command_error ? ERROR_TEXT : "暂无命令记录"}</td></tr>`;
        return `
            <article class="panel">
                <div class="panel-header">
                    <h2>最近命令</h2>
                </div>
                <div class="table-wrap">
                    <table class="s3-table">
                        <thead>
                            <tr>
                                <th>命令</th>
                                <th>目标设备</th>
                                <th>状态</th>
                                <th>创建时间</th>
                                <th>完成时间</th>
                            </tr>
                        </thead>
                        <tbody>${rows}</tbody>
                    </table>
                </div>
            </article>
        `;
    }

    function renderAlarmPanel(data) {
        const alarms = Array.isArray(data.recent_alarms) ? data.recent_alarms : [];
        const rows = alarms.length
            ? alarms.map(alarm => `
                <tr>
                    <td>${escapeHtml(formatTime(alarm.created_at))}</td>
                    <td>${escapeHtml(alarm.device_id || EMPTY_TEXT)}</td>
                    <td>${escapeHtml(alarm.event_type)}</td>
                    <td>${escapeHtml(alarm.message || EMPTY_TEXT)}</td>
                    <td><span class="level-badge level-${getStatusClass(alarm.severity)}">${getStatusText(alarm.severity)}</span></td>
                    <td><span class="level-badge level-${getStatusClass(alarm.status)}">${getStatusText(alarm.status)}</span></td>
                </tr>
            `).join("")
            : `<tr><td colspan="6" class="table-empty">${data.alarm_error ? ERROR_TEXT : "暂无报警信息"}</td></tr>`;
        return `
            <article class="panel">
                <div class="panel-header">
                    <h2>报警信息</h2>
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
                                <th>状态</th>
                            </tr>
                        </thead>
                        <tbody>${rows}</tbody>
                    </table>
                </div>
            </article>
        `;
    }

    function renderActivity(data) {
        return `
            <section class="s3-activity-grid">
                ${renderRecentCommandsPanel(data)}
                ${renderAlarmPanel(data)}
            </section>
        `;
    }

    function renderSystemEventsPanel(data) {
        const events = Array.isArray(data.system_events) ? data.system_events : [];
        const timeline = realtime().EventTimeline
            ? realtime().EventTimeline(events)
            : '<div class="system-log empty">暂无系统事件。</div>';
        return `
            <article class="panel s3-events-panel">
                <div class="panel-header">
                    <h2>最近系统事件</h2>
                    ${updateTime(data.request_meta?.last_sync_at, "System Events", "/api/logs/v1/system")}
                </div>
                ${timeline}
            </article>
        `;
    }

    function applyRealtimeValueAnimation(container) {
        container.querySelectorAll("[data-realtime-value]").forEach(element => {
            const key = element.dataset.realtimeValue;
            const numericText = String(element.textContent || "").replace(/[^\d.-]/g, "");
            const next = Number(numericText);
            if (!Number.isFinite(next)) return;
            const previous = previousRealtimeValues.get(key);
            previousRealtimeValues.set(key, next);
            if (!Number.isFinite(previous) || previous === next) return;
            element.classList.remove("value-increase", "value-decrease");
            void element.offsetWidth;
            element.classList.add(next > previous ? "value-increase" : "value-decrease");
            window.setTimeout(() => {
                element.classList.remove("value-increase", "value-decrease");
            }, 500);
        });
    }

    function renderLoading(container) {
        const data = normalizeOverview(null, [], [], [], null, {});
        const summary = buildHomeSummary(data.devices || []);
        container.innerHTML = `
            <div class="s3-dashboard">
                <div class="s3-page-header">
                    <div>
                        <h1>S3 系统总览</h1>
                        <p>Loading...</p>
                    </div>
                    <span class="state-badge state-warning">Loading...</span>
                </div>
                ${renderSystemHealthBar(data)}
                <div class="s3-overview-grid">
                    ${renderSystemStatusRealtime(data)}
                    ${renderHomeSummary(summary)}
                </div>
                ${renderDeviceOverview(data.devices || [])}
                ${renderActivity(data)}
                ${renderSystemEventsPanel(data)}
            </div>
        `;
    }

    function renderError(container) {
        const data = normalizeOverview(null, [], [], [], null, {
            deviceStatusError: true,
            moduleError: true,
            commandError: true,
            alarmError: true
        });
        const summary = buildHomeSummary(data.devices || []);
        container.innerHTML = `
            <div class="s3-dashboard">
                <div class="s3-page-header">
                    <div>
                        <h1>S3 系统总览</h1>
                        <p>${UNAVAILABLE_TEXT}</p>
                    </div>
                    <span class="state-badge state-danger">网关离线</span>
                </div>
                ${renderSystemHealthBar(data)}
                <div class="s3-overview-grid">
                    ${renderSystemStatusRealtime(data)}
                    ${renderHomeSummary(summary)}
                </div>
                ${renderDeviceOverview(data.devices || [])}
                ${renderActivity(data)}
                ${renderSystemEventsPanel(data)}
            </div>
        `;
    }

    async function render(container) {
        if (!container) return;
        renderLoading(container);
        const requestStart = performance.now();
        try {
            const [
                overviewRaw,
                deviceStatusRaw,
                modulesResult,
                commandsResult,
                alarmsResult,
                eventsResult,
                ...targetResults
            ] = await Promise.allSettled([
                fetchOverview(),
                fetchDeviceStatus(),
                fetchModulesStatus(),
                fetchCommandHistory(),
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
                commandsResult.status === "fulfilled" ? commandsResult.value : [],
                alarmsResult.status === "fulfilled" ? alarmsResult.value : [],
                deviceStatusRaw.status === "fulfilled" ? deviceStatusRaw.value : null,
                {
                    deviceStatusError: deviceStatusRaw.status === "rejected",
                    moduleError: modulesResult.status === "rejected",
                    commandError: commandsResult.status === "rejected",
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
            container.innerHTML = `
                <div class="s3-dashboard">
                    <div class="s3-page-header">
                        <div>
                            <h1>S3 系统总览</h1>
                            <p>系统总览与全屋状态面板</p>
                        </div>
                        <span class="state-badge state-${getStatusClass(data.gateway.online)}">${getStatusText(data.gateway.online)}</span>
                    </div>
                    ${renderSystemHealthBar(data)}
                    <div class="s3-overview-grid">
                        ${renderSystemStatusRealtime(data)}
                        ${renderHomeSummary(summary)}
                    </div>
                    ${renderDeviceOverview(data.devices || [])}
                    ${renderActivity(data)}
                    ${renderSystemEventsPanel(data)}
                </div>
            `;
            applyRealtimeValueAnimation(container);
            realtime().renderClock?.();
        } catch (error) {
            console.warn("[S3Dashboard] overview request failed", error.message);
            renderError(container);
        }
    }

    window.S3Dashboard = {
        render
    };
})();
