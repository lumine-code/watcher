'use strict';
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const {EventEmitter} = require('node:events');
const {execFileSync, fork} = require('node:child_process');

function option(name, fallback) {
  const at = process.argv.indexOf(name);
  return at < 0 ? fallback : process.argv[at + 1];
}
const editor = option('--editor');
if (!editor) throw new Error('Pass --editor with an editor source checkout.');
const editorRoot = path.resolve(editor);
const FileWatchWorker = require(path.join(editorRoot, 'src/file-watch-worker'));
const FileWatchService = require(
  path.join(editorRoot, 'src/file-watch-service'),
);
const FileWatchClient = require(path.join(editorRoot, 'src/file-watch-client'));
const {VERSION} = require(path.join(editorRoot, 'src/file-watch-protocol'));
const iterations = Number(option('--iterations', 8));
if (!Number.isSafeInteger(iterations) || iterations < 1)
  throw new Error('iterations must be positive');
const realChild = process.argv.includes('--real-child');
const platform = option('--platform', process.platform);
const pause = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const trace = [];
const children = [];
const ackTimers = new Set();
let onGuardClose;
function record(value) {
  trace.push(value);
  if (trace.length > 240) trace.shift();
}
async function until(predicate, label) {
  const deadline = Date.now() + 10000;
  while (!predicate()) {
    if (Date.now() > deadline) throw new Error(`Timed out: ${label}`);
    await pause(10);
  }
}

class NativeChild extends EventEmitter {
  constructor(generation) {
    super();
    this.generation = generation;
    this.connected = true;
    const native = require('..').createEngine();
    let nextSource = 0;
    this.runtime = new FileWatchWorker({
      platform,
      engine: {
        watchDirectory(directory, options, callback) {
          const source = ++nextSource;
          record({native: 'open', source, directory, options});
          const handle = native.watchDirectory(
            directory,
            options,
            (message) => {
              record({native: 'event', source, directory, message});
              callback(message);
            },
          );
          return {
            ready: handle.ready,
            // Keep real acquisition/delivery; model a busy close completion so
            // file creation can overlap a guard-to-content topology handoff.
            closed: handle.closed.then(() =>
              options.guard ? pause(80) : undefined,
            ),
            dispose() {
              record({native: 'dispose', source, directory, options});
              handle.dispose();
              if (options.guard) onGuardClose?.(directory);
            },
          };
        },
        close: () => native.close(),
      },
      sendEvent: ({id, type, payload}) => {
        record({worker: type, id, payload});
        this.message({type: 'event', id, eventType: type, payload});
      },
    });
    children.push(this);
    setImmediate(() => this.message({type: 'ready'}));
  }
  message(message) {
    if (this.connected)
      this.emit('message', {
        version: VERSION,
        generation: this.generation,
        ...message,
      });
  }
  send(message, callback) {
    callback?.(null);
    const operation =
      message.type === 'subscribe'
        ? this.runtime.subscribe(message)
        : message.type === 'unsubscribe'
          ? this.runtime.unsubscribe(message.id)
          : message.type === 'diagnostics'
            ? Promise.resolve(this.runtime.diagnostics())
            : this.runtime.close();
    operation.then(
      (payload) =>
        this.message({type: 'reply', requestId: message.requestId, payload}),
      (error) =>
        this.message({type: 'reply', requestId: message.requestId, error}),
    );
  }
  kill() {
    if (!this.connected) return;
    this.connected = false;
    this.runtime.close().finally(() => {
      this.emit('exit', 0, null);
      this.emit('close', 0, null);
    });
  }
}

function rendererClient(service, owner) {
  const listeners = new Set();
  const deliver = (event) => {
    record({renderer: owner, event});
    for (const listener of listeners) listener(event);
  };
  return new FileWatchClient({
    request(request) {
      if (request.type !== 'ack')
        return service.dispatch(owner, request, deliver);
      return new Promise((resolve, reject) => {
        const timer = setTimeout(() => {
          ackTimers.delete(timer);
          service.dispatch(owner, request, deliver).then(resolve, reject);
        }, 500);
        ackTimers.add(timer);
      });
    },
    onEvent(callback) {
      listeners.add(callback);
      return {dispose: () => listeners.delete(callback)};
    },
  });
}

