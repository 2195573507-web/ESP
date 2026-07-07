#!/usr/bin/env node
'use strict';

const fs = require('fs');
const http = require('http');
const { URL } = require('url');
const {
  annotateSampleWithTelemetry,
  buildTelemetryUiModel,
  DEFAULT_ALIGNMENT_WINDOW_MS,
  LINK_IDS,
  normalizeWindowMs,
} = require('./src/csiTelemetryEngine');
const { clampHistoryLimit, createHistoryLimits } = require('./src/historyLimits');
const { createToolResolver } = require('./src/toolResolver');

let SerialPort = null;
let serialportLoadError = null;
try {
  ({ SerialPort } = require('serialport'));
} catch (error) {
  serialportLoadError = error;
  console.warn('Missing optional dependency "serialport"; serial APIs are disabled.');
  console.warn('Run "npm install" to enable serial port mode.');
}

const HOST = '127.0.0.1';
const PORT = parseHttpPort(process.env.PORT, 8787);
const HISTORY_LIMITS = createHistoryLimits();
const MAX_HISTORY = HISTORY_LIMITS.maxHistory;
const DEFAULT_LIMIT = HISTORY_LIMITS.defaultLimit;
const DEFAULT_BAUD = parseBaud(process.env.CSI_BAUD, 115200);
const BOOT_SERIAL_PORT = String(process.env.CSI_SERIAL_PORT || '').trim();
const DEFAULT_DEVICE_ID = String(process.env.CSI_DEVICE_ID || 'c5').trim() || 'c5';
const toolResolver = createToolResolver();
const ROOT_DIR = __dirname;
const PUBLIC_DIR = toolResolver.joinToolPath(ROOT_DIR, 'public');

console.log(`tool platform detected: ${toolResolver.platform.displayName}`);

const globalHistory = [];
const deviceHistories = new Map();
const latestByDevice = new Map();

const serialState = {
  running: false,
  connected: false,
  port: BOOT_SERIAL_PORT,
  baud: DEFAULT_BAUD,
  baudRate: DEFAULT_BAUD,
  env_port_configured: Boolean(BOOT_SERIAL_PORT),
  serialport_available: Boolean(SerialPort),
  serialport_error: serialportLoadError ? serialportLoadError.message : '',
  lines_seen: 0,
  samples_seen: 0,
  last_line: '',
  last_error: serialportLoadError
    ? '未安装 serialport，串口功能不可用，但可使用粘贴日志/模拟样本'
    : '',
  lastError: serialportLoadError
    ? '未安装 serialport，串口功能不可用，但可使用粘贴日志/模拟样本'
    : '',
  opened_at_iso: '',
};

let activeSerialPort = null;
let serialLineBuffer = '';

function parseHttpPort(value, fallback) {
  const parsed = Number.parseInt(value || '', 10);
  return Number.isInteger(parsed) && parsed > 0 && parsed <= 65535 ? parsed : fallback;
}

function parseBaud(value, fallback) {
  const parsed = Number.parseInt(value || '', 10);
  return Number.isInteger(parsed) && parsed > 0 ? parsed : fallback;
}

function clampLimit(value, fallback = DEFAULT_LIMIT) {
  return clampHistoryLimit(value || fallback, HISTORY_LIMITS);
}

function trimToMax(list, max) {
  while (list.length > max) {
    list.shift();
  }
}

function setLastError(message) {
  serialState.last_error = message || '';
  serialState.lastError = message || '';
}

function clearHistory() {
  globalHistory.length = 0;
  deviceHistories.clear();
  latestByDevice.clear();
}

function normalizeDeviceId(sample) {
  if (sample && sample.device_id !== undefined && sample.device_id !== null && String(sample.device_id).trim()) {
    return String(sample.device_id).trim();
  }
  if (sample && sample.deviceId !== undefined && sample.deviceId !== null && String(sample.deviceId).trim()) {
    return String(sample.deviceId).trim();
  }
  return DEFAULT_DEVICE_ID;
}

