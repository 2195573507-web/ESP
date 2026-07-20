'use strict';

const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');

const PLATFORM = {
  MACOS: 'macos',
  WINDOWS: 'windows',
  OTHER: 'other',
};

const MACOS_SERIAL_DEVICE_DIRECTORY = '/dev';
const MACOS_SERIAL_PREFIX = '/dev/cu.';
const WINDOWS_COM_PORT_PATTERN = /^(COM\d+|\\\\\.\\COM\d+)$/i;

function detectToolPlatform(nodePlatform = process.platform) {
  const normalized = String(nodePlatform || '').trim();
  if (normalized === 'darwin') {
    return {
      id: PLATFORM.MACOS,
      nodePlatform: 'darwin',
      displayName: 'macOS',
      branchName: 'Darwin / macOS',
      isMacOS: true,
      isWindows: false,
      isFallback: false,
    };
  }
  if (normalized === 'win32') {
    return {
      id: PLATFORM.WINDOWS,
      nodePlatform: 'win32',
      displayName: 'Windows',
      branchName: 'win32 / Windows',
      isMacOS: false,
      isWindows: true,
      isFallback: false,
    };
  }
  return {
    id: PLATFORM.OTHER,
    nodePlatform: normalized || 'unknown',
    displayName: normalized ? `Other (${normalized})` : 'Other',
    branchName: 'safe fallback',
    isMacOS: false,
    isWindows: false,
    isFallback: true,
  };
}

function getPathApi(platformInfo) {
  return platformInfo.isWindows ? path.win32 : path.posix;
}

function makeCommandSpec(platformInfo, description, command, args = []) {
  return {
    supported: true,
    platform: platformInfo.id,
    platformDisplayName: platformInfo.displayName,
    description,
    command,
    args: args.map((value) => String(value)),
  };
}

function makeUnsupportedSpec(platformInfo, description, message) {
  return {
    supported: false,
    platform: platformInfo.id,
    platformDisplayName: platformInfo.displayName,
    description,
    command: '',
    args: [],
    error: message || `${description} is not supported on ${platformInfo.displayName}.`,
  };
}

function assertText(value, name) {
  const text = String(value || '').trim();
  if (!text) {
    throw new Error(`${name} is required`);
  }
  return text;
}

function quoteForPosixShell(value) {
  return `'${String(value).replace(/'/g, "'\\''")}'`;
}

function normalizeSerialPortMetadata(entry, unavailableReason = '') {
  return {
    path: entry.path || '',
    manufacturer: entry.manufacturer || '',
    serialNumber: entry.serialNumber || '',
    pnpId: entry.pnpId || '',
    locationId: entry.locationId || '',
    vendorId: entry.vendorId || '',
    productId: entry.productId || '',
    unavailable_reason: unavailableReason || undefined,
  };
}

