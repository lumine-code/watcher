# watcher

Watches directories and queries filesystem events from native code.

A fork of [parcel-bundler/watcher](https://github.com/parcel-bundler/watcher) maintained for the Lumine editor.

## Changes from upstream

It differs from upstream in two ways: the Windows backend confines subscription teardown to its I/O thread, fixing a use-after-free where a pending `ReadDirectoryChangesW` completion raced `unsubscribe()` and crashed the process; and it always builds from source instead of downloading platform prebuilds. Everything else deliberately tracks upstream so their changes merge cleanly.

## Features

- **Watch**: subscribes to realtime recursive directory notifications when files or directories are created, updated, or deleted.
- **Query**: reports historical change events in a directory, even for the time a program was not running.
- **Native**: implemented in C++ for performance and low-level integration with the operating system.
- **Cross platform**: ships backends for macOS, Linux, Windows, FreeBSD, and Watchman.
- **Throttled**: coalesces events in C++ so the JavaScript thread survives large changes such as a branch switch or a dependency install.
- **Scalable**: watches or queries tens of thousands of files at once.

## Installation

```sh
npm install @lumine-code/watcher
```

The native addon is compiled during install, so a C++ toolchain and Python are required. Under Electron, rebuild it for the target ABI with [`@electron/rebuild`](https://github.com/electron/rebuild).

## Usage

```javascript
const watcher = require('@lumine-code/watcher');
const path = require('path');

// Subscribe to events
let subscription = await watcher.subscribe(process.cwd(), (err, events) => {
  console.log(events);
});

// later on...
await subscription.unsubscribe();

// Get events since some saved snapshot in the past
let snapshotPath = path.join(process.cwd(), 'snapshot.txt');
let events = await watcher.getEventsSince(process.cwd(), snapshotPath);

// Save a snapshot for later
await watcher.writeSnapshot(process.cwd(), snapshotPath);
```

### Watching

Subscriptions report changes in a directory as they happen. They work recursively, so changes in sub-directories are emitted too.

Events are throttled and coalesced for performance during large changes like `git checkout` or `npm install`, and a single notification is emitted with all of the events at the end.

Only one notification is emitted per file. For example, if a file was both created and updated since the last event, you get only a `create` event. If a file is both created and deleted, you are not notified of that file. Renames cause two events: a `delete` for the old name, and a `create` for the new name.

```javascript
let subscription = await watcher.subscribe(process.cwd(), (err, events) => {
  console.log(events);
});
```

Events have two properties:

- `type` - the event type: `create`, `update`, or `delete`.
- `path` - the absolute path to the file or directory.

To unsubscribe from change notifications, call the `unsubscribe` method on the returned subscription object.

```javascript
await subscription.unsubscribe();
```

The watcher backends, in priority order:

- [FSEvents](https://developer.apple.com/documentation/coreservices/file_system_events) on macOS
- [Watchman](https://facebook.github.io/watchman/) if installed
- [inotify](http://man7.org/linux/man-pages/man7/inotify.7.html) on Linux
- [ReadDirectoryChangesW](https://msdn.microsoft.com/en-us/library/windows/desktop/aa365465%28v%3Dvs.85%29.aspx) on Windows
- [kqueue](https://man.freebsd.org/cgi/man.cgi?kqueue) on FreeBSD, or as an alternative to FSEvents on macOS

You can specify the exact backend you wish to use by passing the `backend` option. If that backend is not available on the current platform, the default backend is used instead.

### Querying

Historical changes made in a directory can be queried even for the time your program was not running. This makes it easy to invalidate a cache and re-build only the files that have changed. It can be **significantly** faster than traversing the entire filesystem to determine what files changed, depending on the platform.

In order to query for historical changes, you first need a previous snapshot to compare to. This can be saved to a file with the `writeSnapshot` function, e.g. just before your program exits.

```javascript
await watcher.writeSnapshot(dirPath, snapshotPath);
```

When your program starts up, you can query for changes that have occurred since that snapshot using the `getEventsSince` function.

```javascript
let events = await watcher.getEventsSince(dirPath, snapshotPath);
```

The events returned are exactly the same as the events that would be passed to the `subscribe` callback (see above).

The query backends, in priority order:

- [FSEvents](https://developer.apple.com/documentation/coreservices/file_system_events) on macOS
- [Watchman](https://facebook.github.io/watchman/) if installed
- [fts](http://man7.org/linux/man-pages/man3/fts.3.html) (brute force) on Linux and FreeBSD
- [FindFirstFile](https://docs.microsoft.com/en-us/windows/desktop/api/fileapi/nf-fileapi-findfirstfilea) (brute force) on Windows

The FSEvents (macOS) and Watchman backends are significantly more performant than the brute force backends used by default on Linux and Windows, for example returning results in milliseconds instead of seconds for large directory trees. This is because a background daemon monitoring filesystem changes on those platforms allows us to query cached data rather than traversing the filesystem manually (brute force).

macOS has good performance with FSEvents by default. For the best performance on other platforms, install [Watchman](https://facebook.github.io/watchman/) and it is used automatically.

### Options

All of the APIs support the following options, which are passed as an object as the last function argument.

- `ignore` - an array of paths or glob patterns to ignore. uses [`is-glob`](https://github.com/micromatch/is-glob) to distinguish paths from globs. glob patterns are parsed with [`picomatch`](https://github.com/micromatch/picomatch) (see [features](https://github.com/micromatch/picomatch#globbing-features)).
  - paths can be relative or absolute and can either be files or directories. No events will be emitted about these files or directories or their children.
  - glob patterns match on relative paths from the root that is watched. No events will be emitted for matching paths.
- `backend` - the name of an explicitly chosen backend to use. Allowed options are `"fs-events"`, `"watchman"`, `"inotify"`, `"kqueue"`, `"windows"`, or `"brute-force"` (only for querying). If the specified backend is not available on the current platform, the default backend is used instead.

## Building

```sh
npm install
npm run build
npm test
```

`scripts/stress-windows.js` exercises subscribe/unsubscribe churn against the Windows backend, which is how the teardown use-after-free described above reproduces. It exits non-zero if the process crashes:

```sh
node scripts/stress-windows.js --iterations 20
```

## Contributing

Got ideas to make this package better, found a bug, or want to help add new features? Just drop your thoughts on GitHub. Any feedback is welcome!
