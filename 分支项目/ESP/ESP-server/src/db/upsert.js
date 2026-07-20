function isSqliteUniqueConstraintError(error) {
    return error?.code === "SQLITE_CONSTRAINT" &&
        String(error?.message || "").toUpperCase().includes("UNIQUE");
}

async function runUpdateThenInsert(dbRun, statements) {
    const updateResult = await dbRun(statements.updateSql, statements.updateParams);
    if (updateResult.changes) {
        return updateResult;
    }

    try {
        return await dbRun(statements.insertSql, statements.insertParams);
    } catch (error) {
        if (!isSqliteUniqueConstraintError(error)) {
            throw error;
        }

        const retryResult = await dbRun(statements.updateSql, statements.updateParams);
        if (retryResult.changes) {
            return retryResult;
        }

        throw error;
    }
}

module.exports = {
    isSqliteUniqueConstraintError,
    runUpdateThenInsert
};
