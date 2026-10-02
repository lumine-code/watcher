'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const {test} = require('node:test');
const {Worker} = require('node:worker_threads');
const {createEngine} = require('..');

const delay = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
async function until(predicate, label) {
  const deadline = Date.now() + 7000;
  while (!predicate()) {
    if (Date.now() > deadline) throw new Error(`Timed out: ${label}`);
    await delay(10);
  }
}
function fixture(t) {
  const root = fs.mkdtempSync(
    path.join(fs.realpathSync(os.tmpdir()), 'lumine-watch-'),
  );
  const engine = createEngine();
  t.after(async () => {
    await engine.close();
    fs.rmSync(root, {recursive: true, force: true});
  });
  return {root, engine};
}
function observe(engine, root, recursive = false) {
  const messages = [];
  const handle = engine.watchDirectory(root, {recursive}, (message) =>
    messages.push(message),
  );
  return {
    handle,
    messages,
    has(action, file) {
      return messages.some((message) =>
        message.events?.some(
          (event) => event.action === action && event.path === file,
        ),
      );
    },
    any(file) {
      return messages.some((message) =>
        message.events?.some((event) => event.path === file),
      );
    },
  };
}

test('exports only the engine factory', () =>
  assert.deepEqual(Object.keys(require('..')), ['createEngine']));

test('ready arms creation, modification and deletion notifications', async (t) => {
  const {root, engine} = fixture(t);
  const watch = observe(engine, root);
  assert.equal(typeof watch.handle.dispose, 'function');
  assert.ok(watch.handle.ready instanceof Promise);
  await watch.handle.ready;
  const file = path.join(root, 'file.txt');
  fs.writeFileSync(file, 'first');
  await until(() => watch.has('created', file), 'create');
  watch.messages.length = 0;
  fs.appendFileSync(file, 'second');
  await until(() => watch.has('updated', file), 'update');
  fs.unlinkSync(file);
  await until(() => watch.has('deleted', file), 'delete');
});

test('shallow and recursive watches coexist and close independently', async (t) => {
  const {root, engine} = fixture(t);
  const child = path.join(root, 'child');
  fs.mkdirSync(child);
  const shallow = observe(engine, root),
    recursive = observe(engine, root, true);
  await Promise.all([shallow.handle.ready, recursive.handle.ready]);
  const file = path.join(child, 'deep.txt');
  fs.writeFileSync(file, 'deep');
  await until(() => recursive.any(file), 'recursive child');
  await delay(150);
  assert.equal(shallow.any(file), false);
  shallow.handle.dispose();
  await shallow.handle.closed;
  const other = path.join(child, 'other.txt');
  fs.writeFileSync(other, 'other');
  await until(() => recursive.any(other), 'recursive survives shallow close');
});

test('a shallow source does not enumerate an existing subtree', async (t) => {
  const {root, engine} = fixture(t);
  fs.mkdirSync(path.join(root, 'deep', 'deeper'), {recursive: true});
  const watch = observe(engine, root);
  await watch.handle.ready;
  const directory = path.join(root, 'new-directory');
  fs.mkdirSync(directory);
  await until(
    () => watch.has('created', directory),
    'shallow direct directory',
  );
  const file = path.join(directory, 'invisible.txt');
  fs.writeFileSync(file, 'x');
  await delay(150);
  assert.equal(watch.any(file), false);
});

test('recursive sources attach a populated subtree moved into the root', async (t) => {
  const {root, engine} = fixture(t);
  const outside = path.join(root, 'outside'),
    watched = path.join(root, 'watched');
  fs.mkdirSync(path.join(outside, 'deep'), {recursive: true});
  fs.mkdirSync(watched);
  fs.writeFileSync(path.join(outside, 'deep', 'file.txt'), 'old');
  const watch = observe(engine, watched, true);
  await watch.handle.ready;
  const incoming = path.join(watched, 'incoming');
  fs.renameSync(outside, incoming);
  await until(() => watch.any(incoming), 'incoming tree');
  const file = path.join(incoming, 'deep', 'file.txt');
  fs.appendFileSync(file, 'new');
  await until(() => watch.any(file), 'incoming grandchild');
  watch.messages.length = 0;
  fs.appendFileSync(file, 'again');
  await until(
    () => watch.has('updated', file),
    'future incoming grandchild update',
  );
});