function normalizeOccupancyState(value) {
  const state = String(value || '').trim().toLowerCase();
  if (['vacant', 'occupied', 'unknown'].includes(state)) {
    return state;
  }
  if (['motion', 'moving', 'active'].includes(state)) {
    return 'occupied';
  }
  if (['idle', 'static', 'empty'].includes(state)) {
    return 'vacant';
  }
  return '';
}

function toFiniteNumber(value) {
  const number = Number(value);
  return Number.isFinite(number) ? number : null;
}

function normalizeBoolean(value) {
  if (value === true || value === false) {
    return value;
  }
  const text = String(value || '').trim().toLowerCase();
  if (['true', '1', 'yes', 'occupied', 'motion'].includes(text)) {
    return true;
  }
  if (['false', '0', 'no', 'vacant', 'idle'].includes(text)) {
    return false;
  }
  return null;
}

function normalizeSampleFields(rawSample, sourceFormat, rawLine) {
  const sample = { ...rawSample };
  const metrics = sample.metrics && typeof sample.metrics === 'object' && !Array.isArray(sample.metrics)
    ? sample.metrics
    : {};
  const features = sample.features && typeof sample.features === 'object' && !Array.isArray(sample.features)
    ? sample.features
    : {};
  const state = normalizeOccupancyState(sample.state ?? sample.occupancy ?? sample.presence_state);
  const motionState = String(sample.motion_state ?? sample.motion ?? '').trim().toLowerCase();

  sample.device_id = normalizeDeviceId(sample);

  if (sample.room_id === undefined && sample.roomId !== undefined) {
    sample.room_id = sample.roomId;
  }
  if (state) {
    sample.state = state;
    sample.occupancy = state;
    if (sample.presence === undefined || sample.presence === null) {
      sample.presence = state === 'occupied' ? true : state === 'vacant' ? false : null;
    }
  } else if (sample.presence !== undefined && sample.presence !== null) {
    const presence = normalizeBoolean(sample.presence);
    if (presence !== null) {
      sample.presence = presence;
      sample.state = presence ? 'occupied' : 'vacant';
      sample.occupancy = sample.state;
    }
  }
  if (motionState) {
    sample.motion_state = motionState;
  } else if (sample.motion_score !== undefined) {
    const motionScore = toFiniteNumber(sample.motion_score);
    if (motionScore !== null) {
      sample.motion_state = motionScore >= 0.5 ? 'motion' : 'idle';
    }
  }

  if (sample.variance === undefined && sample.amplitude_variance !== undefined) {
    sample.variance = sample.amplitude_variance;
  }
  if (sample.amplitude_variance === undefined && sample.variance !== undefined) {
    sample.amplitude_variance = sample.variance;
  }
  if (sample.amplitude === undefined && sample.amplitude_avg !== undefined) {
    sample.amplitude = sample.amplitude_avg;
  }
  if (sample.noise_floor === undefined && sample.noiseFloor !== undefined) {
    sample.noise_floor = sample.noiseFloor;
  }
  if (sample.packet_count === undefined && sample.sample_count !== undefined) {
    sample.packet_count = sample.sample_count;
  }
  if (sample.sample_count === undefined && sample.packet_count !== undefined) {
    sample.sample_count = sample.packet_count;
  }
  if (sample.presence_score === undefined && sample.motion_score !== undefined) {
    sample.presence_score = sample.motion_score;
  }
  if (sample.motion_score === undefined && sample.presence_score !== undefined) {
    sample.motion_score = sample.presence_score;
  }
  if (sample.motion_score === undefined && features.motion_score_local !== undefined) {
    sample.motion_score = features.motion_score_local;
  }
  if (sample.frame_energy === undefined && metrics.frame_energy !== undefined) {
    sample.frame_energy = metrics.frame_energy;
  }
  if (sample.variance === undefined && metrics.variance !== undefined) {
    sample.variance = metrics.variance;
  }
  if (sample.rssi === undefined && metrics.rssi !== undefined) {
    sample.rssi = metrics.rssi;
  }
  if (sample.quality === undefined && features.quality !== undefined) {
    sample.quality = features.quality;
  }
  if (sample.updated_at_ms === undefined && sample.esp_time_ms !== undefined) {
    sample.updated_at_ms = sample.esp_time_ms;
  }
  if (sourceFormat) {
    sample.source_format = sourceFormat;
  }
  if (rawLine) {
    sample.raw_line = rawLine;
  }

  return sample;
}