function createToolResolver(options = {}) {
  const platformInfo = detectToolPlatform(options.platform || process.platform);
  const pathApi = getPathApi(platformInfo);
  const fsModule = options.fsModule || fs;
  const spawnImpl = options.spawnImpl || spawn;
  const env = options.env || process.env;
  const defaultCwd = options.cwd || process.cwd();

  function joinToolPath(...parts) {
    return pathApi.join(...parts.map((part) => String(part)));
  }

  function resolveToolPath(...parts) {
    return pathApi.resolve(...parts.map((part) => String(part)));
  }

  function buildShellCommand(script, commandOptions = {}) {
    const shellScript = assertText(script, 'script');
    if (platformInfo.isMacOS) {
      return makeCommandSpec(
        platformInfo,
        'run shell command',
        commandOptions.shell || env.SHELL || 'zsh',
        ['-lc', shellScript],
      );
    }
    if (platformInfo.isWindows) {
      return makeCommandSpec(
        platformInfo,
        'run shell command',
        commandOptions.shell || 'powershell',
        ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', shellScript],
      );
    }
    return makeCommandSpec(
      platformInfo,
      'run shell command',
      commandOptions.shell || 'sh',
      ['-lc', shellScript],
    );
  }

  function buildOpenDirectoryCommand(directoryPath) {
    const target = resolveToolPath(assertText(directoryPath, 'directoryPath'));
    if (platformInfo.isMacOS) {
      return makeCommandSpec(platformInfo, 'open directory', 'open', [target]);
    }
    if (platformInfo.isWindows) {
      return makeCommandSpec(platformInfo, 'open directory', 'explorer', [target]);
    }
    return makeCommandSpec(platformInfo, 'open directory', 'xdg-open', [target]);
  }

  function buildRevealPathCommand(filePath) {
    const target = resolveToolPath(assertText(filePath, 'filePath'));
    if (platformInfo.isMacOS) {
      return makeCommandSpec(platformInfo, 'reveal path', 'open', ['-R', target]);
    }
    if (platformInfo.isWindows) {
      return makeCommandSpec(platformInfo, 'reveal path', 'explorer', ['/select,', target]);
    }
    return buildOpenDirectoryCommand(pathApi.dirname(target));
  }

  function buildOpenApplicationCommand(applicationName, applicationArgs = []) {
    const app = assertText(applicationName, 'applicationName');
    const args = applicationArgs.map((value) => String(value));
    if (platformInfo.isMacOS) {
      return makeCommandSpec(
        platformInfo,
        'open application',
        'open',
        args.length ? ['-a', app, '--args', ...args] : ['-a', app],
      );
    }
    if (platformInfo.isWindows) {
      const script = [
        '$file = $args[0];',
        '$appArgs = @();',
        'if ($args.Length -gt 1) { $appArgs = $args[1..($args.Length - 1)] }',
        'Start-Process -FilePath $file -ArgumentList $appArgs',
      ].join(' ');
      return makeCommandSpec(
        platformInfo,
        'open application',
        'powershell',
        ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', script, app, ...args],
      );
    }
    return makeUnsupportedSpec(
      platformInfo,
      'open application',
      `Opening applications is not implemented for ${platformInfo.displayName}; use a macOS or Windows resolver branch.`,
    );
  }

  function buildFileSelectorCommand(selectorOptions = {}) {
    const title = String(selectorOptions.title || 'Select a file');
    const initialDirectory = selectorOptions.initialDirectory
      ? resolveToolPath(selectorOptions.initialDirectory)
      : '';
    const multiple = Boolean(selectorOptions.multiple);

    if (platformInfo.isMacOS) {
      const script = [
        `set selectedItems to choose file with prompt ${JSON.stringify(title)}${multiple ? ' with multiple selections allowed' : ''}`,
        'if class of selectedItems is list then',
        '  set outputText to ""',
        '  repeat with selectedItem in selectedItems',
        '    set outputText to outputText & POSIX path of selectedItem & linefeed',
        '  end repeat',
        '  return outputText',
        'end if',
        'return POSIX path of selectedItems',
      ].join('\n');
      return makeCommandSpec(platformInfo, 'select file', 'osascript', ['-e', script]);
    }

    if (platformInfo.isWindows) {
      const script = [
        'Add-Type -AssemblyName System.Windows.Forms;',
        '$dialog = New-Object System.Windows.Forms.OpenFileDialog;',
        `$dialog.Title = ${JSON.stringify(title)};`,
        `$dialog.Multiselect = ${multiple ? '$true' : '$false'};`,
        'if ($args.Length -gt 0 -and $args[0]) { $dialog.InitialDirectory = $args[0]; }',
        'if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {',
        '  if ($dialog.Multiselect) { $dialog.FileNames -join "`n" } else { $dialog.FileName }',
        '} else { exit 2 }',
      ].join(' ');
      const args = ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', script];
      if (initialDirectory) {
        args.push(initialDirectory);
      }
      return makeCommandSpec(platformInfo, 'select file', 'powershell', args);
    }

    return makeUnsupportedSpec(
      platformInfo,
      'select file',
      `File selection is not implemented for ${platformInfo.displayName}; no silent fallback is available.`,
    );
  }

  function buildDirectorySelectorCommand(selectorOptions = {}) {
    const title = String(selectorOptions.title || 'Select a folder');
    const initialDirectory = selectorOptions.initialDirectory
      ? resolveToolPath(selectorOptions.initialDirectory)
      : '';

    if (platformInfo.isMacOS) {
      const script = `POSIX path of (choose folder with prompt ${JSON.stringify(title)})`;
      return makeCommandSpec(platformInfo, 'select directory', 'osascript', ['-e', script]);
    }

    if (platformInfo.isWindows) {
      const script = [
        'Add-Type -AssemblyName System.Windows.Forms;',
        '$dialog = New-Object System.Windows.Forms.FolderBrowserDialog;',
        `$dialog.Description = ${JSON.stringify(title)};`,
        'if ($args.Length -gt 0 -and $args[0]) { $dialog.SelectedPath = $args[0]; }',
        'if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {',
        '  $dialog.SelectedPath',
        '} else { exit 2 }',
      ].join(' ');
      const args = ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', script];
      if (initialDirectory) {
        args.push(initialDirectory);
      }
      return makeCommandSpec(platformInfo, 'select directory', 'powershell', args);
    }

    return makeUnsupportedSpec(
      platformInfo,
      'select directory',
      `Directory selection is not implemented for ${platformInfo.displayName}; no silent fallback is available.`,
    );
  }

  function buildToolExistsCommand(toolName) {
    const tool = assertText(toolName, 'toolName');
    if (platformInfo.isWindows) {
      const script = 'if (Get-Command -Name $args[0] -ErrorAction SilentlyContinue) { exit 0 } exit 1';
      return makeCommandSpec(
        platformInfo,
        'check tool exists',
        'powershell',
        ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', script, tool],
      );
    }
    return buildShellCommand(`command -v -- ${quoteForPosixShell(tool)} >/dev/null 2>&1`);
  }

  function buildRunLocalToolCommand(toolName, toolArgs = [], commandOptions = {}) {
    if (commandOptions.script) {
      return buildShellCommand(commandOptions.script, commandOptions);
    }
    const tool = assertText(toolName, 'toolName');
    return makeCommandSpec(
      platformInfo,
      'run local tool',
      tool,
      toolArgs.map((value) => String(value)),
    );
  }

  function runCommand(commandSpec, runOptions = {}) {
    if (!commandSpec.supported) {
      return Promise.reject(new Error(commandSpec.error));
    }

    return new Promise((resolve, reject) => {
      const child = spawnImpl(commandSpec.command, commandSpec.args, {
        cwd: runOptions.cwd || defaultCwd,
        env: runOptions.env || env,
        stdio: ['ignore', 'pipe', 'pipe'],
        windowsHide: true,
      });

      let stdout = '';
      let stderr = '';
      child.stdout.on('data', (chunk) => {
        stdout += chunk.toString('utf8');
      });
      child.stderr.on('data', (chunk) => {
        stderr += chunk.toString('utf8');
      });
      child.on('error', (error) => {
        reject(new Error(`${commandSpec.description} failed to start on ${platformInfo.displayName}: ${error.message}`));
      });
      child.on('close', (code) => {
        const result = { code, stdout, stderr, command: commandSpec };
        if (code !== 0 && !runOptions.allowNonZero) {
          reject(new Error(`${commandSpec.description} failed on ${platformInfo.displayName} with exit code ${code}: ${stderr || stdout}`.trim()));
          return;
        }
        resolve(result);
      });
    });
  }

  async function openDirectory(directoryPath, runOptions) {
    return runCommand(buildOpenDirectoryCommand(directoryPath), runOptions);
  }

  async function revealPath(filePath, runOptions) {
    return runCommand(buildRevealPathCommand(filePath), runOptions);
  }

  async function openApplication(applicationName, applicationArgs, runOptions) {
    return runCommand(buildOpenApplicationCommand(applicationName, applicationArgs), runOptions);
  }

  async function selectFile(selectorOptions, runOptions) {
    const result = await runCommand(buildFileSelectorCommand(selectorOptions), runOptions);
    return result.stdout.split(/\r?\n/).map((line) => line.trim()).filter(Boolean);
  }

  async function selectDirectory(selectorOptions, runOptions) {
    const result = await runCommand(buildDirectorySelectorCommand(selectorOptions), runOptions);
    return result.stdout.trim();
  }

  async function runLocalTool(toolName, toolArgs, runOptions) {
    return runCommand(buildRunLocalToolCommand(toolName, toolArgs, runOptions), runOptions);
  }

  async function toolExists(toolName) {
    const result = await runCommand(buildToolExistsCommand(toolName), { allowNonZero: true });
    return result.code === 0;
  }

  async function listMacSerialDevicePaths(unavailableReason = '') {
    const devNames = await fsModule.promises.readdir(MACOS_SERIAL_DEVICE_DIRECTORY).catch(() => []);
    return devNames
      .filter((name) => name.startsWith('cu.'))
      .map((name) => normalizeSerialPortMetadata({
        path: joinToolPath(MACOS_SERIAL_DEVICE_DIRECTORY, name),
      }, unavailableReason))
      .sort((a, b) => a.path.localeCompare(b.path));
  }

  async function listSerialPorts(listOptions = {}) {
    const SerialPort = listOptions.SerialPort || null;
    const unavailableReason = listOptions.unavailableReason || '';

    if (!SerialPort) {
      if (platformInfo.isMacOS) {
        return listMacSerialDevicePaths(unavailableReason);
      }
      return [];
    }

    let serialPortMetadata = [];
    try {
      serialPortMetadata = await SerialPort.list();
    } catch (error) {
      if (!platformInfo.isMacOS) {
        throw error;
      }
      return listMacSerialDevicePaths(error.message);
    }

    if (platformInfo.isMacOS) {
      const metadataByPath = new Map(serialPortMetadata.map((entry) => [entry.path, entry]));
      const macPaths = await listMacSerialDevicePaths(unavailableReason);
      return macPaths.map((entry) => normalizeSerialPortMetadata(metadataByPath.get(entry.path) || entry, unavailableReason));
    }

    return serialPortMetadata
      .map((entry) => normalizeSerialPortMetadata(entry, unavailableReason))
      .filter((entry) => entry.path)
      .sort((a, b) => a.path.localeCompare(b.path));
  }

  function normalizeSerialPortPath(portPath) {
    const normalized = String(portPath || '').trim();
    if (!normalized) {
      return { ok: false, statusCode: 400, error: 'serial port is required' };
    }

    if (platformInfo.isMacOS) {
      if (!normalized.startsWith(MACOS_SERIAL_PREFIX)) {
        return { ok: false, statusCode: 400, error: 'serial port must match /dev/cu.* on macOS' };
      }
      return { ok: true, port: normalized };
    }

    if (platformInfo.isWindows) {
      if (!WINDOWS_COM_PORT_PATTERN.test(normalized)) {
        return { ok: false, statusCode: 400, error: 'serial port must be a Windows COM port such as COM3' };
      }
      return { ok: true, port: normalized.toUpperCase() };
    }

    return {
      ok: false,
      statusCode: 400,
      error: `serial ports are not implemented for ${platformInfo.displayName}; add an explicit platform rule instead of guessing`,
    };
  }

  return {
    platform: platformInfo,
    pathApi,
    joinToolPath,
    resolveToolPath,
    buildShellCommand,
    buildOpenDirectoryCommand,
    buildRevealPathCommand,
    buildOpenApplicationCommand,
    buildFileSelectorCommand,
    buildDirectorySelectorCommand,
    buildToolExistsCommand,
    buildRunLocalToolCommand,
    runCommand,
    openDirectory,
    revealPath,
    openApplication,
    selectFile,
    selectDirectory,
    runLocalTool,
    toolExists,
    listSerialPorts,
    normalizeSerialPortPath,
  };
}

module.exports = {
  PLATFORM,
  createToolResolver,
  detectToolPlatform,
};
