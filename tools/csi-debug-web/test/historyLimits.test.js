'use strict';

const assert = require('assert/strict');
const test = require('node:test');
const {
  DEFAULT_HISTORY_LIMIT,
  DEFAULT_MAX_HISTORY,
  clampHistoryLimit,
  createHistoryLimits,
} = require('../src/historyLimits');

test('history limits default to larger visible sample counts', () => {
  const limits = createHistoryLimits({});

  assert.equal(limits.maxHistory, DEFAULT_MAX_HISTORY);
  assert.equal(limits.defaultLimit, DEFAULT_HISTORY_LIMIT);
  assert.equal(limits.maxHistory, 5000);
  assert.equal(limits.defaultLimit, 1000);
});

test('history limit clamps to configured maximum', () => {
  const limits = createHistoryLimits({
    CSI_MAX_HISTORY: '2400',
    CSI_HISTORY_LIMIT: '1800',
  });

  assert.equal(limits.maxHistory, 2400);
  assert.equal(limits.defaultLimit, 1800);
  assert.equal(clampHistoryLimit('9999', limits), 2400);
  assert.equal(clampHistoryLimit('0', limits), 1);
  assert.equal(clampHistoryLimit('not-a-number', limits), 1800);
});

test('default limit never exceeds maximum history capacity', () => {
  const limits = createHistoryLimits({
    CSI_MAX_HISTORY: '500',
    CSI_HISTORY_LIMIT: '2000',
  });

  assert.equal(limits.maxHistory, 500);
  assert.equal(limits.defaultLimit, 500);
});