function recordSample(rawSample, sourceFormat, rawLine) {
  const now = Date.now();
  const normalized = normalizeSampleFields(rawSample, sourceFormat, rawLine);
  const sample = annotateSampleWithTelemetry({
    ...normalized,
    device_id: normalizeDeviceId(normalized),
    server_time_ms: now,
    received_at_iso: new Date(now).toISOString(),
  });

  globalHistory.push(sample);
  trimToMax(globalHistory, MAX_HISTORY);

  if (!deviceHistories.has(sample.device_id)) {
    deviceHistories.set(sample.device_id, []);
  }
  const deviceHistory = deviceHistories.get(sample.device_id);
  deviceHistory.push(sample);
  trimToMax(deviceHistory, MAX_HISTORY);

  latestByDevice.set(sample.device_id, sample);
  return sample;
}

function indexOfIgnoreCase(value, search) {
  return String(value).toLowerCase().indexOf(String(search).toLowerCase());
}

function extractJsonObjectText(payload) {
  const start = payload.indexOf('{');
  const end = payload.lastIndexOf('}');
  if (start === -1 || end < start) {
    return '';
  }
  return payload.slice(start, end + 1);
}

function parseKeyValuePayload(payload) {
  const sample = {};
  const normalized = String(payload || '').replace(/[;,]/g, ' ');
  const regex = /([A-Za-z_][A-Za-z0-9_-]*)\s*[:=]\s*("[^"]*"|'[^']*'|[^\s]+)/g;
  let match;
  while ((match = regex.exec(normalized)) !== null) {
    const key = match[1].replace(/-/g, '_');
    let value = match[2].trim();
    if ((value.startsWith('"') && value.endsWith('"')) || (value.startsWith("'") && value.endsWith("'"))) {
      value = value.slice(1, -1);
    } else if (/^-?\d+(\.\d+)?$/.test(value)) {
      value = Number(value);
    } else {
      const booleanValue = normalizeBoolean(value);
      if (booleanValue !== null) {
        value = booleanValue;
      }
    }
    sample[key] = value;
  }
  return Object.keys(sample).length ? sample : null;
}

function findMarkerPayload(line) {
  const markers = [
    { marker: 'CSI_FUSION_TELEMETRY', sourceFormat: 'CSI_FUSION_TELEMETRY' },
    { marker: 'CSI_C5_FEATURE', sourceFormat: 'CSI_C5_FEATURE' },
    { marker: 'CSI_CANONICAL_EVENT', sourceFormat: 'CSI_CANONICAL_EVENT' },
    { marker: 'csi_service: csi summary', sourceFormat: 'csi summary' },
    { marker: 'csi feature', sourceFormat: 'csi feature' },
    { marker: 'csi summary', sourceFormat: 'csi summary' },
    { marker: 'CSI_SAMPLE', sourceFormat: 'CSI_SAMPLE' },
    { marker: 'CSI:', sourceFormat: 'CSI' },
    { marker: 'csi_placeholder_gateway:', sourceFormat: 'csi_placeholder_gateway' },
  ];

  for (const entry of markers) {
    const index = indexOfIgnoreCase(line, entry.marker);
    if (index !== -1) {
      return {
        sourceFormat: entry.sourceFormat,
        payloadText: String(line).slice(index + entry.marker.length).trim(),
      };
    }
  }

  const jsonText = extractJsonObjectText(String(line));
  if (jsonText && /csi/i.test(line)) {
    return { sourceFormat: 'csi json', payloadText: jsonText };
  }
  return null;
}

