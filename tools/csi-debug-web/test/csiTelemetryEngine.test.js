'use strict';

const assert = require('assert/strict');
const test = require('node:test');
const {
  alignTelemetryEvents,
  buildStateTimeline,
  buildTelemetryUiModel,
  normalizeWindowMs,
  parseTelemetrySample,
} = require('../src/csiTelemetryEngine');

test('parseTelemetrySample detects S3 csi_fusion telemetry and emits link plus fused events', () => {
  const parsed = parseTelemetrySample({
    type: 'csi_fusion',
    tick_id: 42,
    timestamp_ms: 4200,
    link_states: {
      C51: { motion_score: 0.7, variance: 0.12, rssi: -41, quality: 0.8 },
      C52: { motion_score: 0.3, variance: 0.08, rssi: -48, quality: 0.6 },
    },
    fused_state: { motion_score: 0.58, state: 'HOLD', confidence: 0.71 },
  });

  assert.equal(parsed.schema_path, 's3_csi_fusion');
  assert.equal(parsed.schema_version, 's3-csi-fusion-v1');
  assert.equal(parsed.events.length, 2);
  assert.equal(parsed.fusion_events.length, 1);
  assert.deepEqual(parsed.events.map((event) => event.link_id).sort(), ['S3_TO_C51', 'S3_TO_C52']);
  assert.equal(parsed.fusion_events[0].state, 'HOLD');
});

test('parseTelemetrySample accepts compact S3 fusion telemetry lines from firmware', () => {
  const parsed = parseTelemetrySample({
    type: 'csi_fusion',
    schema_version: 2,
    trace_id: 'csi-v2-42',
    tick_id: 42,
    links: ['C51', 'C52'],
    fused_state: {
      state: 'MOTION',
      confidence: 0.73,
      motion_score: 0.73,
    },
    confidence: 0.73,
    motion_score: 0.73,
    timestamp_ms: 4200,
  });

  assert.equal(parsed.schema_path, 's3_csi_fusion');
  assert.equal(parsed.schema_version, 2);
  assert.equal(parsed.events.length, 2);
  assert.equal(parsed.fusion_events.length, 1);
  assert.deepEqual(parsed.events.map((event) => event.link_id).sort(), ['S3_TO_C51', 'S3_TO_C52']);
  assert.equal(parsed.events[0].source_format, '');
  assert.equal(parsed.fusion_events[0].state, 'MOTION');
  assert.equal(parsed.fusion_events[0].motion_score, 0.73);
});

test('parseTelemetrySample remains compatible with canonical CSI facts as area-level link telemetry', () => {
  const parsed = parseTelemetrySample({
    schema_version: 1,
    payload_type: 'csi.motion',
    device_id: 'gateway-1',
    timestamp_ms: 123456,
    payload: {
      link_id: 'fused',
      state: 'MOTION',
      frame_energy: 22.5,
      variance: 0.32,
      rssi: -38,
      motion_score: 0.81,
      confidence: 0.77,
    },
  });

  assert.equal(parsed.schema_path, 'canonical_csi_fact');
  assert.equal(parsed.events.length, 4);
  assert.equal(parsed.fusion_events.length, 1);
  assert.deepEqual(parsed.events.map((event) => event.link_id).sort(), ['C51_TO_C52', 'C52_TO_C51', 'S3_TO_C51', 'S3_TO_C52']);
  assert.equal(parsed.events[0].energy, 22.5);
  assert.equal(parsed.events[0].motion_score, 0.81);
  assert.equal(parsed.fusion_events[0].state, 'MOTION');
});

test('parseTelemetrySample maps older summary facts onto synthetic four-link telemetry', () => {
  const parsed = parseTelemetrySample({
    schema: 1,
    device_id: 'c5',
    state: 'occupied',
    motion_score: 0.72,
    variance: 0.44,
    rssi: -35,
    updated_at_ms: 123456,
  });

  assert.equal(parsed.schema_path, 'legacy_csi_fact');
  assert.equal(parsed.events.length, 4);
  assert.equal(parsed.events[0].synthetic, true);
  assert.equal(parsed.events[0].state, 'MOTION');
});

test('parseTelemetrySample normalizes C5 feature frames', () => {
  const parsed = parseTelemetrySample({
    protocol_version: 1,
    device_id: 'C51',
    link_id: 'S3_TO_C51',
    frame_seq: 7,
    timestamp_ms: 9000,
    metrics: { frame_energy: 18.2, variance: 0.15, rssi: -44 },
    features: { motion_score_local: 0.66, quality: 0.93 },
  });

  assert.equal(parsed.schema_path, 'c5_feature');
  assert.equal(parsed.events.length, 1);
  assert.equal(parsed.events[0].link_id, 'S3_TO_C51');
  assert.equal(parsed.events[0].source, 'S3');
  assert.equal(parsed.events[0].target, 'C51');
  assert.equal(parsed.events[0].quality, 0.93);
});

