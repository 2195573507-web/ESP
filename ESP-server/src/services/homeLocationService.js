const LOCATION_TEXT_LIMIT = 128;

function trimLocationText(value) {
    return typeof value === "string" ? value.trim().slice(0, LOCATION_TEXT_LIMIT) : "";
}

function finiteCoordinate(value, name, min, max) {
    if (value === undefined || value === null || value === "") {
        return null;
    }

    const numeric = Number(value);
    if (!Number.isFinite(numeric) || numeric < min || numeric > max) {
        return { error: `${name} must be between ${min} and ${max}` };
    }

    return numeric;
}

function isValidTimezone(value) {
    if (!value) {
        return true;
    }

    try {
        Intl.DateTimeFormat("en-US", { timeZone: value }).format();
        return true;
    } catch (_) {
        return false;
    }
}

function normalizeHomeLocation(input = {}) {
    if (!input || typeof input !== "object" || Array.isArray(input)) {
        return { ok: false, error: "JSON object body is required" };
    }

    const latitude = finiteCoordinate(input.latitude, "latitude", -90, 90);
    const longitude = finiteCoordinate(input.longitude, "longitude", -180, 180);
    if (latitude?.error) {
        return { ok: false, error: latitude.error };
    }
    if (longitude?.error) {
        return { ok: false, error: longitude.error };
    }

    const location = {
        country: trimLocationText(input.country),
        province: trimLocationText(input.province),
        city: trimLocationText(input.city),
        district: trimLocationText(input.district),
        latitude,
        longitude,
        timezone: trimLocationText(input.timezone)
    };
    if (!isValidTimezone(location.timezone)) {
        return { ok: false, error: "timezone must be a valid IANA timezone" };
    }

    return { ok: true, location };
}

function mapHomeLocation(row) {
    const location = {
        country: row?.country || "",
        province: row?.province || "",
        city: row?.city || "",
        district: row?.district || "",
        latitude: Number.isFinite(Number(row?.latitude)) ? Number(row.latitude) : null,
        longitude: Number.isFinite(Number(row?.longitude)) ? Number(row.longitude) : null,
        timezone: row?.timezone || ""
    };

    return {
        ...location,
        configured: Boolean(row)
    };
}

async function readHomeLocation(dbAll) {
    const rows = await dbAll("SELECT * FROM home_location WHERE id=1 LIMIT 1");
    return mapHomeLocation(rows[0]);
}

async function saveHomeLocation(dbRun, dbAll, input) {
    const normalized = normalizeHomeLocation(input);
    if (!normalized.ok) {
        return normalized;
    }

    const location = normalized.location;
    await dbRun(`
        INSERT INTO home_location
            (id,country,province,city,district,latitude,longitude,timezone,created_at,updated_at)
        VALUES(1,?,?,?,?,?,?,?,datetime('now'),datetime('now'))
        ON CONFLICT(id) DO UPDATE SET
            country=excluded.country,
            province=excluded.province,
            city=excluded.city,
            district=excluded.district,
            latitude=excluded.latitude,
            longitude=excluded.longitude,
            timezone=excluded.timezone,
            updated_at=datetime('now')`,
        [
            location.country,
            location.province,
            location.city,
            location.district,
            location.latitude,
            location.longitude,
            location.timezone
        ]
    );
    // A location change cannot reuse a weather observation for the old coordinates.
    // Standalone home-location callers may intentionally run before weather migration.
    try {
        await dbRun(
            "UPDATE weather_context SET expires_at_ms=0,available=0,updated_at_ms=? WHERE scope_key='home'",
            [Date.now()]
        );
    } catch (error) {
        if (error?.code !== "SQLITE_ERROR" || !/no such table: weather_context/i.test(error?.message || "")) {
            throw error;
        }
    }

    return {
        ok: true,
        location: await readHomeLocation(dbAll)
    };
}

function buildLocationLabel(location) {
    return [location?.district, location?.city, location?.province, location?.country]
        .filter(Boolean)
        .join(", ");
}

module.exports = {
    buildLocationLabel,
    normalizeHomeLocation,
    readHomeLocation,
    saveHomeLocation
};