function parseCsiLineDetailed(rawLine) {
  const line = String(rawLine || '').trim();
  serialState.last_line = line;

  if (!line) {
    return { ok: false, error: 'line is empty', raw_line: rawLine || '' };
  }

  const payload = findMarkerPayload(line);
  if (!payload) {
    return { ok: false, error: 'line did not contain a supported CSI marker', raw_line: line };
  }

  const jsonText = extractJsonObjectText(payload.payloadText);
  if (jsonText) {
    try {
      const parsed = JSON.parse(jsonText);
      if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed)) {
        return { ok: false, error: 'JSON payload must be an object', raw_line: line };
      }
      const sample = recordSample(parsed, payload.sourceFormat, line);
      serialState.samples_seen += 1;
      setLastError('');
      return { ok: true, sample, raw_line: line };
    } catch (error) {
      const message = `parse error (${payload.sourceFormat}): ${error.message}`;
      setLastError(message);
      return { ok: false, error: message, raw_line: line };
    }
  }

  const keyValueSample = parseKeyValuePayload(payload.payloadText);
  if (keyValueSample) {
    const sample = recordSample(keyValueSample, payload.sourceFormat, line);
    serialState.samples_seen += 1;
    setLastError('');
    return { ok: true, sample, raw_line: line };
  }

  const message = `parse error (${payload.sourceFormat}): line did not contain JSON or key=value fields`;
  setLastError(message);
  return { ok: false, error: message, raw_line: line };
}

function parseCsiLine(line) {
  const result = parseCsiLineDetailed(line);
  return result.ok ? result.sample : null;
}

function consumeSerialChunk(chunk, currentBuffer) {
  let buffer = currentBuffer + chunk.toString('utf8');
  let newlineIndex = buffer.search(/\r?\n/);

  while (newlineIndex !== -1) {
    const line = buffer.slice(0, newlineIndex).replace(/\r$/, '');
    buffer = buffer.slice(newlineIndex + (buffer[newlineIndex] === '\r' && buffer[newlineIndex + 1] === '\n' ? 2 : 1));
    serialState.lines_seen += 1;
    parseCsiLine(line);
    newlineIndex = buffer.search(/\r?\n/);
  }

  return buffer;
}

function flushSerialBuffer() {
  const line = serialLineBuffer.trim();
  serialLineBuffer = '';
  if (line) {
    serialState.lines_seen += 1;
    parseCsiLine(line);
  }
}

function getSerialStatus() {
  return {
    running: serialState.running,
    connected: serialState.connected,
    port: serialState.port,
    baud: serialState.baud,
    baudRate: serialState.baudRate,
    env_port_configured: serialState.env_port_configured,
    serialport_available: serialState.serialport_available,
    serialport_error: serialState.serialport_error,
    lines_seen: serialState.lines_seen,
    samples_seen: serialState.samples_seen,
    last_line: serialState.last_line,
    last_error: serialState.last_error,
    lastError: serialState.lastError,
    opened_at_iso: serialState.opened_at_iso,
  };
}

function normalizeSerialPath(portPath) {
  return toolResolver.normalizeSerialPortPath(portPath);
}

function serialUnavailableResult() {
  const error = '未安装 serialport，串口功能不可用，但可使用粘贴日志/模拟样本';
  setLastError(error);
  return { ok: false, statusCode: 503, error, status: getSerialStatus() };
}

async function listAvailableSerialPorts() {
  try {
    return await toolResolver.listSerialPorts({
      SerialPort,
      unavailableReason: serialState.last_error,
    });
  } catch (error) {
    setLastError(`failed to list serial ports: ${error.message}`);
    return [];
  }
}

function closeSerialPort(port) {
  return new Promise((resolve) => {
    if (!port || !port.isOpen) {
      resolve();
      return;
    }

    port.close((error) => {
      if (error && !String(error.message || '').includes('Port is not open')) {
        setLastError(`failed to close serial port: ${error.message}`);
      }
      resolve();
    });
  });
}

async function disconnectSerialPort() {
  if (!activeSerialPort) {
    serialState.running = false;
    serialState.connected = false;
    return false;
  }

  const port = activeSerialPort;
  activeSerialPort = null;
  flushSerialBuffer();
  serialState.running = false;
  serialState.connected = false;
  serialState.opened_at_iso = '';
  await closeSerialPort(port);
  return true;
}

