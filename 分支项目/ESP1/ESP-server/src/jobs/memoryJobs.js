const {
    createDailyMemory,
    createMemoryJobRun,
    upsertProfile
} = require("../memory/store");
const {
    createExperienceMemory,
    createRelationMemory,
    upsertEnvironmentProfile
} = require("../agent/stateStore");
const {
    isIsoDateString
} = require("../utils/date");

function todayDate() {
    return new Date().toISOString().slice(0, 10);
}

function addDays(dateText, days) {
    const date = new Date(`${dateText}T00:00:00.000Z`);
    date.setUTCDate(date.getUTCDate() + days);
    return date.toISOString().slice(0, 10);
}

function dayWindow(dateText) {
    const nextDate = addDays(dateText, 1);
    return {
        start: `${dateText}T00:00:00.000Z`,
        end: `${nextDate}T00:00:00.000Z`,
        startMs: Date.parse(`${dateText}T00:00:00.000Z`),
        endMs: Date.parse(`${nextDate}T00:00:00.000Z`)
    };
}

function weekWindow(weekEnd) {
    const startDate = addDays(weekEnd, -6);
    const nextDate = addDays(weekEnd, 1);
    return {
        startDate,
        endDate: weekEnd,
        start: `${startDate}T00:00:00.000Z`,
        end: `${nextDate}T00:00:00.000Z`,
        startMs: Date.parse(`${startDate}T00:00:00.000Z`),
        endMs: Date.parse(`${nextDate}T00:00:00.000Z`)
    };
}

function readDate(value, invalidCode, field) {
    if (value === undefined || value === null) {
        return {
            ok: true,
            value: todayDate()
        };
    }

    if (typeof value === "string" && value.trim() === "") {
        return {
            ok: true,
            value: todayDate()
        };
    }

    if (isIsoDateString(value)) {
        return {
            ok: true,
            value: value.trim()
        };
    }

    return {
        ok: false,
        statusCode: 400,
        code: invalidCode,
        error: `${field} must use YYYY-MM-DD format`
    };
}

function parseBoolean(value) {
    return value === true || value === "true" || value === 1 || value === "1";
}

function readWeeklyTarget(input = {}) {
    if (input.week_start !== undefined &&
        input.week_end === undefined &&
        input.date === undefined &&
        input.target_date === undefined) {
        const weekStartResult = readDate(input.week_start, "WEEKLY_PROFILE_DATE_INVALID", "week_start");
        if (!weekStartResult.ok) {
            return weekStartResult;
        }

        return {
            ok: true,
            value: addDays(weekStartResult.value, 6),
            week_start: weekStartResult.value
        };
    }

    const field = input.week_end !== undefined
        ? "week_end"
        : input.date !== undefined
            ? "date"
            : input.target_date !== undefined
                ? "target_date"
                : "week_end";
    return readDate(input.week_end ?? input.date ?? input.target_date, "WEEKLY_PROFILE_DATE_INVALID", field);
}

function numericAverage(rows, field) {
    const values = rows
        .map(row => Number(row[field]))
        .filter(Number.isFinite);
    if (!values.length) {
        return null;
    }

    return Math.round((values.reduce((sum, value) => sum + value, 0) / values.length) * 100) / 100;
}

function firstNumber(rows, field) {
    for (const row of rows) {
        const value = Number(row[field]);
        if (Number.isFinite(value)) {
            return value;
        }
    }

    return null;
}

function statusCounts(rows) {
    return rows.reduce((counts, row) => {
        const status = row.status || "unknown";
        counts[status] = (counts[status] || 0) + 1;
        return counts;
    }, {});
}

function sampleIds(rows, limit = 10) {
    return rows.slice(0, limit).map(row => row.id).filter(id => id !== undefined && id !== null);
}

