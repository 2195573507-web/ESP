'use strict';

const DEFAULT_MAX_HISTORY = 5000;
const DEFAULT_HISTORY_LIMIT = 1000;

function parsePositiveInteger(value, fallback) {
  const parsed = Number.parseInt(value || '', 10);
  return Number.isInteger(parsed) && parsed > 0 ? parsed : fallback;
}

function createHistoryLimits(env = process.env) {
  const maxHistory = parsePositiveInteger(env.CSI_MAX_HISTORY, DEFAULT_MAX_HISTORY);
  const requestedDefault = parsePositiveInteger(env.CSI_HISTORY_LIMIT, DEFAULT_HISTORY_LIMIT);
  return {
    maxHistory,
    defaultLimit: Math.min(maxHistory, requestedDefault),
  };
}

function clampHistoryLimit(value, limits = createHistoryLimits()) {
  const parsed = Number.parseInt(value || '', 10);
  const limit = Number.isInteger(parsed) ? parsed : limits.defaultLimit;
  return Math.max(1, Math.min(limits.maxHistory, limit));
}

module.exports = {
  DEFAULT_HISTORY_LIMIT,
  DEFAULT_MAX_HISTORY,
  clampHistoryLimit,
  createHistoryLimits,
};
