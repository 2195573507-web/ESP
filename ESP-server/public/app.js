const LEVELS = {
    normal: { label: "正常", className: "normal" },
    warning: { label: "警告", className: "warning" },
    danger: { label: "严重", className: "danger" }
};

const metricDefinitions = {
    temperature: {
        name: "温度",
        unit: "°C",
        accent: "#2874ff",
        icon: "thermometer",
        mockValue: 25.6,
        trend: [21, 22, 23, 20, 20, 22, 24, 25, 23, 22, 25, 24, 26, 25, 28, 26, 24]
    },
    humidity: {
        name: "湿度",
        unit: "%",
        accent: "#10b981",
        icon: "drop",
        mockValue: 58.3,
        trend: [55, 59, 53, 49, 46, 50, 54, 58, 52, 53, 48, 51, 55, 49, 52, 62, 57]
    },
    air: {
        name: "空气质量",
        unit: "",
        accent: "#8a35ea",
        icon: "cloud",
        mockValue: 132,
        trend: [92, 105, 88, 99, 94, 104, 128, 111, 104, 119, 101, 126, 116, 146, 132, 119]
    },
    esp: {
        name: "ESP 状态",
        unit: "",
        accent: "#f97316",
        icon: "chip",
        mockValue: "在线",
        latency: 32,
        trend: [26, 25, 26, 31, 35, 36, 35, 31, 29, 34, 32, 37, 35, 39, 36, 35, 31]
    }
};

const mockSensorData = {
    temperature: 25.6,
    humidity: 58.3,
    aqi: 132,
    timestamp: Date.now()
};

const mockASRData = {
    timestamp: Date.now(),
    text: "暂无真实 ASR 数据，等待设备上传"
};

const mockLLMData = {
    timestamp: Date.now(),
    response: "暂无真实 LLM 数据，等待服务返回"
};

// mockHistoryData: 当前后端没有历史曲线接口，主图暂时使用前端 mock 数据。
const mockHistoryData = [
    { time: "12:00", temperature: 26, humidity: 58, air: 10 },
    { time: "14:00", temperature: 28, humidity: 62, air: 14 },
    { time: "16:00", temperature: 29, humidity: 70, air: 18 },
    { time: "18:00", temperature: 24, humidity: 66, air: 18 },
    { time: "20:00", temperature: 27, humidity: 56, air: 13 },
    { time: "22:00", temperature: 28, humidity: 60, air: 14 },
    { time: "00:00", temperature: 30, humidity: 72, air: 18 },
    { time: "02:00", temperature: 29, humidity: 74, air: 19 },
    { time: "04:00", temperature: 32, humidity: 66, air: 21 },
    { time: "06:00", temperature: 34, humidity: 70, air: 18 },
    { time: "08:00", temperature: 31, humidity: 76, air: 13 },
    { time: "10:00", temperature: 25, humidity: 68, air: 15 },
    { time: "12:00", temperature: 28, humidity: 69, air: 18 }
];

// mockAlertLogs: 当前后端没有报警日志接口，报警日志表格暂时使用前端 mock 数据。
const mockAlertLogs = [
    { time: "13:58:21", type: "空气质量异常", content: "AQI 132，超过阈值 (100)", level: "warning", status: "未处理" },
    { time: "13:55:10", type: "湿度偏高", content: "湿度 78%，超过阈值 (75%)", level: "warning", status: "未处理" },
    { time: "13:50:02", type: "ESP 离线", content: "超过 10 秒未收到心跳", level: "danger", status: "已恢复" },
    { time: "13:45:30", type: "温度偏高", content: "温度 36.2°C，超过阈值 (35°C)", level: "warning", status: "已恢复" }
];

const mockSystemLogs = [
    { time: "13:58:24", text: "系统运行正常，等待最新设备数据", color: "#2874ff", source: "mock" },
    { time: "13:58:20", text: "数据上传通道已就绪", color: "#10b981", source: "mock" },
    { time: "13:58:10", text: "设备连接状态监测中", color: "#7c3aed", source: "mock" }
];

const SMART_HOME_UNAVAILABLE_MESSAGE = "当前服务暂未提供智能家居状态接口。";
const SMART_HOME_DEVICES = [
    { id: "air_conditioner", name: "空调", icon: "air-conditioner" },
    { id: "fan", name: "风扇", icon: "fan" },
    { id: "door", name: "门", icon: "door" },
    { id: "light", name: "灯", icon: "light" },
    { id: "air_purifier", name: "空气净化器", icon: "air-purifier" },
    { id: "humidifier", name: "加湿器", icon: "humidifier" }
];

// 智能家居控制：集中维护前端设备配置；当前没有真实接口时全部置为 disabled。
function createSmartHomeDeviceState() {
    return SMART_HOME_DEVICES.map(device => ({
        ...device,
        status: null,
        disabled: true,
        loading: false,
        unavailableReason: SMART_HOME_UNAVAILABLE_MESSAGE
    }));
}

let dashboardState = {
    sensor: mockSensorData,
    asr: mockASRData,
    llm: mockLLMData,
    metrics: {},
    history: mockHistoryData,
    alertLogs: mockAlertLogs,
    systemLogs: mockSystemLogs,
    commandLogs: [],
    operationLogs: [],
    csiHistory: [],
    smartHomeDevices: createSmartHomeDeviceState(),
    sources: {
        sensor: "mock",
        asr: "mock",
        llm: "mock"
    }
};

let lastSourceSignature = "";
const THEME_STORAGE_KEY = "dashboardTheme";
const ESP_ONLINE_THRESHOLD_MS = 10000;
const DASHBOARD_REFRESH_INTERVAL_MS = 3000;
const ESP_DELAY_REFRESH_INTERVAL_MS = 1000;
const CHART_RANGE_OPTIONS = [12, 24, 36, 48];
const DEFAULT_CHART_RANGE_HOURS = 24;
const ALERT_LOG_PREVIEW_LIMIT = 4;
const SYSTEM_LOG_PREVIEW_LIMIT = 4;
const OPERATION_LOG_PREVIEW_LIMIT = 5;
const CUSTOM_COMMAND_MAX_LENGTH = 500;
let dashboardRefreshTimer = null;
let espDelayRefreshTimer = null;
let selectedChartRangeHours = DEFAULT_CHART_RANGE_HOURS;
let activeLogModalType = null;
let pendingConfirmAction = null;
let pendingSmartHomeAction = null;
let activeDashboardPage = "c51";
let s3DashboardRendered = false;

// 主题功能：读取 CSS 主题变量，Canvas 图表调用它来适配黑色/白色背景。
function readThemeColor(name, fallback) {
    const value = getComputedStyle(document.body).getPropertyValue(name).trim();
    return value || fallback;
}

// 主题功能：更新黑白切换按钮文字；dark 显示“白色模式”，light 显示“黑色模式”。
function updateThemeButtonText(button, theme) {
    if (theme === "dark") {
        button.textContent = "☀️ 白色模式";
    } else {
        button.textContent = "🌙 黑色模式";
    }
}

// 主题功能：初始化右上角黑白背景切换按钮；页面加载时读取 localStorage，默认使用 dark。
function initThemeToggle() {
    const themeToggleBtn = document.getElementById("themeToggleBtn");
    if (!themeToggleBtn) {
        console.warn("未找到主题切换按钮 themeToggleBtn");
        return;
    }

    const savedTheme = localStorage.getItem(THEME_STORAGE_KEY) || "dark";
    const initialTheme = savedTheme === "light" ? "light" : "dark";
    document.body.dataset.theme = initialTheme;
    updateThemeButtonText(themeToggleBtn, initialTheme);

    themeToggleBtn.addEventListener("click", () => {
        const currentTheme = document.body.dataset.theme || "dark";
        const nextTheme = currentTheme === "dark" ? "light" : "dark";

        document.body.dataset.theme = nextTheme;
        localStorage.setItem(THEME_STORAGE_KEY, nextTheme);
        updateThemeButtonText(themeToggleBtn, nextTheme);

        if (typeof window.updateChartTheme === "function") {
            window.updateChartTheme(nextTheme);
        }
    });
}

function isPlainObject(value) {
    return Boolean(value) && typeof value === "object" && !Array.isArray(value);
}

function hasData(value) {
    return isPlainObject(value) && Object.keys(value).length > 0;
}

function unwrapDashboardV1Data(value, key) {
    if (isPlainObject(value) && value.ok === true && isPlainObject(value.data)) {
        return key ? value.data[key] : value.data;
    }

    return value;
}

function formatTime(timestamp = Date.now()) {
    const date = parseTimestamp(timestamp) || new Date();
    return date.toLocaleTimeString("zh-CN", {
        hour12: false,
        hour: "2-digit",
        minute: "2-digit",
        second: "2-digit"
    });
}

// 曲线时间范围：格式化横轴标签，36/48 小时时显示日期，避免跨天数据看不清。
function formatChartTime(timestamp) {
    const date = parseTimestamp(timestamp);
    if (!date) return "--";

    const options = selectedChartRangeHours > 24
        ? { month: "2-digit", day: "2-digit", hour: "2-digit", minute: "2-digit", hour12: false }
        : { hour: "2-digit", minute: "2-digit", hour12: false };
    return date.toLocaleString("zh-CN", options);
}

function parseTimestamp(value) {
    if (value === undefined || value === null || value === "") {
        return null;
    }

    if (typeof value === "number") {
        const milliseconds = value < 10000000000 ? value * 1000 : value;
        const date = new Date(milliseconds);
        return Number.isNaN(date.getTime()) ? null : date;
    }

    const numeric = Number(value);
    if (Number.isFinite(numeric)) {
        return parseTimestamp(numeric);
    }

    const date = new Date(value);
    return Number.isNaN(date.getTime()) ? null : date;
}

function pickFirst(data, keys) {
    if (!isPlainObject(data)) {
        return undefined;
    }
    for (const key of keys) {
        if (data[key] !== undefined && data[key] !== null && data[key] !== "") {
            return data[key];
        }
    }
    return undefined;
}

function toNumber(value) {
    const numeric = Number(value);
    return Number.isFinite(numeric) ? numeric : null;
}

