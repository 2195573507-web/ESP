'use strict';

const NODE_IDS = ['C51', 'C52', 'S3'];
const LINK_DEFINITIONS = [
  { link_id: 'S3_TO_C51', source: 'S3', target: 'C51' },
  { link_id: 'S3_TO_C52', source: 'S3', target: 'C52' },
  { link_id: 'C51_TO_C52', source: 'C51', target: 'C52' },
  { link_id: 'C52_TO_C51', source: 'C52', target: 'C51' },
];
const LINK_IDS = LINK_DEFINITIONS.map((link) => link.link_id);
const LINK_BY_ID = Object.fromEntries(LINK_DEFINITIONS.map((link) => [link.link_id, link]));

const DEFAULT_ALIGNMENT_WINDOW_MS = 400;
const MIN_ALIGNMENT_WINDOW_MS = 200;
const MAX_ALIGNMENT_WINDOW_MS = 500;
const DEFAULT_MIN_DWELL_MS = 600;
const DEFAULT_DECAY_MS = 1600;
const LINK_HEALTH_STALE_MS = 5000;

const DEFAULT_NODE_GEOMETRY = {
  C51: { x: 0.5, y: 0.14 },
  C52: { x: 0.18, y: 0.78 },
  S3: { x: 0.82, y: 0.78 },
};

function clamp(value, min, max) {
  const number = Number(value);
  if (!Number.isFinite(number)) {
    return min;
  }
  return Math.min(max, Math.max(min, number));
}

function clamp01(value) {
  return clamp(value, 0, 1);
}

function finiteNumber(value) {
  if (value === null || value === undefined || value === '') {
    return null;
  }
  const number = Number(value);
  return Number.isFinite(number) ? number : null;
}

function firstFinite(...values) {
  for (const value of values) {
    const number = finiteNumber(value);
    if (number !== null) {
      return number;
    }
  }
  return null;
}

function asObject(value) {
  return value && typeof value === 'object' && !Array.isArray(value) ? value : null;
}

function normalizeState(value) {
  const state = String(value || '').trim().toUpperCase();
  if (state === 'MOTION' || state === 'MOVING' || state === 'ACTIVE' || state === 'OCCUPIED') {
    return 'MOTION';
  }
  if (state === 'HOLD' || state === 'STATIC' || state === 'PRESENT') {
    return 'HOLD';
  }
  if (state === 'IDLE' || state === 'VACANT' || state === 'EMPTY') {
    return 'IDLE';
  }
  return '';
}

function motionScoreForState(value) {
  const state = normalizeState(value);
  if (state === 'MOTION') {
    return 0.72;
  }
  if (state === 'HOLD') {
    return 0.44;
  }
  if (state === 'IDLE') {
    return 0.08;
  }
  return null;
}

function normalizeSchemaVersion(sample) {
  const explicit = sample.schema_version ?? sample.schemaVersion ?? sample.schema;
  if (explicit !== undefined && explicit !== null) {
    return explicit;
  }
  if (isS3FusionShape(sample)) {
    return 's3-csi-fusion-v1';
  }
  if (sample.payload && sample.payload_type) {
    return 'canonical-csi-fact-v1';
  }
  if (sample.metrics || sample.features || sample.frame_seq !== undefined || sample.state_hint !== undefined) {
    return 'c5-feature-v1';
  }
  return 'legacy-csi-fact-v1';
}

function normalizeToken(value) {
  return String(value || '')
    .trim()
    .toUpperCase()
    .replace(/[\s-]+/g, '_');
}

function normalizeLinkId(...values) {
  const tokens = values
    .filter((value) => value !== undefined && value !== null && String(value).trim())
    .map(normalizeToken);

  for (const token of tokens) {
    for (const linkId of LINK_IDS) {
      if (token === linkId || token.includes(linkId)) {
        return linkId;
      }
    }
  }

  const text = tokens.join(' ');
  if (/\bC51\b|ESPC51|SENSAIR_C51/.test(text)) {
    return 'S3_TO_C51';
  }
  if (/\bC52\b|ESPC52|SENSAIR_C52/.test(text)) {
    return 'S3_TO_C52';
  }
  return '';
}

function timestampFrom(sample, fallback = null) {
  const timestamp = firstFinite(
    sample.timestamp_ms,
    sample.timestamp,
    sample.updated_at_ms,
    sample.esp_time_ms,
    sample.received_at_ms,
    sample.server_time_ms,
  );
  if (timestamp !== null) {
    return Math.round(timestamp);
  }
  if (sample.received_at_iso) {
    const parsed = Date.parse(sample.received_at_iso);
    if (Number.isFinite(parsed)) {
      return parsed;
    }
  }
  return fallback;
}

function observedAtFrom(sample, fallback = null) {
  const observedAt = firstFinite(sample.server_time_ms, sample.received_at_ms);
  if (observedAt !== null) {
    return Math.round(observedAt);
  }
  if (sample.received_at_iso) {
    const parsed = Date.parse(sample.received_at_iso);
    if (Number.isFinite(parsed)) {
      return parsed;
    }
  }
  return fallback;
}

