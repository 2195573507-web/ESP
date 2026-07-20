(function () {
    "use strict";

    const SCHEMA_VERSION = 1;
    const MAX_PROMPT_LENGTH = 95;
    const tabs = [
        { key: "overview", label: "运行概览" },
        { key: "rooms", label: "房间配置" },
        { key: "rules", label: "规则与部署" },
        { key: "devices", label: "虚拟设备" },
        { key: "decisions", label: "决策与抑制" },
        { key: "learning", label: "反馈与记忆" },
        { key: "tools", label: "工具与简报" }
    ];
    const conditionFields = [
        ["presence_state", "有人状态"],
        ["stable_target_count", "稳定人数"],
        ["occupancy_mode", "人数模式"],
        ["environment_fresh", "环境数据新鲜"],
        ["radar_fresh", "雷达数据新鲜"],
        ["quiet_state", "静音状态"],
        ["time_window", "时间窗"],
        ["temperature_c", "温度"],
        ["humidity_percent", "湿度"],
        ["air_quality_score", "空气质量"],
        ["weather_dark", "天气昏暗"]
    ];
    const conditionOperators = [
        ["eq", "等于"],
        ["neq", "不等于"],
        ["gt", "大于"],
        ["gte", "大于等于"],
        ["lt", "小于"],
        ["lte", "小于等于"],
        ["in", "属于"],
        ["range", "范围"]
    ];
    const deviceTypes = [
        ["light", "灯"],
        ["air_conditioner", "空调"],
        ["fan", "风扇"]
    ];
    const actionTypes = [
        ["turn_on", "打开"],
        ["turn_off", "关闭"],
        ["play_prompt", "播报"]
    ];
    const state = {
        root: null,
        activeTab: "overview",
        data: {},
        errors: {},
        loading: false,
        dirty: false,
        draft: null,
        timer: null,
        noticeTimer: null
    };

    function escapeHtml(value) {
        return String(value ?? "")
            .replace(/&/g, "&amp;")
            .replace(/</g, "&lt;")
            .replace(/>/g, "&gt;")
            .replace(/"/g, "&quot;")
            .replace(/'/g, "&#39;");
    }

    function formatTime(value) {
        if (value === null || value === undefined || value === "") return "暂无时间";
        const numeric = Number(value);
        const date = Number.isFinite(numeric)
            ? new Date(numeric < 10000000000 ? numeric * 1000 : numeric)
            : new Date(value);
        return Number.isNaN(date.getTime()) ? "暂无时间" : date.toLocaleString("zh-CN", {
            hour12: false,
            month: "2-digit",
            day: "2-digit",
            hour: "2-digit",
            minute: "2-digit"
        });
    }

    function unwrap(payload) {
        return payload && payload.ok && Object.prototype.hasOwnProperty.call(payload, "data")
            ? payload.data
            : payload;
    }

    async function request(path, options = {}) {
        const response = await fetch(path, {
            cache: "no-store",
            headers: {
                "Content-Type": "application/json",
                ...(options.headers || {})
            },
            ...options
        });
        let body = null;
        try {
            body = await response.json();
        } catch (_) {
            body = null;
        }
        if (!response.ok) {
            const error = new Error(body?.error?.message || body?.message || body?.error || `HTTP ${response.status}`);
            error.status = response.status;
            error.code = body?.error?.code || body?.code || "HOME_AI_REQUEST_FAILED";
            throw error;
        }
        return unwrap(body);
    }

    function friendlyError(error) {
        const code = String(error?.code || "");
        const labels = {
            WEATHER_TIMEOUT: "天气服务超时，未使用旧数据。",
            WEATHER_STALE: "天气数据已过期，依赖天气的规则已跳过。",
            WEATHER_LOCATION_NOT_CONFIGURED: "请先配置有效的家庭位置。",
            NEWS_TIMEOUT: "新闻服务超时。",
            NEWS_STALE: "没有可用的新鲜新闻。",
            NEWS_NOT_CONFIGURED: "请先配置 HTTPS 新闻服务。",
            HOME_LOCATION_INVALID: "家庭位置或时区无效，请使用 IANA 时区。"
        };
        if (labels[code]) return labels[code];
        if (error?.status === 409) return "当前版本已变化，请重新读取后再试。";
        if (error?.status === 400) return "提交内容未通过校验。";
        if (error?.status === 404) return "目标记录不存在或已过期。";
        return "部分 Home AI 数据暂时不可用。";
    }

    function showNotice(message, tone = "info") {
        const node = state.root?.querySelector("[data-home-ai-notice]");
        if (!node) return;
        node.hidden = !message;
        node.className = `home-ai-notice ${tone}`;
        node.textContent = message || "";
        if (state.noticeTimer) clearTimeout(state.noticeTimer);
        if (message) {
            state.noticeTimer = setTimeout(() => {
                node.hidden = true;
            }, 6000);
        }
    }

    function setStatus(text, tone = "neutral") {
        const node = state.root?.querySelector("[data-home-ai-sync]");
        if (!node) return;
        node.className = `home-ai-sync ${tone}`;
        node.textContent = text;
    }

    function dataOr(key, fallback) {
        return state.data[key] === undefined ? fallback : state.data[key];
    }

    function rooms() {
        return Array.isArray(dataOr("rooms", {})?.rooms) ? dataOr("rooms", {}).rooms : [];
    }

    function currentPackage() {
        return dataOr("current", {})?.rule_package || null;
    }

    function roomName(roomId) {
        return rooms().find(room => room.room_id === roomId)?.room_name || roomId || "未分配";
    }

    function statusLabel(value) {
        const labels = {
            PUBLISHED: "已发布",
            ACTIVE: "已生效",
            ACTIVE_PARTIAL: "部分生效",
            REJECTED: "已拒绝",
            ROLLED_BACK: "已回滚",
            MIGRATED: "已迁移",
            PENDING_REBIND: "待重绑",
            PENDING: "等待下发",
            SUPERSEDED: "已被替代",
            CANDIDATE: "候选",
            READY: "门禁通过",
            PROBATION: "试运行",
            CONFIRMED: "已确认",
            REJECTED_MEMORY: "已拒绝",
            DISABLED: "已停用",
            RUNNING: "运行中",
            FAILED: "失败",
            SUSPENDED: "已暂停",
            EXPIRED: "证据不足",
            WAITING_ACK: "等待 ACK",
            PARTIAL: "部分完成",
            COMPLETED: "已完成",
            SUCCEEDED: "成功",
            PLANNED: "已规划",
            SUPPRESSED: "已抑制"
        };
        return labels[String(value || "").toUpperCase()] || String(value || "未知");
    }

    function statusTone(value) {
        const text = String(value || "").toUpperCase();
        if (["ACTIVE", "PUBLISHED", "CONFIRMED", "COMPLETED", "SUCCEEDED", "MIGRATED"].includes(text)) return "good";
        if (["ACTIVE_PARTIAL", "PENDING", "CANDIDATE", "READY", "PROBATION", "RUNNING", "WAITING_ACK", "PARTIAL", "PLANNED", "SUPPRESSED"].includes(text)) return "warn";
        if (["REJECTED", "ROLLED_BACK", "FAILED", "DISABLED", "SUSPENDED", "EXPIRED", "PENDING_REBIND"].includes(text)) return "bad";
        return "neutral";
    }

    function badge(value) {
        return `<span class="home-ai-badge ${statusTone(value)}">${escapeHtml(statusLabel(value))}</span>`;
    }

    function optionList(options, selected) {
        return options.map(([value, label]) =>
            `<option value="${escapeHtml(value)}"${value === selected ? " selected" : ""}>${escapeHtml(label)}</option>`
        ).join("");
    }

    function latestEvents(type) {
        return (Array.isArray(dataOr("events", {})?.events) ? dataOr("events", {}).events : [])
            .filter(event => !type || event.event_type === type)
            .sort((a, b) => Number(b.received_at_ms || b.occurred_at_ms || 0) - Number(a.received_at_ms || a.occurred_at_ms || 0));
    }

    function agentDecisions() {
        return Array.isArray(dataOr("agentDecisions", {})?.decisions)
            ? dataOr("agentDecisions", {}).decisions
            : [];
    }

    function renderShell(root) {
        root.innerHTML = `
            <section class="home-ai-workspace" aria-labelledby="homeAiTitle">
                <header class="home-ai-header">
                    <div>
                        <span class="home-ai-kicker">LOCAL RUNTIME</span>
                        <h2 id="homeAiTitle">Home AI 本地自治</h2>
                        <p class="home-ai-subtitle">规则、房间状态与虚拟执行的实际记录</p>
                    </div>
                    <div class="home-ai-header-actions">
                        <span class="home-ai-sync neutral" data-home-ai-sync>准备读取</span>
                        <button class="home-ai-icon-button" type="button" data-home-ai-action="refresh" title="刷新 Home AI 数据" aria-label="刷新 Home AI 数据">↻</button>
                    </div>
                </header>
                <div class="home-ai-notice info" data-home-ai-notice hidden role="status"></div>
                <nav class="home-ai-tabs" role="tablist" aria-label="Home AI 页面">
                    ${tabs.map((tab, index) => `
                        <button type="button" role="tab" class="home-ai-tab${index === 0 ? " active" : ""}"
                            aria-selected="${index === 0 ? "true" : "false"}" data-home-ai-tab="${tab.key}">
                            ${escapeHtml(tab.label)}
                        </button>
                    `).join("")}
                </nav>
                <div class="home-ai-view" data-home-ai-view role="tabpanel" tabindex="-1"></div>
            </section>
        `;
        root.addEventListener("click", handleClick);
        root.addEventListener("submit", handleSubmit);
        root.addEventListener("input", handleInput);
        root.addEventListener("change", handleInput);
        renderView();
    }

    function renderErrorList() {
        const values = Object.values(state.errors).filter(Boolean);
        if (!values.length) return "";
        return `<div class="home-ai-inline-error" role="status">${escapeHtml(friendlyError(values[0]))}</div>`;
    }

    function renderOverview() {
        const pkg = currentPackage();
        const deployments = dataOr("deployments", {})?.deployments || [];
        const latestDeployment = deployments[0];
        const devices = dataOr("devices", {})?.devices || [];
        const buffer = latestEvents("offline_buffer")[0];
        const bufferPayload = buffer?.payload || {};
        const roomEvents = latestEvents("room_state");
        const latestByRoom = new Map();
        roomEvents.forEach(event => {
            if (event.room_id && !latestByRoom.has(event.room_id)) latestByRoom.set(event.room_id, event);
        });
        const decisions = latestEvents().filter(event => ["decision", "suppressed_action", "virtual_device_state"].includes(event.event_type)).slice(0, 6);
        const emergencies = latestEvents("emergency")
            .filter(event => String(event.payload?.state || "").toUpperCase() !== "RESOLVED")
            .slice(0, 8);
        return `
            <div class="home-ai-status-strip" aria-label="Home AI 状态摘要">
                <div><span>规则版本</span><strong>${escapeHtml(pkg?.version ?? "未发布")}</strong></div>
                <div><span>最近部署</span><strong>${escapeHtml(latestDeployment ? statusLabel(latestDeployment.state) : "暂无记录")}</strong></div>
                <div><span>虚拟设备</span><strong>${escapeHtml(String(devices.length))}</strong></div>
                <div><span>同步时间</span><strong>${escapeHtml(formatTime(dataOr("loadedAt", null)))}</strong></div>
            </div>
            ${renderErrorList()}
            ${emergencies.length ? `<section class="home-ai-block"><div class="home-ai-block-heading"><div><h3>紧急告警</h3><span>确认只降低播报频率，不会关闭安全保护</span></div><span>${emergencies.length} 条活动事件</span></div><div class="home-ai-decision-list">${emergencies.map(renderEmergencyLine).join("")}</div></section>` : ""}
            <div class="home-ai-overview-grid">
                <section class="home-ai-block">
                    <div class="home-ai-block-heading"><h3>房间状态</h3><span>${rooms().length} 个房间</span></div>
                    <div class="home-ai-room-list">
                        ${rooms().map(room => {
                            const event = latestByRoom.get(room.room_id);
                            const payload = event?.payload || {};
                            return `<div class="home-ai-room-row">
                                <div><strong>${escapeHtml(room.room_name)}</strong><small>${escapeHtml(room.room_id)}</small></div>
                                <span class="home-ai-state-dot ${payload.presence_state === "occupied" ? "occupied" : payload.presence_state === "vacant" ? "vacant" : "unknown"}"></span>
                                <span>${escapeHtml(payload.presence_state === "occupied" ? "有人" : payload.presence_state === "vacant" ? "无人" : "未知")}</span>
                                <span class="home-ai-room-count">${escapeHtml(payload.stable_target_count === undefined ? "--" : `${payload.stable_target_count} 人`)}</span>
                            </div>`;
                        }).join("") || `<div class="home-ai-empty">暂无房间配置</div>`}
                    </div>
                </section>
                <section class="home-ai-block">
                    <div class="home-ai-block-heading"><h3>离线缓冲</h3><span>${buffer ? formatTime(buffer.occurred_at_ms) : "暂无事件"}</span></div>
                    <div class="home-ai-buffer-readout">
                        <strong>${buffer ? `${Number(bufferPayload.capacity_percent || 0)}%` : "--"}</strong>
                        <span>${bufferPayload.capacity_warning ? "接近容量上限" : buffer ? "容量正常" : "等待 S3 上报"}</span>
                    </div>
                    <dl class="home-ai-definition-list">
                        <div><dt>未上传</dt><dd>${buffer ? Number(bufferPayload.unuploaded_count || 0) : "--"}</dd></div>
                        <div><dt>已落盘</dt><dd>${buffer ? Number(bufferPayload.persisted_count || 0) : "--"}</dd></div>
                        <div><dt>未持久化丢弃</dt><dd>${buffer ? Number(bufferPayload.dropped_unpersisted || 0) : "--"}</dd></div>
                        <div><dt>覆盖丢弃</dt><dd>${buffer ? Number(bufferPayload.dropped_overwrite || 0) : "--"}</dd></div>
                        <div><dt>存储错误</dt><dd>${buffer ? Number(bufferPayload.storage_errors || 0) : "--"}</dd></div>
                        <div><dt>留存淘汰</dt><dd>${buffer ? Number(bufferPayload.retention_evictions || 0) : "--"}</dd></div>
                        <div><dt>受保护拒绝</dt><dd>${buffer ? Number(bufferPayload.protected_rejections || 0) : "--"}</dd></div>
                    </dl>
                </section>
            </div>
            <section class="home-ai-block">
                <div class="home-ai-block-heading"><h3>最近决策</h3><button class="home-ai-link" type="button" data-home-ai-tab-jump="decisions">查看全部</button></div>
                <div class="home-ai-decision-list">
                    ${decisions.map(renderDecisionLine).join("") || `<div class="home-ai-empty">暂无决策事件</div>`}
                </div>
            </section>
        `;
    }

    function renderDecisionLine(event) {
        const payload = event.payload || {};
        const title = event.event_type === "suppressed_action" ? "动作被抑制" : event.event_type === "virtual_device_state" ? "虚拟设备状态" : "规则执行";
        const detail = payload.reason || payload.decision_reason || payload.action || payload.power || "已记录";
        return `<div class="home-ai-decision-line">
            <time>${escapeHtml(formatTime(event.occurred_at_ms))}</time>
            <div><strong>${escapeHtml(title)} · ${escapeHtml(roomName(event.room_id))}</strong><small>${escapeHtml(detail)}</small></div>
            ${event.event_type === "suppressed_action" ? `<span class="home-ai-badge warn">已抑制</span>` : `<span class="home-ai-badge neutral">已记录</span>`}
        </div>`;
    }

    function renderEmergencyLine(event) {
        const payload = event.payload || {};
        const emergencyState = String(payload.state || "DETECTED").toUpperCase();
        const acknowledged = emergencyState === "ACKNOWLEDGED" || event.user_acknowledged === true;
        return `<div class="home-ai-decision-line"><time>${escapeHtml(formatTime(event.occurred_at_ms))}</time><div><strong>${escapeHtml(emergencyState)} · ${escapeHtml(roomName(event.room_id))}</strong><small>环境紧急事件</small></div><span class="home-ai-badge ${acknowledged ? "good" : "bad"}">${acknowledged ? "已确认" : "需确认"}</span>${acknowledged ? "" : `<button class="home-ai-mini" type="button" data-home-ai-emergency-ack="${escapeHtml(event.event_id)}">确认告警</button>`}</div>`;
    }

    function renderRooms() {
        return `
            ${renderErrorList()}
            <form class="home-ai-form" data-home-ai-form="rooms">
                <div class="home-ai-form-heading"><div><h3>房间与感知来源</h3><p>修改后由 S3 在下一次配置同步中原子切换。</p></div><button class="home-ai-primary" type="submit">保存配置</button></div>
                <div class="home-ai-room-editor-grid">
                    ${rooms().map((room, index) => `
                        <fieldset class="home-ai-room-editor">
                            <legend>${escapeHtml(room.room_name || room.room_id)}</legend>
                            <div class="home-ai-field-grid">
                                ${inputField("房间 ID", "room_id", room.room_id, "text", true, index)}
                                ${inputField("房间名称", "room_name", room.room_name, "text", false, index)}
                                ${inputField("感知来源（逗号分隔）", "sensing_sources", (room.sensing_sources || []).join(","), "text", false, index)}
                                ${inputField("语音终端 ID", "voice_terminal_device_id", room.voice_terminal_device_id || "", "text", false, index)}
                                ${inputField("有人确认（秒）", "presence_confirm_ms", Number(room.presence_confirm_ms || 1500) / 1000, "number", false, index, "0.5", "30")}
                                ${inputField("无人确认（秒）", "vacant_confirm_ms", Number(room.vacant_confirm_ms || 60000) / 1000, "number", false, index, "10", "900")}
                                ${inputField("多人确认（秒）", "multiple_confirm_ms", Number(room.multiple_confirm_ms || 3000) / 1000, "number", false, index, "0.5", "60")}
                                ${inputField("单人确认（秒）", "single_confirm_ms", Number(room.single_confirm_ms || 10000) / 1000, "number", false, index, "0.5", "120")}
                                ${inputField("静音开始", "quiet_start", room.quiet_start || "23:00", "time", false, index)}
                                ${inputField("静音结束", "quiet_end", room.quiet_end || "07:00", "time", false, index)}
                            </div>
                        </fieldset>
                    `).join("")}
                </div>
            </form>
        `;
    }

    function inputField(label, field, value, type, required, index, min = "", max = "") {
        return `<label class="home-ai-field"><span>${escapeHtml(label)}</span><input data-room-index="${index}" data-room-field="${escapeHtml(field)}" type="${type}" value="${escapeHtml(value)}"${required ? " required" : ""}${type === "number" ? " step=\"0.1\"" : ""}${min ? ` min="${min}"` : ""}${max ? ` max="${max}"` : ""}></label>`;
    }

    function ruleInputField(label, field, value, type, min = "", max = "") {
        return `<label class="home-ai-field"><span>${escapeHtml(label)}</span><input data-rule-field="${escapeHtml(field)}" type="${type}" value="${escapeHtml(value)}" required${min ? ` min="${min}"` : ""}${max ? ` max="${max}"` : ""}></label>`;
    }

    function renderRuleRow(rule) {
        const binding = rule.binding || {};
        const bindingBadge = binding.state === "PENDING_REBIND"
            ? badge("PENDING_REBIND")
            : binding.state === "MIGRATED" ? badge("MIGRATED") : "";
        const enabledBadge = rule.enabled === false
            ? `<span class="home-ai-badge neutral">已停用</span>`
            : `<span class="home-ai-badge good">启用</span>`;
        const detail = binding.state === "PENDING_REBIND"
            ? `待重绑 · 原房间 ${binding.from_room_id || rule.room_id}`
            : `${roomName(rule.room_id)} · 优先级 ${rule.priority}`;
        return `<div class="home-ai-rule-row"><div><strong>${escapeHtml(rule.rule_id)}</strong><small>${escapeHtml(detail)}</small></div>${bindingBadge}${enabledBadge}<button class="home-ai-link" type="button" data-home-ai-load-rule="${escapeHtml(rule.rule_id)}">编辑</button></div>`;
    }

    function renderRules() {
        const pkg = currentPackage();
        const packages = dataOr("rules", {})?.packages || [];
        const deployments = dataOr("deployments", {})?.deployments || [];
        const draft = state.draft || defaultDraft(pkg);
        return `
            ${renderErrorList()}
            <div class="home-ai-rule-toolbar">
                <div><h3>规则包 ${pkg ? `v${escapeHtml(pkg.version)}` : "未发布"}</h3><p>${pkg ? `${pkg.rules?.length || 0} 条规则，${statusLabel(pkg.status)}` : "先创建第一条本地规则"}</p>${pkg?.control?.reason === "room_config_migration" ? `<p class="home-ai-migration-note">房间配置变更已触发规则迁移：${Number(pkg.control.pending_rebind_rule_ids?.length || 0)} 条待重绑，${Number(pkg.control.migrated_rule_ids?.length || 0)} 条已迁移。</p>` : ""}</div>
                <div class="home-ai-button-row"><button class="home-ai-secondary" type="button" data-home-ai-action="new-rule">新建规则</button>${packages.length > 1 ? `<button class="home-ai-secondary danger-text" type="button" data-home-ai-action="rollback">回滚到上一版</button>` : ""}</div>
            </div>
            <section class="home-ai-rule-list" aria-label="当前规则">
                ${(pkg?.rules || []).map(renderRuleRow).join("") || `<div class="home-ai-empty">当前规则包没有规则</div>`}
            </section>
            <div class="home-ai-rule-layout">
                <form class="home-ai-form home-ai-rule-form" data-home-ai-form="rule">
                    <div class="home-ai-form-heading"><div><h3>规则编辑器</h3><p>受限条件 DSL，最多 8 条条件和 4 个动作。</p></div><button class="home-ai-primary" type="submit">验证并发布</button></div>
                    <div class="home-ai-field-grid">
                        ${ruleInputField("规则 ID", "rule_id", draft.rule_id, "text")}
                        <label class="home-ai-field"><span>房间</span><select data-rule-field="room_id" required>${rooms().map(room => `<option value="${escapeHtml(room.room_id)}"${room.room_id === draft.room_id ? " selected" : ""}>${escapeHtml(room.room_name)}</option>`).join("")}</select></label>
                        ${ruleInputField("优先级", "priority", draft.priority, "number", "0", "1000")}
                        ${ruleInputField("冷却（秒）", "cooldown_seconds", draft.cooldown_seconds, "number", "0", "86400")}
                        ${ruleInputField("最小保持（秒）", "minimum_active_seconds", draft.minimum_active_seconds, "number", "0", "86400")}
                        <label class="home-ai-field"><span>离线策略</span><select data-rule-field="offline_policy"><option value="continue"${draft.offline_policy === "continue" ? " selected" : ""}>继续执行</option><option value="pause"${draft.offline_policy === "pause" ? " selected" : ""}>暂停</option><option value="require_server"${draft.offline_policy === "require_server" ? " selected" : ""}>需要联网</option></select></label>
                    </div>
                    <label class="home-ai-checkbox"><input type="checkbox" data-rule-field="enabled"${draft.enabled !== false ? " checked" : ""}>启用规则</label>
                    <div class="home-ai-subform-heading"><h4>全部条件（AND）</h4><button class="home-ai-link" type="button" data-home-ai-action="add-condition">添加条件</button></div>
                    <div class="home-ai-condition-list" data-condition-list>${(draft.conditions || []).map(renderConditionRow).join("")}</div>
                    <div class="home-ai-subform-heading"><h4>执行动作</h4><button class="home-ai-link" type="button" data-home-ai-action="add-action">添加动作</button></div>
                    <div class="home-ai-action-list" data-action-list>${(draft.actions || []).map(renderActionRow).join("")}</div>
                </form>
                <section class="home-ai-block home-ai-deployment-block">
                    <div class="home-ai-block-heading"><h3>部署状态</h3><span>${deployments.length} 条记录</span></div>
                    ${deployments.length ? `<div class="home-ai-deployment-list">${deployments.slice(0, 8).map(renderDeployment).join("")}</div>` : `<div class="home-ai-empty">尚无 S3 部署回执</div>`}
                </section>
            </div>
            <section class="home-ai-block">
                <div class="home-ai-block-heading"><h3>历史版本</h3><span>发布版本和回滚版本</span></div>
                <div class="home-ai-version-list">${packages.slice(0, 8).map(pkgItem => `<div class="home-ai-version-row"><div><strong>v${escapeHtml(pkgItem.version)}</strong><small>${escapeHtml(formatTime(pkgItem.published_at_ms))}</small></div>${badge(pkgItem.status)}<span>${escapeHtml(String(pkgItem.rules?.length || 0))} 条规则</span></div>`).join("") || `<div class="home-ai-empty">暂无历史版本</div>`}</div>
            </section>
        `;
    }

    function defaultDraft(pkg) {
        const roomId = rooms()[0]?.room_id || "living_room";
        return {
            rule_id: "presence_light",
            room_id: roomId,
            enabled: true,
            priority: 500,
            cooldown_seconds: 120,
            minimum_active_seconds: 0,
            offline_policy: "continue",
            conditions: [{ field: "presence_state", operator: "eq", value: "occupied", duration_ms: 0 }],
            actions: [{ device_id: `${roomId}_light`, device_type: "light", action: "turn_on", prompt: "" }],
            version: Number(pkg?.version || 0) + 1
        };
    }

    function renderConditionRow(condition = {}) {
        const value = Array.isArray(condition.value) ? condition.value.join(",") : condition.value ?? "";
        return `<div class="home-ai-builder-row" data-condition-row>
            <select data-condition-field aria-label="条件字段">${optionList(conditionFields, condition.field || "presence_state")}</select>
            <select data-condition-operator aria-label="条件运算符">${optionList(conditionOperators, condition.operator || "eq")}</select>
            <input data-condition-value aria-label="条件值" type="text" value="${escapeHtml(value)}" placeholder="例如 occupied 或 26">
            <input data-condition-duration aria-label="持续秒数" type="number" min="0" max="3600" value="${Math.round(Number(condition.duration_ms || 0) / 1000)}" placeholder="持续秒">
            <button class="home-ai-remove-button" type="button" data-home-ai-action="remove-condition" title="删除条件" aria-label="删除条件">×</button>
        </div>`;
    }

    function renderActionRow(action = {}) {
        return `<div class="home-ai-builder-row" data-action-row>
            <select data-action-type aria-label="设备类型">${optionList(deviceTypes, action.device_type || "light")}</select>
            <input data-action-device aria-label="设备 ID" type="text" value="${escapeHtml(action.device_id || "")}" placeholder="设备 ID">
            <select data-action-name aria-label="动作">${optionList(actionTypes, action.action || "turn_on")}</select>
            <input data-action-prompt aria-label="播报文本" type="text" maxlength="${MAX_PROMPT_LENGTH}" value="${escapeHtml(action.prompt || "")}" placeholder="播报文本（可选）">
            <button class="home-ai-remove-button" type="button" data-home-ai-action="remove-action" title="删除动作" aria-label="删除动作">×</button>
        </div>`;
    }

    function renderDeployment(deployment) {
        const result = deployment.result || {};
        const items = Array.isArray(result.items) ? result.items.filter(item => !item.accepted) : [];
        return `<details class="home-ai-deployment-row"><summary><span>v${escapeHtml(deployment.package_version)}</span>${badge(deployment.state)}<span>${escapeHtml(deployment.gateway_id || "S3")}</span><time>${escapeHtml(formatTime(deployment.updated_at_ms))}</time></summary>
            <div class="home-ai-deployment-detail"><span>已接受 ${Number(result.accepted_count || 0)} 条，拒绝 ${Number(result.rejected_count || 0)} 条</span>${items.length ? `<ul>${items.map(item => `<li>${escapeHtml(item.rule_id || "规则")}：${escapeHtml(item.code || "未说明")}</li>`).join("")}</ul>` : ""}</div>
        </details>`;
    }

    function renderDevices() {
        const devices = dataOr("devices", {})?.devices || [];
        const overrides = dataOr("overrides", {})?.overrides || [];
        return `
            ${renderErrorList()}
            <div class="home-ai-block-heading"><div><h3>虚拟设备</h3><span>模拟执行，真实完成以 S3 ACK 为准</span></div></div>
            <div class="home-ai-device-list">
                ${devices.map(device => {
                    const current = device.state || {};
                    const power = current.power || "off";
                    return `<article class="home-ai-device-row">
                        <div class="home-ai-device-mark ${power === "on" ? "on" : "off"}">${device.device_type === "light" ? "灯" : device.device_type === "fan" ? "扇" : "冷"}</div>
                        <div class="home-ai-device-copy"><strong>${escapeHtml(device.device_id)}</strong><small>${escapeHtml(roomName(device.room_id))} · ${escapeHtml(device.device_type)} · ${power === "on" ? "开启" : "关闭"}</small><small>${escapeHtml(current.decision_reason || "暂无最近原因")}</small></div>
                        <div class="home-ai-button-row"><button class="home-ai-secondary" type="button" data-home-ai-device-action="turn_on" data-home-ai-device-id="${escapeHtml(device.device_id)}" data-home-ai-room-id="${escapeHtml(device.room_id)}">打开</button><button class="home-ai-secondary" type="button" data-home-ai-device-action="turn_off" data-home-ai-device-id="${escapeHtml(device.device_id)}" data-home-ai-room-id="${escapeHtml(device.room_id)}">关闭</button></div>
                    </article>`;
                }).join("") || `<div class="home-ai-empty">S3 尚未上报虚拟设备状态</div>`}
            </div>
            <section class="home-ai-block home-ai-override-block">
                <div class="home-ai-block-heading"><h3>用户覆盖</h3><span>覆盖优先级 900，支持到期时间</span></div>
                <form class="home-ai-inline-form" data-home-ai-form="override">
                    <label><span>设备</span><select name="device_id" required><option value="">选择设备</option>${devices.map(device => `<option value="${escapeHtml(device.device_id)}">${escapeHtml(device.device_id)}</option>`).join("")}</select></label>
                    <label><span>动作</span><select name="action"><option value="keep_on">保持打开</option><option value="keep_off">保持关闭</option><option value="pause_automation">暂停自动化</option><option value="mute">静音</option></select></label>
                    <label><span>时长（分钟）</span><input name="duration_minutes" type="number" min="1" max="10080" value="60"></label>
                    <button class="home-ai-primary" type="submit">保存覆盖</button>
                </form>
                <div class="home-ai-override-list">${overrides.map(override => `<div class="home-ai-override-row"><div><strong>${escapeHtml(override.action)}</strong><small>${escapeHtml(override.scope?.device_id || override.scope?.room_id || "全局")}</small></div><span>至 ${escapeHtml(formatTime(override.expires_at_ms))}</span><button class="home-ai-link danger-text" type="button" data-home-ai-remove-override="${escapeHtml(override.override_id)}">恢复自动</button></div>`).join("") || `<div class="home-ai-empty">暂无活动覆盖</div>`}</div>
            </section>
        `;
    }

    function renderDecisions() {
        const events = latestEvents().filter(event => ["decision", "suppressed_action", "virtual_device_state", "offline_buffer", "rule_sync", "emergency"].includes(event.event_type)).slice(0, 80);
        const decisions = agentDecisions().slice(0, 30);
        return `
            ${renderErrorList()}
            <section class="home-ai-block">
                <div class="home-ai-block-heading"><div><h3>Agent 决策</h3><span>意图、步骤、工具结果和最终语音</span></div><span>${decisions.length} 条</span></div>
                <div class="home-ai-agent-list">
                    ${decisions.map(decision => {
                        const stepCount = Array.isArray(decision.steps) ? decision.steps.length : 0;
                        const tools = (decision.execution?.steps || []).flatMap(step => step.actions || []).map(action => `${action.tool}:${action.status}`).slice(0, 6);
                        return `<details class="home-ai-agent-row"><summary><span class="home-ai-agent-id">${escapeHtml(decision.decision_id)}</span>${badge(decision.status)}<span>${escapeHtml(decision.response_type || "")}</span><time>${escapeHtml(formatTime(decision.updated_at_ms))}</time></summary>
                            <div class="home-ai-agent-detail"><p><strong>步骤 ${stepCount}</strong> · ${escapeHtml(decision.intent?.type || "未知意图")} · ${escapeHtml(decision.speech?.channel || "voice")}</p><p>${escapeHtml(decision.speech?.final || "尚未生成最终语音")}</p><ul>${tools.map(tool => `<li>${escapeHtml(tool)}</li>`).join("") || "<li>尚无工具结果</li>"}</ul><div class="home-ai-button-row"><button class="home-ai-mini" type="button" data-home-ai-feedback="accepted" data-home-ai-decision="${escapeHtml(decision.decision_id)}" data-home-ai-rule="" data-home-ai-room="${escapeHtml(decision.intent?.room_id || "")}">有效</button><button class="home-ai-mini" type="button" data-home-ai-feedback="rejected" data-home-ai-decision="${escapeHtml(decision.decision_id)}" data-home-ai-rule="" data-home-ai-room="${escapeHtml(decision.intent?.room_id || "")}">纠正</button></div></div></details>`;
                    }).join("") || `<div class="home-ai-empty">暂无 Agent 决策</div>`}
                </div>
            </section>
            <section class="home-ai-block">
            <div class="home-ai-block-heading"><div><h3>决策审计</h3><span>执行、抑制、同步和容量事件</span></div><span>${events.length} 条</span></div>
            <div class="home-ai-event-table-wrap"><table class="home-ai-event-table"><thead><tr><th>时间</th><th>房间</th><th>事件</th><th>原因/结果</th><th>反馈</th></tr></thead><tbody>
                ${events.map(event => {
                    const payload = event.payload || {};
                    const isDecision = ["decision", "suppressed_action"].includes(event.event_type);
                    const isEmergency = event.event_type === "emergency" && String(payload.state || "").toUpperCase() !== "RESOLVED";
                    const emergencyAcknowledged = event.user_acknowledged === true || String(payload.state || "").toUpperCase() === "ACKNOWLEDGED";
                    const detail = payload.reason || payload.decision_reason || payload.execution_result || payload.state || payload.capacity_percent !== undefined ? (payload.reason || payload.decision_reason || payload.execution_result || payload.state || `${payload.capacity_percent}%`) : "已记录";
                    return `<tr><td>${escapeHtml(formatTime(event.occurred_at_ms))}</td><td>${escapeHtml(roomName(event.room_id))}</td><td>${escapeHtml(event.event_type)}</td><td title="${escapeHtml(JSON.stringify(payload))}">${escapeHtml(detail)}</td><td>${isDecision ? `<div class="home-ai-button-row"><button class="home-ai-mini" type="button" data-home-ai-feedback="accepted" data-home-ai-event="${escapeHtml(event.event_id)}" data-home-ai-rule="${escapeHtml(payload.rule_id || "")}" data-home-ai-room="${escapeHtml(event.room_id || "")}">有效</button><button class="home-ai-mini" type="button" data-home-ai-feedback="rejected" data-home-ai-event="${escapeHtml(event.event_id)}" data-home-ai-rule="${escapeHtml(payload.rule_id || "")}" data-home-ai-room="${escapeHtml(event.room_id || "")}">纠正</button></div>` : isEmergency ? emergencyAcknowledged ? `<span class="home-ai-badge good">已确认</span>` : `<button class="home-ai-mini" type="button" data-home-ai-emergency-ack="${escapeHtml(event.event_id)}">确认告警</button>` : ""}</td></tr>`;
                }).join("") || `<tr><td colspan="5" class="home-ai-empty">暂无事件</td></tr>`}
            </tbody></table></div></section>
        `;
    }

    function renderTools() {
        const settings = dataOr("toolSettings", {}) || {};
        const location = settings.home_location || {};
        const news = settings.news_provider || {};
        const weather = settings.weather_context || {};
        const retention = dataOr("retention", {})?.policy || {};
        const maintenanceRuns = dataOr("maintenance", {})?.runs || [];
        const latestMaintenance = maintenanceRuns[0] || null;
        const capacity = latestMaintenance?.result?.capacity || {};
        const room = rooms()[0] || {};
        const latestRoomEvent = latestEvents("room_state").find(event => event.room_id === room.room_id);
        const payload = latestRoomEvent?.payload || {};
        const occupied = payload.presence_state === "occupied";
        const fresh = payload.radar_fresh !== false && Number(latestRoomEvent?.occurred_at_ms || 0) > Date.now() - 5 * 60 * 1000;
        return `
            ${renderErrorList()}
            <section class="home-ai-block">
                <div class="home-ai-block-heading"><div><h3>联网工具</h3><span>Server-only，S3 不直接访问天气或新闻服务</span></div><span class="home-ai-badge ${weather.available ? "good" : "warn"}">${weather.available ? "天气新鲜" : "天气不可用"}</span></div>
                <form class="home-ai-form" data-home-ai-form="tools">
                    <div class="home-ai-subform-heading"><h4>家庭位置</h4><span>使用 IANA 时区，例如 Asia/Shanghai</span></div>
                    <div class="home-ai-field-grid home-ai-tool-grid">
                        ${toolInput("城市", "city", location.city || "")}
                        ${toolInput("纬度", "latitude", location.latitude ?? "", "number", "-90", "90", "0.0001")}
                        ${toolInput("经度", "longitude", location.longitude ?? "", "number", "-180", "180", "0.0001")}
                        ${toolInput("时区", "timezone", location.timezone || "Asia/Shanghai")}
                    </div>
                    <div class="home-ai-subform-heading"><h4>新闻服务</h4><span>${news.api_key_configured ? "密钥已保存" : "未配置密钥"}</span></div>
                    <div class="home-ai-field-grid home-ai-tool-grid">
                        ${toolInput("HTTPS Endpoint", "news_endpoint", news.endpoint || "")}
                        ${toolInput("API Key（留空保持现值）", "news_api_key", "", "password")}
                    </div>
                    <div class="home-ai-button-row"><button class="home-ai-primary" type="submit">保存工具设置</button><button class="home-ai-secondary" type="button" data-home-ai-action="refresh-weather">刷新天气</button></div>
                </form>
                <dl class="home-ai-definition-list"><div><dt>天气状态</dt><dd>${escapeHtml(weather.available ? (weather.dark ? "昏暗" : "明亮") : "不可用")}</dd></div><div><dt>观测时间</dt><dd>${escapeHtml(formatTime(weather.observed_at_ms))}</dd></div><div><dt>原因</dt><dd>${escapeHtml(weather.reason || "暂无")}</dd></div></dl>
            </section>
            <section class="home-ai-block">
                <div class="home-ai-block-heading"><div><h3>场景简报</h3><span>只在有人、数据新鲜且未重复时生成；客厅默认 Web-only</span></div></div>
                <form class="home-ai-inline-form home-ai-briefing-form" data-home-ai-form="briefing">
                    <label><span>房间</span><select name="room_id">${rooms().map(item => `<option value="${escapeHtml(item.room_id)}">${escapeHtml(item.room_name)}</option>`).join("")}</select></label>
                    <label><span>场景</span><select name="scene"><option value="user_requested">用户请求</option><option value="wake_up">起床</option><option value="leaving_home">准备出门</option><option value="severe_weather">严重天气变化</option></select></label>
                    <label><span>内容主题</span><input name="query" maxlength="120" value="家庭简报"></label>
                    <button class="home-ai-primary" type="submit" ${occupied && fresh ? "" : "title=\"当前房间无人或数据过期\""}>生成简报</button>
                </form>
                <p class="home-ai-tool-note">最近状态：${escapeHtml(occupied ? "有人" : "无人/未知")} · ${escapeHtml(fresh ? "数据新鲜" : "等待新鲜数据")}</p>
            </section>
            <section class="home-ai-block">
                <div class="home-ai-block-heading"><div><h3>数据留存</h3><span>先聚合再清理，作业失败不阻塞数据接收</span></div>${latestMaintenance ? badge(latestMaintenance.status) : `<span class="home-ai-badge neutral">尚未运行</span>`}</div>
                <form class="home-ai-form" data-home-ai-form="retention">
                    <div class="home-ai-field-grid home-ai-retention-grid">
                        ${toolInput("雷达坐标（天）", "radar_coordinate_days", retention.radar_coordinate_days ?? 7, "number", "1", "30", "1")}
                        ${toolInput("环境原始采样（天）", "environment_raw_days", retention.environment_raw_days ?? 90, "number", "7", "3650", "1")}
                        ${toolInput("小时聚合（天）", "hourly_aggregate_days", retention.hourly_aggregate_days ?? 3650, "number", "30", "36500", "1")}
                        ${toolInput("每日聚合（天）", "daily_aggregate_days", retention.daily_aggregate_days ?? 3650, "number", "30", "36500", "1")}
                        ${toolInput("容量基准（MB）", "capacity_warning_mb", retention.capacity_warning_mb ?? 512, "number", "64", "10240", "1")}
                        ${toolInput("容量告警（%）", "capacity_warning_percent", retention.capacity_warning_percent ?? 80, "number", "50", "95", "1")}
                    </div>
                    <div class="home-ai-button-row"><button class="home-ai-primary" type="submit">保存留存策略</button></div>
                </form>
                <dl class="home-ai-definition-list">
                    <div><dt>最近作业</dt><dd>${escapeHtml(latestMaintenance ? formatTime(latestMaintenance.finished_at_ms) : "尚未运行")}</dd></div>
                    <div><dt>数据库使用</dt><dd>${capacity.capacity_percent === undefined ? "--" : `${Number(capacity.capacity_percent).toFixed(2)}%`}</dd></div>
                    <div><dt>最近清理</dt><dd>${latestMaintenance ? Number(latestMaintenance.result?.retention?.deleted?.sensor_records || 0) : "--"}</dd></div>
                </dl>
            </section>
        `;
    }

    function toolInput(label, name, value, type = "text", min = "", max = "", step = "") {
        return `<label class="home-ai-field"><span>${escapeHtml(label)}</span><input name="${escapeHtml(name)}" type="${type}" value="${escapeHtml(value)}"${min ? ` min="${min}"` : ""}${max ? ` max="${max}"` : ""}${step ? ` step="${step}"` : ""}></label>`;
    }

    function renderLearning() {
        const feedback = dataOr("feedback", {})?.feedback || [];
        const candidates = dataOr("memory", {})?.candidates || [];
        const habits = dataOr("habits", {})?.habits || [];
        const ruleCandidates = dataOr("ruleCandidates", {})?.candidates || [];
        const probation = dataOr("probation", {})?.runs || [];
        return `
            ${renderErrorList()}
            <div class="home-ai-learning-grid">
                <section class="home-ai-block">
                    <div class="home-ai-block-heading"><div><h3>反馈记录</h3><span>只有明确反馈才进入习惯证据</span></div><span>${feedback.length} 条</span></div>
                    <form class="home-ai-inline-form home-ai-feedback-form" data-home-ai-form="feedback">
                        <label><span>反馈类型</span><select name="feedback_type"><option value="accepted">有效</option><option value="rejected">纠正</option><option value="modified">修改</option><option value="cancelled">取消</option><option value="reverted">撤销</option><option value="manual_override">手动覆盖</option></select></label>
                        <label><span>规则 ID</span><input name="rule_id" maxlength="64"></label>
                        <label><span>房间 ID</span><input name="room_id" maxlength="32"></label>
                        <button class="home-ai-primary" type="submit">记录反馈</button>
                    </form>
                    <div class="home-ai-feedback-list">${feedback.slice(0, 30).map(item => `<div class="home-ai-feedback-row"><div><strong>${escapeHtml(item.feedback_type)}</strong><small>${escapeHtml(item.rule_id || item.decision_id || "未关联规则")}</small></div><time>${escapeHtml(formatTime(item.created_at_ms))}</time></div>`).join("") || `<div class="home-ai-empty">暂无反馈</div>`}</div>
                </section>
                <section class="home-ai-block">
                    <div class="home-ai-block-heading"><div><h3>Memory candidate</h3><span>候选不会直接参与高影响控制</span></div><span>${candidates.length} 条</span></div>
                    <form class="home-ai-inline-form home-ai-memory-form" data-home-ai-form="memory"><label><span>内容</span><input name="content" maxlength="512" required placeholder="记录稳定偏好或事实"></label><label><span>类别</span><input name="category" maxlength="64" value="preference" required></label><button class="home-ai-primary" type="submit">新增候选</button></form>
                    <div class="home-ai-memory-list">${candidates.slice(0, 30).map(item => `<div class="home-ai-memory-row"><div><div class="home-ai-memory-fields"><input class="home-ai-memory-edit" data-memory-field="content" data-memory-id="${escapeHtml(item.candidate_id)}" value="${escapeHtml(item.content)}" aria-label="记忆内容"><input class="home-ai-memory-edit" data-memory-field="category" data-memory-id="${escapeHtml(item.candidate_id)}" value="${escapeHtml(item.category)}" aria-label="记忆类别"></div><small>${escapeHtml(item.room_id || "全局")} · 置信度 ${(Number(item.confidence || 0) * 100).toFixed(0)}% · 来源 ${escapeHtml(item.source?.source || item.source?.habit_id || "未说明")} · 自动化 ${item.automation_allowed === true ? "允许" : "禁止"}</small><small>影响规则：${escapeHtml((item.affected_rule_ids || []).join("、") || "无")}</small></div>${badge(item.status)}<div class="home-ai-button-row"><button class="home-ai-mini" type="button" data-home-ai-memory-action="save" data-home-ai-memory-id="${escapeHtml(item.candidate_id)}">保存</button><button class="home-ai-mini" type="button" data-home-ai-memory-action="confirm" data-home-ai-memory-id="${escapeHtml(item.candidate_id)}">确认（禁止自动化）</button><button class="home-ai-mini" type="button" data-home-ai-memory-action="enable" data-home-ai-memory-id="${escapeHtml(item.candidate_id)}">允许自动化</button><button class="home-ai-mini" type="button" data-home-ai-memory-action="reject" data-home-ai-memory-id="${escapeHtml(item.candidate_id)}">拒绝</button><button class="home-ai-mini danger-text" type="button" data-home-ai-memory-action="delete" data-home-ai-memory-id="${escapeHtml(item.candidate_id)}">删除</button></div></div>`).join("") || `<div class="home-ai-empty">暂无 memory candidate</div>`}</div>
                </section>
            </div>
            <section class="home-ai-block">
                <div class="home-ai-block-heading"><div><h3>房间习惯</h3><span>只由明确反馈累积；无反馈不改变置信度</span></div><span>${habits.length} 条</span></div>
                <div class="home-ai-habit-list">${habits.slice(0, 30).map(item => `<div class="home-ai-habit-row"><div><strong>${escapeHtml(item.room_id || "全局")}</strong><small>${escapeHtml(item.pattern?.rule_id || "未关联规则")} · ${item.evidence_count} 次证据</small></div>${badge(item.status)}<strong>${(Number(item.confidence || 0) * 100).toFixed(0)}%</strong></div>`).join("") || `<div class="home-ai-empty">暂无习惯证据</div>`}</div>
            </section>
            <section class="home-ai-block">
                <div class="home-ai-block-heading"><div><h3>规则候选与门禁</h3><span>Schema、样本、冲突、安全、回放、资源和触发率全部通过后才可试运行</span></div><span>${ruleCandidates.length} 条</span></div>
                <div class="home-ai-rule-candidate-list">${ruleCandidates.slice(0, 30).map(item => { const locked = ["PUBLISHED", "PROBATION", "SUSPENDED"].includes(item.status); return `<div class="home-ai-rule-candidate-row"><div><strong>${escapeHtml(item.candidate_id)}</strong><small>${escapeHtml(item.room_id || "全局")} · ${(Number(item.confidence || 0) * 100).toFixed(0)}% · ${item.sample_count} 个样本</small></div>${badge(item.status)}<div class="home-ai-button-row"><button class="home-ai-mini" type="button" data-home-ai-candidate-action="evaluate" data-home-ai-candidate-id="${escapeHtml(item.candidate_id)}"${locked ? " disabled" : ""}>评估门禁</button><button class="home-ai-mini" type="button" data-home-ai-candidate-action="publish" data-home-ai-candidate-id="${escapeHtml(item.candidate_id)}"${locked ? " disabled" : ""}>通过并试运行</button></div><small class="home-ai-gate-summary">${escapeHtml(Object.entries(item.gates || {}).map(([key, value]) => `${key}:${value === true ? "通过" : value === false ? "拒绝" : value}`).join(" · ") || "尚未评估")} · 回放 ${Number(item.gate_details?.replay_event_count || 0)} 条 · 预测 ${Number(item.gate_details?.predicted_trigger_rate_per_hour || 0).toFixed(2)} 次/小时</small></div>`; }).join("") || `<div class="home-ai-empty">暂无规则候选</div>`}</div>
            </section>
            <section class="home-ai-block">
                <div class="home-ai-block-heading"><div><h3>试运行与自动回滚</h3><span>至少 5 次触发，失败率超过 20% 或 3 次覆盖会暂停并回滚</span></div><span>${probation.length} 条</span></div>
                <div class="home-ai-probation-list">${probation.slice(0, 30).map(item => `<div class="home-ai-probation-row"><div><strong>${escapeHtml(item.rule_id)}</strong><small>${escapeHtml(item.run_id)} · 结束 ${escapeHtml(formatTime(item.ends_at_ms))}</small></div>${badge(item.status)}<span>触发 ${item.trigger_count} · 失败 ${item.failure_count} · 覆盖 ${item.override_count}</span><button class="home-ai-mini" type="button" data-home-ai-probation-action="evaluate" data-home-ai-probation-id="${escapeHtml(item.run_id)}">重新评估</button></div>`).join("") || `<div class="home-ai-empty">暂无试运行</div>`}</div>
            </section>
        `;
    }

    function renderView() {
        const view = state.root?.querySelector("[data-home-ai-view]");
        if (!view) return;
        const renderers = { overview: renderOverview, rooms: renderRooms, rules: renderRules, devices: renderDevices, decisions: renderDecisions, learning: renderLearning, tools: renderTools };
        view.innerHTML = (renderers[state.activeTab] || renderOverview)();
        state.root.querySelectorAll("[data-home-ai-tab]").forEach(tab => {
            const active = tab.dataset.homeAiTab === state.activeTab;
            tab.classList.toggle("active", active);
            tab.setAttribute("aria-selected", active ? "true" : "false");
        });
    }

    function markDirty() {
        state.dirty = true;
    }

    function handleInput(event) {
        if (event.target.closest("form")) markDirty();
    }

    function switchTab(next) {
        if (!tabs.some(tab => tab.key === next)) return;
        if (state.dirty && ["rooms", "rules"].includes(state.activeTab) && next !== state.activeTab) {
            showNotice("当前编辑尚未保存，请先提交或刷新后再切换。", "warn");
            return;
        }
        state.activeTab = next;
        state.dirty = false;
        renderView();
    }

    async function handleClick(event) {
        const tab = event.target.closest("[data-home-ai-tab], [data-home-ai-tab-jump]");
        if (tab) {
            switchTab(tab.dataset.homeAiTab || tab.dataset.homeAiTabJump);
            return;
        }
        const action = event.target.closest("[data-home-ai-action]")?.dataset.homeAiAction;
        if (action === "refresh") {
            await refresh();
            return;
        }
        if (action === "refresh-weather") {
            try {
                await request("/api/home-ai/v1/tools/get_weather/execute", {
                    method: "POST",
                    body: JSON.stringify({})
                });
                showNotice("天气已更新，依赖天气的规则将随下一次 S3 配置同步生效。", "good");
                await refresh({ quiet: true });
            } catch (error) {
                showNotice(friendlyError(error), "bad");
            }
            return;
        }
        if (action === "new-rule") {
            state.draft = defaultDraft(currentPackage());
            state.dirty = false;
            switchTab("rules");
            return;
        }
        if (action === "add-condition") {
            state.root.querySelector("[data-condition-list]")?.insertAdjacentHTML("beforeend", renderConditionRow());
            markDirty();
            return;
        }
        if (action === "remove-condition") {
            const rows = state.root.querySelectorAll("[data-condition-row]");
            if (rows.length <= 1) {
                showNotice("规则至少需要一条条件。", "warn");
                return;
            }
            event.target.closest("[data-condition-row]")?.remove();
            markDirty();
            return;
        }
        if (action === "add-action") {
            state.root.querySelector("[data-action-list]")?.insertAdjacentHTML("beforeend", renderActionRow());
            markDirty();
            return;
        }
        if (action === "remove-action") {
            const rows = state.root.querySelectorAll("[data-action-row]");
            if (rows.length <= 1) {
                showNotice("规则至少需要一个动作。", "warn");
                return;
            }
            event.target.closest("[data-action-row]")?.remove();
            markDirty();
            return;
        }
        if (action === "rollback") {
            const packages = dataOr("rules", {})?.packages || [];
            const target = packages.find(item => item.version !== currentPackage()?.version);
            if (!target) return;
            if (!window.confirm(`确认回滚到规则包 v${target.version}？`)) return;
            await mutate(`/api/home-ai/v1/rules/rollback`, "POST", { target_version: target.version, reason: "web_manual_rollback" }, "已提交回滚，等待 S3 同步");
            return;
        }
        const loadRule = event.target.closest("[data-home-ai-load-rule]");
        if (loadRule) {
            const rule = (currentPackage()?.rules || []).find(item => item.rule_id === loadRule.dataset.homeAiLoadRule);
            if (rule) {
                state.draft = JSON.parse(JSON.stringify(rule));
                state.dirty = false;
                renderView();
            }
            return;
        }
        const deviceButton = event.target.closest("[data-home-ai-device-action]");
        if (deviceButton) {
            await controlDevice(deviceButton.dataset.homeAiDeviceId, deviceButton.dataset.homeAiDeviceAction, deviceButton.dataset.homeAiRoomId);
            return;
        }
        const feedback = event.target.closest("[data-home-ai-feedback]");
        if (feedback) {
            await mutate("/api/home-ai/v1/feedback", "POST", {
                feedback_id: `web_${Date.now()}_${Math.random().toString(16).slice(2)}`,
                decision_id: feedback.dataset.homeAiDecision || feedback.dataset.homeAiEvent,
                rule_id: feedback.dataset.homeAiRule,
                room_id: feedback.dataset.homeAiRoom,
                feedback_type: feedback.dataset.homeAiFeedback,
                payload: { source: "web" }
            }, "反馈已记录");
            return;
        }
        const emergencyAck = event.target.closest("[data-home-ai-emergency-ack]");
        if (emergencyAck) {
            await mutate(
                `/api/home-ai/v1/emergencies/${encodeURIComponent(emergencyAck.dataset.homeAiEmergencyAck)}/acknowledge`,
                "POST",
                { source: "web" },
                "告警已确认，后续播报将降频"
            );
            return;
        }
        const memory = event.target.closest("[data-home-ai-memory-action]");
        if (memory) {
            const id = memory.dataset.homeAiMemoryId;
            const memoryAction = memory.dataset.homeAiMemoryAction;
            if (memoryAction === "delete") {
                await mutate(`/api/home-ai/v1/memory-candidates/${encodeURIComponent(id)}`, "DELETE", null, "候选已删除");
            } else if (memoryAction === "save") {
                const fields = Array.from(state.root.querySelectorAll("[data-memory-field]"))
                    .filter(node => node.dataset.memoryId === id);
                const value = name => fields.find(node => node.dataset.memoryField === name)?.value || "";
                await mutate(`/api/home-ai/v1/memory-candidates/${encodeURIComponent(id)}`, "PATCH", {
                    content: value("content"),
                    category: value("category")
                }, "候选已修改");
            } else {
                const enabled = memoryAction === "enable";
                await mutate(`/api/home-ai/v1/memory-candidates/${encodeURIComponent(id)}`, "PATCH", {
                    status: ["confirm", "enable"].includes(memoryAction) ? "CONFIRMED" : "REJECTED",
                    automation_allowed: enabled
                }, enabled ? "记忆已确认并允许自动化" : memoryAction === "confirm" ? "候选已确认且禁止自动化" : "候选已拒绝");
            }
            return;
        }
        const candidate = event.target.closest("[data-home-ai-candidate-action]");
        if (candidate) {
            const candidateId = candidate.dataset.homeAiCandidateId;
            const autoPublish = candidate.dataset.homeAiCandidateAction === "publish";
            try {
                const result = await request(`/api/home-ai/v1/rule-candidates/${encodeURIComponent(candidateId)}/evaluate`, {
                    method: "POST",
                    body: JSON.stringify({ auto_publish: autoPublish, duration_days: 3 })
                });
                const status = String(result?.status || "");
                showNotice(status === "PROBATION" ? "候选已通过 Server 门禁并进入试运行。" :
                    status === "READY" ? "Server 门禁已通过，可进入试运行。" :
                        status === "REJECTED" ? "候选未通过 Server 门禁，请查看拒绝项。" :
                            status === "SUSPENDED" ? "候选发布失败，已暂停。" : "候选门禁已评估。",
                ["PROBATION", "READY"].includes(status) ? "good" : status === "REJECTED" ? "warn" : "bad");
                await refresh({ quiet: true });
            } catch (error) {
                showNotice(friendlyError(error), "bad");
            }
            return;
        }
        const probation = event.target.closest("[data-home-ai-probation-action]");
        if (probation) {
            const run = (dataOr("probation", {})?.runs || []).find(item => item.run_id === probation.dataset.homeAiProbationId);
            if (!run) return;
            await mutate(`/api/home-ai/v1/probation/${encodeURIComponent(run.run_id)}/evaluate`, "POST", {
                metrics: {
                    trigger_count: run.trigger_count,
                    failure_count: run.failure_count,
                    override_count: run.override_count
                }
            }, "试运行状态已重新评估");
            return;
        }
        const removeOverride = event.target.closest("[data-home-ai-remove-override]");
        if (removeOverride) {
            await mutate(`/api/home-ai/v1/overrides/${encodeURIComponent(removeOverride.dataset.homeAiRemoveOverride)}`, "DELETE", null, "覆盖已移除，恢复自动控制");
        }
    }

    async function controlDevice(deviceId, action, roomId) {
        try {
            await request(`/api/home-ai/v1/tools/control_virtual_device/execute`, {
                method: "POST",
                body: JSON.stringify({ device_id: deviceId, action, room_id: roomId })
            });
            showNotice("命令已排队，等待 S3 ACK；界面不会提前标记为成功。", "info");
            await refresh({ quiet: true });
        } catch (error) {
            showNotice(friendlyError(error), "bad");
        }
    }

    async function mutate(path, method, body, message) {
        try {
            await request(path, { method, body: body === null ? undefined : JSON.stringify(body) });
            state.dirty = false;
            showNotice(message, "good");
            await refresh({ quiet: true });
        } catch (error) {
            showNotice(friendlyError(error), "bad");
        }
    }

    async function handleSubmit(event) {
        const form = event.target.closest("form[data-home-ai-form]");
        if (!form) return;
        event.preventDefault();
        const kind = form.dataset.homeAiForm;
        if (kind === "rooms") {
            const nextRooms = rooms().map((room, index) => {
                const value = field => form.querySelector(`[data-room-index="${index}"][data-room-field="${field}"]`)?.value ?? room[field] ?? "";
                return {
                    ...room,
                    room_id: value("room_id").trim(),
                    room_name: value("room_name").trim(),
                    sensing_sources: value("sensing_sources").split(",").map(item => item.trim()).filter(Boolean),
                    voice_terminal_device_id: value("voice_terminal_device_id").trim(),
                    presence_confirm_ms: Math.round(Number(value("presence_confirm_ms")) * 1000),
                    vacant_confirm_ms: Math.round(Number(value("vacant_confirm_ms")) * 1000),
                    multiple_confirm_ms: Math.round(Number(value("multiple_confirm_ms")) * 1000),
                    single_confirm_ms: Math.round(Number(value("single_confirm_ms")) * 1000),
                    quiet_start: value("quiet_start"),
                    quiet_end: value("quiet_end")
                };
            });
            await mutate("/api/home-ai/v1/rooms", "PUT", { schema_version: SCHEMA_VERSION, rooms: nextRooms }, "房间配置已保存，等待 S3 同步");
            return;
        }
        if (kind === "rule") {
            const pkg = currentPackage();
            const rule = collectRule(form);
            const nextRules = (pkg?.rules || []).filter(item => item.rule_id !== rule.rule_id).concat(rule);
            const candidate = {
                schema_version: SCHEMA_VERSION,
                version: Math.max(Number(pkg?.version || 0) + 1, Number(rule.version || 1)),
                generated_at_ms: Date.now(),
                rooms: rooms(),
                rules: nextRules
            };
            try {
                const validation = await request("/api/home-ai/v1/rules/validate", { method: "POST", body: JSON.stringify(candidate) });
                await request("/api/home-ai/v1/rules/publish", { method: "POST", body: JSON.stringify(validation.package || candidate) });
                state.dirty = false;
                showNotice("规则已验证并发布，等待 S3 部署回执。", "good");
                await refresh({ quiet: true });
            } catch (error) {
                showNotice(friendlyError(error), "bad");
            }
            return;
        }
        if (kind === "override") {
            const values = new FormData(form);
            const deviceId = String(values.get("device_id") || "");
            const duration = Math.max(1, Number(values.get("duration_minutes") || 60));
            await mutate("/api/home-ai/v1/overrides", "POST", {
                scope: { room_id: rooms().find(room => (dataOr("devices", {})?.devices || []).find(device => device.device_id === deviceId)?.room_id === room.room_id)?.room_id || "living_room", device_id: deviceId },
                action: values.get("action"),
                source: "web_user_command",
                expires_at_ms: Date.now() + duration * 60000,
                priority: 900,
                allow_safety_override: true
            }, "用户覆盖已保存");
            return;
        }
        if (kind === "feedback") {
            const values = new FormData(form);
            await mutate("/api/home-ai/v1/feedback", "POST", {
                feedback_id: `web_${Date.now()}_${Math.random().toString(16).slice(2)}`,
                feedback_type: String(values.get("feedback_type") || "accepted"),
                rule_id: String(values.get("rule_id") || ""),
                room_id: String(values.get("room_id") || ""),
                payload: { source: "web", explicit: true }
            }, "明确反馈已记录");
            return;
        }
        if (kind === "memory") {
            const values = new FormData(form);
            await mutate("/api/home-ai/v1/memory-candidates", "POST", {
                category: String(values.get("category") || "preference"),
                content: String(values.get("content") || ""),
                confidence: 0.5,
                source: { source: "web" }
            }, "Memory candidate 已创建");
            return;
        }
        if (kind === "tools") {
            const values = new FormData(form);
            try {
                await request("/api/home-ai/v1/tools/settings/home-location", {
                    method: "PUT",
                    body: JSON.stringify({
                        city: String(values.get("city") || ""),
                        latitude: Number(values.get("latitude")),
                        longitude: Number(values.get("longitude")),
                        timezone: String(values.get("timezone") || "")
                    })
                });
                await request("/api/home-ai/v1/tools/settings/news", {
                    method: "PUT",
                    body: JSON.stringify({
                        endpoint: String(values.get("news_endpoint") || ""),
                        api_key: String(values.get("news_api_key") || "")
                    })
                });
                state.dirty = false;
                showNotice("工具设置已保存。", "good");
                await refresh({ quiet: true });
            } catch (error) {
                showNotice(friendlyError(error), "bad");
            }
            return;
        }
        if (kind === "retention") {
            const values = new FormData(form);
            await mutate("/api/home-ai/v1/data/retention", "PUT", {
                radar_coordinate_days: Number(values.get("radar_coordinate_days")),
                environment_raw_days: Number(values.get("environment_raw_days")),
                hourly_aggregate_days: Number(values.get("hourly_aggregate_days")),
                daily_aggregate_days: Number(values.get("daily_aggregate_days")),
                capacity_warning_mb: Number(values.get("capacity_warning_mb")),
                capacity_warning_percent: Number(values.get("capacity_warning_percent"))
            }, "留存策略已保存");
            return;
        }
        if (kind === "briefing") {
            const values = new FormData(form);
            const roomId = String(values.get("room_id") || "");
            const event = latestEvents("room_state").find(item => item.room_id === roomId);
            const payload = event?.payload || {};
            const fresh = Number(event?.occurred_at_ms || 0) > Date.now() - 5 * 60 * 1000;
            try {
                const result = await request("/api/home-ai/v1/agent/orchestrate", {
                    method: "POST",
                    body: JSON.stringify({
                        intent: {
                            type: "briefing",
                            confidence: 0.95,
                            scene: String(values.get("scene") || "user_requested"),
                            room_id: roomId,
                            query: String(values.get("query") || "家庭简报"),
                            presence_state: payload.presence_state || "unknown",
                            presence_confidence: payload.presence_state === "occupied" ? 0.95 : 0,
                            data_valid: fresh,
                            voice_terminal_available: false
                        }
                    })
                });
                showNotice(result.speech?.final || "简报决策已记录。", result.status === "SUPPRESSED" ? "warn" : "good");
                await refresh({ quiet: true });
            } catch (error) {
                showNotice(friendlyError(error), "bad");
            }
        }
    }

    function collectRule(form) {
        const value = field => form.querySelector(`[data-rule-field="${field}"]`)?.value;
        const conditions = Array.from(form.querySelectorAll("[data-condition-row]")).map(row => {
            const fieldName = row.querySelector("[data-condition-field]")?.value || "presence_state";
            const operator = row.querySelector("[data-condition-operator]")?.value || "eq";
            const raw = row.querySelector("[data-condition-value]")?.value || "";
            const valueTypeNumber = ["stable_target_count", "temperature_c", "humidity_percent", "air_quality_score"].includes(fieldName);
            const valueTypeBoolean = ["environment_fresh", "radar_fresh", "weather_dark"].includes(fieldName);
            const values = ["in", "range"].includes(operator) ? raw.split(",").map(item => item.trim()).filter(Boolean) : [raw.trim()];
            const normalized = values.map(item => valueTypeNumber ? Number(item) : valueTypeBoolean ? item.toLowerCase() === "true" : item);
            return { field: fieldName, operator, value: ["in", "range"].includes(operator) ? normalized : normalized[0], duration_ms: Math.max(0, Number(row.querySelector("[data-condition-duration]")?.value || 0) * 1000) };
        });
        const actions = Array.from(form.querySelectorAll("[data-action-row]")).map(row => ({
            device_type: row.querySelector("[data-action-type]")?.value || "light",
            device_id: row.querySelector("[data-action-device]")?.value.trim() || "",
            action: row.querySelector("[data-action-name]")?.value || "turn_on",
            prompt: row.querySelector("[data-action-prompt]")?.value.trim() || ""
        }));
        return {
            rule_id: value("rule_id")?.trim(),
            version: Number(currentPackage()?.version || 0) + 1,
            rule_type: "basic_automation",
            source: "manual",
            room_id: value("room_id"),
            enabled: Boolean(form.querySelector("[data-rule-field=enabled]")?.checked),
            priority: Number(value("priority") || 500),
            conditions,
            actions,
            cooldown_seconds: Number(value("cooldown_seconds") || 0),
            minimum_active_seconds: Number(value("minimum_active_seconds") || 0),
            offline_policy: value("offline_policy") || "continue",
            expires_at_ms: null,
            probation: { enabled: false, until_ms: null }
        };
    }

    async function refresh(options = {}) {
        if (!state.root || state.loading) return;
        if (document.hidden || state.root.closest("[hidden]")) return;
        state.loading = true;
        setStatus("读取中", "warn");
        const calls = {
            rooms: "/api/home-ai/v1/rooms",
            current: "/api/home-ai/v1/rules/current",
            rules: "/api/home-ai/v1/rules?limit=30",
            deployments: "/api/home-ai/v1/rules/deployments?limit=30",
            devices: "/api/home-ai/v1/virtual-devices",
            overrides: "/api/home-ai/v1/overrides?limit=30",
            events: "/api/home-ai/v1/events?limit=100",
            feedback: "/api/home-ai/v1/feedback?limit=50",
            memory: "/api/home-ai/v1/memory-candidates?limit=50",
            habits: "/api/home-ai/v1/habits?limit=50",
            ruleCandidates: "/api/home-ai/v1/rule-candidates?limit=50",
            probation: "/api/home-ai/v1/probation?limit=50",
            agentDecisions: "/api/home-ai/v1/agent/decisions?limit=50",
            toolSettings: "/api/home-ai/v1/tools/settings",
            retention: "/api/home-ai/v1/data/retention",
            maintenance: "/api/home-ai/v1/data/maintenance?limit=10"
        };
        const entries = await Promise.all(Object.entries(calls).map(async ([key, path]) => {
            try {
                return [key, await request(path), null];
            } catch (error) {
                return [key, null, error];
            }
        }));
        entries.forEach(([key, value, error]) => {
            if (value !== null) state.data[key] = value;
            state.errors[key] = error;
        });
        state.data.loadedAt = Date.now();
        state.loading = false;
        setStatus(Object.values(state.errors).some(Boolean) ? "部分可用" : "已同步", Object.values(state.errors).some(Boolean) ? "warn" : "good");
        if (!state.dirty || !["rooms", "rules"].includes(state.activeTab) || !options.quiet) {
            renderView();
        }
    }

    function mount(container) {
        if (!container) return;
        if (state.root === container && state.root.dataset.homeAiMounted === "true") return;
        state.root = container;
        state.root.dataset.homeAiMounted = "true";
        renderShell(container);
        void refresh();
        if (state.timer) clearInterval(state.timer);
        state.timer = setInterval(() => void refresh({ quiet: true }), 60000);
    }

    window.HomeAiDashboard = { mount, refresh };
})();
