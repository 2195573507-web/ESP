function findStringField(value, keys, visited = new Set()) {
    if (!value || typeof value !== "object") {
        return "";
    }

    if (visited.has(value)) {
        return "";
    }
    visited.add(value);

    if (Array.isArray(value)) {
        for (const item of value) {
            const found = findStringField(item, keys, visited);
            if (found) {
                return found;
            }
        }

        return "";
    }

    for (const key of keys) {
        const fieldValue = value[key];
        if (typeof fieldValue === "string" && fieldValue.trim()) {
            return fieldValue.trim();
        }
    }

    for (const fieldValue of Object.values(value)) {
        const found = findStringField(fieldValue, keys, visited);
        if (found) {
            return found;
        }
    }

    return "";
}

function extractAsrTextFromBody(responseBody) {
    const trimmed = responseBody.trim();
    if (!trimmed) {
        return "";
    }

    try {
        const payload = JSON.parse(trimmed);
        return findStringField(payload, [
            "text",
            "asr_text",
            "transcript",
            "utterance",
            "result_text",
            "final_text",
            "content"
        ]);
    } catch (_) {
        return trimmed;
    }
}

function decodeBase64Buffer(value) {
    if (typeof value !== "string") {
        return null;
    }

    let normalized = value.trim();
    const dataUrlMatch = normalized.match(/^data:[^,]+,(.+)$/i);
    if (dataUrlMatch) {
        normalized = dataUrlMatch[1];
    }

    normalized = normalized.replace(/\s+/g, "").replace(/-/g, "+").replace(/_/g, "/");
    if (!normalized || normalized.length % 4 === 1 || !/^[A-Za-z0-9+/]*={0,2}$/.test(normalized)) {
        return null;
    }

    const decoded = Buffer.from(normalized, "base64");
    return decoded.length > 0 ? decoded : null;
}

module.exports = {
    decodeBase64Buffer,
    extractAsrTextFromBody,
    findStringField
};