function isS3FusionShape(sample) {
  return sample.type === 'csi_fusion' ||
    sample.link_states !== undefined ||
    (sample.fused_state !== undefined && (sample.links !== undefined || sample.confidence !== undefined));
}

function detectSchema(sample) {
  if (isS3FusionShape(sample)) {
    return 's3_csi_fusion';
  }
  if (asObject(sample.payload) &&
      (sample.payload.motion_score !== undefined || sample.payload.frame_energy !== undefined ||
       sample.payload.variance !== undefined || sample.payload.state !== undefined ||
       sample.payload.confidence !== undefined || sample.payload.fused_state !== undefined)) {
    return 'canonical_csi_fact';
  }
  if (String(sample.schema_version || '').toLowerCase().includes('c5-feature') ||
      asObject(sample.metrics) || asObject(sample.features) ||
      sample.motion_score_local !== undefined || sample.state_hint !== undefined) {
    return 'c5_feature';
  }
  return 'legacy_csi_fact';
}

function compactLinkEvent(input) {
  const linkId = normalizeLinkId(input.link_id, input.linkId, input.role, input.device_id, input.source, input.target);
  if (!LINK_BY_ID[linkId]) {
    return null;
  }
  const link = LINK_BY_ID[linkId];
  const timestamp = input.timestamp_ms !== null && input.timestamp_ms !== undefined
    ? Math.round(input.timestamp_ms)
    : null;
  const observedAtMs = input.observed_at_ms !== null && input.observed_at_ms !== undefined
    ? Math.round(input.observed_at_ms)
    : null;
  const motionScore = firstFinite(
    input.motion_score,
    input.presence_score,
    input.motion_score_local,
    input.confidence,
    motionScoreForState(input.state_hint),
    motionScoreForState(input.state),
  );
  const quality = firstFinite(input.quality, input.link_quality, input.confidence);
  const state = normalizeState(input.state ?? input.motion_state ?? input.state_hint);

  return {
    timestamp,
    timestamp_ms: timestamp,
    link_id: linkId,
    source: link.source,
    target: link.target,
    motion_score: motionScore !== null ? clamp01(motionScore) : null,
    energy: firstFinite(input.energy, input.frame_energy, input.amplitude),
    variance: firstFinite(input.variance, input.amplitude_variance),
    quality: quality !== null ? clamp01(quality) : null,
    rssi: firstFinite(input.rssi),
    state,
    confidence: input.confidence !== undefined && input.confidence !== null ? clamp01(input.confidence) : null,
    tick_id: input.tick_id !== null && input.tick_id !== undefined ? Math.round(Number(input.tick_id)) : null,
    frame_seq: input.frame_seq !== undefined && input.frame_seq !== null ? Math.round(Number(input.frame_seq)) : null,
    observed_at_ms: observedAtMs,
    schema_version: input.schema_version,
    schema_path: input.schema_path,
    source_format: input.source_format || '',
    device_id: input.device_id || '',
    trace_id: input.trace_id || '',
    synthetic: Boolean(input.synthetic),
    valid: input.valid !== false,
  };
}

function compactFusionEvent(input) {
  const timestamp = input.timestamp_ms !== null && input.timestamp_ms !== undefined
    ? Math.round(input.timestamp_ms)
    : null;
  const observedAtMs = input.observed_at_ms !== null && input.observed_at_ms !== undefined
    ? Math.round(input.observed_at_ms)
    : null;
  const state = normalizeState(input.state ?? input.fused_state);
  const confidence = firstFinite(input.confidence, input.quality, input.motion_score, motionScoreForState(state));
  const motionScore = firstFinite(input.motion_score, input.confidence, motionScoreForState(state));

  return {
    timestamp,
    timestamp_ms: timestamp,
    link_id: 'FUSION_AREA',
    source: 'S3',
    target: 'AREA',
    motion_score: motionScore !== null ? clamp01(motionScore) : null,
    energy: firstFinite(input.energy, input.frame_energy),
    variance: firstFinite(input.variance),
    quality: confidence !== null ? clamp01(confidence) : null,
    rssi: firstFinite(input.rssi),
    state,
    confidence: confidence !== null ? clamp01(confidence) : null,
    tick_id: input.tick_id !== null && input.tick_id !== undefined ? Math.round(Number(input.tick_id)) : null,
    frame_seq: input.frame_seq !== undefined && input.frame_seq !== null ? Math.round(Number(input.frame_seq)) : null,
    observed_at_ms: observedAtMs,
    schema_version: input.schema_version,
    schema_path: input.schema_path,
    source_format: input.source_format || '',
    device_id: input.device_id || '',
    trace_id: input.trace_id || '',
    synthetic: Boolean(input.synthetic),
    valid: input.valid !== false,
  };
}

function linkStateEntries(linkStates) {
  if (Array.isArray(linkStates)) {
    return linkStates.map((state, index) => {
      const object = asObject(state) || {};
      return [object.link_id || object.role || object.device_id || `link_${index}`, object];
    });
  }
  const object = asObject(linkStates) || {};
  return Object.entries(object);
}

