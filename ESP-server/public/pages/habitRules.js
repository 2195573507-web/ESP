(function () {
    "use strict";

    const ruleTypeLabels = {
        PERSON_ENTER_ROOM: "人员进入房间",
        PERSON_LEAVE_ROOM: "人员离开房间",
        PERSON_LONG_STAY: "长时间停留",
        ROOM_EMPTY_TIMEOUT: "无人超时",
        NIGHT_ACTIVITY: "夜间活动",
        LONG_OCCUPANCY: "长期占用"
    };

    let rulesById = new Map();

    function escapeHtml(value) {
        return String(value ?? "").replace(/[&<>'"]/g, character => ({
            "&": "&amp;", "<": "&lt;", ">": "&gt;", "'": "&#39;", "\"": "&quot;"
        })[character]);
    }

    function minuteField(label, name, value, minimum) {
        return `<label class="habit-rule-field">${label}<input name="${name}" type="number" min="${minimum}" max="10080" step="1" value="${Number(value)}" required></label>`;
    }

    function configFields(rule) {
        const config = rule.config || {};
        const room = `<label class="habit-rule-field">房间<input name="room" maxlength="128" value="${escapeHtml(config.room)}" required></label>`;
        if (rule.type === "PERSON_LEAVE_ROOM") {
            return room + minuteField("离开持续时间（分钟）", "duration_minutes", config.duration_minutes, 0);
        }
        if (["PERSON_LONG_STAY", "ROOM_EMPTY_TIMEOUT", "LONG_OCCUPANCY"].includes(rule.type)) {
            return room + minuteField("阈值（分钟）", "threshold_minutes", config.threshold_minutes, 1);
        }
        if (rule.type === "NIGHT_ACTIVITY") {
            return room +
                `<label class="habit-rule-field">开始时间<input name="start_time" type="time" value="${escapeHtml(config.start_time)}" required></label>` +
                `<label class="habit-rule-field">结束时间<input name="end_time" type="time" value="${escapeHtml(config.end_time)}" required></label>`;
        }
        return room;
    }

    function ruleCard(rule) {
        const title = escapeHtml(rule.name || ruleTypeLabels[rule.type] || rule.type);
        const typeLabel = escapeHtml(ruleTypeLabels[rule.type] || rule.type);
        return `
            <article class="habit-rule-card" data-habit-rule-id="${escapeHtml(rule.id)}">
                <header class="habit-rule-card-header">
                    <div>
                        <h2>${title}</h2>
                        <span>规则类型：${typeLabel}</span>
                    </div>
                    <label class="habit-rule-toggle">
                        <span>启用</span>
                        <input name="enabled" type="checkbox" ${rule.enabled ? "checked" : ""}>
                    </label>
                </header>
                <div class="habit-rule-fields">${configFields(rule)}</div>
            </article>`;
    }

    function setStatus(message, state = "") {
        const status = document.querySelector("[data-habit-rules-status]");
        if (!status) return;
        status.textContent = message;
        status.dataset.state = state;
    }

    function render(rules) {
        const list = document.querySelector("[data-habit-rules-list]");
        if (!list) return;
        rulesById = new Map(rules.map(rule => [rule.id, rule]));
        list.innerHTML = rules.length
            ? rules.map(ruleCard).join("")
            : '<p class="habit-rules-empty">尚未创建习惯规则。</p>';
    }

    async function load() {
        try {
            setStatus("正在读取习惯规则...");
            const response = await fetch("/api/habit-rules", { cache: "no-store" });
            const payload = await response.json();
            if (!response.ok || !payload?.ok) throw new Error(payload?.error?.message || "read failed");
            render(payload.data.rules || []);
            setStatus("规则已加载", "ready");
        } catch (_) {
            setStatus("习惯规则读取失败", "error");
        }
    }

    function readRule(card) {
        const original = rulesById.get(card.dataset.habitRuleId);
        if (!original) return null;
        const enabled = card.querySelector('[name="enabled"]').checked;
        const config = { enabled, room: card.querySelector('[name="room"]').value.trim() };
        if (original.type === "PERSON_LEAVE_ROOM") {
            config.duration_minutes = Number(card.querySelector('[name="duration_minutes"]').value);
        } else if (["PERSON_LONG_STAY", "ROOM_EMPTY_TIMEOUT", "LONG_OCCUPANCY"].includes(original.type)) {
            config.threshold_minutes = Number(card.querySelector('[name="threshold_minutes"]').value);
        } else if (original.type === "NIGHT_ACTIVITY") {
            config.start_time = card.querySelector('[name="start_time"]').value;
            config.end_time = card.querySelector('[name="end_time"]').value;
        }
        return { name: original.name, type: original.type, enabled, config };
    }

    function init() {
        const form = document.querySelector("[data-habit-rules-form]");
        if (!form) return;
        form.addEventListener("submit", async event => {
            event.preventDefault();
            const submit = form.querySelector('button[type="submit"]');
            const updates = Array.from(form.querySelectorAll("[data-habit-rule-id]"))
                .map(readRule)
                .filter(Boolean);
            submit.disabled = true;
            try {
                setStatus("正在保存习惯规则...");
                for (const [index, rule] of updates.entries()) {
                    const id = Array.from(form.querySelectorAll("[data-habit-rule-id]"))[index].dataset.habitRuleId;
                    const response = await fetch(`/api/habit-rules/${encodeURIComponent(id)}`, {
                        method: "PUT",
                        headers: { "Content-Type": "application/json" },
                        body: JSON.stringify(rule)
                    });
                    const payload = await response.json();
                    if (!response.ok || !payload?.ok) throw new Error(payload?.error?.message || "save failed");
                }
                await load();
                setStatus("习惯规则已保存", "ready");
            } catch (_) {
                setStatus("保存失败，请检查规则参数。", "error");
            } finally {
                submit.disabled = false;
            }
        });
    }

    window.HabitRulesDashboard = Object.freeze({ init, load });
}());