async function connectSerialPort(portPath, baudRate) {
  if (!SerialPort) {
    return serialUnavailableResult();
  }

  const portCheck = normalizeSerialPath(portPath);
  if (!portCheck.ok) {
    return portCheck;
  }

  const parsedBaud = parseBaud(baudRate, NaN);
  if (!Number.isInteger(parsedBaud) || parsedBaud <= 0) {
    return { ok: false, statusCode: 400, error: 'baudRate must be a positive integer' };
  }

  await disconnectSerialPort();

  serialState.running = false;
  serialState.connected = false;
  serialState.port = portCheck.port;
  serialState.baud = parsedBaud;
  serialState.baudRate = parsedBaud;
  serialState.lines_seen = 0;
  serialState.samples_seen = 0;
  serialState.last_line = '';
  setLastError('');
  serialState.opened_at_iso = '';
  serialLineBuffer = '';

  const port = new SerialPort({
    path: portCheck.port,
    baudRate: parsedBaud,
    autoOpen: false,
  });

  activeSerialPort = port;

  port.on('data', (chunk) => {
    if (activeSerialPort !== port) {
      return;
    }
    serialLineBuffer = consumeSerialChunk(chunk, serialLineBuffer);
  });

  port.on('error', (error) => {
    if (activeSerialPort !== port) {
      return;
    }
    setLastError(`serial port error: ${error.message}`);
  });

  port.on('close', () => {
    if (activeSerialPort !== port) {
      return;
    }
    flushSerialBuffer();
    activeSerialPort = null;
    serialState.running = false;
    serialState.connected = false;
    serialState.opened_at_iso = '';
  });

  try {
    await new Promise((resolve, reject) => {
      port.open((error) => {
        if (error) {
          reject(error);
          return;
        }
        resolve();
      });
    });
  } catch (error) {
    if (activeSerialPort === port) {
      activeSerialPort = null;
    }
    serialState.running = false;
    serialState.connected = false;
    setLastError(`failed to open ${portCheck.port}: ${error.message}`);
    await closeSerialPort(port);
    return { ok: false, statusCode: 503, error: serialState.last_error };
  }

  serialState.running = true;
  serialState.connected = true;
  serialState.opened_at_iso = new Date().toISOString();
  return { ok: true, status: getSerialStatus() };
}

function sendJson(res, statusCode, value) {
  const payload = JSON.stringify(value, null, 2);
  res.writeHead(statusCode, {
    'Content-Type': 'application/json; charset=utf-8',
    'Content-Length': Buffer.byteLength(payload),
    'Cache-Control': 'no-store',
  });
  res.end(payload);
}

function sendDownload(res, statusCode, contentType, filename, body) {
  res.writeHead(statusCode, {
    'Content-Type': contentType,
    'Content-Disposition': `attachment; filename="${filename}"`,
    'Content-Length': Buffer.byteLength(body),
    'Cache-Control': 'no-store',
  });
  res.end(body);
}

function sendText(res, statusCode, text) {
  res.writeHead(statusCode, {
    'Content-Type': 'text/plain; charset=utf-8',
    'Content-Length': Buffer.byteLength(text),
    'Cache-Control': 'no-store',
  });
  res.end(text);
}

function readRequestBody(req) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    let total = 0;

    req.on('data', (chunk) => {
      total += chunk.length;
      if (total > 1024 * 1024) {
        reject(new Error('request body too large'));
        req.destroy();
        return;
      }
      chunks.push(chunk);
    });

    req.on('end', () => {
      const text = Buffer.concat(chunks).toString('utf8').trim();
      if (!text) {
        resolve({});
        return;
      }
      try {
        resolve(JSON.parse(text));
      } catch (error) {
        reject(new Error(`invalid JSON body: ${error.message}`));
      }
    });

    req.on('error', reject);
  });
}

function getHistory(deviceId, limit) {
  const safeLimit = clampLimit(limit);
  const source = deviceId ? deviceHistories.get(deviceId) || [] : globalHistory;
  return source.slice(-safeLimit);
}

function getTelemetryPayload(url) {
  const deviceId = url.searchParams.get('device_id') || '';
  const limit = url.searchParams.get('limit') || String(DEFAULT_LIMIT);
  const windowMs = normalizeWindowMs(url.searchParams.get('window_ms'));
  const minDwellMs = toFiniteNumber(url.searchParams.get('min_dwell_ms'));
  const history = getHistory(deviceId, limit);

  return buildTelemetryUiModel(history, {
    windowMs,
    minDwellMs: minDwellMs !== null ? minDwellMs : undefined,
  });
}