function fusedStateObject(sample) {
  const fused = sample.fused_state;
  if (asObject(fused)) {
    return fused;
  }
  if (typeof fused === 'string') {
    return { state: fused, confidence: sample.confidence, motion_score: sample.motion_score };
  }
  return {};
}

function parseS3FusionTelemetry(sample, sourceFormat) {
  const events = [];
  const fusionEvents = [];
  const timestampMs = timestampFrom(sample);
  const observedAtMs = observedAtFrom(sample, timestampMs);
  const tickId = sample.tick_id;
  const schemaVersion = normalizeSchemaVersion(sample);
  const fusedState = fusedStateObject(sample);

  for (const [linkName, linkState] of linkStateEntries(sample.link_states)) {
    const state = asObject(linkState) || {};
    const event = compactLinkEvent({
      schema_version: schemaVersion,
      schema_path: 's3_csi_fusion.link_state',
      device_id: state.device_id || linkName,
      link_id: state.link_id || linkName,
      trace_id: state.trace_id || sample.trace_id,
      tick_id: state.tick_id ?? tickId,
      timestamp_ms: timestampFrom(state, timestampMs),
      observed_at_ms: observedAtFrom(state, observedAtMs),
      motion_score: firstFinite(state.motion_score, state.presence_score, state.confidence),
      energy: firstFinite(state.frame_energy, state.energy),
      variance: state.variance,
      rssi: state.rssi,
      quality: firstFinite(state.quality, state.confidence),
      confidence: state.confidence,
      state: state.state ?? fusedState.state,
      frame_seq: state.frame_seq,
      source_format: sourceFormat || sample.source_format,
      valid: state.valid !== false,
    });
    if (event) {
      events.push(event);
    }
  }

  const links = Array.isArray(sample.links) ? sample.links : [];
  for (const linkName of links) {
    const event = compactLinkEvent({
      schema_version: schemaVersion,
      schema_path: 's3_csi_fusion.link_presence',
      device_id: linkName,
      link_id: linkName,
      trace_id: sample.trace_id,
      tick_id: tickId,
      timestamp_ms: timestampMs,
      observed_at_ms: observedAtMs,
      motion_score: firstFinite(sample.motion_score, sample.confidence, motionScoreForState(fusedState.state)),
      quality: firstFinite(sample.quality, sample.confidence),
      confidence: sample.confidence,
      state: fusedState.state,
      source_format: sourceFormat || sample.source_format,
    });
    if (event) {
      events.push(event);
    }
  }

  if (Object.keys(fusedState).length || sample.confidence !== undefined || sample.motion_score !== undefined) {
    fusionEvents.push(compactFusionEvent({
      schema_version: schemaVersion,
      schema_path: 's3_csi_fusion.fused_state',
      device_id: sample.device_id || sample.gateway_id || 'S3',
      trace_id: sample.trace_id,
      tick_id: tickId,
      timestamp_ms: timestampMs,
      observed_at_ms: observedAtMs,
      motion_score: firstFinite(fusedState.motion_score, sample.motion_score, sample.confidence),
      confidence: firstFinite(fusedState.confidence, sample.confidence),
      energy: firstFinite(sample.frame_energy, sample.energy),
      variance: sample.variance,
      rssi: sample.rssi,
      state: fusedState.state ?? sample.state,
      source_format: sourceFormat || sample.source_format,
    }));
  }

  return { events, fusionEvents };
}

function expandAreaFactToLinks(baseInput, linkId) {
  if (LINK_BY_ID[linkId]) {
    const event = compactLinkEvent({ ...baseInput, link_id: linkId });
    return event ? [event] : [];
  }
  return LINK_IDS
    .map((id) => compactLinkEvent({ ...baseInput, link_id: id, synthetic: true }))
    .filter(Boolean);
}

function parseCanonicalOrLegacy(sample, payload, schemaPath, sourceFormat) {
  const merged = { ...sample, ...payload };
  const timestampMs = timestampFrom(merged, timestampFrom(sample));
  const observedAtMs = observedAtFrom(merged, observedAtFrom(sample, timestampMs));
  const linkId = normalizeLinkId(merged.link_id, merged.device_id, merged.source, merged.target);
  const baseInput = {
    schema_version: normalizeSchemaVersion(sample),
    schema_path: schemaPath,
    device_id: merged.device_id || sample.device_id || sample.gateway_id || '',
    link_id: linkId,
    trace_id: merged.trace_id || sample.trace_id,
    tick_id: merged.tick_id,
    timestamp_ms: timestampMs,
    observed_at_ms: observedAtMs,
    motion_score: firstFinite(merged.motion_score, merged.presence_score, merged.confidence),
    energy: firstFinite(merged.frame_energy, merged.energy, merged.amplitude),
    variance: firstFinite(merged.variance, merged.amplitude_variance),
    rssi: merged.rssi,
    quality: firstFinite(merged.quality, merged.confidence),
    confidence: merged.confidence,
    state: merged.state ?? merged.motion_state ?? merged.occupancy ?? merged.fused_state,
    frame_seq: merged.frame_seq ?? merged.seq,
    source_format: sourceFormat || merged.source_format,
  };

  const events = expandAreaFactToLinks(baseInput, linkId);
  const fusionEvents = [];
  if (!linkId || merged.fused_state !== undefined || schemaPath === 'canonical_csi_fact') {
    fusionEvents.push(compactFusionEvent({
      ...baseInput,
      schema_path: `${schemaPath}.fused_state`,
      link_id: 'FUSION_AREA',
    }));
  }
  return { events, fusionEvents: fusionEvents.filter(Boolean) };
}

