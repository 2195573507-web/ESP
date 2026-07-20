const {
    ensureTableColumns
} = require("./migrations");

const ASR_RECORD_COLUMNS = [
    { name: "id", type: "INTEGER PRIMARY KEY AUTOINCREMENT" },
    { name: "timestamp", type: "INTEGER" },
    { name: "text", type: "TEXT" }
];

const LLM_RECORD_COLUMNS = [
    { name: "id", type: "INTEGER PRIMARY KEY AUTOINCREMENT" },
    { name: "timestamp", type: "INTEGER" },
    { name: "prompt", type: "TEXT" },
    { name: "response", type: "TEXT" }
];

function columnSql(columns) {
    return columns.map(column => `${column.name} ${column.type}`).join(",\n            ");
}

function addableColumns(columns) {
    return columns.filter(column => !column.type.includes("PRIMARY KEY"));
}

async function ensureRecordTables(dbRun, dbAll) {
    await dbRun(`
        CREATE TABLE IF NOT EXISTS asr_records (
            ${columnSql(ASR_RECORD_COLUMNS)}
        )
    `);

    await dbRun(`
        CREATE TABLE IF NOT EXISTS llm_records (
            ${columnSql(LLM_RECORD_COLUMNS)}
        )
    `);

    if (typeof dbAll === "function") {
        await ensureTableColumns(dbRun, dbAll, "asr_records", addableColumns(ASR_RECORD_COLUMNS));
        await ensureTableColumns(dbRun, dbAll, "llm_records", addableColumns(LLM_RECORD_COLUMNS));
    }
}

module.exports = {
    ensureRecordTables
};
