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
    let existingRows;
    try {
        existingRows = await dbAll(`PRAGMA table_info(${quoteIdentifier(tableName)})`);
    } catch (error) {
        throw migrationError(tableName, "PRAGMA table_info", error);
    }
    const existingNames = new Set(existingRows.map(column => column.name));

    for (const column of columns) {
        if (!existingNames.has(column.name)) {
            try {
                await dbRun(`ALTER TABLE ${quoteIdentifier(tableName)} ADD COLUMN ${quoteIdentifier(column.name)} ${column.addType || normalizeAddColumnType(column.type)}`);
                existingNames.add(column.name);
            } catch (error) {
                // A concurrent or interrupted deployment can leave the column in
                // place after our initial PRAGMA. Treat only that proven state as
                // idempotent; every other SQLite error must stop migration.
                let refreshedRows;
                try {
                    refreshedRows = await dbAll(`PRAGMA table_info(${quoteIdentifier(tableName)})`);
                } catch (pragmaError) {
                    throw migrationError(tableName, column.name, pragmaError, error);
                }
                if (refreshedRows.some(existing => existing.name === column.name)) {
                    existingNames.add(column.name);
                    continue;
                }
                throw migrationError(tableName, column.name, error);
            }
        }
    }

    let verifiedRows;
    try {
        verifiedRows = await dbAll(`PRAGMA table_info(${quoteIdentifier(tableName)})`);
    } catch (error) {
        throw migrationError(tableName, "PRAGMA table_info verification", error);
    }
    const verifiedNames = new Set(verifiedRows.map(column => column.name));
    for (const column of columns) {
        if (!verifiedNames.has(column.name)) {
            throw new Error(`[db:migration] table=${tableName} column=${column.name} SQLite error: column missing after ensure`);
        }
    }
}

function migrationError(tableName, columnName, error, originalError = null) {
    const sqliteMessage = error?.message || String(error);
    const suffix = originalError ? `; original SQLite error: ${originalError?.message || originalError}` : "";
    return new Error(`[db:migration] table=${tableName} column=${columnName} SQLite error: ${sqliteMessage}${suffix}`);
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