function latestTelemetryModel(limit = DEFAULT_LIMIT) {
  return buildTelemetryUiModel(getHistory('', limit), {
    windowMs: DEFAULT_ALIGNMENT_WINDOW_MS,
  });
}

function getLatestPayload() {
  const latestEntries = Array.from(latestByDevice.entries()).sort(([a], [b]) => a.localeCompare(b));
  const latest = Object.fromEntries(latestEntries);
  const latestSample = globalHistory[globalHistory.length - 1] || null;
  const telemetry = latestTelemetryModel(Math.min(DEFAULT_LIMIT, 1000));
  return {
    ok: true,
    count: globalHistory.length,
    devices: latestEntries.map(([device]) => device),
    links: LINK_IDS,
    latest,
    latest_sample: latestSample,
    sample: latestSample,
    fusion_status: telemetry.fusion_status,
    telemetry,
    status: latestSample
      ? {
        device_id: latestSample.device_id || '',
        room_id: latestSample.room_id || '',
        occupancy: latestSample.occupancy || latestSample.state || '',
        motion_state: latestSample.motion_state || '',
        motion_score: latestSample.motion_score ?? latestSample.presence_score ?? null,
        rssi: latestSample.rssi ?? null,
        noise_floor: latestSample.noise_floor ?? null,
        packet_count: latestSample.packet_count ?? latestSample.sample_count ?? null,
        received_at_iso: latestSample.received_at_iso || '',
      }
      : null,
  };
}

function createMockSample() {
  const now = Date.now();
  const tickId = Math.round(now / 100);
  const phase = (globalHistory.length % 36) / 36;
  const wave = (offset) => Math.max(0, Math.min(1, 0.45 + Math.sin((phase + offset) * Math.PI * 2) * 0.34 + Math.random() * 0.06));
  const linkStates = {};
  const linkOffsets = {
    S3_TO_C51: 0,
    S3_TO_C52: 0.14,
    C51_TO_C52: 0.31,
    C52_TO_C51: 0.48,
  };

  for (const linkId of LINK_IDS) {
    const motionScore = Number(wave(linkOffsets[linkId] || 0).toFixed(3));
    const energy = Number((8 + motionScore * 24 + Math.random() * 3).toFixed(3));
    const variance = Number((0.03 + motionScore * 0.28 + Math.random() * 0.02).toFixed(5));
    linkStates[linkId] = {
      link_id: linkId,
      motion_score: motionScore,
      frame_energy: energy,
      variance,
      quality: Number((0.68 + Math.random() * 0.27).toFixed(3)),
      rssi: Math.round(-36 - Math.random() * 18),
      state: motionScore >= 0.62 ? 'MOTION' : motionScore >= 0.34 ? 'HOLD' : 'IDLE',
      frame_seq: globalHistory.length + 1,
      timestamp_ms: now,
    };
  }

  const scores = LINK_IDS.map((linkId) => linkStates[linkId].motion_score);
  const motionScore = scores.reduce((sum, value) => sum + value, 0) / scores.length;
  const fusedState = motionScore >= 0.62 ? 'MOTION' : motionScore >= 0.34 ? 'HOLD' : 'IDLE';
  return {
    type: 'csi_fusion',
    schema_version: 'mock-csi-fusion-v1',
    device_id: 'S3',
    gateway_id: 'S3',
    tick_id: tickId,
    timestamp_ms: now,
    link_states: linkStates,
    fused_state: {
      state: fusedState,
      motion_score: Number(motionScore.toFixed(3)),
      confidence: Number((0.72 + Math.random() * 0.2).toFixed(3)),
    },
  };
}