test('parseTelemetrySample normalizes C5 feature marker payloads', () => {
  const parsed = parseTelemetrySample({
    schema_version: 'c5-feature-v1',
    device_id: 'C51',
    link_id: 'S3_TO_C51',
    trace_id: 'feature-1',
    timestamp_ms: 12000,
    metrics: {
      frame_energy: 18.2,
      variance: 0.15,
      cv: 0.09,
      rssi: -44,
      quality: 0.93,
    },
    state_hint: 'MOTION',
    source: 'csi_phase_a',
  }, { sourceFormat: 'CSI_C5_FEATURE' });

  assert.equal(parsed.schema_path, 'c5_feature');
  assert.equal(parsed.events.length, 1);
  assert.equal(parsed.events[0].link_id, 'S3_TO_C51');
  assert.equal(parsed.events[0].energy, 18.2);
  assert.equal(parsed.events[0].variance, 0.15);
  assert.equal(parsed.events[0].motion_score, 0.72);
  assert.equal(parsed.events[0].rssi, -44);
  assert.equal(parsed.events[0].quality, 0.93);
  assert.equal(parsed.events[0].source_format, 'CSI_C5_FEATURE');
  assert.equal(parsed.events[0].state, 'MOTION');
});

test('alignTelemetryEvents aligns four-link telemetry by tick_id before timestamp window', () => {
  const fusionParsed = parseTelemetrySample({
    type: 'csi_fusion',
    tick_id: 10,
    timestamp_ms: 1000,
    link_states: {
      C51: { motion_score: 0.6, variance: 0.1, quality: 0.8 },
      C52: { motion_score: 0.4, variance: 0.2, quality: 0.7 },
    },
    fused_state: { motion_score: 0.5, state: 'HOLD', confidence: 0.75 },
  });
  const c5Parsed = parseTelemetrySample({
    device_id: 'C52',
    link_id: 'S3_TO_C52',
    timestamp_ms: 1160,
    metrics: { frame_energy: 12, variance: 0.21, rssi: -47 },
    features: { motion_score_local: 0.46, quality: 0.82 },
  });
  const events = [
    ...fusionParsed.events,
    ...c5Parsed.events,
  ];

  const frames = alignTelemetryEvents(events, { windowMs: 250, fusionEvents: fusionParsed.fusion_events });

  assert.equal(frames.length, 2);
  assert.equal(frames[0].alignment_basis, 'tick_id');
  assert.equal(frames[0].tick_id, 10);
  assert.equal(frames[0].links.S3_TO_C51.motion_score, 0.6);
  assert.equal(frames[0].links.S3_TO_C52.motion_score, 0.4);
  assert.equal(frames[0].fusion.state, 'HOLD');
  assert.equal(frames[1].links.S3_TO_C52.motion_score, 0.46);
});

test('buildStateTimeline applies dwell-time hysteresis to avoid rapid UI state flips', () => {
  const frames = [0, 200, 400, 600, 800, 1000, 1200, 1400].map((timestamp, index) => ({
    frame_type: 'aligned_frame',
    timestamp_ms: timestamp,
    tick_id: index,
    window_ms: 200,
    links: {
      S3_TO_C51: { motion_score: index >= 1 && index <= 2 ? 0.9 : 0.1, energy: 1, variance: 0.01, quality: 0.8 },
      S3_TO_C52: { motion_score: index >= 1 && index <= 2 ? 0.85 : 0.1, energy: 1, variance: 0.01, quality: 0.8 },
      C51_TO_C52: null,
      C52_TO_C51: null,
    },
    fusion: { motion_score: index >= 1 && index <= 2 ? 0.88 : 0.1, confidence: 0.8, state: '' },
  }));

  const timeline = buildStateTimeline(frames, { minDwellMs: 600 });

  assert.equal(timeline[0].state, 'IDLE');
  assert.equal(timeline[2].state, 'IDLE');
  assert.equal(timeline.at(-1).state, 'IDLE');
});

test('buildTelemetryUiModel outputs UI-facing radar and timeline structures', () => {
  const model = buildTelemetryUiModel([
    {
      type: 'csi_fusion',
      tick_id: 100,
      timestamp_ms: 10000,
      link_states: {
        C51: { motion_score: 0.72, frame_energy: 20, variance: 0.13, rssi: -40, quality: 0.91 },
        C52: { motion_score: 0.48, frame_energy: 14, variance: 0.17, rssi: -46, quality: 0.74 },
      },
      fused_state: { motion_score: 0.63, state: 'MOTION', confidence: 0.82 },
    },
  ], { generatedAtMs: 20000 });

  assert.equal(model.ok, true);
  assert.equal(model.radar_frame.frame_type, 'radar_frame');
  assert.equal(model.radar_frame.activity_heat.length, 4);
  assert.equal(model.fusion_status.links_total, 4);
  assert.equal(model.topology.edges.length, 4);
  assert.equal(model.motion_heat_series.length, 1);
  assert.equal(model.confidence_envelope.length, 1);
  assert.equal(model.aligned_csi_timeline.length, 1);
  assert.equal(model.aligned_csi_timeline[0].frame_type, 'aligned_frame');
});

test('normalizeWindowMs clamps to the supported 200-500ms sliding window range', () => {
  assert.equal(normalizeWindowMs(100), 200);
  assert.equal(normalizeWindowMs(350), 350);
  assert.equal(normalizeWindowMs(800), 500);
  assert.equal(normalizeWindowMs('not-a-number'), 400);
});