test('recursive sources remove moved-out coverage and attach internal renamed descendants', async (t) => {
  const {root, engine} = fixture(t);
  const watched = path.join(root, 'watched');
  fs.mkdirSync(path.join(watched, 'child', 'deep'), {recursive: true});
  const watch = observe(engine, watched, true);
  await watch.handle.ready;
  const renamed = path.join(watched, 'renamed');
  fs.renameSync(path.join(watched, 'child'), renamed);
  await until(() => watch.any(renamed), 'renamed directory');
  const file = path.join(renamed, 'deep', 'new.txt');
  fs.writeFileSync(file, 'x');
  await until(() => watch.any(file), 'renamed deep file');
  fs.renameSync(renamed, path.join(root, 'gone'));
  await until(() => watch.has('deleted', renamed), 'moved-out directory');
  watch.messages.length = 0;
  fs.writeFileSync(path.join(root, 'gone', 'deep', 'outside.txt'), 'x');
  await delay(150);
  assert.equal(
    watch.messages.some((message) =>
      message.events?.some((event) => event.path.endsWith('outside.txt')),
    ),
    false,
  );
});

test('mass deletion of nested files preserves directory cleanup and later recreations', async (t) => {
  const {root, engine} = fixture(t);
  const watched = path.join(root, 'watched');
  fs.mkdirSync(watched);
  const watch = observe(engine, watched, true);
  await watch.handle.ready;
  const tree = path.join(watched, 'tree');
  const files = [];
  for (let directory = 0; directory < 8; ++directory) {
    const nested = path.join(tree, String(directory), 'nested');
    fs.mkdirSync(nested, {recursive: true});
    for (let index = 0; index < 32; ++index) {
      const file = path.join(nested, String(index));
      fs.writeFileSync(file, 'before');
      files.push(file);
    }
  }
  const survivor = path.join(watched, 'survivor');
  fs.writeFileSync(survivor, 'before');
  await until(
    () => files.every((file) => watch.has('created', file)),
    'populated nested tree',
  );
  watch.messages.length = 0;
  for (const file of files) fs.unlinkSync(file);
  await until(
    () => files.every((file) => watch.has('deleted', file)),
    'each nested file deletion',
  );
  watch.messages.length = 0;
  for (const file of files) fs.writeFileSync(file, 'before');
  await until(
    () => files.every((file) => watch.has('created', file)),
    'nested files recreated before directory relocation',
  );
  // Moving out a populated directory need not enumerate child deletions.
  // Its directory event must discard cached descendants at their old names.
  fs.renameSync(tree, path.join(root, 'moved-out'));
  await until(() => watch.has('deleted', tree), 'nested directory deletion');
  watch.messages.length = 0;
  const recreated = files[0];
  fs.mkdirSync(path.dirname(recreated), {recursive: true});
  fs.writeFileSync(recreated, 'after!');
  await until(() => watch.has('created', recreated), 'fresh recreated entry');
  fs.writeFileSync(survivor, 'after!');
  await until(() => watch.has('updated', survivor), 'unrelated file survives');
});

test('identical native directory sources remain independent', async (t) => {
  const {root, engine} = fixture(t);
  const first = observe(engine, root),
    second = observe(engine, root);
  await Promise.all([first.handle.ready, second.handle.ready]);
  first.handle.dispose();
  await first.handle.closed;
  const file = path.join(root, 'survivor');
  fs.writeFileSync(file, 'x');
  await until(() => second.any(file), 'second identical source');
  assert.equal(first.any(file), false);
});

test('ready rejects absent paths and file targets with structured errors', async (t) => {
  const {root, engine} = fixture(t);
  const missing = observe(engine, path.join(root, 'missing'));
  await assert.rejects(missing.handle.ready, {code: 'ENOENT'});
  await missing.handle.closed;
  const file = path.join(root, 'file');
  fs.writeFileSync(file, 'x');
  const regular = observe(engine, file);
  await assert.rejects(regular.handle.ready, {code: 'ENOTDIR'});
  await regular.handle.closed;
  assert.equal(regular.messages[0].type, 'error');
  assert.equal(regular.messages[0].error.path, file);
  assert.ok(regular.messages[0].error.backend);
});

test('dispose before readiness is synchronous, cancellable and idempotent', async (t) => {
  const {root, engine} = fixture(t);
  const watch = observe(engine, root, true);
  watch.handle.dispose();
  watch.handle.dispose();
  await assert.rejects(watch.handle.ready, {
    name: 'AbortError',
    code: 'ABORT_ERR',
  });
  await watch.handle.closed;
  assert.deepEqual(watch.messages, []);
});

