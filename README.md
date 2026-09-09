# watcher

Observes directory changes through native filesystem backends.

A Node-API library maintained by lumine-code, derived from the Parcel watcher project. It provides the native directory sources used by the editor's filesystem observation service.

## Features

- **Native sources**: uses ReadDirectoryChangesW on Windows, inotify on Linux, and FSEvents on macOS.
- **Explicit lifecycle**: exposes immediate cancellation with promises for armed readiness and completed cleanup.
- **Directory modes**: observes immediate children or recursive descendants without following directory symlinks during Linux traversal.
- **Loss reporting**: emits invalidation when native events or the bounded JavaScript delivery queue overflow.
- **Isolation**: owns resources per engine and supports independent Node-API environments and worker termination.

## Installation

Install this repository at an immutable Git commit. Installation compiles the addon using node-gyp; Node.js 24 or newer and a working native build toolchain are required.

## Usage

```js
const {createEngine} = require('@lumine-code/watcher');
const engine = createEngine();
const watch = engine.watchDirectory(
  '/absolute/project',
  {recursive: true},
  (message) => {
    if (message.type === 'changes') console.log(message.events);
    else if (message.type === 'invalidate')
      console.log('Re-read the directory:', message.reason);
    else console.error(message.error);
  },
);
await watch.ready;
// Changes made after ready are observed by the armed native source.
watch.dispose();
await watch.closed;
await engine.close();
```

## API

`createEngine()` creates an independent native engine with one owner thread. `engine.watchDirectory(path, {recursive: false}, callback)` returns a handle immediately. Paths are resolved to absolute paths; the target must be an existing directory. Readiness failures reject `ready` with an error carrying `code`, `path`, and `backend`. Missing targets and fixed-name file behavior belong to the editor service above this library.

`ready` resolves after the OS source is armed. `dispose()` is synchronous and idempotent, suppresses queued delivery immediately, and rejects unresolved readiness with `AbortError`. `closed` resolves after native resources and pending callbacks have been released. `engine.close()` cancels all sources, waits for native cleanup and is idempotent. Explicitly close each engine when its owner is destroyed.

The callback receives `{type: 'changes', events: [{action, path}]}`, with actions `created`, `updated`, and `deleted`; `{type: 'invalidate', reason}`; or `{type: 'error', error: {message, code, path, backend}}`. Notifications describe observed activity and may coalesce; they are not a complete transaction history. Rename is represented by activity at the old and new names. A consumer must reread current state after invalidation. An inotify kernel overflow closes affected sources so their owner can rebuild descriptor coverage; terminal native errors also close their source.

The native delivery queue holds at most 8192 change events and one thread-safe-function wakeup. Overflow replaces lost events with invalidations while preserving lifecycle messages. Directory loss is reported explicitly. Windows does not report a watched directory's own rename; the editor service uses parent and ancestor guards to maintain fixed lexical locations. FSEvents uses its root-change capability. No automatic external file following, snapshot history, Watchman selection, or polling fallback is provided.

The editor service can request `{recursive: false, guard: true}` for directory membership and location guards. On macOS these use shared vnode descriptors inside the same native backend, with unnamed `{type: 'guard'}` notifications and root-change invalidation. They do not start FSEvents streams or subscribe to descendant contents, including when guarding `/`. Ancestor vnode descriptors detect relocation while remaining shared between guards. Windows and Linux accept this option through their shallow sources; the editor uses it only on macOS. Normal shallow FSEvents sources seed immediate entry metadata after arming so delayed creation flags and access-time-only changes do not masquerade as fresh content writes.

## Building

Run `npm ci`, `npm test`, and `npm run lint`. The CI matrix builds and tests Node.js 24 on Windows, macOS and Linux. `npm run benchmark` reports readiness, close latency, idle CPU, memory and event latency on a synthetic tree. `node scripts/stress-windows.js --iterations 20` exercises teardown while notifications are in flight.

## Contributing

Got ideas to make this package better, found a bug, or want to help add new features? Just drop your thoughts on GitHub. Any feedback is welcome!