async function main() {
  const sourceSha = execFileSync(
    'git',
    ['-C', editorRoot, 'rev-parse', 'HEAD'],
    {encoding: 'utf8'},
  ).trim();
  console.log(
    JSON.stringify({
      editor: sourceSha,
      nativePlatform: process.platform,
      policyPlatform: platform,
      realChild,
      electron: process.versions.electron,
      iterations,
    }),
  );
  const unresolvedRoot = fs.mkdtempSync(
    path.join(os.tmpdir(), 'editor-watch-replay-'),
  );
  const root = fs.realpathSync.native(unresolvedRoot);
  const moduleRoot = path.join(root, '.native-modules');
  if (realChild) {
    fs.mkdirSync(path.join(moduleRoot, '@lumine-code'), {recursive: true});
    fs.symlinkSync(
      path.resolve(__dirname, '..'),
      path.join(moduleRoot, '@lumine-code', 'watcher'),
      process.platform === 'win32' ? 'junction' : 'dir',
    );
  }
  const service = new FileWatchService({
    spawnWorker: ({generation}) => {
      if (!realChild) return new NativeChild(generation);
      const child = fork(
        path.join(editorRoot, 'src/file-watch-worker-bootstrap'),
        [],
        {
          env: {
            ...process.env,
            NODE_PATH: moduleRoot,
            ELECTRON_RUN_AS_NODE: '1',
            ELECTRON_NO_ATTACH_CONSOLE: '1',
            LUMINE_FILE_WATCH_GENERATION: String(generation),
          },
          execArgv: [],
          silent: true,
          windowsHide: true,
        },
      );
      child.on('message', (message) => record({bootstrap: message}));
      child.stderr.on('data', (data) =>
        record({bootstrapStderr: data.toString()}),
      );
      return child;
    },
  });
  const application = service.createClient('application');
  let client;
  try {
    const profile = path.join(unresolvedRoot, 'profile');
    fs.mkdirSync(profile);
    const config = path.join(profile, 'config');
    fs.writeFileSync(config, 'persistent');
    const stable = application.watchFile(config);
    const stableMissing = application.watchFile(
      path.join(profile, 'missing-config'),
    );
    const tree = application.watchDirectory(root, {recursive: true});
    await Promise.all([stable.ready, stableMissing.ready, tree.ready]);
    for (let iteration = 0; iteration < iterations; ++iteration) {
      client = rendererClient(service, `renderer-${iteration}`);
      const directory = path.join(root, `case-${iteration}`);
      fs.mkdirSync(directory);
      const file = path.join(directory, 'missing', 'nested', 'file');
      const events = [];
      const handle = client.watchFile(file);
      handle.onDidChange((batch) => events.push(...batch));
      handle.onDidError((error) => record({clientError: error.message}));
      await handle.ready;
      const noise = path.join(directory, 'noise');
      fs.writeFileSync(noise, 'before');
      const noiseHandle = client.watchFile(noise);
      let noiseObserved = false;
      noiseHandle.onDidChange(() => {
        noiseObserved = true;
      });
      await noiseHandle.ready;
      fs.appendFileSync(noise, 'after');
      await until(() => noiseObserved, 'renderer ACK gate');
      let createdDuringClose = false;
      onGuardClose = (closingDirectory) => {
        if (closingDirectory !== directory || createdDuringClose) return;
        createdDuringClose = true;
        record({filesystem: 'create-during-old-guard-close', file});
        fs.writeFileSync(file, 'created');
      };
      fs.mkdirSync(path.dirname(file), {recursive: true});
      if (realChild || platform !== 'darwin') fs.writeFileSync(file, 'created');
      await until(
        () => events.some((event) => event.action === 'created'),
        `iteration ${iteration} missing-parent creation`,
      );
      onGuardClose = null;
      fs.renameSync(file, `${file}.moved`);
      await until(
        () => events.some((event) => event.action === 'deleted'),
        `iteration ${iteration} rename-away deletion`,
      );
      const createdCount = events.filter(
        (event) => event.action === 'created',
      ).length;
      fs.writeFileSync(file, 'recreated after rename');
      await until(
        () =>
          events.filter((event) => event.action === 'created').length >
          createdCount,
        `iteration ${iteration} recreation`,
      );
      const updatedCount = events.filter(
        (event) => event.action === 'updated',
      ).length;
      fs.writeFileSync(file, 'external write after recreation');
      await until(
        () =>
          events.filter((event) => event.action === 'updated').length >
          updatedCount,
        `iteration ${iteration} resumed writes`,
      );
      await client.close();
      client = null;
      fs.rmSync(directory, {recursive: true, force: true});
      console.log(
        `persistent service iteration ${iteration + 1}/${iterations} passed`,
      );
    }
  } catch (error) {
    console.error(
      JSON.stringify(
        {
          error: error.stack,
          diagnostics: realChild
            ? await service
                .requestWorker('diagnostics')
                .catch((error) => ({error: error.message}))
            : children.map((child) => child.runtime.diagnostics()),
          trace,
        },
        null,
        2,
      ),
    );
    throw error;
  } finally {
    onGuardClose = null;
    await client?.close();
    await application.close();
    await service.close();
    for (const timer of ackTimers) clearTimeout(timer);
    fs.rmSync(root, {recursive: true, force: true});
  }
}
main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
