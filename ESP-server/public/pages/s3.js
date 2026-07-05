(function () {
    const s3MockData = {
        gateway: {
            name: "S3 Gateway",
            online: true,
            cloud_connected: true,
            latency_ms: 38,
            local_degraded: false,
            modules: [
                { id: "sensor_bridge", name: "传感器桥接", status: "running", detail: "C51 / C52 数据同步正常" },
                { id: "voice_router", name: "语音路由", status: "running", detail: "ASR 事件监听中" },
                { id: "command_queue", name: "命令队列", status: "running", detail: "无积压命令" },
                { id: "cloud_sync", name: "云端同步", status: "running", detail: "最近同步 12 秒前" },
                { id: "local_fallback", name: "本地降级", status: "standby", detail: "等待触发" }
            ]
        },
        devices: [
            {
                id: "c51",
                name: "C51 卧室",
                room: "卧室",
                online: true,
                sensors: {
                    temperature: 25.6,
                    humidity: 58.3,
                    pressure: 1009.4,
                    gas_resistance: 18240,
                    air_quality_score: 82,
                    air_quality_level: "良好"
                },
                appliances: {
                    air_conditioner: { name: "空调", online: true, state: "制冷 26°C" },
                    fan: { name: "风扇", online: true, state: "低速" },
                    light: { name: "灯", online: true, state: "关闭" },
                    tv: { name: "电视", online: false, state: "离线" },
                    curtain: { name: "窗帘", online: true, state: "半开" }
                }
            },
            {
                id: "c52",
                name: "C52 客厅",
                room: "客厅",
                online: true,
                sensors: {
                    temperature: 26.8,
                    humidity: 54.7,
                    pressure: 1011.1,
                    gas_resistance: 21680,
                    air_quality_score: 88,
                    air_quality_level: "优秀"
                },
                appliances: {
                    air_conditioner: { name: "空调", online: true, state: "待机" },
                    fan: { name: "风扇", online: true, state: "关闭" },
                    light: { name: "灯", online: true, state: "开启" },
                    tv: { name: "电视", online: true, state: "关闭" },
                    curtain: { name: "窗帘", online: true, state: "打开" }
                }
            }
        ],
        recent_voice_events: [
            { text: "打开客厅灯", room: "客厅", time: "14:21:08" },
            { text: "卧室空调调到二十六度", room: "卧室", time: "14:08:43" },
            { text: "关闭电视", room: "客厅", time: "13:56:12" },
            { text: "查看空气质量", room: "卧室", time: "13:42:30" }
        ],
        recent_commands: [
            { command: "light.turn_on", target: "C52 客厅灯", status: "completed", created_at: "14:21:09", completed_at: "14:21:10" },
            { command: "air_conditioner.set_temperature", target: "C51 卧室空调", status: "completed", created_at: "14:08:45", completed_at: "14:08:47" },
            { command: "tv.turn_off", target: "C52 客厅电视", status: "completed", created_at: "13:56:14", completed_at: "13:56:15" },
            { command: "air_quality.read", target: "C51 卧室传感器", status: "completed", created_at: "13:42:31", completed_at: "13:42:32" }
        ]
    };

    const applianceSlots = [
        { key: "air_conditioner", label: "空调", icon: "❄️" },
        { key: "light", label: "灯", icon: "💡" },
        { key: "fan", label: "风扇", icon: "🌀" },
        { key: "humidifier", label: "加湿", icon: "💧" },
        { key: "air_purifier", label: "净化", icon: "🌿" }
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
        "air_quality.read": "读取空气质量",
        "temperature.read": "读取温度",
        "humidity.read": "读取湿度"
    };

    const fieldDisplayMap = {
        TV: "电视",
        tv: "电视",
        Temperature: "温度",
        temperature: "温度",
        Humidity: "湿度",
        humidity: "湿度",
        Pressure: "气压",
        pressure: "气压",
        "Air Quality": "空气质量",
        air_quality: "空气质量"
    };

    function escapeHtml(value) {
        return String(value ?? "")
            .replace(/&/g, "&amp;")
            .replace(/</g, "&lt;")
            .replace(/>/g, "&gt;")
            .replace(/"/g, "&quot;")
            .replace(/'/g, "&#39;");
    }

    function formatNumber(value, digits = 1) {
        const numeric = Number(value);
        if (!Number.isFinite(numeric)) return "--";
        return Number(numeric.toFixed(digits)).toString();
    }

    function formatInteger(value) {
        const numeric = Number(value);
        if (!Number.isFinite(numeric)) return "--";
        return Math.round(numeric).toLocaleString("zh-CN");
    }

    function getStatusClass(status) {
        if (status === "running" || status === "completed" || status === true) return "normal";
        if (status === "standby" || status === "pending") return "warning";
        return "danger";
    }

    function getStatusText(status) {
        const textMap = {
            running: "运行中",
            standby: "待命",
            stopped: "已停止",
            completed: "已完成",
            pending: "处理中",
            failed: "失败"
        };
        if (status === true) return "在线";
        if (status === false) return "离线";
        return textMap[status] || status || "--";
    }

    function localizeCommandText(value) {
        const text = String(value ?? "");
        if (!text) return "--";
        if (commandDisplayMap[text]) return commandDisplayMap[text];

        return text
            .split(/([._\s-]+)/)
            .map(part => fieldDisplayMap[part] || part)
            .join("");
    }

    function getApplianceStatus(appliances, slot) {
        const appliance = appliances?.[slot.key];
        const rawState = String(appliance?.state ?? appliance?.status ?? "").trim();
        const normalizedState = rawState.toLowerCase();
        const explicitOn = appliance?.on ?? appliance?.enabled ?? appliance?.power;
        const hasExplicitOn = typeof explicitOn === "boolean";
        const inactiveStates = ["off", "offline", "closed", "standby", "关闭", "离线", "未接入", "待机"];
        const activeStates = ["on", "open", "active", "running", "开启", "打开", "运行", "低速", "中速", "高速", "制冷"];
        const isActive = hasExplicitOn
            ? explicitOn
            : Boolean(appliance?.online) &&
                !inactiveStates.includes(normalizedState) &&
                !inactiveStates.includes(rawState) &&
                (activeStates.some(state => normalizedState.includes(state) || rawState.includes(state)) || rawState !== "");

        return {
            label: appliance?.name || slot.label,
            icon: slot.icon,
            state: rawState || (appliance ? "关闭" : "未接入"),
            isActive
        };
    }

    function buildHomeSummary(devices) {
        const onlineDevices = devices.filter(device => device.online).length;
        const offlineDevices = devices.length - onlineDevices;
        const sensorValues = devices.map(device => device.sensors);
        const average = (key, digits = 1) => {
            const values = sensorValues
                .map(sensor => Number(sensor[key]))
                .filter(Number.isFinite);
            if (!values.length) return "--";
            const total = values.reduce((sum, value) => sum + value, 0);
            return formatNumber(total / values.length, digits);
        };

        let applianceOnline = 0;
        let applianceOffline = 0;
        devices.forEach(device => {
            Object.values(device.appliances || {}).forEach(appliance => {
                if (appliance.online) {
                    applianceOnline += 1;
                } else {
                    applianceOffline += 1;
                }
            });
        });

        return {
            avg_temperature: average("temperature"),
            avg_humidity: average("humidity"),
            avg_air_quality: average("air_quality_score", 0),
            online_device_count: onlineDevices + applianceOnline,
            offline_device_count: offlineDevices + applianceOffline
        };
    }

    function renderGatewayPanel(gateway) {
        return `
            <article class="panel s3-gateway-panel">
                <div class="panel-header">
                    <h2>网关状态面板</h2>
                    <span class="state-badge state-${getStatusClass(gateway.online)}">${getStatusText(gateway.online)}</span>
                </div>
                <div class="s3-status-grid">
                    ${renderStatusTile("S3 在线状态", getStatusText(gateway.online), getStatusClass(gateway.online))}
                    ${renderStatusTile("云端连接状态", gateway.cloud_connected ? "已连接" : "未连接", getStatusClass(gateway.cloud_connected))}
                    ${renderStatusTile("延迟", `${formatInteger(gateway.latency_ms)} ms`, gateway.latency_ms <= 80 ? "normal" : "warning")}
                    ${renderStatusTile("本地降级状态", gateway.local_degraded ? "已启用" : "未启用", gateway.local_degraded ? "warning" : "normal")}
                </div>
                <div class="s3-module-list" aria-label="各模块运行状态">
                    ${gateway.modules.map(module => `
                        <div class="s3-module-row">
                            <span class="status-dot ${module.status === "running" ? "online" : ""}"></span>
                            <div>
                                <strong>${escapeHtml(module.name)}</strong>
                                <small>${escapeHtml(module.detail)}</small>
                            </div>
                            <span class="level-badge level-${getStatusClass(module.status)}">${getStatusText(module.status)}</span>
                        </div>
                    `).join("")}
                </div>
            </article>
        `;
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

    function renderHomeSummary(summary) {
        const items = [
            { label: "全屋平均温度", value: `${summary.avg_temperature}°C`, accent: "blue" },
            { label: "全屋平均湿度", value: `${summary.avg_humidity}%`, accent: "green" },
            { label: "全屋平均空气质量", value: `${summary.avg_air_quality} 分`, accent: "purple" },
            { label: "在线 / 离线设备", value: `${summary.online_device_count} / ${summary.offline_device_count}`, accent: "orange" }
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

    function renderDeviceOverview(devices) {
        return `
            <section class="s3-section">
                <div class="s3-section-heading">
                    <h2>设备总览</h2>
                </div>
                <div class="s3-device-grid">
                    ${devices.map(renderDeviceCard).join("")}
                </div>
            </section>
        `;
    }

    function renderDeviceCard(device) {
        const sensors = device.sensors;
        return `
            <article class="panel s3-device-card">
                <div class="panel-header">
                    <h2>${escapeHtml(device.name)}</h2>
                    <span class="state-badge state-${getStatusClass(device.online)}">${getStatusText(device.online)}</span>
                </div>
                <div class="s3-sensor-grid">
                    ${renderSensorMetric("温湿度", `${formatNumber(sensors.temperature)}°C / ${formatNumber(sensors.humidity)}%`)}
                    ${renderSensorMetric("气压", `${formatNumber(sensors.pressure)} hPa`)}
                    ${renderSensorMetric("空气质量", `${formatNumber(sensors.air_quality_score, 0)} 分 · ${sensors.air_quality_level}`)}
                </div>
                <div class="s3-appliance-grid" aria-label="${escapeHtml(device.room)}设备状态">
                    ${applianceSlots.map(slot => {
                        const appliance = getApplianceStatus(device.appliances, slot);
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

    function renderSensorMetric(label, value) {
        return `
            <div class="s3-sensor-metric">
                <span>${escapeHtml(label)}</span>
                <strong>${escapeHtml(value)}</strong>
            </div>
        `;
    }

    function renderRecentCommands(data) {
        return `
            <section class="s3-activity-grid">
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
                            <tbody>
                                ${data.recent_commands.map(command => `
                                    <tr>
                                        <td>${escapeHtml(localizeCommandText(command.command))}</td>
                                        <td>${escapeHtml(command.target)}</td>
                                        <td><span class="level-badge level-${getStatusClass(command.status)}">${getStatusText(command.status)}</span></td>
                                        <td>${escapeHtml(command.created_at)}</td>
                                        <td>${escapeHtml(command.completed_at)}</td>
                                    </tr>
                                `).join("")}
                            </tbody>
                        </table>
                    </div>
                </article>
            </section>
        `;
    }

    function render(container, data = s3MockData) {
        if (!container) return;

        const summary = data.home_summary || buildHomeSummary(data.devices || []);
        container.innerHTML = `
            <div class="s3-dashboard">
                <div class="s3-page-header">
                    <div>
                        <h1>S3 系统总览</h1>
                        <p>系统总览与全屋状态面板</p>
                    </div>
                    <span class="state-badge state-normal">模拟数据</span>
                </div>
                <div class="s3-overview-grid">
                    ${renderGatewayPanel(data.gateway)}
                    ${renderHomeSummary(summary)}
                </div>
                ${renderDeviceOverview(data.devices || [])}
                ${renderRecentCommands(data)}
            </div>
        `;
    }

    window.S3Dashboard = {
        mockData: s3MockData,
        render
    };
})();