function jsonToCsv(rows) {
  const telemetry = buildTelemetryUiModel(rows);
  const telemetryRows = telemetry.telemetry_events || [];
  const preferred = [
    'timestamp',
    'link_id',
    'source',
    'target',
    'motion_score',
    'energy',
    'variance',
    'quality',
    'rssi',
    'state',
    'confidence',
    'tick_id',
    'frame_seq',
    'source_format',
    'schema_path',
    'device_id',
    'trace_id',
  ];
  const escapeCsv = (value) => {
    if (value === undefined || value === null) {
      return '';
    }
    const text = typeof value === 'object' ? JSON.stringify(value) : String(value);
    return /[",\n\r]/.test(text) ? `"${text.replace(/"/g, '""')}"` : text;
  };
  if (telemetryRows.length) {
    return `${preferred.join(',')}\n${telemetryRows.map((row) => preferred.map((header) => escapeCsv(row[header])).join(',')).join('\n')}\n`;
  }
  if (!rows.length) {
    return `${preferred.join(',')}\n`;
  }
  const fallbackPreferred = [
    'received_at_iso',
    'device_id',
    'room_id',
    'state',
    'occupancy',
    'motion_state',
    'motion_score',
    'presence_score',
    'rssi',
    'noise_floor',
    'packet_count',
    'sample_count',
    'variance',
    'amplitude_variance',
    'amplitude',
    'cv',
    'updated_at_ms',
    'source_format',
  ];
  const seen = new Set(fallbackPreferred);
  rows.forEach((row) => {
    Object.keys(row).forEach((key) => {
      if (!seen.has(key) && key !== 'raw_line') {
        seen.add(key);
      }
    });
  });
  const headers = Array.from(seen);
  return `${headers.join(',')}\n${rows.map((row) => headers.map((header) => escapeCsv(row[header])).join(',')).join('\n')}\n`;
}

function serveIndex(req, res) {
  const indexPath = toolResolver.joinToolPath(PUBLIC_DIR, 'index.html');
  fs.readFile(indexPath, (error, body) => {
    if (error) {
      sendText(res, 500, `failed to read index.html: ${error.message}`);
      return;
    }
    res.writeHead(200, {
      'Content-Type': 'text/html; charset=utf-8',
      'Content-Length': body.length,
      'Cache-Control': 'no-store',
    });
    if (req.method === 'HEAD') {
      res.end();
      return;
    }
    res.end(body);
  });
}

async function handleConnectRequest(req, res) {
  try {
    const body = await readRequestBody(req);
    const baudRate = body.baudRate ?? body.baud ?? DEFAULT_BAUD;
    const result = await connectSerialPort(body.port, baudRate);
    if (!result.ok) {
      sendJson(res, result.statusCode || 500, { ok: false, error: result.error, status: getSerialStatus() });
      return;
    }
    sendJson(res, 200, { ok: true, status: result.status });
  } catch (error) {
    sendJson(res, 400, { ok: false, error: error.message, status: getSerialStatus() });
  }
}

async function handleDisconnectRequest(res) {
  const stopped = await disconnectSerialPort();
  sendJson(res, 200, { ok: true, stopped, status: getSerialStatus() });
}

async function handleApi(req, res, url) {
  if (req.method === 'GET' && url.pathname === '/api/serial/ports') {
    const ports = await listAvailableSerialPorts();
    sendJson(res, 200, {
      ok: true,
      serialport_available: Boolean(SerialPort),
      serialport_error: serialState.serialport_error,
      ports,
      status: getSerialStatus(),
    });
    return;
  }

  if (req.method === 'GET' && url.pathname === '/api/serial/status') {
    sendJson(res, 200, getSerialStatus());
    return;
  }

  if (req.method === 'POST' && url.pathname === '/api/serial/connect') {
    await handleConnectRequest(req, res);
    return;
  }

  if (req.method === 'POST' && url.pathname === '/api/serial/disconnect') {
    await handleDisconnectRequest(res);
    return;
  }

  if (req.method === 'POST' && url.pathname === '/api/csi/sample') {
    try {
      const body = await readRequestBody(req);
      if (!body || typeof body !== 'object' || Array.isArray(body)) {
        sendJson(res, 400, { ok: false, error: 'sample body must be a JSON object' });
        return;
      }
      const sample = recordSample(body, 'manual');
      sendJson(res, 200, { ok: true, sample, latest: getLatestPayload() });
    } catch (error) {
      sendJson(res, 400, { ok: false, error: error.message });
    }
    return;
  }

  if (req.method === 'POST' && url.pathname === '/api/csi/mock') {
    const sample = recordSample(createMockSample(), 'mock');
    sendJson(res, 200, { ok: true, sample, latest: getLatestPayload() });
    return;
  }

  if (req.method === 'POST' && url.pathname === '/api/csi/log-line') {
    try {
      const body = await readRequestBody(req);
      const line = typeof body.line === 'string' ? body.line : '';
      if (!line.trim()) {
        sendJson(res, 400, { ok: false, error: 'line must be a non-empty string', raw_line: line, status: getSerialStatus() });
        return;
      }
      serialState.lines_seen += 1;
      const result = parseCsiLineDetailed(line);
      sendJson(res, result.ok ? 200 : 400, { ...result, status: getSerialStatus() });
    } catch (error) {
      sendJson(res, 400, { ok: false, error: error.message, status: getSerialStatus() });
    }
    return;
  }

  if (req.method === 'GET' && url.pathname === '/api/csi/latest') {
    sendJson(res, 200, getLatestPayload());
    return;
  }

  if (req.method === 'GET' && url.pathname === '/api/csi/history') {
    const deviceId = url.searchParams.get('device_id') || '';
    const limit = url.searchParams.get('limit') || String(DEFAULT_LIMIT);
    sendJson(res, 200, {
      ok: true,
      device_id: deviceId || null,
      limit: clampLimit(limit),
      count: globalHistory.length,
      history: getHistory(deviceId, limit),
    });
    return;
  }

  if (req.method === 'GET' && url.pathname === '/api/csi/telemetry') {
    sendJson(res, 200, getTelemetryPayload(url));
    return;
  }

  if (req.method === 'DELETE' && url.pathname === '/api/csi/history') {
    clearHistory();
    sendJson(res, 200, { ok: true, cleared: true, count: 0 });
    return;
  }

  if (req.method === 'GET' && url.pathname === '/api/csi/export.json') {
    sendDownload(
      res,
      200,
      'application/json; charset=utf-8',
      'csi-history.json',
      JSON.stringify({ exported_at_iso: new Date().toISOString(), count: globalHistory.length, history: globalHistory }, null, 2),
    );
    return;
  }

  if (req.method === 'GET' && url.pathname === '/api/csi/export.csv') {
    sendDownload(
      res,
      200,
      'text/csv; charset=utf-8',
      'csi-history.csv',
      jsonToCsv(globalHistory),
    );
    return;
  }

  sendJson(res, 404, { ok: false, error: 'not found' });
}

const server = http.createServer((req, res) => {
  let url;
  try {
    url = new URL(req.url, `http://${HOST}:${PORT}`);
  } catch (error) {
    sendJson(res, 400, { ok: false, error: `invalid URL: ${error.message}` });
    return;
  }

  if ((req.method === 'GET' || req.method === 'HEAD') && (url.pathname === '/' || url.pathname === '/index.html')) {
    serveIndex(req, res);
    return;
  }

  if (url.pathname.startsWith('/api/')) {
    handleApi(req, res, url).catch((error) => {
      sendJson(res, 500, { ok: false, error: error.message });
    });
    return;
  }

  sendText(res, 404, 'not found');
});

async function shutdown() {
  await disconnectSerialPort();
  server.close(() => process.exit(0));
}

process.on('SIGINT', () => {
  shutdown().catch(() => process.exit(1));
});

process.on('SIGTERM', () => {
  shutdown().catch(() => process.exit(1));
});

server.listen(PORT, HOST, () => {
  console.log(`CSI debug web listening at http://${HOST}:${PORT}`);
  if (!BOOT_SERIAL_PORT) {
    console.log('CSI_SERIAL_PORT is not set; serial is not connected.');
    return;
  }

  connectSerialPort(BOOT_SERIAL_PORT, DEFAULT_BAUD).then((result) => {
    if (result.ok) {
      console.log(`CSI serial connected: ${BOOT_SERIAL_PORT} @ ${DEFAULT_BAUD}`);
      return;
    }
    console.error(result.error);
  });
});