function parseC5Feature(sample, sourceFormat) {
  const metrics = asObject(sample.metrics) || {};
  const features = asObject(sample.features) || {};
  const timestampMs = timestampFrom(sample);
  const observedAtMs = observedAtFrom(sample, timestampMs);
  const stateHint = sample.state_hint ?? sample.stateHint ?? sample.state;
  const event = compactLinkEvent({
    schema_version: normalizeSchemaVersion(sample),
    schema_path: 'c5_feature',
    device_id: sample.device_id || '',
    link_id: sample.link_id || sample.role || sample.device_id,
    trace_id: sample.trace_id,
    tick_id: sample.tick_id,
    timestamp_ms: timestampMs,
    observed_at_ms: observedAtMs,
    motion_score: firstFinite(sample.motion_score, sample.motion_score_local, features.motion_score_local, sample.confidence),
    energy: firstFinite(sample.frame_energy, metrics.frame_energy, sample.energy),
    variance: firstFinite(sample.variance, metrics.variance),
    rssi: firstFinite(sample.rssi, metrics.rssi),
    quality: firstFinite(sample.quality, metrics.quality, features.quality),
    confidence: firstFinite(sample.confidence, features.confidence),
    state: sample.state,
    state_hint: stateHint,
    frame_seq: sample.frame_seq ?? sample.seq,
    source_format: sourceFormat || sample.source_format,
  });

  return { events: event ? [event] : [], fusionEvents: [] };
}

function parseTelemetrySample(rawSample, options = {}) {
  const sample = asObject(rawSample) || {};
  const sourceFormat = options.sourceFormat || sample.source_format || '';
  const schemaPath = detectSchema(sample);
  let parsed;

  if (schemaPath === 's3_csi_fusion') {
    parsed = parseS3FusionTelemetry(sample, sourceFormat);
  } else if (schemaPath === 'canonical_csi_fact') {
    parsed = parseCanonicalOrLegacy(sample, asObject(sample.payload) || {}, 'canonical_csi_fact', sourceFormat);
  } else if (schemaPath === 'c5_feature') {
    parsed = parseC5Feature(sample, sourceFormat);
  } else {
    parsed = parseCanonicalOrLegacy(sample, {}, 'legacy_csi_fact', sourceFormat);
  }

  const events = parsed.events
    .filter((event) => event && (event.timestamp !== null || event.motion_score !== null || event.variance !== null));
  const fusionEvents = parsed.fusionEvents
    .filter((event) => event && (event.timestamp !== null || event.motion_score !== null || event.state));

  return {
    schema_version: normalizeSchemaVersion(sample),
    schema_path: schemaPath,
    events,
    fusion_events: fusionEvents,
  };
}

function annotateSampleWithTelemetry(sample) {
  const parsed = parseTelemetrySample(sample);
  return {
    ...sample,
    telemetry_schema: parsed.schema_version,
    telemetry_schema_path: parsed.schema_path,
    telemetry_events: parsed.events,
    telemetry_fusion_events: parsed.fusion_events,
  };
}

function normalizeWindowMs(value) {
  const number = Number(value ?? DEFAULT_ALIGNMENT_WINDOW_MS);
  return Math.round(clamp(
    Number.isFinite(number) ? number : DEFAULT_ALIGNMENT_WINDOW_MS,
    MIN_ALIGNMENT_WINDOW_MS,
    MAX_ALIGNMENT_WINDOW_MS,
  ));
}

function median(values) {
  if (!values.length) {
    return null;
  }
  const sorted = values.slice().sort((a, b) => a - b);
  const middle = Math.floor(sorted.length / 2);
  return sorted.length % 2 === 0
    ? (sorted[middle - 1] + sorted[middle]) / 2
    : sorted[middle];
}

function average(values) {
  const numbers = values.filter((value) => Number.isFinite(value));
  if (!numbers.length) {
    return null;
  }
  return numbers.reduce((sum, value) => sum + value, 0) / numbers.length;
}

function stddev(values) {
  const numbers = values.filter((value) => Number.isFinite(value));
  if (numbers.length < 2) {
    return 0;
  }
  const mean = average(numbers);
  const variance = average(numbers.map((value) => (value - mean) ** 2));
  return Math.sqrt(variance);
}