test('dispose suppresses already queued notifications and closed drains native work', async (t) => {
  const {root, engine} = fixture(t);
  const watch = observe(engine, root);
  await watch.handle.ready;
  for (let i = 0; i < 100; ++i)
    fs.writeFileSync(path.join(root, `burst-${i}`), 'x');
  watch.handle.dispose();
  const count = watch.messages.length;
  await watch.handle.closed;
  await delay(100);
  assert.equal(watch.messages.length, count);
  fs.rmSync(root, {recursive: true});
  fs.mkdirSync(root);
});

test('engine close drains active and starting sources and rejects further work', async (t) => {
  const {root, engine} = fixture(t);
  const first = observe(engine, root);
  await first.handle.ready;
  const second = observe(engine, root, true);
  const closed = engine.close();
  assert.equal(engine.close(), closed);
  await closed;
  await Promise.all([first.handle.closed, second.handle.closed]);
  await assert.rejects(second.handle.ready, {name: 'AbortError'});
  assert.throws(() => observe(engine, root), {code: 'ERR_ENGINE_CLOSED'});
});

test(
  'closed engines release Linux control descriptors even while their JS objects are retained',
  {skip: process.platform !== 'linux'},
  async () => {
    const count = () => fs.readdirSync('/proc/self/fd').length;
    const before = count();
    const retained = [];
    for (let i = 0; i < 50; ++i) {
      const engine = createEngine();
      retained.push(engine);
      await engine.close();
    }
    assert.ok(
      count() <= before + 2,
      `Descriptors increased from ${before} to ${count()}`,
    );
    assert.equal(retained.length, 50);
  },
);

test(
  'Linux closes its engine while independent producers continue flooding events',
  {skip: process.platform !== 'linux', timeout: 20000},
  async (t) => {
    const root = fs.mkdtempSync(
      path.join(fs.realpathSync(os.tmpdir()), 'lumine-watch-flood-'),
    );
    const engine = createEngine();
    const producers = [];
    t.after(async () => {
      await Promise.all(producers.map((producer) => producer.terminate()));
      await engine.close();
      fs.rmSync(root, {recursive: true, force: true});
    });
    const handle = engine.watchDirectory(root, {recursive: false}, () => {});
    await handle.ready;
    const readiness = [];
    for (let index = 0; index < 2; ++index) {
      const producer = new Worker(
        `const fs = require('node:fs');
         const path = require('node:path');
         const {parentPort, workerData} = require('node:worker_threads');
         const descriptors = Array.from({length: 64}, (_, file) =>
           fs.openSync(path.join(workerData.root, workerData.index + '-' + file), 'w'));
         const content = Buffer.from('x');
         parentPort.postMessage('ready');
         while (true) {
           for (const descriptor of descriptors)
             fs.writeSync(descriptor, content, 0, 1, 0);
         }`,
        {eval: true, workerData: {root, index}},
      );
      producers.push(producer);
      readiness.push(
        new Promise((resolve, reject) => {
          producer.once('message', resolve);
          producer.once('error', reject);
        }),
      );
    }
    await Promise.all(readiness);
    await delay(10);
    let timeout;
    try {
      await Promise.race([
        engine.close(),
        new Promise((_, reject) => {
          timeout = setTimeout(
            () =>
              reject(new Error('Engine close starved behind event producers')),
            5000,
          );
        }),
      ]);
      await handle.closed;
      assert.ok(producers.every((producer) => producer.threadId !== -1));
    } finally {
      clearTimeout(timeout);
    }
  },
);

test('deleted native roots explicitly invalidate instead of remaining apparently active', async (t) => {
  const {root, engine} = fixture(t);
  const directory = path.join(root, 'watched');
  fs.mkdirSync(directory);
  const watch = observe(engine, directory, true);
  await watch.handle.ready;
  fs.rmdirSync(directory);
  await until(
    () => watch.messages.some((message) => message.type === 'invalidate'),
    'root invalidation',
  );
  await watch.handle.closed;
});

test('Unicode and prototype-like names remain ordinary paths', async (t) => {
  const {root, engine} = fixture(t);
  const directory = path.join(root, '__proto__', 'zażółć-😀');
  fs.mkdirSync(directory, {recursive: true});
  const watch = observe(engine, directory);
  await watch.handle.ready;
  const file = path.join(directory, 'constructor');
  fs.writeFileSync(file, 'x');
  await until(() => watch.any(file), 'Unicode source path');
});

