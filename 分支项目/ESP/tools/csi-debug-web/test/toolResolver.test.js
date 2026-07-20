'use strict';

const assert = require('assert/strict');
const test = require('node:test');
const { createToolResolver, detectToolPlatform } = require('../src/toolResolver');

test('detectToolPlatform maps Node platform values to resolver branches', () => {
  assert.equal(detectToolPlatform('darwin').displayName, 'macOS');
  assert.equal(detectToolPlatform('win32').displayName, 'Windows');
  assert.equal(detectToolPlatform('linux').id, 'other');
});

test('macOS resolver builds Darwin commands and POSIX-safe paths', () => {
  const resolver = createToolResolver({ platform: 'darwin', cwd: '/tmp' });

  assert.equal(resolver.platform.displayName, 'macOS');

  const joined = resolver.joinToolPath('/tmp', 'atlasdeck', 'tools');
  assert.equal(joined, '/tmp/atlasdeck/tools');
  assert.equal(joined.includes('\\'), false);

  assert.deepEqual(resolver.buildOpenDirectoryCommand('/tmp/atlasdeck'), {
    supported: true,
    platform: 'macos',
    platformDisplayName: 'macOS',
    description: 'open directory',
    command: 'open',
    args: ['/tmp/atlasdeck'],
  });

  const appCommand = resolver.buildOpenApplicationCommand('Safari');
  assert.equal(appCommand.command, 'open');
  assert.deepEqual(appCommand.args, ['-a', 'Safari']);

  const fileSelector = resolver.buildFileSelectorCommand({ title: 'Pick log' });
  assert.equal(fileSelector.command, 'osascript');
  assert.match(fileSelector.args.join(' '), /choose file/);

  const toolExists = resolver.buildToolExistsCommand('node');
  assert.match(toolExists.command, /(zsh|bash|sh)$/);
  assert.match(toolExists.args.join(' '), /command -v/);

  assert.deepEqual(resolver.normalizeSerialPortPath('/dev/cu.usbmodem123'), {
    ok: true,
    port: '/dev/cu.usbmodem123',
  });
  assert.equal(resolver.normalizeSerialPortPath('/dev/tty.usbmodem123').ok, false);
});

test('Windows resolver builds win32 commands and Windows-safe paths', () => {
  const resolver = createToolResolver({ platform: 'win32', cwd: 'C:\\work' });

  assert.equal(resolver.platform.displayName, 'Windows');

  const joined = resolver.joinToolPath('C:\\work', 'AtlasDeck', 'tools');
  assert.equal(joined, 'C:\\work\\AtlasDeck\\tools');
  assert.equal(joined.includes('/'), false);

  const openDirectory = resolver.buildOpenDirectoryCommand('C:\\work\\AtlasDeck');
  assert.equal(openDirectory.command, 'explorer');
  assert.deepEqual(openDirectory.args, ['C:\\work\\AtlasDeck']);

  const appCommand = resolver.buildOpenApplicationCommand('notepad');
  assert.equal(appCommand.command, 'powershell');
  assert.match(appCommand.args.join(' '), /Start-Process/);

  const fileSelector = resolver.buildFileSelectorCommand({ title: 'Pick log' });
  assert.equal(fileSelector.command, 'powershell');
  assert.match(fileSelector.args.join(' '), /OpenFileDialog/);

  const toolExists = resolver.buildToolExistsCommand('node');
  assert.equal(toolExists.command, 'powershell');
  assert.match(toolExists.args.join(' '), /Get-Command/);

  assert.deepEqual(resolver.normalizeSerialPortPath('COM3'), {
    ok: true,
    port: 'COM3',
  });
  assert.equal(resolver.normalizeSerialPortPath('/dev/cu.usbmodem123').ok, false);
});

test('safe fallback keeps unsupported UI actions explicit', () => {
  const resolver = createToolResolver({ platform: 'linux', cwd: '/tmp' });

  assert.equal(resolver.platform.displayName, 'Other (linux)');
  assert.equal(resolver.joinToolPath('/tmp', 'atlasdeck', 'tools'), '/tmp/atlasdeck/tools');

  const fileSelector = resolver.buildFileSelectorCommand({ title: 'Pick log' });
  assert.equal(fileSelector.supported, false);
  assert.match(fileSelector.error, /not implemented/);
});