async function fetchDailyInputs(dbAll, targetDate) {
    const window = dayWindow(targetDate);
    const sensorRows = await dbAll(
        `SELECT * FROM sensor_records
        WHERE deleted_at IS NULL
          AND COALESCE(server_recv_ms, timestamp) >= ?
          AND COALESCE(server_recv_ms, timestamp) < ?
        ORDER BY COALESCE(server_recv_ms, timestamp, id) ASC`,
        [window.startMs, window.endMs]
    );
    const deviceStatusRows = await dbAll(
        `SELECT * FROM device_status
        WHERE deleted_at IS NULL
          AND COALESCE(updated_at, last_seen_iso, created_at) < ?
        ORDER BY updated_at DESC, id DESC`,
        [window.end]
    );
    const moduleRows = await dbAll(
        `SELECT * FROM device_module_status
        WHERE deleted_at IS NULL
          AND COALESCE(updated_at, last_seen_iso, created_at) < ?
        ORDER BY updated_at DESC, id DESC`,
        [window.end]
    );
    const voiceRows = await dbAll(
        `SELECT * FROM voice_turns
        WHERE deleted_at IS NULL
          AND created_at >= ?
          AND created_at < ?
        ORDER BY id ASC`,
        [window.start, window.end]
    );
    const commandRows = await dbAll(
        `SELECT * FROM command_queue
        WHERE deleted_at IS NULL
          AND created_at >= ?
          AND created_at < ?
        ORDER BY id ASC`,
        [window.start, window.end]
    );
    const conversationRows = await dbAll(
        `SELECT * FROM conversation_turns
        WHERE deleted_at IS NULL
          AND COALESCE(memory_level, '') <> 'archived'
          AND created_at >= ?
          AND created_at < ?
        ORDER BY id ASC`,
        [window.start, window.end]
    );
    const correctionRows = await dbAll(
        `SELECT * FROM memory_corrections
        WHERE deleted_at IS NULL
          AND COALESCE(status, '') NOT IN ('deleted','inactive','archived')
          AND created_at >= ?
          AND created_at < ?
        ORDER BY id ASC`,
        [window.start, window.end]
    );

    return {
        window,
        sensorRows,
        deviceStatusRows,
        moduleRows,
        voiceRows,
        commandRows,
        conversationRows,
        correctionRows
    };
}

function buildDailyStats(input) {
    return {
        sensor_records: {
            count: input.sensorRows.length,
            avg_temperature: numericAverage(input.sensorRows, "temperature"),
            avg_humidity: numericAverage(input.sensorRows, "humidity"),
            avg_pressure: numericAverage(input.sensorRows, "pressure"),
            avg_gas_resistance: numericAverage(input.sensorRows, "gas_resistance"),
            avg_upload_delay_ms: numericAverage(input.sensorRows, "upload_delay_ms"),
            first_timestamp: firstNumber(input.sensorRows, "timestamp"),
            sample_ids: sampleIds(input.sensorRows)
        },
        device_status: {
            count: input.deviceStatusRows.length,
            device_ids: input.deviceStatusRows.map(row => row.device_id).filter(Boolean).slice(0, 20)
        },
        device_module_status: {
            count: input.moduleRows.length,
            module_types: [...new Set(input.moduleRows.map(row => row.module_type).filter(Boolean))].slice(0, 20)
        },
        voice_turns: {
            count: input.voiceRows.length,
            status_counts: statusCounts(input.voiceRows),
            avg_total_ms: numericAverage(input.voiceRows, "total_ms"),
            sample_ids: sampleIds(input.voiceRows)
        },
        command_queue: {
            count: input.commandRows.length,
            status_counts: statusCounts(input.commandRows),
            sample_ids: sampleIds(input.commandRows)
        },
        conversation_turns: {
            count: input.conversationRows.length,
            sample_ids: sampleIds(input.conversationRows)
        },
        memory_corrections: {
            count: input.correctionRows.length,
            status_counts: statusCounts(input.correctionRows),
            sample_ids: sampleIds(input.correctionRows)
        }
    };
}

function totalDailySamples(stats) {
    return stats.sensor_records.count +
        stats.device_status.count +
        stats.device_module_status.count +
        stats.voice_turns.count +
        stats.command_queue.count +
        stats.conversation_turns.count +
        stats.memory_corrections.count;
}

