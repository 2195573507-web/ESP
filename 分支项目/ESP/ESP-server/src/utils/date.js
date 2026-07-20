function isIsoDateString(value) {
    if (typeof value !== "string") {
        return false;
    }

    const text = value.trim();
    if (!/^\d{4}-\d{2}-\d{2}$/.test(text)) {
        return false;
    }

    const [year, month, day] = text.split("-").map(Number);
    const date = new Date(Date.UTC(year, month - 1, day));
    return date.getUTCFullYear() === year &&
        date.getUTCMonth() === month - 1 &&
        date.getUTCDate() === day;
}

module.exports = {
    isIsoDateString
};