function buildAnchors(events, fusionEvents, windowMs) {
  const tickAnchors = new Map();
  const timeAnchors = new Map();

  for (const event of [...events, ...fusionEvents]) {
    if (event.tick_id !== null && event.tick_id !== undefined) {
      const key = String(event.tick_id);
      if (!tickAnchors.has(key)) {
        tickAnchors.set(key, { key: `tick:${key}`, tick_id: event.tick_id, timestamps: [], observed: [], basis: 'tick_id' });
      }
      if (event.timestamp !== null) {
        tickAnchors.get(key).timestamps.push(event.timestamp);
      }
      if (event.observed_at_ms !== null) {
        tickAnchors.get(key).observed.push(event.observed_at_ms);
      }
      continue;
    }

    if (event.timestamp === null || event.timestamp === undefined) {
      continue;
    }
    const bucket = Math.round(event.timestamp / windowMs);
    const key = String(bucket);
    if (!timeAnchors.has(key)) {
      timeAnchors.set(key, { key: `time:${key}`, tick_id: null, timestamps: [], observed: [], basis: 'timestamp_ms' });
    }
    timeAnchors.get(key).timestamps.push(event.timestamp);
    if (event.observed_at_ms !== null) {
      timeAnchors.get(key).observed.push(event.observed_at_ms);
    }
  }

  return [...tickAnchors.values(), ...timeAnchors.values()]
    .map((anchor) => ({
      ...anchor,
      timestamp_ms: Math.round(median(anchor.timestamps) ?? 0),
      observed_at_ms: Math.round(median(anchor.observed) ?? 0) || null,
    }))
    .filter((anchor) => anchor.timestamp_ms > 0 || anchor.tick_id !== null)
    .sort((a, b) => {
      if (a.timestamp_ms !== b.timestamp_ms) {
        return a.timestamp_ms - b.timestamp_ms;
      }
      return String(a.key).localeCompare(String(b.key));
    });
}

function selectBestEvent(events, matcher, anchor, windowMs) {
  const halfWindow = windowMs / 2;
  const candidates = events
    .filter(matcher)
    .map((event) => {
      const sameTick = anchor.tick_id !== null && event.tick_id === anchor.tick_id;
      const delta = event.timestamp !== null && anchor.timestamp_ms
        ? Math.abs(event.timestamp - anchor.timestamp_ms)
        : Number.POSITIVE_INFINITY;
      return { event, sameTick, delta };
    })
    .filter((candidate) => candidate.sameTick || candidate.delta <= halfWindow);

  candidates.sort((a, b) => {
    if (a.sameTick !== b.sameTick) {
      return a.sameTick ? -1 : 1;
    }
    if (a.delta !== b.delta) {
      return a.delta - b.delta;
    }
    return (b.event.observed_at_ms || b.event.timestamp || 0) -
      (a.event.observed_at_ms || a.event.timestamp || 0);
  });

  return candidates.length ? candidates[0].event : null;
}

function metricBounds(frames, field) {
  const values = [];
  for (const frame of frames) {
    for (const linkId of LINK_IDS) {
      const value = frame.links[linkId]?.[field];
      if (Number.isFinite(value)) {
        values.push(value);
      }
    }
  }
  return values.length
    ? { min: Math.min(...values), max: Math.max(...values) }
    : { min: 0, max: 0 };
}

function normalizeMetric(value, min, max) {
  if (!Number.isFinite(value)) {
    return 0;
  }
  if (!Number.isFinite(min) || !Number.isFinite(max) || max <= min) {
    return value > 0 ? 0.5 : 0;
  }
  return clamp01((value - min) / (max - min));
}

function ageForEvent(event, frame, referenceObservedAtMs) {
  if (!event) {
    return null;
  }
  if (event.observed_at_ms !== null && Number.isFinite(referenceObservedAtMs)) {
    return Math.max(0, referenceObservedAtMs - event.observed_at_ms);
  }
  if (event.timestamp !== null && frame.timestamp_ms !== null) {
    return Math.max(0, frame.timestamp_ms - event.timestamp);
  }
  return null;
}

function decorateLinkEvent(event, frame, referenceObservedAtMs) {
  if (!event) {
    return null;
  }
  const ageMs = ageForEvent(event, frame, referenceObservedAtMs);
  const decayMs = DEFAULT_DECAY_MS;
  const freshness = ageMs === null ? 1 : clamp01(Math.exp(-ageMs / decayMs));
  const quality = clamp01(event.quality ?? event.confidence ?? 0.5);
  const motion = clamp01(event.motion_score ?? 0);
  const health = clamp01(quality * freshness);
  const visualStrength = clamp01(((motion * 0.7) + (quality * 0.3)) * freshness);

  return {
    ...event,
    age_ms: ageMs,
    freshness,
    health,
    visual_strength: visualStrength,
    healthy: health >= 0.28 && (ageMs === null || ageMs <= LINK_HEALTH_STALE_MS),
  };
}