// 日志弹窗：所有日志内容写入 HTML 前先转义，防止日志文本破坏页面结构。
function escapeHtml(value) {
    return String(value ?? "")
        .replace(/&/g, "&amp;")
        .replace(/</g, "&lt;")
        .replace(/>/g, "&gt;")
        .replace(/"/g, "&quot;")
        .replace(/'/g, "&#39;");
}

function formatNumber(value, digits = 1) {
    const numeric = toNumber(value);
    if (numeric === null) {
        return "--";
    }
    return Number(numeric.toFixed(digits)).toString();
}

function clamp01(value) {
    const numeric = toNumber(value);
    if (numeric === null) {
        return null;
    }
    return Math.min(Math.max(numeric, 0), 1);
}

// 曲线时间范围：从历史点中读取真实时间戳字段；没有绝对时间的点不参与 12/24/36/48 小时筛选。
function getHistoryPointTimestamp(point) {
    if (!isPlainObject(point)) return null;

    const timestampValue = pickFirst(point, [
        "timestamp",
        "created_at",
        "createdAt",
        "updated_at",
        "updatedAt",
        "received_at",
        "receivedAt",
        "upload_time",
        "uploadTime",
        "time"
    ]);
    return parseTimestamp(timestampValue);
}

// 曲线时间范围：把后端历史数据点规范成 Canvas 可绘制结构，只保留真实数值字段。
function normalizeHistoryPoint(point) {
    if (!isPlainObject(point)) return null;

    const timestamp = getHistoryPointTimestamp(point);
    if (!timestamp) return null;

    const temperature = toNumber(pickFirst(point, ["temperature", "temp"]));
    const humidity = toNumber(pickFirst(point, ["humidity"]));
    const airQualityObject = isPlainObject(point.air_quality) ? point.air_quality : {};
    const air = toNumber(pickFirst(point, [
        "air_quality_score",
        "aqi",
        "air_quality",
        "air"
    ])) ?? toNumber(pickFirst(airQualityObject, ["air_quality_score", "score", "aqi"]));

    if (temperature === null && humidity === null && air === null) {
        return null;
    }

    return {
        timestamp,
        time: formatChartTime(timestamp),
        temperature,
        humidity,
        air
    };
}

// 曲线时间范围：将 dashboard v1 最新传感器数据作为当前点，不新增接口也不修改数据来源。
function getLatestSensorChartPoint() {
    const sensor = dashboardState.sensor;
    if (!sensor || sensor.source !== "real" || !sensor.timestamp) return null;

    return {
        timestamp: sensor.timestamp,
        time: formatChartTime(sensor.timestamp),
        temperature: toNumber(sensor.temperature),
        humidity: toNumber(sensor.humidity),
        air: toNumber(sensor.airValue)
    };
}

// 曲线时间范围：按当前下拉选中的小时数筛选真实时间戳数据，时间戳无效的数据会被跳过。
function getFilteredChartData() {
    const rangeMs = selectedChartRangeHours * 60 * 60 * 1000;
    const now = Date.now();
    const history = Array.isArray(dashboardState.history) ? dashboardState.history : [];
    const points = history
        .map(normalizeHistoryPoint)
        .filter(Boolean);
    const latestPoint = getLatestSensorChartPoint();

    if (latestPoint) {
        const duplicateIndex = points.findIndex(point => point.timestamp.getTime() === latestPoint.timestamp.getTime());
        if (duplicateIndex >= 0) {
            points[duplicateIndex] = latestPoint;
        } else {
            points.push(latestPoint);
        }
    }

    return points
        .filter(point => {
            const timestamp = point.timestamp.getTime();
            return Number.isFinite(timestamp) && timestamp <= now && now - timestamp <= rangeMs;
        })
        .sort((a, b) => a.timestamp.getTime() - b.timestamp.getTime());
}

// 曲线时间范围：更新下拉按钮文字和选中态，供初始化和点击选项后调用。
function updateChartRangeSelector() {
    const label = document.querySelector("[data-range-label]");
    const button = document.getElementById("chartRangeButton");

    if (label) {
        label.textContent = `最近 ${selectedChartRangeHours} 小时`;
    }

    document.querySelectorAll("[data-range-hours]").forEach(option => {
        const selected = Number(option.dataset.rangeHours) === selectedChartRangeHours;
        option.setAttribute("aria-selected", selected ? "true" : "false");
    });

    if (button) {
        button.setAttribute("aria-label", `当前显示最近 ${selectedChartRangeHours} 小时数据`);
    }
}

// 曲线时间范围：初始化自定义下拉菜单，点击选项后只改变前端筛选范围并重绘图表。
function initChartRangeSelector() {
    const selector = document.querySelector("[data-range-selector]");
    const button = document.getElementById("chartRangeButton");
    const menu = document.querySelector("[data-range-menu]");
    if (!selector || !button || !menu) return;

    const closeMenu = () => {
        selector.classList.remove("is-open");
        menu.hidden = true;
        button.setAttribute("aria-expanded", "false");
    };

    const openMenu = () => {
        selector.classList.add("is-open");
        menu.hidden = false;
        button.setAttribute("aria-expanded", "true");
    };

    updateChartRangeSelector();

    button.addEventListener("click", event => {
        event.stopPropagation();
        if (menu.hidden) {
            openMenu();
        } else {
            closeMenu();
        }
    });

    menu.querySelectorAll("[data-range-hours]").forEach(option => {
        option.addEventListener("click", () => {
            const nextHours = Number(option.dataset.rangeHours);
            if (CHART_RANGE_OPTIONS.includes(nextHours)) {
                selectedChartRangeHours = nextHours;
                updateChartRangeSelector();
                renderMainChart();
            }
            closeMenu();
        });
    });

    document.addEventListener("click", event => {
        if (!selector.contains(event.target)) {
            closeMenu();
        }
    });

    document.addEventListener("keydown", event => {
        if (event.key === "Escape") {
            closeMenu();
        }
    });
}

// ESP 延迟显示：把毫秒格式化为 ms、s 或 m s，不写死 1ms。
function formatDelayText(prefix, delayMs, options = {}) {
    const numeric = toNumber(delayMs);
    if (numeric === null) {
        return `${prefix} --`;
    }

    const decimalSeconds = options.decimalSeconds !== false;
    const safeMs = Math.max(0, Math.round(numeric));
    if (safeMs < 1000) {
        return `${prefix} ${safeMs}ms`;
    }

    if (safeMs < 60000) {
        const seconds = decimalSeconds ? (safeMs / 1000).toFixed(1) : String(Math.round(safeMs / 1000));
        return `${prefix} ${seconds}s`;
    }

    const minutes = Math.floor(safeMs / 60000);
    const seconds = Math.floor((safeMs % 60000) / 1000);
    return `${prefix} ${minutes}m ${String(seconds).padStart(2, "0")}s`;
}

// ESP 延迟显示：基于最新传感器 timestamp 计算数据新鲜度；时间戳无效时返回 null。
function getSensorDelayMs(sensor) {
    if (!sensor || sensor.source !== "real" || !sensor.timestamp || Number.isNaN(sensor.timestamp.getTime())) {
        return null;
    }

    return Math.max(0, Date.now() - sensor.timestamp.getTime());
}

function cloneMockData(mockData) {
    return {
        ...mockData,
        timestamp: Date.now()
    };
}

async function readEndpointResponse(response, mockData, label) {
    if (!response.ok) {
        throw new Error(`${response.url} ${response.status}`);
    }

    const data = await response.json();
    if (hasData(data)) {
        return { data, source: "real" };
    }

    console.warn(`[Dashboard] ${label}: mock fallback because API returned empty data`);
    return { data: cloneMockData(mockData), source: "mock" };
}

function readEndpointFallback(error, mockData, label) {
    console.warn(`[Dashboard] ${label}: mock fallback`, error.message);
    return { data: cloneMockData(mockData), source: "mock" };
}

async function fetchLatestSensor() {
    try {
        const response = await fetch("/api/dashboard/v1/sensors/latest", { cache: "no-store" });
        const result = await readEndpointResponse(response, mockSensorData, "Sensor");
        return {
            ...result,
            data: unwrapDashboardV1Data(result.data, "sensor")
        };
    } catch (error) {
        return readEndpointFallback(error, mockSensorData, "Sensor");
    }
}

async function fetchLatestASR() {
    try {
        const response = await fetch("/api/dashboard/v1/asr/latest", { cache: "no-store" });
        const result = await readEndpointResponse(response, mockASRData, "ASR");
        return {
            ...result,
            data: unwrapDashboardV1Data(result.data, "asr")
        };
    } catch (error) {
        return readEndpointFallback(error, mockASRData, "ASR");
    }
}

async function fetchLatestLLM() {
    try {
        const response = await fetch("/api/dashboard/v1/llm/latest", { cache: "no-store" });
        const result = await readEndpointResponse(response, mockLLMData, "LLM");
        return {
            ...result,
            data: unwrapDashboardV1Data(result.data, "llm")
        };
    } catch (error) {
        return readEndpointFallback(error, mockLLMData, "LLM");
    }
}

async function fetchHistoryData() {
    // 当前后端没有历史数据接口，保留 mockHistoryData 作为曲线占位。
    return mockHistoryData;
}

async function fetchCsiHistory() {
    try {
        const query = new URLSearchParams({
            limit: "80"
        });
        const response = await fetch(`/api/dashboard/v1/csi/history?${query.toString()}`, {
            cache: "no-store"
        });
        if (!response.ok) {
            throw new Error(`/api/dashboard/v1/csi/history ${response.status}`);
        }
        const payload = await response.json();
        const data = unwrapDashboardV1Data(payload);
        return Array.isArray(data?.events) ? data.events : [];
    } catch (error) {
        console.warn("[Dashboard] CSI history unavailable", error.message);
        return [];
    }
}

async function fetchAlertLogs() {
    // 当前后端没有报警日志接口，保留 mockAlertLogs 作为表格占位。
    return mockAlertLogs;
}

async function fetchSystemLogs() {
    return mockSystemLogs;
}

function normalizeSensor(rawSensor, source) {
    const temperature = toNumber(pickFirst(rawSensor, ["temperature", "temp"]));
    const humidity = toNumber(pickFirst(rawSensor, ["humidity"]));
    const pressure = toNumber(pickFirst(rawSensor, ["pressure"]));
    const airQualityObject = isPlainObject(rawSensor?.air_quality) ? rawSensor.air_quality : {};
    const aqi = toNumber(pickFirst(rawSensor, [
        "air_quality_score",
        "aqi",
        "air_quality"
    ])) ?? toNumber(pickFirst(airQualityObject, ["air_quality_score", "score", "aqi"]));
    const timestampValue = pickFirst(rawSensor, [
        "timestamp",
        "created_at",
        "createdAt",
        "updated_at",
        "updatedAt",
        "last_seen",
        "lastSeen",
        "received_at",
        "receivedAt",
        "upload_time",
        "uploadTime",
        "time"
    ]);
    const timestamp = parseTimestamp(timestampValue);

    let airValue = aqi;
    let airMode = "aqi";
    let airLabel = "空气质量";
    let airUnit = "AQI";

    if (airValue === null) {
        airValue = metricDefinitions.air.mockValue;
        airMode = "mock-aqi";
    }

    return {
        raw: rawSensor,
        source,
        temperature: temperature ?? metricDefinitions.temperature.mockValue,
        humidity: humidity ?? metricDefinitions.humidity.mockValue,
        pressure,
        airValue,
        airMode,
        airLabel,
        airUnit,
        timestamp,
        hasTimestamp: Boolean(timestamp),
        usedMockFields: {
            temperature: temperature === null,
            humidity: humidity === null,
            air: aqi === null
        }
    };
}

function getTemperatureLevel(value) {
    if (value > 45) return "danger";
    if (value > 35) return "warning";
    return "normal";
}

function getHumidityLevel(value) {
    if (value > 85) return "danger";
    if (value > 75) return "warning";
    return "normal";
}

function getAirLevel(value) {
    if (value > 150) return "danger";
    if (value > 100) return "warning";
    return "normal";
}

function getEspStatus(sensor) {
    const ageMs = getSensorDelayMs(sensor);
    if (ageMs === null) {
        return {
            value: "--",
            latency: null,
            level: "warning",
            note: "延迟 --"
        };
    }

    const isOnline = ageMs <= ESP_ONLINE_THRESHOLD_MS;
    return {
        value: isOnline ? "在线" : "离线",
        latency: ageMs,
        level: isOnline ? "normal" : "danger",
        note: isOnline ? formatDelayText("延迟", ageMs) : formatDelayText("离线", ageMs, { decimalSeconds: false })
    };
}

function getOverallLevel(metrics) {
    const levels = [metrics.temperature.level, metrics.humidity.level, metrics.air.level, metrics.esp.level];
    if (levels.includes("danger")) return "danger";
    if (levels.includes("warning")) return "warning";
    return "normal";
}

function buildMetrics(sensor) {
    const esp = getEspStatus(sensor);

    return {
        temperature: {
            value: sensor.temperature,
            display: formatNumber(sensor.temperature),
            level: getTemperatureLevel(sensor.temperature),
            source: sensor.usedMockFields.temperature ? "mock" : sensor.source
        },
        humidity: {
            value: sensor.humidity,
            display: formatNumber(sensor.humidity),
            level: getHumidityLevel(sensor.humidity),
            source: sensor.usedMockFields.humidity ? "mock" : sensor.source
        },
        air: {
            value: sensor.airValue,
            display: formatNumber(sensor.airValue, 0),
            level: getAirLevel(sensor.airValue, sensor.airMode),
            mode: sensor.airMode,
            label: sensor.airLabel,
            unit: sensor.airUnit,
            source: sensor.usedMockFields.air ? "mock" : sensor.source
        },
        esp,
        overall: getOverallLevel({
            temperature: { level: getTemperatureLevel(sensor.temperature) },
            humidity: { level: getHumidityLevel(sensor.humidity) },
            air: { level: getAirLevel(sensor.airValue, sensor.airMode) },
            esp
        })
    };
}

function iconSvg(name) {
    const icons = {
        thermometer: '<svg viewBox="0 0 24 24"><path d="M10 4a2 2 0 1 1 4 0v8.4a5 5 0 1 1-4 0V4Zm2 1.5a.5.5 0 0 0-.5.5v9.1l-.8.5a3 3 0 1 0 3.2 0l-.9-.5V6a.5.5 0 0 0-.5-.5Z"/></svg>',
        drop: '<svg viewBox="0 0 24 24"><path d="M12 2.5 6.4 10a7 7 0 1 0 11.2 0L12 2.5Zm0 18a5 5 0 0 1-5-5c0-1.5.9-3.1 2-4.7.8-1.2 1.8-2.5 3-4.1 1.2 1.6 2.2 2.9 3 4.1 1.1 1.6 2 3.2 2 4.7a5 5 0 0 1-5 5Z"/></svg>',
        cloud: '<svg viewBox="0 0 24 24"><path d="M8.5 18.5a5.5 5.5 0 0 1-.9-10.9 6 6 0 0 1 11.5 2 4.5 4.5 0 0 1-.6 8.9h-10Z"/></svg>',
        chip: '<svg viewBox="0 0 24 24"><path d="M8 3h8v3h3v3h2v6h-2v3h-3v3H8v-3H5v-3H3V9h2V6h3V3Zm1.5 6.5v5h5v-5h-5Z"/></svg>',
        bell: '<svg viewBox="0 0 24 24"><path d="M12 22a2.5 2.5 0 0 0 2.5-2h-5A2.5 2.5 0 0 0 12 22Zm7-6V11a7 7 0 0 0-5-6.7V3a2 2 0 1 0-4 0v1.3A7 7 0 0 0 5 11v5l-2 2v1h18v-1l-2-2Z"/></svg>',
        "air-conditioner": '<svg viewBox="0 0 24 24"><path d="M4 5h16a2 2 0 0 1 2 2v6H2V7a2 2 0 0 1 2-2Zm0 2v6h16V7H4Zm2 2h8v2H6V9Zm13.5 0a1.5 1.5 0 1 1-3 0 1.5 1.5 0 0 1 3 0ZM7 15h2v2.2l2-1.1 1 1.7-2 1.2 2 1.2-1 1.7-2-1.1V23H7v-2.2l-2 1.1-1-1.7L6 19l-2-1.2 1-1.7 2 1.1V15Zm8 0h2v2.2l2-1.1 1 1.7-2 1.2 2 1.2-1 1.7-2-1.1V23h-2v-2.2l-2 1.1-1-1.7 2-1.2-2-1.2 1-1.7 2 1.1V15Z"/></svg>',
        fan: '<svg viewBox="0 0 24 24"><path d="M12 10a2 2 0 1 1 0 4 2 2 0 0 1 0-4Zm1-8c3.2 0 5 1.8 5 4.4 0 2.3-1.7 4.3-4.2 5.2-.2-.9-.8-1.7-1.6-2.1C13.4 8 14 6.8 13.8 5.8 13.6 4.8 12.9 4 11 4V2h2ZM6.4 6.1c2-.9 4.6-.1 6.4 1.9-.8.4-1.4 1.1-1.7 1.9-1.9-.4-3.3-.1-4 .7-.7.7-.8 1.7.1 3.3l-1.7 1C3.9 12.2 4 7.3 6.4 6.1Zm11 9.8c-2 1.2-4.7.8-6.7-.9.7-.5 1.3-1.2 1.5-2 1.9.1 3.2-.4 3.8-1.3.6-.8.5-1.8-.5-3.3l1.7-1.1c1.8 2.7 2.1 7.3-.8 8.9ZM11 22c-3.2 0-5-1.8-5-4.4 0-2.2 1.6-4.1 4-5.1.3.9.9 1.6 1.7 2-1.2 1.5-1.7 2.7-1.5 3.7.2 1 .9 1.8 2.8 1.8v2h-2Z"/></svg>',
        door: '<svg viewBox="0 0 24 24"><path d="M5 3h11a2 2 0 0 1 2 2v16h2v2H3v-2h2V3Zm2 18h9V5H7v16Zm10 0h1V5h-1v16Zm-4-9h2v2h-2v-2Z"/></svg>',
        light: '<svg viewBox="0 0 24 24"><path d="M12 2a7 7 0 0 1 4 12.7V17a2 2 0 0 1-2 2H10a2 2 0 0 1-2-2v-2.3A7 7 0 0 1 12 2Zm0 2a5 5 0 0 0-3 9l1 .7V17h4v-3.3l1-.7A5 5 0 0 0 12 4Zm-2 17h4v2h-4v-2Z"/></svg>',
        "air-purifier": '<svg viewBox="0 0 24 24"><path d="M7 3h10a3 3 0 0 1 3 3v15H4V6a3 3 0 0 1 3-3Zm0 2a1 1 0 0 0-1 1v13h12V6a1 1 0 0 0-1-1H7Zm2 2h6v2H9V7Zm-1 5c2.2-1.5 4.2.8 6.4-.6.8-.5 1.6-.4 2.2.2l-1.2 1.6c-.4-.3-.8-.2-1.3.1-2.2 1.5-4.2-.8-6.4.6-.7.5-1.5.4-2.2-.2l1.2-1.6c.4.3.8.2 1.3-.1Zm0 4c2.2-1.5 4.2.8 6.4-.6.8-.5 1.6-.4 2.2.2l-1.2 1.6c-.4-.3-.8-.2-1.3.1-2.2 1.5-4.2-.8-6.4.6-.7.5-1.5.4-2.2-.2l1.2-1.6c.4.3.8.2 1.3-.1Z"/></svg>',
        humidifier: '<svg viewBox="0 0 24 24"><path d="M7 8h10a3 3 0 0 1 3 3v10H4V11a3 3 0 0 1 3-3Zm0 2a1 1 0 0 0-1 1v8h12v-8a1 1 0 0 0-1-1H7Zm2 4h6v2H9v-2ZM9 2h2v2a2 2 0 0 1-2 2H8V4h1V2Zm5 0h2v2a2 2 0 0 1-2 2h-1V4h1V2Z"/></svg>'
    };
    return icons[name] || "";
}

function createSparkline(values, color) {
    const width = 260;
    const height = 58;
    const min = Math.min(...values);
    const max = Math.max(...values);
    const range = max - min || 1;
    const points = values.map((value, index) => {
        const x = (index / (values.length - 1)) * width;
        const y = height - 10 - ((value - min) / range) * (height - 20);
        return `${x.toFixed(1)},${y.toFixed(1)}`;
    });
    const area = `0,${height} ${points.join(" ")} ${width},${height}`;

    return `
        <svg viewBox="0 0 ${width} ${height}" preserveAspectRatio="none" aria-hidden="true">
            <polygon points="${area}" fill="${color}" opacity="0.12"></polygon>
            <polyline points="${points.join(" ")}" fill="none" stroke="${color}" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"></polyline>
            ${points.map(point => `<circle cx="${point.split(",")[0]}" cy="${point.split(",")[1]}" r="2.3" fill="${color}"></circle>`).join("")}
        </svg>
    `;
}

function setText(selector, value) {
    const element = document.querySelector(selector);
    if (element) {
        element.textContent = value;
    }
}

function setStateBadge(selector, levelKey) {
    const element = document.querySelector(selector);
    if (!element) return;

    const level = LEVELS[levelKey] || LEVELS.normal;
    element.textContent = level.label;
    element.className = `state-badge state-${level.className}`;
}

function clearStateBadge(selector) {
    const element = document.querySelector(selector);
    if (!element) return;

    element.textContent = "";
    element.className = "state-badge is-empty";
}

function setMetricChange(selector, text, levelKey = "normal") {
    const element = document.querySelector(selector);
    if (!element) return;

    element.textContent = text;
    element.className = `metric-change ${levelKey === "normal" ? "" : levelKey}`;
}

function renderMetricCards() {
    Object.entries(metricDefinitions).forEach(([key, definition]) => {
        const icon = document.querySelector(`[data-metric-icon="${key}"]`);
        const sparkline = document.querySelector(`[data-sparkline="${key}"]`);
        if (icon) icon.innerHTML = iconSvg(definition.icon);
        if (sparkline) sparkline.innerHTML = createSparkline(definition.trend, definition.accent);
    });

    const metrics = dashboardState.metrics;
    setText("#temperatureValue", metrics.temperature.display);
    setText("#humidityValue", metrics.humidity.display);
    setText("#airQualityValue", metrics.air.display);
    setText("#airQualityLabel", metrics.air.label);
    setText("#airQualityUnit", metrics.air.unit);
    setText("#espStatusValue", metrics.esp.value);

    setStateBadge('[data-field="temperatureStatus"]', metrics.temperature.level);
    setStateBadge('[data-field="humidityStatus"]', metrics.humidity.level);
    clearStateBadge('[data-field="airStatus"]');
    setStateBadge('[data-field="espStatusBadge"]', metrics.esp.level);

    setMetricChange('[data-field="temperatureChange"]', `来源：${metrics.temperature.source}`, metrics.temperature.level);
    setMetricChange('[data-field="humidityChange"]', `来源：${metrics.humidity.source}`, metrics.humidity.level);
    setMetricChange('[data-field="airChange"]', `来源：${metrics.air.source}`, "normal");
    setMetricChange('[data-field="espLatency"]', metrics.esp.note, metrics.esp.level);
}

// ESP 延迟显示：每秒用最新 sensor.timestamp 重新计算数据新鲜度，不新增接口请求。
function refreshEspDelayDisplay() {
    if (!dashboardState.sensor) return;
    if (!dashboardState.metrics.temperature || !dashboardState.metrics.humidity || !dashboardState.metrics.air) return;

    dashboardState.metrics.esp = getEspStatus(dashboardState.sensor);
    dashboardState.metrics.overall = getOverallLevel(dashboardState.metrics);

    setText("#espStatusValue", dashboardState.metrics.esp.value);
    setStateBadge('[data-field="espStatusBadge"]', dashboardState.metrics.esp.level);
    setMetricChange('[data-field="espLatency"]', dashboardState.metrics.esp.note, dashboardState.metrics.esp.level);
}

function renderMainChart() {
    const canvas = document.getElementById("mainChart");
    if (!canvas) return;

    const context = canvas.getContext("2d");
    const ratio = window.devicePixelRatio || 1;
    const rect = canvas.getBoundingClientRect();
    canvas.width = rect.width * ratio;
    canvas.height = rect.height * ratio;
    context.setTransform(ratio, 0, 0, ratio, 0, 0);
    context.clearRect(0, 0, rect.width, rect.height);

    const padding = { top: 22, right: 26, bottom: 42, left: 52 };
    const width = rect.width - padding.left - padding.right;
    const height = rect.height - padding.top - padding.bottom;
    const yMax = 100;
    const data = getFilteredChartData();
    // 主题功能：Canvas 不能自动继承 CSS 颜色，所以每次绘图时读取当前黑白主题变量。
    const chartColors = {
        grid: readThemeColor("--chart-grid", "#dfe7f3"),
        label: readThemeColor("--chart-label", "#33537f"),
        axisLabel: readThemeColor("--chart-axis-label", "#1f3b68"),
        temperature: readThemeColor("--chart-temperature", "#2266f3"),
        humidity: readThemeColor("--chart-humidity", "#10b981"),
        air: readThemeColor("--chart-air", "#7c3aed")
    };

    context.strokeStyle = chartColors.grid;
    context.lineWidth = 1;
    context.setLineDash([3, 4]);
    context.font = "13px Avenir Next, PingFang SC, sans-serif";
    context.fillStyle = chartColors.label;

    [0, 25, 50, 75, 100].forEach(value => {
        const y = padding.top + height - (value / yMax) * height;
        context.beginPath();
        context.moveTo(padding.left, y);
        context.lineTo(padding.left + width, y);
        context.stroke();
        context.fillText(String(value), 18, y + 4);
    });

    context.setLineDash([]);
    if (data.length === 0) {
        context.fillStyle = chartColors.axisLabel;
        context.font = "15px Avenir Next, PingFang SC, sans-serif";
        context.textAlign = "center";
        context.fillText("暂无真实时间戳数据", rect.width / 2, padding.top + height / 2);
        context.textAlign = "left";
        return;
    }

    const xFor = index => data.length === 1
        ? padding.left + width / 2
        : padding.left + (index / (data.length - 1)) * width;
    const yFor = value => padding.top + height - (Math.max(0, Math.min(yMax, value)) / yMax) * height;

    const drawLine = (field, color) => {
        const drawablePoints = data
            .map((point, index) => ({ point, index, value: toNumber(point[field]) }))
            .filter(item => item.value !== null);

        if (drawablePoints.length === 0) return;

        context.beginPath();
        drawablePoints.forEach((item, drawableIndex) => {
            const x = xFor(item.index);
            const y = yFor(item.value);
            if (drawableIndex === 0) context.moveTo(x, y);
            else context.lineTo(x, y);
        });
        context.strokeStyle = color;
        context.lineWidth = 3;
        context.lineJoin = "round";
        context.lineCap = "round";
        if (drawablePoints.length > 1) {
            context.stroke();
        }

        drawablePoints.forEach(item => {
            const x = xFor(item.index);
            const y = yFor(item.value);
            context.beginPath();
            context.arc(x, y, 4, 0, Math.PI * 2);
            context.fillStyle = color;
            context.fill();
        });
    };

    drawLine("temperature", chartColors.temperature);
    drawLine("humidity", chartColors.humidity);
    drawLine("air", chartColors.air);

    context.fillStyle = chartColors.axisLabel;
    data.forEach((point, index) => {
        if (index % 2 === 0 || rect.width > 760) {
            context.fillText(point.time, xFor(index) - 17, padding.top + height + 30);
        }
    });
}

function normalizeCsiEvent(event) {
    if (!isPlainObject(event)) return null;
    const timestamp = parseTimestamp(pickFirst(event, ["timestamp", "server_recv_ms", "created_at"]));
    const state = String(event.state || "IDLE").toUpperCase();
    if (!["IDLE", "MOTION", "HOLD"].includes(state)) return null;

    return {
        device_id: String(event.device_id || ""),
        link_id: String(event.link_id || "fused"),
        state,
        frame_energy: toNumber(event.frame_energy),
        variance: toNumber(event.variance),
        rssi: toNumber(event.rssi),
        motion_score: clamp01(event.motion_score),
        timestamp
    };
}

function renderCsiMiniTrend(events, field, maxValue, accent) {
    const width = 360;
    const height = 72;
    const values = events
        .map(event => toNumber(event[field]))
        .filter(value => value !== null);

    if (!values.length) {
        return '<div class="csi-empty">暂无数据</div>';
    }

    const minValue = 0;
    const yMax = maxValue || Math.max(...values, 1);
    const points = values.map((value, index) => {
        const x = values.length === 1 ? width / 2 : (index / (values.length - 1)) * width;
        const clamped = Math.max(minValue, Math.min(yMax, value));
        const y = height - 8 - ((clamped - minValue) / (yMax - minValue || 1)) * (height - 16);
        return `${x.toFixed(1)},${y.toFixed(1)}`;
    });
    const area = `0,${height} ${points.join(" ")} ${width},${height}`;

    return `
        <svg viewBox="0 0 ${width} ${height}" preserveAspectRatio="none" aria-hidden="true">
            <polygon points="${area}" fill="${accent}" opacity="0.12"></polygon>
            <polyline points="${points.join(" ")}" fill="none" stroke="${accent}" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"></polyline>
        </svg>
    `;
}

function csiStateClass(state) {
    if (state === "MOTION") return "danger";
    if (state === "HOLD") return "warning";
    return "normal";
}

function renderCsiPanel() {
    const events = (Array.isArray(dashboardState.csiHistory) ? dashboardState.csiHistory : [])
        .map(normalizeCsiEvent)
        .filter(Boolean)
        .sort((a, b) => {
            const left = a.timestamp ? a.timestamp.getTime() : 0;
            const right = b.timestamp ? b.timestamp.getTime() : 0;
            return left - right;
        });
    const latest = events[events.length - 1] || null;

    const state = latest?.state || "IDLE";
    const stateClass = csiStateClass(state);
    const badge = document.querySelector("[data-csi-state-badge]");
    if (badge) {
        badge.textContent = state;
        badge.className = `state-badge state-${stateClass}`;
    }

    setText("[data-csi-score]", latest?.motion_score === null || latest?.motion_score === undefined ? "--" : formatNumber(latest.motion_score, 3));
    setText("[data-csi-energy]", latest?.frame_energy === null || latest?.frame_energy === undefined ? "--" : formatNumber(latest.frame_energy, 2));
    setText("[data-csi-variance]", latest?.variance === null || latest?.variance === undefined ? "--" : formatNumber(latest.variance, 4));
    setText("[data-csi-rssi]", latest?.rssi === null || latest?.rssi === undefined ? "--" : `${formatNumber(latest.rssi, 0)} dBm`);

    const scoreTrend = document.querySelector("[data-csi-score-trend]");
    const energyTrend = document.querySelector("[data-csi-energy-trend]");
    if (scoreTrend) {
        scoreTrend.innerHTML = renderCsiMiniTrend(events, "motion_score", 1, readThemeColor("--csi-score", "#2f6df6"));
    }
    if (energyTrend) {
        const energyMax = Math.max(...events.map(event => toNumber(event.frame_energy) || 0), 1);
        energyTrend.innerHTML = renderCsiMiniTrend(events, "frame_energy", energyMax, readThemeColor("--csi-energy", "#10b981"));
    }

    const timeline = document.querySelector("[data-csi-timeline]");
    if (!timeline) return;
    const tail = events.slice(-18);
    if (!tail.length) {
        timeline.innerHTML = '<div class="csi-empty">等待 S3 上报 canonical CSI 状态</div>';
        return;
    }

    timeline.innerHTML = tail.map(event => `
        <div class="csi-timeline-item ${csiStateClass(event.state)}" title="${escapeHtml(formatTime(event.timestamp))} ${escapeHtml(event.state)}">
            <span>${escapeHtml(event.state)}</span>
            <small>${escapeHtml(formatTime(event.timestamp))}</small>
        </div>
    `).join("");
}

function buildDynamicAlertLogs(metrics) {
    const alerts = [];
    const now = formatTime();

    if (metrics.temperature.level !== "normal") {
        alerts.push({
            time: now,
            type: "温度异常",
            content: `温度 ${metrics.temperature.display}°C，状态 ${LEVELS[metrics.temperature.level].label}`,
            level: metrics.temperature.level,
            status: "未处理"
        });
    }

    if (metrics.humidity.level !== "normal") {
        alerts.push({
            time: now,
            type: "湿度异常",
            content: `湿度 ${metrics.humidity.display}%，状态 ${LEVELS[metrics.humidity.level].label}`,
            level: metrics.humidity.level,
            status: "未处理"
        });
    }

    if (metrics.air.level !== "normal") {
        alerts.push({
            time: now,
            type: "空气质量异常",
            content: `${metrics.air.label} ${metrics.air.display}${metrics.air.unit || ""}，状态 ${LEVELS[metrics.air.level].label}`,
            level: metrics.air.level,
            status: "未处理"
        });
    }

    if (metrics.esp.level !== "normal") {
        alerts.push({
            time: now,
            type: "ESP 异常",
            content: `ESP 状态 ${metrics.esp.value}，${metrics.esp.note}`,
            level: metrics.esp.level,
            status: "未处理"
        });
    }

    return alerts;
}

function renderAlertSummary() {
    const container = document.querySelector("[data-alert-summary]");
    if (!container) return;

    const rows = [
        { label: "温度", value: `${dashboardState.metrics.temperature.display}°C`, key: "temperature", icon: "thermometer" },
        { label: "湿度", value: `${dashboardState.metrics.humidity.display}%`, key: "humidity", icon: "drop" },
        {
            label: dashboardState.metrics.air.label,
            value: `${dashboardState.metrics.air.display}${dashboardState.metrics.air.unit ? ` ${dashboardState.metrics.air.unit}` : dashboardState.metrics.air.mode === "aqi" || dashboardState.metrics.air.mode === "mock-aqi" ? " (AQI)" : ""}`,
            key: "air",
            icon: "cloud"
        },
        { label: "ESP 状态", value: `${dashboardState.metrics.esp.value} (${dashboardState.metrics.esp.note})`, key: "esp", icon: "chip" }
    ];

    container.innerHTML = rows.map(row => {
        const metric = dashboardState.metrics[row.key];
        const level = LEVELS[metric.level];
        const definition = metricDefinitions[row.key];

        return `
            <div class="summary-row">
                <span class="summary-icon" style="--accent:${definition.accent}">${iconSvg(row.icon)}</span>
                <span>${row.label}</span>
                <span class="summary-value">${row.value}</span>
                <span class="summary-state ${level.className}">${level.label}</span>
            </div>
        `;
    }).join("");
}

function renderAlertLogs() {
    const body = document.querySelector("[data-alert-logs]");
    if (!body) return;

    const previewLogs = dashboardState.alertLogs.slice(0, ALERT_LOG_PREVIEW_LIMIT);
    body.innerHTML = previewLogs.map(log => {
        const type = String(log.type || "报警");
        const time = log.time || "--";
        const content = log.content || "";
        const status = log.status || "--";
        const level = LEVELS[log.level] || LEVELS.normal;
        const accent = log.level === "danger" ? "#ef3340" : log.level === "warning" ? "#f97316" : "#10b981";
        const statusClass = status === "已恢复" ? "recovered" : "pending";

        return `
            <tr>
                <td>${escapeHtml(time)}</td>
                <td><span class="type-cell"><i class="type-icon" style="--accent:${accent}">${iconSvg(type.includes("湿度") ? "drop" : type.includes("温度") ? "thermometer" : type.includes("ESP") ? "chip" : "cloud")}</i>${escapeHtml(type)}</span></td>
                <td>${escapeHtml(content)}</td>
                <td><span class="level-badge level-${level.className}">${level.label}</span></td>
                <td><span class="status-text ${statusClass}">${escapeHtml(status)}</span></td>
            </tr>
        `;
    }).join("");
}

function formatCommandCandidate(commandValue) {
    if (commandValue === undefined || commandValue === null || commandValue === "") {
        return "--";
    }

    if (Array.isArray(commandValue)) {
        const commands = commandValue.map(item => {
            if (isPlainObject(item)) {
                return pickFirst(item, ["command", "name", "command_name", "commandName", "command_id", "commandId"]) || JSON.stringify(item);
            }
            return String(item);
        }).filter(Boolean);
        return commands.length ? commands.join(", ") : "--";
    }

    if (isPlainObject(commandValue)) {
        return pickFirst(commandValue, ["command", "name", "command_name", "commandName", "command_id", "commandId"]) || JSON.stringify(commandValue);
    }

    return String(commandValue);
}

function formatLatestCommand(...records) {
    const commandKeys = [
        "command",
        "last_command",
        "lastCommand",
        "command_name",
        "commandName",
        "last_command_id",
        "lastCommandId"
    ];
    const commandListKeys = ["commands", "recent_commands", "command_queue", "commandQueue"];

    for (const record of records) {
        if (!isPlainObject(record)) continue;

        const candidates = [
            pickFirst(record, commandKeys),
            pickFirst(record, commandListKeys)
        ];

        if (isPlainObject(record.structured)) {
            candidates.push(pickFirst(record.structured, commandKeys));
            candidates.push(pickFirst(record.structured, commandListKeys));
        }

        if (isPlainObject(record.command)) {
            candidates.push(record.command);
        }

        for (const candidate of candidates) {
            const formatted = formatCommandCandidate(candidate);
            if (formatted !== "--") {
                return formatted;
            }
        }
    }

    return "--";
}

function formatLatestAirQuality(rawSensor, metrics) {
    const airQualityObject = isPlainObject(rawSensor?.air_quality) ? rawSensor.air_quality : {};
    const score = toNumber(pickFirst(rawSensor || {}, [
        "air_quality",
        "air_quality_score",
        "aqi",
        "air"
    ])) ?? toNumber(pickFirst(airQualityObject, ["air_quality_score", "score", "aqi"]));
    const level = pickFirst(rawSensor || {}, [
        "air_quality_level",
        "air_quality_label"
    ]) ?? pickFirst(airQualityObject, ["air_quality_level", "level", "label"]);

    if (score !== null && level) {
        return `${formatNumber(score, 0)} (${level})`;
    }
    if (score !== null) {
        return formatNumber(score, 0);
    }
    if (level) {
        return String(level);
    }
    if (metrics?.air && metrics.air.display !== "--") {
        return metrics.air.display;
    }
    return "--";
}

function buildSystemLogs(sensor, asr, llm, sources) {
    const logs = [];
    const sensorTime = sensor.timestamp ? formatTime(sensor.timestamp) : formatTime();
    const commandText = formatLatestCommand(sensor.raw, asr, llm);
    const airQualityText = formatLatestAirQuality(sensor.raw, dashboardState.metrics);

    logs.push({
        time: sensorTime,
        text: `传感器数据更新：温度 ${dashboardState.metrics.temperature.display}°C，湿度 ${dashboardState.metrics.humidity.display}%，Air Quality: ${airQualityText}`,
        color: "#10b981",
        source: sources.sensor
    });

    logs.push({
        time: sensorTime,
        text: `Command: ${commandText}`,
        color: "#2874ff",
        source: sources.sensor
    });

    if (logs.length === 1 && sources.asr === "mock" && sources.llm === "mock") {
        logs.push(...mockSystemLogs);
    }

    return [...dashboardState.commandLogs, ...logs].slice(0, 8);
}

function renderSystemLogs() {
    const container = document.getElementById("latestLogList");
    if (!container) return;

    const previewLogs = dashboardState.systemLogs.slice(0, SYSTEM_LOG_PREVIEW_LIMIT);
    container.innerHTML = previewLogs.map(log => `
        <div class="system-log">
            <i style="--accent:${log.color}"></i>
            <time>${escapeHtml(log.time)}</time>
            <span>${escapeHtml(log.text)}</span>
        </div>
    `).join("");
}

const OPERATION_STATUS_LABELS = {
    pending: "请求中",
    success: "成功",
    failed: "失败",
    unavailable: "不可用"
};

// 命令控制：操作记录只保存在当前页面内存中，不写入 localStorage、数据库或服务器日志。
function addOperationLog(entry) {
    const log = {
        id: `${Date.now()}-${Math.random().toString(16).slice(2)}`,
        time: formatTime(),
        type: entry.type || "操作",
        content: entry.content || "",
        status: entry.status || "pending",
        result: entry.result || ""
    };

    dashboardState.operationLogs.unshift(log);
    renderOperationLogs();
    renderActiveLogModal();
    return log.id;
}

// 命令控制：更新请求中的操作记录，真实请求完成后写入成功或失败结果。
function updateOperationLog(id, changes) {
    const target = dashboardState.operationLogs.find(log => log.id === id);
    if (!target) return;

    Object.assign(target, changes);
    renderOperationLogs();
    renderActiveLogModal();
}

function renderOperationLogItem(log) {
    const status = log.status || "pending";
    const statusLabel = OPERATION_STATUS_LABELS[status] || status;

    return `
        <article class="operation-log-item">
            <div class="operation-log-meta">
                <time>${escapeHtml(log.time || "--")}</time>
                <span class="operation-status ${escapeHtml(status)}">${escapeHtml(statusLabel)}</span>
            </div>
            <strong class="operation-log-title">${escapeHtml(log.type || "操作")}</strong>
            ${log.content ? `<div class="operation-log-content">${escapeHtml(log.content)}</div>` : ""}
            ${log.result ? `<div class="operation-log-result">${escapeHtml(log.result)}</div>` : ""}
        </article>
    `;
}

function renderOperationLogs() {
    const container = document.querySelector("[data-operation-log-list]");
    if (!container) return;

    const previewLogs = dashboardState.operationLogs.slice(0, OPERATION_LOG_PREVIEW_LIMIT);
    if (previewLogs.length === 0) {
        container.innerHTML = '<div class="operation-log-item empty">暂无页面操作记录</div>';
        return;
    }

    container.innerHTML = previewLogs.map(renderOperationLogItem).join("");
}

// 命令控制：弹窗中展示全部前端临时操作记录，服务器记录不会被改动。
function renderOperationLogModal(logs) {
    if (!logs.length) {
        return '<div class="log-empty">暂无页面操作记录</div>';
    }

    return `
        <div class="modal-system-log-list">
            ${logs.map(renderOperationLogItem).join("")}
        </div>
    `;
}

// 日志弹窗：渲染报警日志完整列表，数据只来自 dashboardState.alertLogs。
function renderAlertLogModal(logs) {
    if (!logs.length) {
        return '<div class="log-empty">暂无报警日志</div>';
    }

    return `
        <div class="modal-alert-list">
            ${logs.map(log => {
                const type = String(log.type || "报警");
                const time = log.time || "--";
                const content = log.content || "";
                const status = log.status || "--";
                const level = LEVELS[log.level] || LEVELS.normal;
                const statusClass = status === "已恢复" ? "recovered" : "pending";

                return `
                    <article class="modal-alert-log">
                        <time>${escapeHtml(time)}</time>
                        <strong class="modal-alert-type">${escapeHtml(type)}</strong>
                        <span class="modal-alert-content">${escapeHtml(content)}</span>
                        <span class="level-badge level-${level.className}">${level.label}</span>
                        <span class="status-text ${statusClass}">${escapeHtml(status)}</span>
                    </article>
                `;
            }).join("")}
        </div>
    `;
}

// 日志弹窗：渲染最新日志完整列表，保持“时间 + 来源 + 内容”横向阅读方式。
function renderSystemLogModal(logs) {
    if (!logs.length) {
        return '<div class="log-empty">暂无最新日志</div>';
    }

    return `
        <div class="modal-system-log-list">
            ${logs.map(log => `
                <article class="modal-system-log">
                    <time>${escapeHtml(log.time || "--")}</time>
                    <span class="modal-source-badge">${escapeHtml(log.source || "local")}</span>
                    <span class="modal-system-content">${escapeHtml(log.text || "")}</span>
                </article>
            `).join("")}
        </div>
    `;
}

// 日志弹窗：根据当前打开类型刷新弹窗内容，Dashboard 轮询更新后会复用它。
function renderActiveLogModal() {
    if (!activeLogModalType) return;

    const title = document.querySelector("[data-log-modal-title]");
    const body = document.querySelector("[data-log-modal-body]");
    if (!title || !body) return;

    if (activeLogModalType === "alerts") {
        title.textContent = "报警日志";
        body.innerHTML = renderAlertLogModal(dashboardState.alertLogs);
        return;
    }

    if (activeLogModalType === "operations") {
        title.textContent = "页面操作记录";
        body.innerHTML = renderOperationLogModal(dashboardState.operationLogs);
        return;
    }

    title.textContent = "最新日志";
    body.innerHTML = renderSystemLogModal(dashboardState.systemLogs);
}

// 日志弹窗：打开覆盖层并渲染当前前端已获取的完整日志数组。
function openLogModal(type) {
    const overlay = document.querySelector("[data-log-modal]");
    if (!overlay) return;

    if (type === "alerts" || type === "operations") {
        activeLogModalType = type;
    } else {
        activeLogModalType = "system";
    }
    renderActiveLogModal();
    overlay.hidden = false;
    document.body.classList.add("log-modal-open");
}

// 日志弹窗：关闭覆盖层并清空当前类型，避免遮罩残留。
function closeLogModal() {
    const overlay = document.querySelector("[data-log-modal]");
    if (!overlay) return;

    overlay.hidden = true;
    activeLogModalType = null;
    document.body.classList.remove("log-modal-open");
}

// 日志弹窗：绑定两个“查看更多”按钮、遮罩、关闭按钮和 Esc 快捷键。
function initLogModals() {
    const overlay = document.querySelector("[data-log-modal]");
    if (!overlay) return;

    document.querySelectorAll("[data-open-log-modal]").forEach(button => {
        button.addEventListener("click", () => {
            openLogModal(button.dataset.openLogModal);
        });
    });

    document.querySelectorAll("[data-open-operation-log]").forEach(button => {
        button.addEventListener("click", () => {
            openLogModal("operations");
        });
    });

    document.querySelectorAll("[data-close-log-modal]").forEach(button => {
        button.addEventListener("click", closeLogModal);
    });

    overlay.addEventListener("click", event => {
        if (event.target === overlay) {
            closeLogModal();
        }
    });

    document.addEventListener("keydown", event => {
        if (event.key === "Escape" && activeLogModalType) {
            closeLogModal();
        }
    });
}

function renderStatusHeader() {
    const now = formatTime();
    const dynamicAlerts = buildDynamicAlertLogs(dashboardState.metrics);
    const activeAlerts = dynamicAlerts.filter(log => log.level !== "normal" && log.status !== "已恢复");
    const newestAlert = activeAlerts[0] || dashboardState.alertLogs.find(log => log.level !== "normal");
    const overallLevel = dashboardState.metrics.overall;
    const level = LEVELS[overallLevel];

    document.querySelectorAll("[data-last-updated]").forEach(element => {
        element.textContent = now;
    });

    setText("[data-alert-badge]", activeAlerts.length);
    setText("[data-alert-count]", activeAlerts.length);
    setText("[data-alert-log-count]", dashboardState.alertLogs.length);
    setText("[data-current-state]", level.label);

    const stateElement = document.querySelector("[data-current-state]");
    if (stateElement) {
        stateElement.style.color = overallLevel === "danger" ? "#ef3340" : overallLevel === "warning" ? "#f97316" : "#10b981";
    }

    const alertPanel = document.getElementById("alertPanel");
    if (alertPanel) {
        alertPanel.dataset.level = overallLevel;
    }

    if (newestAlert) {
        setText("[data-latest-alert] span", `最近报警：${newestAlert.time.slice(0, 5)} ${newestAlert.content}`);
    } else {
        setText("[data-latest-alert] span", "最近报警：暂无异常");
    }
}

function renderSourceDebug() {
    const sources = dashboardState.sources;
    setText('[data-source="sensor"]', sources.sensor);
    setText('[data-source="asr"]', sources.asr);
    setText('[data-source="llm"]', sources.llm);

    const signature = `Sensor: ${sources.sensor} / ASR: ${sources.asr} / LLM: ${sources.llm}`;
    if (signature !== lastSourceSignature) {
        console.info(`[Dashboard] data source -> ${signature}`);
        lastSourceSignature = signature;
    }
}

function showDashboardToast(message, status = "success") {
    const previous = document.querySelector(".dashboard-toast");
    if (previous) {
        previous.remove();
    }

    const toast = document.createElement("div");
    toast.className = `dashboard-toast ${status}`;
    toast.textContent = message;
    document.body.appendChild(toast);
    window.setTimeout(() => {
        toast.remove();
    }, 3600);
}

// 智能家居控制：当前前端没有发现读取家居设备状态的真实接口方法，因此返回 null。
function getSmartHomeStatusMethod() {
    return null;
}

// 智能家居控制：当前前端没有发现切换家居设备开关的真实接口方法，因此返回 null。
function getSmartHomeToggleMethod() {
    return null;
}

function parseSmartHomeStatus(value) {
    if (typeof value === "boolean") return value;
    if (typeof value === "number") {
        if (value === 1) return true;
        if (value === 0) return false;
    }
    if (typeof value === "string") {
        const normalized = value.trim().toLowerCase();
        if (["on", "open", "opened", "true", "1", "开", "开启", "已开启"].includes(normalized)) return true;
        if (["off", "closed", "close", "false", "0", "关", "关闭", "已关闭"].includes(normalized)) return false;
    }
    return null;
}

function getSmartHomeDeviceStatusText(device) {
    if (device.loading) return "提交中";
    if (device.disabled) return "未连接";
    if (device.status === true) return "开";
    if (device.status === false) return "关";
    return "暂不可用";
}

function renderSmartHomeControls() {
    const list = document.querySelector("[data-smart-home-list]");
    const note = document.querySelector("[data-smart-home-note]");
    if (!list) return;

    const devices = Array.isArray(dashboardState.smartHomeDevices) ? dashboardState.smartHomeDevices : [];
    const hasEnabledDevice = devices.some(device => !device.disabled);
    if (note) {
        note.hidden = hasEnabledDevice;
        note.textContent = SMART_HOME_UNAVAILABLE_MESSAGE;
    }

    list.innerHTML = devices.map(device => {
        const disabled = Boolean(device.disabled || device.loading);
        const statusText = getSmartHomeDeviceStatusText(device);
        const rowClass = [
            "smart-device-row",
            disabled ? "is-disabled" : "",
            device.loading ? "is-loading" : ""
        ].filter(Boolean).join(" ");
        const disabledAttr = disabled ? " disabled" : "";

        return `
            <div class="${rowClass}" data-smart-home-device="${escapeHtml(device.id)}">
                <span class="smart-device-icon" aria-hidden="true">${iconSvg(device.icon)}</span>
                <strong class="smart-device-name">${escapeHtml(device.name)}</strong>
                <span class="smart-device-status">当前状态：${escapeHtml(statusText)}</span>
                <div class="smart-device-segment" role="group" aria-label="${escapeHtml(device.name)}开关状态">
                    <button class="smart-device-option ${device.status === true ? "is-active" : ""}" type="button" data-smart-home-action="on"${disabledAttr}>开</button>
                    <button class="smart-device-option ${device.status === false ? "is-active" : ""}" type="button" data-smart-home-action="off"${disabledAttr}>关</button>
                </div>
            </div>
        `;
    }).join("");
}

function setSmartHomeDeviceLoading(deviceId, loading) {
    dashboardState.smartHomeDevices = dashboardState.smartHomeDevices.map(device => (
        device.id === deviceId ? { ...device, loading } : device
    ));
    renderSmartHomeControls();
}

function closeSmartHomeConfirmModal() {
    const overlay = document.querySelector("[data-smart-home-confirm-modal]");
    if (!overlay) return;

    overlay.hidden = true;
    pendingSmartHomeAction = null;
    document.body.classList.remove("log-modal-open");
}

function openSmartHomeConfirmModal(device, nextStatus) {
    const overlay = document.querySelector("[data-smart-home-confirm-modal]");
    const title = document.querySelector("[data-smart-home-confirm-title]");
    const message = document.querySelector("[data-smart-home-confirm-message]");
    const submit = document.querySelector("[data-smart-home-confirm-submit]");
    if (!overlay || !title || !message || !submit) return;

    const isOpening = nextStatus === true;
    pendingSmartHomeAction = { deviceId: device.id, nextStatus };
    title.textContent = isOpening ? "确认开启设备？" : "确认关闭设备？";
    message.textContent = `确定要${isOpening ? "开启" : "关闭"}“${device.name}”吗？`;
    submit.textContent = isOpening ? "确认开启" : "确认关闭";
    submit.classList.toggle("danger-action", !isOpening);
    overlay.hidden = false;
    document.body.classList.add("log-modal-open");
}

async function handleSmartHomeToggleConfirm() {
    const action = pendingSmartHomeAction;
    if (!action) return;

    const device = dashboardState.smartHomeDevices.find(item => item.id === action.deviceId);
    if (!device) {
        closeSmartHomeConfirmModal();
        return;
    }

    const toggleMethod = getSmartHomeToggleMethod();
    const targetText = action.nextStatus ? "开启" : "关闭";
    if (!toggleMethod) {
        addOperationLog({
            type: "智能家居控制",
            content: `尝试${targetText}${device.name}。`,
            status: "unavailable",
            result: SMART_HOME_UNAVAILABLE_MESSAGE
        });
        showDashboardToast(SMART_HOME_UNAVAILABLE_MESSAGE, "unavailable");
        closeSmartHomeConfirmModal();
        return;
    }

    const submit = document.querySelector("[data-smart-home-confirm-submit]");
    const logId = addOperationLog({
        type: "智能家居控制",
        content: `已发送请求：${targetText}${device.name}`,
        status: "pending",
        result: "请求中..."
    });

    if (submit) submit.disabled = true;
    setSmartHomeDeviceLoading(device.id, true);

    try {
        const result = await toggleMethod(device.id, action.nextStatus);
        dashboardState.smartHomeDevices = dashboardState.smartHomeDevices.map(item => (
            item.id === device.id ? { ...item, status: action.nextStatus, disabled: false, loading: false } : item
        ));
        renderSmartHomeControls();
        updateOperationLog(logId, {
            status: "success",
            result: typeof result === "string" ? result : `${device.name}已${action.nextStatus ? "开启" : "关闭"}`
        });
        showDashboardToast(`${device.name}已${action.nextStatus ? "开启" : "关闭"}`, "success");
        closeSmartHomeConfirmModal();
    } catch (error) {
        setSmartHomeDeviceLoading(device.id, false);
        const message = `${device.name}${targetText}失败：${error.message}`;
        updateOperationLog(logId, { status: "failed", result: message });
        showDashboardToast(message, "failed");
    } finally {
        if (submit) submit.disabled = false;
    }
}

function handleSmartHomeOptionClick(event) {
    const button = event.target.closest("[data-smart-home-action]");
    if (!button || button.disabled) return;

    const row = button.closest("[data-smart-home-device]");
    if (!row) return;

    const device = dashboardState.smartHomeDevices.find(item => item.id === row.dataset.smartHomeDevice);
    if (!device || device.disabled || device.loading) return;

    const nextStatus = button.dataset.smartHomeAction === "on";
    if (device.status === nextStatus) return;

    openSmartHomeConfirmModal(device, nextStatus);
}

async function loadSmartHomeStatuses() {
    const statusMethod = getSmartHomeStatusMethod();
    if (!statusMethod) {
        dashboardState.smartHomeDevices = createSmartHomeDeviceState();
        renderSmartHomeControls();
        return;
    }

    try {
        const statusResult = await statusMethod();
        dashboardState.smartHomeDevices = SMART_HOME_DEVICES.map(device => {
            const rawStatus = isPlainObject(statusResult) ? statusResult[device.id] : undefined;
            const parsedStatus = parseSmartHomeStatus(rawStatus);
            return {
                ...device,
                status: parsedStatus,
                disabled: parsedStatus === null,
                loading: false,
                unavailableReason: parsedStatus === null ? SMART_HOME_UNAVAILABLE_MESSAGE : ""
            };
        });
        renderSmartHomeControls();
    } catch (error) {
        dashboardState.smartHomeDevices = createSmartHomeDeviceState();
        renderSmartHomeControls();
        showDashboardToast(`智能家居状态读取失败：${error.message}`, "failed");
    }
}

// 智能家居控制：绑定卡片按钮和确认弹窗，只复用现有前端方法，不新增 API 地址。
function initSmartHomeControls() {
    const list = document.querySelector("[data-smart-home-list]");
    const overlay = document.querySelector("[data-smart-home-confirm-modal]");
    const submit = document.querySelector("[data-smart-home-confirm-submit]");

    renderSmartHomeControls();

    if (list) {
        list.addEventListener("click", handleSmartHomeOptionClick);
    }

    document.querySelectorAll("[data-smart-home-confirm-cancel]").forEach(button => {
        button.addEventListener("click", closeSmartHomeConfirmModal);
    });

    if (overlay) {
        overlay.addEventListener("click", event => {
            if (event.target === overlay) {
                closeSmartHomeConfirmModal();
            }
        });
    }

    if (submit) {
        submit.addEventListener("click", handleSmartHomeToggleConfirm);
    }

    document.addEventListener("keydown", event => {
        if (event.key === "Escape" && overlay && !overlay.hidden) {
            closeSmartHomeConfirmModal();
        }
    });

    loadSmartHomeStatuses();
}

function setCommandButtonLoading(button, loading, loadingText = "处理中...") {
    if (!button) return;

    const label = button.querySelector("span");
    if (label && !button.dataset.originalLabel) {
        button.dataset.originalLabel = label.textContent;
    }

    button.disabled = loading;
    button.classList.toggle("is-loading", loading);

    if (label) {
        label.textContent = loading ? loadingText : button.dataset.originalLabel;
    }
}

function getCommandButton(action) {
    return document.querySelector(`[data-command-action="${action}"]`);
}

function buildSensorSnapshotText(rawSensor, sensor) {
    const parts = [];
    const temperature = toNumber(pickFirst(rawSensor, ["temperature", "temp"]));
    const humidity = toNumber(pickFirst(rawSensor, ["humidity"]));
    const airQualityObject = isPlainObject(rawSensor?.air_quality) ? rawSensor.air_quality : {};
    const aqi = toNumber(pickFirst(rawSensor, [
        "air_quality_score",
        "aqi",
        "air_quality"
    ])) ?? toNumber(pickFirst(airQualityObject, ["air_quality_score", "score", "aqi"]));

    if (temperature !== null) {
        parts.push(`温度 ${formatNumber(temperature)}°C`);
    }
    if (humidity !== null) {
        parts.push(`湿度 ${formatNumber(humidity)}%`);
    }
    if (aqi !== null) {
        parts.push(`空气质量 ${formatNumber(aqi, 0)} AQI`);
    }
    if (sensor && sensor.hasTimestamp) {
        parts.push(`时间 ${formatTime(sensor.timestamp)}`);
    }

    const esp = sensor ? getEspStatus(sensor) : null;
    if (esp && esp.value !== "--") {
        parts.push(`ESP 状态：${esp.value}`);
    }

    return parts.length > 0 ? `已获取当前传感器数据：${parts.join("，")}。` : "已获取当前传感器数据，但接口未返回可展示字段。";
}

function closeCustomCommandModal() {
    const overlay = document.querySelector("[data-custom-command-modal]");
    if (!overlay) return;

    overlay.hidden = true;
    document.body.classList.remove("log-modal-open");
}

function openCustomCommandModal() {
    const overlay = document.querySelector("[data-custom-command-modal]");
    const input = document.querySelector("[data-custom-command-input]");
    const error = document.querySelector("[data-custom-command-error]");
    if (!overlay || !input) return;

    input.value = "";
    if (error) error.textContent = "";
    updateCustomCommandCount();
    overlay.hidden = false;
    document.body.classList.add("log-modal-open");
    input.focus();
}

function updateCustomCommandCount() {
    const input = document.querySelector("[data-custom-command-input]");
    const count = document.querySelector("[data-custom-command-count]");
    if (!input || !count) return;

    count.textContent = `${input.value.length} / ${CUSTOM_COMMAND_MAX_LENGTH}`;
}

function closeCommandConfirmModal() {
    const overlay = document.querySelector("[data-command-confirm-modal]");
    if (!overlay) return;

    overlay.hidden = true;
    pendingConfirmAction = null;
    document.body.classList.remove("log-modal-open");
}

function openCommandConfirmModal(config) {
    const overlay = document.querySelector("[data-command-confirm-modal]");
    const title = document.querySelector("[data-command-confirm-title]");
    const message = document.querySelector("[data-command-confirm-message]");
    const submit = document.querySelector("[data-command-confirm-submit]");
    if (!overlay || !title || !message || !submit) return;

    pendingConfirmAction = config;
    title.textContent = config.title;
    message.textContent = config.message;
    submit.textContent = config.submitText;
    submit.classList.toggle("danger-action", Boolean(config.danger));
    overlay.hidden = false;
    document.body.classList.add("log-modal-open");
}

async function updateDashboard() {
    const [sensorResult, asrResult, llmResult, history, mockLogs, csiHistory] = await Promise.all([
        fetchLatestSensor(),
        fetchLatestASR(),
        fetchLatestLLM(),
        fetchHistoryData(),
        fetchAlertLogs(),
        fetchCsiHistory()
    ]);

    const sensor = normalizeSensor(sensorResult.data, sensorResult.source);

    dashboardState.sensor = sensor;
    dashboardState.asr = asrResult.data;
    dashboardState.llm = llmResult.data;
    dashboardState.sources = {
        sensor: sensorResult.source,
        asr: asrResult.source,
        llm: llmResult.source
    };
    dashboardState.metrics = buildMetrics(sensor);
    dashboardState.history = history;
    dashboardState.csiHistory = csiHistory;

    const dynamicAlerts = buildDynamicAlertLogs(dashboardState.metrics);
    dashboardState.alertLogs = dynamicAlerts.length > 0 ? [...dynamicAlerts, ...mockLogs].slice(0, 6) : mockLogs;
    dashboardState.systemLogs = buildSystemLogs(sensor, asrResult.data, llmResult.data, dashboardState.sources);

    renderMetricCards();
    renderMainChart();
    renderAlertSummary();
    renderAlertLogs();
    renderSystemLogs();
    renderCsiPanel();
    renderActiveLogModal();
    renderStatusHeader();
    renderSourceDebug();
}

// 命令控制：当前前端没有可提交自然语言请求的 POST/WebSocket 方法，所以自定义请求只提示未配置。
function getNaturalLanguageSubmitMethod() {
    return null;
}

// 命令控制：当前前端未发现重启、校准、服务器日志清理等设备操作请求方法。
function getDeviceOperationMethod() {
    return null;
}

async function handleFetchCurrentData(button) {
    const logId = addOperationLog({
        type: "获取当前数据",
        content: "调用现有 /api/dashboard/v1/sensors/latest 接口读取最新传感器数据。",
        status: "pending",
        result: "请求中..."
    });

    setCommandButtonLoading(button, true, "获取中...");

    try {
        const sensorResult = await fetchLatestSensor();
        if (sensorResult.source !== "real") {
            const message = "最新传感器接口不可用或返回空数据，未使用模拟数据更新快照。";
            updateOperationLog(logId, { status: "failed", result: message });
            showDashboardToast(message, "failed");
            return;
        }

        const sensor = normalizeSensor(sensorResult.data, sensorResult.source);
        dashboardState.sensor = sensor;
        dashboardState.sources.sensor = sensorResult.source;
        dashboardState.metrics = buildMetrics(sensor);
        dashboardState.systemLogs = buildSystemLogs(sensor, dashboardState.asr, dashboardState.llm, dashboardState.sources);

        const snapshot = buildSensorSnapshotText(sensorResult.data, sensor);
        renderMetricCards();
        renderMainChart();
        renderAlertSummary();
        renderAlertLogs();
        renderSystemLogs();
        renderStatusHeader();
        renderSourceDebug();
        renderActiveLogModal();

        updateOperationLog(logId, { status: "success", result: snapshot });
        showDashboardToast("已获取当前数据。", "success");
    } catch (error) {
        const message = `获取当前数据失败：${error.message}`;
        updateOperationLog(logId, { status: "failed", result: message });
        showDashboardToast(message, "failed");
    } finally {
        setCommandButtonLoading(button, false);
    }
}

function handleUnavailableOperation(type, content) {
    addOperationLog({
        type,
        content,
        status: "unavailable",
        result: "当前服务暂未配置"
    });
    showDashboardToast("当前服务暂未配置", "unavailable");
}

async function handleCustomCommandSubmit(event) {
    event.preventDefault();

    const input = document.querySelector("[data-custom-command-input]");
    const error = document.querySelector("[data-custom-command-error]");
    const submit = document.querySelector("[data-submit-custom-command]");
    if (!input || !submit) return;

    const content = input.value.trim();
    if (!content) {
        if (error) error.textContent = "请输入请求内容";
        return;
    }

    if (content.length > CUSTOM_COMMAND_MAX_LENGTH) {
        if (error) error.textContent = `最多 ${CUSTOM_COMMAND_MAX_LENGTH} 个字符`;
        return;
    }

    if (error) error.textContent = "";
    submit.disabled = true;
    submit.textContent = "提交中...";

    const submitMethod = getNaturalLanguageSubmitMethod();
    if (!submitMethod) {
        addOperationLog({
            type: "自定义请求",
            content,
            status: "unavailable",
            result: "当前服务暂未配置"
        });
        showDashboardToast("当前服务暂未配置", "unavailable");
        submit.disabled = false;
        submit.textContent = "提交";
        return;
    }

    const logId = addOperationLog({
        type: "自定义请求",
        content,
        status: "pending",
        result: "请求中..."
    });

    try {
        const result = await submitMethod(content);
        updateOperationLog(logId, {
            status: "success",
            result: typeof result === "string" ? result : JSON.stringify(result)
        });
        closeCustomCommandModal();
        showDashboardToast("自定义请求已返回。", "success");
    } catch (error) {
        updateOperationLog(logId, { status: "failed", result: error.message });
        showDashboardToast(`自定义请求失败：${error.message}`, "failed");
    } finally {
        submit.disabled = false;
        submit.textContent = "提交";
    }
}

function handleConfirmedUnavailableAction(action) {
    const config = {
        calibrate: {
            type: "校准传感器",
            content: "尝试发送传感器校准请求。"
        },
        reinitialize: {
            type: "重新初始化设备",
            content: "尝试发送设备重新初始化请求，设备可能短暂离线。"
        }
    }[action];

    if (!config) return;

    const method = getDeviceOperationMethod(action);
    if (!method) {
        handleUnavailableOperation(config.type, config.content);
        return;
    }
}

function handleClearLogs() {
    dashboardState.operationLogs = [];
    addOperationLog({
        type: "清理日志",
        content: "清理本页面临时操作记录。",
        status: "success",
        result: "仅清理了本页面临时记录，服务器记录未变更"
    });
    showDashboardToast("仅清理了本页面临时记录，服务器记录未变更", "success");
}

function handleCommandAction(action, button) {
    if (action === "custom") {
        openCustomCommandModal();
        return;
    }

    if (action === "fetch-data") {
        handleFetchCurrentData(button);
        return;
    }

    if (action === "calibrate") {
        openCommandConfirmModal({
            action,
            title: "确认校准传感器？",
            message: "校准期间传感器读数可能短暂波动，请确认是否继续。",
            submitText: "开始校准",
            danger: false
        });
        return;
    }

    if (action === "reinitialize") {
        openCommandConfirmModal({
            action,
            title: "确认重新初始化设备？",
            message: "设备可能短暂离线，请确认是否继续。",
            submitText: "确认重新初始化",
            danger: true
        });
        return;
    }

    if (action === "clear-logs") {
        openCommandConfirmModal({
            action,
            title: "确认清理日志？",
            message: "当前后端未提供服务器日志清理接口；确认后仅清理本页面临时操作记录。",
            submitText: "确认清理",
            danger: true
        });
    }
}

function bindCommandButtons() {
    document.querySelectorAll("[data-command-action]").forEach(button => {
        button.addEventListener("click", () => {
            handleCommandAction(button.dataset.commandAction, button);
        });
    });
}

// 命令控制：绑定自定义请求弹窗、确认弹窗和本页面临时操作记录。
function initCommandControls() {
    const customOverlay = document.querySelector("[data-custom-command-modal]");
    const confirmOverlay = document.querySelector("[data-command-confirm-modal]");
    const customForm = document.querySelector("[data-custom-command-form]");
    const customInput = document.querySelector("[data-custom-command-input]");
    const confirmSubmit = document.querySelector("[data-command-confirm-submit]");

    renderOperationLogs();

    if (customInput) {
        customInput.addEventListener("input", updateCustomCommandCount);
    }

    if (customForm) {
        customForm.addEventListener("submit", handleCustomCommandSubmit);
    }

    document.querySelectorAll("[data-close-custom-command]").forEach(button => {
        button.addEventListener("click", closeCustomCommandModal);
    });

    document.querySelectorAll("[data-command-confirm-cancel]").forEach(button => {
        button.addEventListener("click", closeCommandConfirmModal);
    });

    if (customOverlay) {
        customOverlay.addEventListener("click", event => {
            if (event.target === customOverlay) {
                closeCustomCommandModal();
            }
        });
    }

    if (confirmOverlay) {
        confirmOverlay.addEventListener("click", event => {
            if (event.target === confirmOverlay) {
                closeCommandConfirmModal();
            }
        });
    }

    if (confirmSubmit) {
        confirmSubmit.addEventListener("click", () => {
            const config = pendingConfirmAction;
            if (!config) return;

            confirmSubmit.disabled = true;
            if (config.action === "clear-logs") {
                handleClearLogs();
            } else {
                handleConfirmedUnavailableAction(config.action);
            }
            confirmSubmit.disabled = false;
            closeCommandConfirmModal();
        });
    }

    document.addEventListener("keydown", event => {
        if (event.key !== "Escape") return;

        if (customOverlay && !customOverlay.hidden) {
            closeCustomCommandModal();
        }
        if (confirmOverlay && !confirmOverlay.hidden) {
            closeCommandConfirmModal();
        }
    });
}

// 响应式侧边栏：仅控制手机端抽屉导航的打开和关闭，不修改菜单链接或业务逻辑。
function bindMobileSidebar() {
    const sidebar = document.getElementById("dashboardSidebar");
    const toggleButton = document.querySelector("[data-sidebar-toggle]");
    const overlay = document.querySelector("[data-sidebar-overlay]");
    if (!sidebar || !toggleButton || !overlay) return;

    const closeSidebar = () => {
        document.body.classList.remove("sidebar-open");
        toggleButton.setAttribute("aria-expanded", "false");
    };

    const openSidebar = () => {
        document.body.classList.add("sidebar-open");
        toggleButton.setAttribute("aria-expanded", "true");
    };

    toggleButton.addEventListener("click", () => {
        if (document.body.classList.contains("sidebar-open")) {
            closeSidebar();
        } else {
            openSidebar();
        }
    });

    overlay.addEventListener("click", closeSidebar);

    sidebar.querySelectorAll(".nav-item").forEach(item => {
        item.addEventListener("click", closeSidebar);
    });

    document.addEventListener("keydown", event => {
        if (event.key === "Escape") {
            closeSidebar();
        }
    });
}

function normalizeDashboardPage(value) {
    return ["s3", "c51", "c52"].includes(value) ? value : "c51";
}

function getDashboardPageFromHash() {
    return normalizeDashboardPage(window.location.hash.replace("#", ""));
}

function renderS3DashboardIfNeeded(force = false) {
    const container = document.querySelector("[data-s3-dashboard]");
    if (!container || !window.S3Dashboard || typeof window.S3Dashboard.render !== "function") {
        return;
    }

    if (!s3DashboardRendered || force) {
        window.S3Dashboard.render(container);
        s3DashboardRendered = true;
    }
}

function updateRouteChrome(page) {
    document.querySelectorAll("[data-dashboard-page]").forEach(item => {
        const isActive = item.dataset.dashboardPage === page;
        item.classList.toggle("active", isActive);
        item.setAttribute("aria-current", isActive ? "page" : "false");
    });

    const s3Page = document.querySelector('[data-page="s3"]');
    const cDevicePage = document.querySelector("[data-c-device-page]");
    if (s3Page) {
        s3Page.hidden = page !== "s3";
    }
    if (cDevicePage) {
        cDevicePage.hidden = page === "s3";
        cDevicePage.dataset.activeDevice = page === "c52" ? "c52" : "c51";
    }

    const dataSourcePanel = document.getElementById("dataSourcePanel");
    if (dataSourcePanel) {
        dataSourcePanel.hidden = page === "s3";
    }
}

function setDashboardPage(page, options = {}) {
    const nextPage = normalizeDashboardPage(page);
    activeDashboardPage = nextPage;
    updateRouteChrome(nextPage);

    if (nextPage === "s3") {
        cleanupDashboardTimers();
        renderS3DashboardIfNeeded();
        return;
    }

    if (options.refresh !== false) {
        updateDashboard();
    }
    startDashboardTimers();
}

function handleDashboardRoute() {
    setDashboardPage(getDashboardPageFromHash());
}

window.addEventListener("resize", () => {
    if (activeDashboardPage === "s3") {
        renderS3DashboardIfNeeded(true);
        return;
    }

    renderMainChart();
});

// 主题功能：切换黑白模式后只重绘现有 Canvas 图表颜色，不改变图表数据来源。
window.updateChartTheme = () => {
    if (activeDashboardPage === "s3") {
        renderS3DashboardIfNeeded(true);
        return;
    }

    renderMainChart();
    renderCsiPanel();
};

// 前端定时器：集中启动和清理 Dashboard 轮询，避免重复 setInterval。
function startDashboardTimers() {
    if (dashboardRefreshTimer) {
        clearInterval(dashboardRefreshTimer);
    }
    if (espDelayRefreshTimer) {
        clearInterval(espDelayRefreshTimer);
    }

    dashboardRefreshTimer = setInterval(updateDashboard, DASHBOARD_REFRESH_INTERVAL_MS);
    espDelayRefreshTimer = setInterval(refreshEspDelayDisplay, ESP_DELAY_REFRESH_INTERVAL_MS);
}

// 前端定时器：页面关闭或刷新时清理定时器，防止内存泄漏。
function cleanupDashboardTimers() {
    if (dashboardRefreshTimer) {
        clearInterval(dashboardRefreshTimer);
        dashboardRefreshTimer = null;
    }
    if (espDelayRefreshTimer) {
        clearInterval(espDelayRefreshTimer);
        espDelayRefreshTimer = null;
    }
}

document.addEventListener("DOMContentLoaded", () => {
    initThemeToggle();
    initChartRangeSelector();
    initLogModals();
    bindCommandButtons();
    initCommandControls();
    initSmartHomeControls();
    bindMobileSidebar();
    window.addEventListener("hashchange", handleDashboardRoute);
    handleDashboardRoute();
});

window.addEventListener("beforeunload", cleanupDashboardTimers);
