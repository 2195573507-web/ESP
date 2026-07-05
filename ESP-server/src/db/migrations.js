function normalizeAddColumnType(type) {
    let normalized = String(type || "TEXT")
        .replace(/\s+PRIMARY\s+KEY\b/ig, "")
        .replace(/\s+AUTOINCREMENT\b/ig, "")
        .replace(/\s+UNIQUE\b/ig, "")
        .trim();

    if (/\bNOT\s+NULL\b/i.test(normalized) && !/\bDEFAULT\b/i.test(normalized)) {
        normalized = normalized.replace(/\s+NOT\s+NULL\b/ig, "").trim();
    }

    return normalized || "TEXT";
}

function quoteIdentifier(identifier) {
    const value = String(identifier || "");
    if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(value)) {
        throw new Error(`Invalid SQLite identifier: ${value}`);
    }

    return `"${value}"`;
}

async function ensureTableColumns(dbRun, dbAll, tableName, columns) {
    const existingRows = await dbAll(`PRAGMA table_info(${quoteIdentifier(tableName)})`);
    const existingNames = new Set(existingRows.map(column => column.name));

    for (const column of columns) {
        if (!existingNames.has(column.name)) {
            await dbRun(`ALTER TABLE ${quoteIdentifier(tableName)} ADD COLUMN ${quoteIdentifier(column.name)} ${column.addType || normalizeAddColumnType(column.type)}`);
            existingNames.add(column.name);
        }
    }
}

async function tableHasUniqueIndex(dbAll, tableName, columns) {
    const indexes = await dbAll(`PRAGMA index_list(${quoteIdentifier(tableName)})`);
    for (const index of indexes) {
        if (!Number(index.unique)) {
            continue;
        }

        const indexedColumns = await dbAll(`PRAGMA index_info(${quoteIdentifier(index.name)})`);
        const names = indexedColumns
            .sort((a, b) => a.seqno - b.seqno)
            .map(column => column.name);
        if (names.length === columns.length && names.every((name, index) => name === columns[index])) {
            return true;
        }
    }

    return false;
}

async function hasDuplicateKeyRows(dbAll, tableName, columns) {
    const quotedTable = quoteIdentifier(tableName);
    const quotedColumns = columns.map(quoteIdentifier);
    const notNullPredicates = quotedColumns.map(column => `${column} IS NOT NULL`);
    const rows = await dbAll(
        `SELECT 1
        FROM ${quotedTable}
        WHERE ${notNullPredicates.join(" AND ")}
        GROUP BY ${quotedColumns.join(", ")}
        HAVING COUNT(*) > 1
        LIMIT 1`
    );
    return rows.length > 0;
}

async function ensureUniqueIndex(dbRun, dbAll, tableName, indexName, columns) {
    if (!Array.isArray(columns) || columns.length === 0) {
        throw new Error("ensureUniqueIndex requires at least one column");
    }

    if (await tableHasUniqueIndex(dbAll, tableName, columns)) {
        return true;
    }

    if (await hasDuplicateKeyRows(dbAll, tableName, columns)) {
        console.warn(`[db:migration] skip unique index ${indexName} on ${tableName}(${columns.join(",")}) because duplicate keys already exist`);
        return false;
    }

    await dbRun(
        `CREATE UNIQUE INDEX IF NOT EXISTS ${quoteIdentifier(indexName)}
        ON ${quoteIdentifier(tableName)} (${columns.map(quoteIdentifier).join(", ")})`
    );
    return true;
}

module.exports = {
    ensureUniqueIndex,
    ensureTableColumns,
    normalizeAddColumnType
};