function buildDailySummary(targetDate, stats) {
    const pieces = [
        `${targetDate} daily summary: sensor samples ${stats.sensor_records.count}`,
        `device snapshots ${stats.device_status.count}`,
        `module snapshots ${stats.device_module_status.count}`,
        `voice turns ${stats.voice_turns.count}`,
        `commands ${stats.command_queue.count}`,
        `conversation turns ${stats.conversation_turns.count}`,
        `corrections ${stats.memory_corrections.count}`
    ];

    if (stats.sensor_records.avg_temperature !== null) {
        pieces.push(`avg temperature ${stats.sensor_records.avg_temperature} C`);
    }
    if (stats.sensor_records.avg_humidity !== null) {
        pieces.push(`avg humidity ${stats.sensor_records.avg_humidity}%`);
    }
    if (stats.command_queue.status_counts.failed) {
        pieces.push(`failed commands ${stats.command_queue.status_counts.failed}`);
    }
    if (stats.voice_turns.status_counts.failed) {
        pieces.push(`failed voice turns ${stats.voice_turns.status_counts.failed}`);
    }

    return `${pieces.join("; ")}.`;
}

async function existingDailyMemory(dbAll, targetDate, memoryType) {
    const rows = await dbAll(
        `SELECT * FROM daily_memory
        WHERE deleted_at IS NULL
          AND memory_date=?
          AND memory_type=?
        ORDER BY id DESC
        LIMIT 1`,
        [targetDate, memoryType]
    );
    return rows[0] || null;
}

async function runDailySummaryJob(dbRun, dbAll, input = {}) {
    const targetDateResult = readDate(input.date ?? input.target_date, "DAILY_SUMMARY_DATE_INVALID", "date");
    if (!targetDateResult.ok) {
        return targetDateResult;
    }

    const targetDate = targetDateResult.value;
    const force = parseBoolean(input.force);
    const dryRun = parseBoolean(input.dry_run);
    const existing = await existingDailyMemory(dbAll, targetDate, "daily_summary");
    if (existing && !force) {
        return {
            ok: true,
            status: "skipped",
            skipped: true,
            reason: "daily summary already exists",
            memory_id: existing.id,
            memory_date: targetDate,
            server_time_ms: Date.now()
        };
    }

    const dailyInput = await fetchDailyInputs(dbAll, targetDate);
    const stats = buildDailyStats(dailyInput);
    const sampleCount = totalDailySamples(stats);
    const summary = input.summary || buildDailySummary(targetDate, stats);
    const evidence = [
        {
            source: "daily_summary_job",
            window_start: dailyInput.window.start,
            window_end: dailyInput.window.end,
            stats
        }
    ];

    if (dryRun) {
        return {
            ok: true,
            status: "dry_run",
            dry_run: true,
            memory_date: targetDate,
            memory_type: "daily_summary",
            summary,
            stats,
            evidence,
            sample_count: sampleCount,
            server_time_ms: Date.now()
        };
    }

    const job = await createMemoryJobRun(dbRun, {
        job_name: "daily_summary",
        target_date: targetDate,
        status: "completed",
        input: {
            target_date: targetDate,
            force,
            dry_run: false
        },
        result: {
            summary,
            sample_count: sampleCount,
            stats
        }
    });

    const memory = await createDailyMemory(dbRun, {
        memory_date: targetDate,
        memory_type: "daily_summary",
        summary,
        status: "candidate",
        source: "daily_summary_job",
        confidence: sampleCount > 0 ? 0.7 : 0.3,
        input: stats,
        raw: {
            job_id: job.job_id,
            stats
        },
        evidence,
        window_start: dailyInput.window.start,
        window_end: dailyInput.window.end,
        sample_count: sampleCount
    });

    return {
        ok: true,
        status: "completed",
        job_id: job.job_id,
        memory_id: memory.id,
        memory_date: targetDate,
        memory_type: "daily_summary",
        summary,
        stats,
        sample_count: sampleCount,
        server_time_ms: Date.now()
    };
}