function frameFusion(links, fusionEvent) {
  const linkEvents = LINK_IDS.map((linkId) => links[linkId]).filter(Boolean);
  const scores = linkEvents.map((event) => event.motion_score).filter((value) => Number.isFinite(value));
  const qualities = linkEvents.map((event) => event.quality ?? event.confidence)
    .filter((value) => Number.isFinite(value));
  const motionScore = fusionEvent?.motion_score ?? average(scores) ?? 0;
  const confidence = fusionEvent?.confidence ?? fusionEvent?.quality ?? average(qualities) ?? clamp01(1 - stddev(scores));
  return {
    motion_score: clamp01(motionScore),
    state: fusionEvent?.state || '',
    confidence: clamp01(confidence),
    reported_state: fusionEvent?.state || '',
  };
}

function alignTelemetryEvents(rawEvents, options = {}) {
  const windowMs = normalizeWindowMs(options.windowMs);
  const referenceObservedAtMs = Number(options.referenceObservedAtMs ?? options.generatedAtMs ?? Date.now());
  const events = rawEvents
    .map((event) => compactLinkEvent(event) || event)
    .filter((event) => event && LINK_BY_ID[event.link_id] && event.valid !== false);
  const fusionEvents = (options.fusionEvents || [])
    .map((event) => compactFusionEvent(event) || event)
    .filter((event) => event && event.valid !== false);
  const anchors = buildAnchors(events, fusionEvents, windowMs);

  return anchors.map((anchor) => {
    const links = {};
    for (const linkId of LINK_IDS) {
      const selected = selectBestEvent(events, (event) => event.link_id === linkId, anchor, windowMs);
      links[linkId] = selected ? decorateLinkEvent(selected, { timestamp_ms: anchor.timestamp_ms }, referenceObservedAtMs) : null;
    }

    const fusionEvent = selectBestEvent(fusionEvents, () => true, anchor, windowMs);
    const activeLinks = LINK_IDS.filter((linkId) => links[linkId]);
    const timestamps = activeLinks.map((linkId) => links[linkId].timestamp).filter((value) => Number.isFinite(value));
    const timestampMs = Math.round(median(timestamps) ?? fusionEvent?.timestamp ?? anchor.timestamp_ms);
    const observedAtMs = Math.round(median(activeLinks.map((linkId) => links[linkId].observed_at_ms).filter(Number.isFinite)) ??
      fusionEvent?.observed_at_ms ?? anchor.observed_at_ms ?? 0) || null;
    const frame = {
      frame_type: 'aligned_frame',
      alignment_key: anchor.key,
      alignment_basis: anchor.basis,
      tick_id: anchor.tick_id,
      timestamp_ms: timestampMs,
      timestamp: timestampMs,
      observed_at_ms: observedAtMs,
      window_ms: windowMs,
      links,
      active_link_count: activeLinks.length,
      missing_links: LINK_IDS.filter((linkId) => !links[linkId]),
      fusion: frameFusion(links, fusionEvent),
    };

    for (const linkId of LINK_IDS) {
      if (frame.links[linkId]) {
        frame.links[linkId] = decorateLinkEvent(frame.links[linkId], frame, referenceObservedAtMs);
      }
    }
    return frame;
  });
}

function frameMetrics(frame, bounds) {
  const linkEvents = LINK_IDS.map((linkId) => frame.links[linkId]).filter(Boolean);
  const motionScore = frame.fusion.motion_score ?? average(linkEvents.map((event) => event.motion_score)) ?? 0;
  const energy = average(linkEvents.map((event) => event.energy));
  const variance = average(linkEvents.map((event) => event.variance));
  const energyScore = normalizeMetric(energy, bounds.energy.min, bounds.energy.max);
  const varianceScore = normalizeMetric(variance, bounds.variance.min, bounds.variance.max);
  const compositeScore = clamp01((0.68 * motionScore) + (0.18 * energyScore) + (0.14 * varianceScore));
  return {
    motion_score: clamp01(motionScore),
    energy,
    variance,
    composite_score: compositeScore,
  };
}

function nextStateForScore(currentState, score, thresholds) {
  if (currentState === 'MOTION') {
    return score <= thresholds.motionExit ? 'HOLD' : 'MOTION';
  }
  if (currentState === 'HOLD') {
    if (score >= thresholds.motionEnter) {
      return 'MOTION';
    }
    return score <= thresholds.idleEnter ? 'IDLE' : 'HOLD';
  }
  return score >= thresholds.motionEnter ? 'MOTION' : 'IDLE';
}

