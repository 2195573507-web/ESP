function summarizeSecret(value) {
    const secret = String(value || "");
    const length = secret.length;
    if (length === 0) {
        return "len=0, masked=-";
    }

    if (length <= 8) {
        return `len=${length}, masked=${secret.slice(0, 1)}***${secret.slice(-1)}`;
    }

    return `len=${length}, masked=${secret.slice(0, 4)}***${secret.slice(-4)}`;
}

function maskLogValue(value) {
    if (!value) {
        return "-";
    }

    if (value.length <= 6) {
        return `${value.slice(0, 1)}***len${value.length}`;
    }

    return `${value.slice(0, 3)}***${value.slice(-2)}len${value.length}`;
}

function normalizeLogPreview(value, maxLength = 500) {
    const preview = String(value || "")
        .replace(/\s+/g, " ")
        .trim();

    if (preview.length <= maxLength) {
        return preview;
    }

    return `${preview.slice(0, maxLength)}...`;
}

function maskUrlForLog(value) {
    if (!value) {
        return "-";
    }

    try {
        const url = new URL(value);
        for (const key of Array.from(url.searchParams.keys())) {
            if (/key|token|secret|auth|password/i.test(key)) {
                url.searchParams.set(key, "***");
            }
        }
        return url.toString();
    } catch (_) {
        return normalizeLogPreview(value, 160);
    }
}

function extractErrorMessageFromBody(body) {
    const preview = normalizeLogPreview(body);
    if (!preview) {
        return "";
    }

    try {
        const payload = JSON.parse(body);
        const message = payload?.error?.message ||
            payload?.error?.code ||
            payload?.error ||
            payload?.message ||
            payload?.code;

        if (typeof message === "string" && message.trim()) {
            return message.trim();
        }
    } catch (_) {
        // Non-JSON upstream errors are still useful as bounded log previews.
    }

    return preview;
}

module.exports = {
    extractErrorMessageFromBody,
    maskLogValue,
    maskUrlForLog,
    normalizeLogPreview,
    summarizeSecret
};