async function fetchWeeklyInputs(dbAll, weekEnd) {
    const window = weekWindow(weekEnd);
    const dailyRows = await dbAll(
        `SELECT * FROM daily_memory
        WHERE deleted_at IS NULL
          AND COALESCE(status, '') NOT IN ('deleted','inactive','archived')
          AND memory_type='daily_summary'
          AND memory_date >= ?
          AND memory_date <= ?
        ORDER BY memory_date ASC, id ASC`,
        [window.startDate, window.endDate]
    );
    const sensorRows = await dbAll(
        `SELECT * FROM sensor_records
        WHERE deleted_at IS NULL
          AND COALESCE(server_recv_ms, timestamp) >= ?
          AND COALESCE(server_recv_ms, timestamp) < ?
        ORDER BY COALESCE(server_recv_ms, timestamp, id) ASC`,
        [window.startMs, window.endMs]
    );
    const voiceRows = await dbAll(
        `SELECT * FROM voice_turns
        WHERE deleted_at IS NULL
          AND created_at >= ?
          AND created_at < ?
        ORDER BY id ASC`,
        [window.start, window.end]
    );
    const commandRows = await dbAll(
        `SELECT * FROM command_queue
        WHERE deleted_at IS NULL
          AND created_at >= ?
          AND created_at < ?
        ORDER BY id ASC`,
        [window.start, window.end]
    );
    const conversationRows = await dbAll(
        `SELECT * FROM conversation_turns
        WHERE deleted_at IS NULL
          AND COALESCE(memory_level, '') <> 'archived'
          AND created_at >= ?
          AND created_at < ?
        ORDER BY id ASC`,
        [window.start, window.end]
    );
    const correctionRows = await dbAll(
        `SELECT * FROM memory_corrections
        WHERE deleted_at IS NULL
          AND COALESCE(status, '') NOT IN ('deleted','inactive','archived')
          AND created_at >= ?
          AND created_at < ?
        ORDER BY id ASC`,
        [window.start, window.end]
    );

    return {
        window,
        dailyRows,
        sensorRows,
        voiceRows,
        commandRows,
        conversationRows,
        correctionRows
    };
}

function buildWeeklyStats(input) {
    return {
        daily_memory: {
            count: input.dailyRows.length,
            memory_ids: sampleIds(input.dailyRows)
        },
        sensor_records: {
            count: input.sensorRows.length,
            avg_temperature: numericAverage(input.sensorRows, "temperature"),
            avg_humidity: numericAverage(input.sensorRows, "humidity"),
            avg_upload_delay_ms: numericAverage(input.sensorRows, "upload_delay_ms")
        },
        voice_turns: {
            count: input.voiceRows.length,
            status_counts: statusCounts(input.voiceRows)
        },
        command_queue: {
            count: input.commandRows.length,
            status_counts: statusCounts(input.commandRows)
        },
        conversation_turns: {
            count: input.conversationRows.length
        },
        memory_corrections: {
            count: input.correctionRows.length
        }
    };
}

function totalWeeklySamples(stats) {
    return stats.daily_memory.count +
        stats.sensor_records.count +
        stats.voice_turns.count +
        stats.command_queue.count +
        stats.conversation_turns.count +
        stats.memory_corrections.count;
}

function buildWeeklySummary(weekEnd, stats, window) {
    return [
        `${window.startDate} to ${weekEnd} weekly summary`,
        `daily summaries ${stats.daily_memory.count}`,
        `sensor samples ${stats.sensor_records.count}`,
        `voice turns ${stats.voice_turns.count}`,
        `commands ${stats.command_queue.count}`,
        `conversation turns ${stats.conversation_turns.count}`,
        `memory corrections ${stats.memory_corrections.count}`
    ].join("; ") + ".";
}