function buildStateTimeline(frames, options = {}) {
  const minDwellMs = Math.max(0, Number(options.minDwellMs ?? DEFAULT_MIN_DWELL_MS));
  const thresholds = {
    motionEnter: clamp01(options.motionEnter ?? 0.62),
    motionExit: clamp01(options.motionExit ?? 0.42),
    idleEnter: clamp01(options.idleEnter ?? 0.24),
  };
  const bounds = {
    energy: metricBounds(frames, 'energy'),
    variance: metricBounds(frames, 'variance'),
  };

  let state = 'IDLE';
  let stateSinceMs = null;
  let pendingState = '';
  let pendingSinceMs = null;

  return frames.map((frame, index) => {
    const metrics = frameMetrics(frame, bounds);
    const timestampMs = frame.timestamp_ms ?? index;
    let candidateState = frame.fusion.state || nextStateForScore(state, metrics.composite_score, thresholds);

    if (index === 0) {
      state = candidateState || 'IDLE';
      stateSinceMs = timestampMs;
      pendingState = '';
      pendingSinceMs = null;
    } else if (candidateState && candidateState !== state) {
      if (pendingState !== candidateState) {
        pendingState = candidateState;
        pendingSinceMs = timestampMs;
      }
      const pendingAge = timestampMs - pendingSinceMs;
      const stateAge = timestampMs - stateSinceMs;
      if (pendingAge >= minDwellMs && stateAge >= minDwellMs) {
        state = candidateState;
        stateSinceMs = timestampMs;
        pendingState = '';
        pendingSinceMs = null;
      } else {
        candidateState = state;
      }
    } else {
      pendingState = '';
      pendingSinceMs = null;
    }

    return {
      timestamp: timestampMs,
      timestamp_ms: timestampMs,
      tick_id: frame.tick_id,
      state,
      candidate_state: candidateState || state,
      reported_state: frame.fusion.reported_state || '',
      dwell_ms: stateSinceMs !== null ? Math.max(0, timestampMs - stateSinceMs) : 0,
      score: metrics.composite_score,
      motion_score: metrics.motion_score,
      energy: metrics.energy,
      variance: metrics.variance,
      confidence: frame.fusion.confidence,
      hysteresis: {
        min_dwell_ms: minDwellMs,
        motion_enter: thresholds.motionEnter,
        motion_exit: thresholds.motionExit,
        idle_enter: thresholds.idleEnter,
      },
    };
  });
}

function midpoint(a, b) {
  return {
    x: Number(((a.x + b.x) / 2).toFixed(4)),
    y: Number(((a.y + b.y) / 2).toFixed(4)),
  };
}

function buildTopologyFrame(frame) {
  const nodes = NODE_IDS.map((nodeId) => ({
    node_id: nodeId,
    ...(DEFAULT_NODE_GEOMETRY[nodeId] || { x: 0, y: 0 }),
  }));
  const edges = LINK_DEFINITIONS.map((link) => {
    const event = frame?.links?.[link.link_id] || null;
    return {
      ...link,
      state: event?.state || frame?.fusion?.state || '',
      motion_score: event?.motion_score ?? null,
      energy: event?.energy ?? null,
      variance: event?.variance ?? null,
      quality: event?.quality ?? null,
      rssi: event?.rssi ?? null,
      age_ms: event?.age_ms ?? null,
      health: event?.health ?? 0,
      visual_strength: event?.visual_strength ?? 0,
      healthy: Boolean(event?.healthy),
      synthetic: Boolean(event?.synthetic),
      timestamp: event?.timestamp ?? null,
    };
  });
  return { nodes, edges };
}

function buildRadarFrame(frame) {
  if (!frame) {
    return null;
  }
  const heat = LINK_DEFINITIONS.map((link) => {
    const source = DEFAULT_NODE_GEOMETRY[link.source];
    const target = DEFAULT_NODE_GEOMETRY[link.target];
    const point = midpoint(source, target);
    const event = frame.links[link.link_id];
    return {
      link_id: link.link_id,
      source: link.source,
      target: link.target,
      x: point.x,
      y: point.y,
      motion_intensity: clamp01(event?.motion_score ?? 0),
      confidence: clamp01(event?.quality ?? event?.confidence ?? 0),
      activity_heat: clamp01(event?.visual_strength ?? 0),
      health: clamp01(event?.health ?? 0),
      age_ms: event?.age_ms ?? null,
    };
  });

  return {
    frame_type: 'radar_frame',
    center: 'S3',
    timestamp: frame.timestamp,
    timestamp_ms: frame.timestamp_ms,
    tick_id: frame.tick_id,
    fused_state: frame.fusion,
    motion_intensity: frame.fusion.motion_score,
    confidence: frame.fusion.confidence,
    activity_heat: heat,
    max_intensity: heat.length ? Math.max(...heat.map((point) => point.motion_intensity)) : 0,
  };
}

function valueAt(frame, field, linkId) {
  if (field === 'health') {
    return frame.links[linkId]?.health ?? null;
  }
  if (field === 'quality') {
    return frame.links[linkId]?.quality ?? null;
  }
  return frame.links[linkId]?.[field] ?? null;
}

function buildSeries(frames) {
  const build = (field) => frames.map((frame) => {
    const row = {
      timestamp: frame.timestamp,
      timestamp_ms: frame.timestamp_ms,
      tick_id: frame.tick_id,
      fused: field === 'motion_score' ? frame.fusion.motion_score : null,
    };
    for (const linkId of LINK_IDS) {
      row[linkId] = valueAt(frame, field, linkId);
    }
    return row;
  });

  return {
    motion_score: build('motion_score'),
    energy: build('energy'),
    quality: build('quality'),
    link_health: build('health'),
  };
}