test('atomic replacement emits activity at the original path', async (t) => {
  const {root, engine} = fixture(t);
  const file = path.join(root, 'file');
  fs.writeFileSync(file, 'old');
  const watch = observe(engine, root);
  await watch.handle.ready;
  const temp = path.join(root, 'temp');
  fs.writeFileSync(temp, 'new');
  fs.renameSync(temp, file);
  await until(() => watch.any(file), 'atomic replacement');
  assert.equal(fs.readFileSync(file, 'utf8'), 'new');
});

test('multiple N-API environments close independently', async (t) => {
  const {root, engine} = fixture(t);
  const watch = observe(engine, root);
  await watch.handle.ready;
  const worker = new Worker(
    `const {parentPort}=require('node:worker_threads'); const e=require(${JSON.stringify(path.resolve(__dirname, '..'))}).createEngine(); const h=e.watchDirectory(${JSON.stringify(root)}, {recursive:true},()=>{}); h.ready.then(()=>parentPort.postMessage('ready'));`,
    {eval: true},
  );
  t.after(() => worker.terminate());
  await new Promise((resolve, reject) => {
    worker.once('message', resolve);
    worker.once('error', reject);
  });
  await worker.terminate();
  const file = path.join(root, 'after-worker');
  fs.writeFileSync(file, 'x');
  await until(
    () => watch.any(file),
    'main environment survived worker termination',
  );
});

test('rejects malformed API inputs without allocating sources', async (t) => {
  const {root, engine} = fixture(t);
  for (const value of ['', null, 42, 'bad\0path'])
    assert.throws(() => engine.watchDirectory(value, {}, () => {}), TypeError);
  assert.throws(
    () => engine.watchDirectory(root, {recursive: 'yes'}, () => {}),
    TypeError,
  );
  assert.throws(() => engine.watchDirectory(root, {}, null), TypeError);
});

test('content hints distinguish writes from reads and preserve ambiguous chmod activity', async (t) => {
  const {root, engine} = fixture(t);
  const watch = observe(engine, root);
  await watch.handle.ready;
  const file = path.join(root, 'content');
  fs.writeFileSync(file, 'old');
  await until(() => watch.any(file), 'initial content');
  fs.appendFileSync(file, 'new');
  await until(
    () =>
      watch.messages.some((message) =>
        message.events?.some(
          (event) => event.path === file && event.contentChanged,
        ),
      ),
    'native content hint',
  );
  await delay(100);
  watch.messages.length = 0;
  fs.readFileSync(file);
  await delay(150);
  assert.equal(
    watch.messages.some((message) =>
      message.events?.some((event) => event.contentChanged),
    ),
    false,
  );
  fs.chmodSync(file, 0o400);
  try {
    await delay(150);
    // FSEvents may retain ItemModified on chmod. Changed ctime cannot prove
    // that no same-stamp write was coalesced with that permissions change.
    if (process.platform !== 'darwin') {
      assert.equal(
        watch.messages.some((message) =>
          message.events?.some((event) => event.contentChanged),
        ),
        false,
      );
    } else {
      await until(
        () => watch.has('updated', file),
        'permissions activity on the watched file',
      );
    }
    assert.equal(fs.readFileSync(file, 'utf8'), 'oldnew');
  } finally {
    fs.chmodSync(file, 0o600);
  }
});

test('recursive traversal does not follow directory symlinks outside its root', async (t) => {
  const {root, engine} = fixture(t);
  const watched = path.join(root, 'watched'),
    outside = path.join(root, 'outside');
  fs.mkdirSync(watched);
  fs.mkdirSync(outside);
  fs.symlinkSync(
    outside,
    path.join(watched, 'linked'),
    process.platform === 'win32' ? 'junction' : 'dir',
  );
  const watch = observe(engine, watched, true);
  await watch.handle.ready;
  fs.writeFileSync(path.join(outside, 'external'), 'x');
  await delay(150);
  assert.equal(
    watch.messages.some((message) =>
      message.events?.some((event) => event.path.endsWith('external')),
    ),
    false,
  );
});

test('case-only renames report the old and new spelling', async (t) => {
  const {root, engine} = fixture(t);
  const oldPath = path.join(root, 'case-name'),
    newPath = path.join(root, 'CASE-NAME');
  fs.writeFileSync(oldPath, 'x');
  const watch = observe(engine, root);
  await watch.handle.ready;
  fs.renameSync(oldPath, newPath);
  await until(
    () => watch.has('deleted', oldPath) && watch.has('created', newPath),
    'case-only rename pair',
  );
});