async function writeWeeklyCandidates(dbRun, weekEnd, stats, evidence) {
    const profileResult = await upsertProfile(dbRun, {
        profile_key: `weekly.${weekEnd}.usage`,
        profile_value: `Weekly candidate based on ${stats.conversation_turns.count} conversation turns and ${stats.memory_corrections.count} corrections.`,
        category: "user",
        status: "candidate",
        confidence: 0.45,
        evidence,
        source: "weekly_profile_job"
    });
    const environmentResult = await upsertEnvironmentProfile(dbRun, {
        profile_key: `weekly.${weekEnd}.environment`,
        profile_value: `Weekly environment candidate based on ${stats.sensor_records.count} sensor samples.`,
        status: "candidate",
        confidence: 0.45,
        evidence,
        source: "weekly_profile_job"
    });
    const experienceResult = await createExperienceMemory(dbRun, {
        title: `Weekly device experience ${weekEnd}`,
        situation: `Observed ${stats.voice_turns.count} voice turns and ${stats.command_queue.count} commands.`,
        action: "Generated candidate weekly operational memory.",
        outcome: "Candidate only; requires review before active use.",
        status: "candidate",
        confidence: 0.4,
        evidence,
        source: "weekly_profile_job"
    });
    const relationResult = await createRelationMemory(dbRun, {
        subject: "user",
        predicate: "used_device_during_week",
        object: weekEnd,
        relation_type: "weekly_usage",
        status: "candidate",
        confidence: 0.4,
        evidence,
        source: "weekly_profile_job"
    });

    return {
        profile: profileResult,
        environment: environmentResult,
        experience: experienceResult,
        relation: relationResult
    };
}

async function runWeeklyProfileJob(dbRun, dbAll, input = {}) {
    const targetDateResult = readWeeklyTarget(input);
    if (!targetDateResult.ok) {
        return targetDateResult;
    }

    const weekEnd = targetDateResult.value;
    const force = parseBoolean(input.force);
    const dryRun = parseBoolean(input.dry_run);
    const existing = await existingDailyMemory(dbAll, weekEnd, "weekly_summary");
    if (existing && !force) {
        return {
            ok: true,
            status: "skipped",
            skipped: true,
            reason: "weekly summary already exists",
            memory_id: existing.id,
            memory_date: weekEnd,
            memory_type: "weekly_summary",
            server_time_ms: Date.now()
        };
    }

    const weeklyInput = await fetchWeeklyInputs(dbAll, weekEnd);
    const stats = buildWeeklyStats(weeklyInput);
    const sampleCount = totalWeeklySamples(stats);
    const summary = input.summary || buildWeeklySummary(weekEnd, stats, weeklyInput.window);
    const evidence = [
        {
            source: "weekly_profile_job",
            window_start: weeklyInput.window.start,
            window_end: weeklyInput.window.end,
            daily_memory_ids: stats.daily_memory.memory_ids,
            stats
        }
    ];

    if (dryRun) {
        return {
            ok: true,
            status: "dry_run",
            dry_run: true,
            memory_date: weekEnd,
            memory_type: "weekly_summary",
            summary,
            stats,
            evidence,
            sample_count: sampleCount,
            server_time_ms: Date.now()
        };
    }

    const job = await createMemoryJobRun(dbRun, {
        job_name: "weekly_profile",
        target_date: weekEnd,
        status: "completed",
        input: {
            week_end: weekEnd,
            force,
            dry_run: false
        },
        result: {
            summary,
            sample_count: sampleCount,
            stats
        }
    });
    const memory = await createDailyMemory(dbRun, {
        memory_date: weekEnd,
        memory_type: "weekly_summary",
        summary,
        status: "candidate",
        source: "weekly_profile_job",
        confidence: sampleCount > 0 ? 0.65 : 0.25,
        input: stats,
        raw: {
            job_id: job.job_id,
            stats
        },
        evidence,
        window_start: weeklyInput.window.start,
        window_end: weeklyInput.window.end,
        sample_count: sampleCount
    });
    const candidates = await writeWeeklyCandidates(dbRun, weekEnd, stats, evidence);

    return {
        ok: true,
        status: "completed",
        job_id: job.job_id,
        memory_id: memory.id,
        memory_date: weekEnd,
        memory_type: "weekly_summary",
        summary,
        stats,
        candidates,
        sample_count: sampleCount,
        server_time_ms: Date.now()
    };
}

module.exports = {
    runDailySummaryJob,
    runWeeklyProfileJob
};