function buildConfidenceEnvelope(frames) {
  return frames.map((frame) => {
    const scores = LINK_IDS.map((linkId) => frame.links[linkId]?.motion_score)
      .filter((value) => Number.isFinite(value));
    const score = frame.fusion.motion_score ?? average(scores) ?? 0;
    const stability = clamp01(1 - stddev(scores));
    const confidence = clamp01(frame.fusion.confidence ?? stability);
    const spread = clamp01(((1 - stability) * 0.5) + ((1 - confidence) * 0.25));
    return {
      timestamp: frame.timestamp,
      timestamp_ms: frame.timestamp_ms,
      tick_id: frame.tick_id,
      score: clamp01(score),
      confidence,
      stability,
      lower: clamp01(score - spread),
      upper: clamp01(score + spread),
    };
  });
}

function buildMotionHeatSeries(frames, stateTimeline) {
  return frames.map((frame, index) => {
    const row = {
      timestamp: frame.timestamp,
      timestamp_ms: frame.timestamp_ms,
      tick_id: frame.tick_id,
      state: stateTimeline[index]?.state || frame.fusion.state || '',
      fused: frame.fusion.motion_score,
      confidence: frame.fusion.confidence,
    };
    for (const linkId of LINK_IDS) {
      row[linkId] = frame.links[linkId]?.motion_score ?? null;
    }
    return row;
  });
}

function buildFusionStatus(latestFrame, stateTimeline) {
  const latestState = stateTimeline[stateTimeline.length - 1];
  const topology = buildTopologyFrame(latestFrame);
  const healthyLinkCount = topology.edges.filter((edge) => edge.healthy).length;
  const motionIntensity = latestFrame?.fusion?.motion_score ?? 0;
  const confidence = latestFrame?.fusion?.confidence ?? 0;
  return {
    title: 'CSI Fusion Status',
    nodes: NODE_IDS,
    links_healthy: healthyLinkCount,
    links_total: LINK_IDS.length,
    current_state: latestState?.state || latestFrame?.fusion?.state || 'IDLE',
    confidence: clamp01(confidence),
    confidence_percent: Math.round(clamp01(confidence) * 100),
    motion_intensity: clamp01(motionIntensity),
    latest_timestamp: latestFrame?.timestamp ?? null,
    latest_tick_id: latestFrame?.tick_id ?? null,
  };
}

function buildTelemetryUiModel(samples, options = {}) {
  const generatedAtMs = Number(options.generatedAtMs ?? Date.now());
  const parsedSamples = samples.map((sample) => parseTelemetrySample(sample));
  const schemaCounts = {};
  const events = [];
  const fusionEvents = [];

  for (const parsed of parsedSamples) {
    schemaCounts[parsed.schema_path] = (schemaCounts[parsed.schema_path] || 0) + 1;
    events.push(...parsed.events);
    fusionEvents.push(...parsed.fusion_events);
  }

  const alignedFrames = alignTelemetryEvents(events, {
    ...options,
    fusionEvents,
    generatedAtMs,
    referenceObservedAtMs: options.referenceObservedAtMs ?? generatedAtMs,
  });
  const stateTimeline = buildStateTimeline(alignedFrames, options);
  const latestFrame = alignedFrames[alignedFrames.length - 1] || null;
  const topology = buildTopologyFrame(latestFrame);

  return {
    ok: true,
    generated_at_ms: generatedAtMs,
    parser: {
      sample_count: samples.length,
      event_count: events.length,
      fusion_event_count: fusionEvents.length,
      schema_counts: schemaCounts,
    },
    data_model: {
      fields: ['timestamp', 'link_id', 'source', 'target', 'motion_score', 'energy', 'variance', 'quality', 'rssi', 'state'],
      link_ids: LINK_IDS,
      nodes: NODE_IDS,
    },
    fusion_status: buildFusionStatus(latestFrame, stateTimeline),
    alignment: {
      window_ms: normalizeWindowMs(options.windowMs),
      link_ids: LINK_IDS,
    },
    topology,
    radar_frame: buildRadarFrame(latestFrame),
    series: buildSeries(alignedFrames),
    motion_heat_series: buildMotionHeatSeries(alignedFrames, stateTimeline),
    confidence_envelope: buildConfidenceEnvelope(alignedFrames),
    aligned_csi_timeline: alignedFrames,
    state_timeline: stateTimeline,
    telemetry_events: events,
    fusion_events: fusionEvents,
  };
}

module.exports = {
  DEFAULT_ALIGNMENT_WINDOW_MS,
  LINK_DEFINITIONS,
  LINK_IDS,
  NODE_IDS,
  annotateSampleWithTelemetry,
  alignTelemetryEvents,
  buildConfidenceEnvelope,
  buildMotionHeatSeries,
  buildRadarFrame,
  buildStateTimeline,
  buildTelemetryUiModel,
  detectSchema,
  normalizeLinkId,
  normalizeWindowMs,
  parseTelemetrySample,
};