test('last-access reads on a pooled parent do not become content updates', async (t) => {
  const {root, engine} = fixture(t);
  const watch = observe(engine, root);
  await watch.handle.ready;
  const file = path.join(root, 'access-only');
  const old = new Date(Date.now() - 60000);
  fs.writeFileSync(file, 'seeded');
  fs.utimesSync(file, old, old);
  await until(() => watch.any(file), 'seeded pooled entry');
  await delay(100);
  watch.messages.length = 0;
  fs.readFileSync(file);
  await delay(200);
  assert.equal(
    watch.messages.some((message) =>
      message.events?.some(
        (event) => event.path === file && event.contentChanged,
      ),
    ),
    false,
  );
});

test('existing files rewritten with restored size and mtime retain a content hint', async (t) => {
  const {root, engine} = fixture(t);
  const file = path.join(root, 'same-stamp');
  const old = new Date(Date.now() - 60000);
  fs.writeFileSync(file, 'before');
  fs.utimesSync(file, old, old);
  const watch = observe(engine, root);
  await watch.handle.ready;
  fs.writeFileSync(file, 'after!');
  fs.utimesSync(file, old, old);
  await until(
    () =>
      watch.messages.some((message) =>
        message.events?.some(
          (event) => event.path === file && event.contentChanged,
        ),
      ),
    'same-stamp rewrite',
  );
});

test('same-stamp writes retain a content hint when access time also changes', async (t) => {
  const {root, engine} = fixture(t);
  const file = path.join(root, 'same-stamp-access');
  const old = new Date(Date.now() - 60000);
  fs.writeFileSync(file, 'before');
  fs.utimesSync(file, old, old);
  const watch = observe(engine, root);
  await watch.handle.ready;
  await delay(50);
  fs.writeFileSync(file, 'after!');
  fs.utimesSync(file, new Date(), old);
  await until(
    () =>
      watch.messages.some((message) =>
        message.events?.some(
          (event) => event.path === file && event.contentChanged,
        ),
      ),
    'same-stamp rewrite with changed access time',
  );
});

test(
  'macOS preserves same-stamp writes coalesced with permissions changes',
  {skip: process.platform !== 'darwin'},
  async (t) => {
    const {root, engine} = fixture(t);
    const file = path.join(root, 'same-stamp-permissions');
    const old = new Date(Date.now() - 60000);
    fs.writeFileSync(file, 'before');
    fs.chmodSync(file, 0o600);
    fs.utimesSync(file, old, old);
    const watch = observe(engine, root);
    await watch.handle.ready;
    await delay(50);
    fs.writeFileSync(file, 'after!');
    fs.utimesSync(file, old, old);
    fs.chmodSync(file, 0o400);
    try {
      await until(
        () =>
          watch.messages.some((message) =>
            message.events?.some(
              (event) => event.path === file && event.contentChanged,
            ),
          ),
        'same-stamp rewrite with changed permissions',
      );
    } finally {
      fs.chmodSync(file, 0o600);
    }
  },
);

test(
  'a stalled JS consumer gets bounded-queue invalidation and watching continues',
  {timeout: 30000},
  async (t) => {
    const {root, engine} = fixture(t);
    const watches = [observe(engine, root), observe(engine, root)];
    await Promise.all(watches.map((watch) => watch.handle.ready));
    // Both create and write notifications accumulate while JS cannot drain the
    // native queue. macOS may coalesce writes; exceed the cap with distinct paths.
    for (let i = 0; i < 10000; ++i)
      fs.writeFileSync(path.join(root, `burst-${i}`), 'x');
    // Lifecycle acknowledgements survive replacement of queued changes with
    // invalidations, including a source armed while those messages are pending.
    const starting = observe(engine, root);
    await starting.handle.ready;
    starting.handle.dispose();
    await starting.handle.closed;
    for (const watch of watches) {
      await until(
        () => watch.messages.some((message) => message.type === 'invalidate'),
        'queue overflow invalidation for each independent source',
      );
      if (process.platform === 'linux') {
        // An inotify kernel overflow invalidates and closes all sources sharing
        // its fd. A JS-queue overflow keeps each source armed; both are explicit.
        const nativeLoss = watch.messages.some(
          (message) => message.reason === 'native-overflow',
        );
        if (nativeLoss) {
          await watch.handle.closed;
          continue;
        }
      }
      const file = path.join(root, `after-overflow-${watches.indexOf(watch)}`);
      fs.writeFileSync(file, 'x');
      await until(() => watch.any(file), 'continued delivery after overflow');
      watch.handle.dispose();
      await watch.handle.closed;
    }
  },
);
