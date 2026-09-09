'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const {test} = require('node:test');
const {createEngine} = require('..');

const pause = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
async function until(predicate, label) {
  const deadline = Date.now() + 7000;
  while (!predicate()) {
    if (Date.now() > deadline) throw new Error(`Timed out: ${label}`);
    await pause(10);
  }
}
function fixture(t) {
  const root = fs.mkdtempSync(
    path.join(fs.realpathSync(os.tmpdir()), 'lumine-guard-'),
  );
  const engine = createEngine();
  t.after(async () => {
    await engine.close();
    fs.rmSync(root, {recursive: true, force: true});
  });
  return {root, engine};
}
function observe(engine, directory) {
  const messages = [];
  const handle = engine.watchDirectory(
    directory,
    {recursive: false, guard: true},
    (message) => messages.push(message),
  );
  return {handle, messages};
}

test('directory guards report unnamed membership activity', async (t) => {
  const {root, engine} = fixture(t);
  const {handle, messages} = observe(engine, root);
  await handle.ready;
  const child = path.join(root, 'entry');
  fs.mkdirSync(child);
  await until(
    () => messages.some((message) => message.type === 'guard'),
    'guard creation',
  );
  messages.length = 0;
  fs.rmdirSync(child);
  await until(
    () => messages.some((message) => message.type === 'guard'),
    'guard deletion',
  );
  assert.equal(
    messages.some((message) => message.type === 'changes'),
    false,
  );
});

test('directory guards invalidate and close when their root is removed', async (t) => {
  const {root, engine} = fixture(t);
  const directory = path.join(root, 'guarded');
  fs.mkdirSync(directory);
  const {handle, messages} = observe(engine, directory);
  await handle.ready;
  fs.rmdirSync(directory);
  await until(
    () =>
      messages.some(
        (message) =>
          message.type === 'invalidate' && message.reason === 'root-changed',
      ),
    'guard root removal',
  );
  await handle.closed;
});

test('directory guards do not observe grandchild content writes', async (t) => {
  const {root, engine} = fixture(t);
  const file = path.join(root, 'child', 'file');
  fs.mkdirSync(path.dirname(file));
  fs.writeFileSync(file, 'before');
  const {handle, messages} = observe(engine, root);
  await handle.ready;
  fs.appendFileSync(file, 'after');
  await pause(200);
  assert.deepEqual(messages, []);
});

test('guard cancellation suppresses startup and queued callbacks', async (t) => {
  const {root, engine} = fixture(t);
  const {handle, messages} = observe(engine, root);
  handle.dispose();
  handle.dispose();
  await assert.rejects(handle.ready, {name: 'AbortError'});
  await handle.closed;
  assert.deepEqual(messages, []);
});

test(
  'macOS guards exclude even immediate file content changes',
  {skip: process.platform !== 'darwin'},
  async (t) => {
    const {root, engine} = fixture(t);
    const file = path.join(root, 'file');
    fs.writeFileSync(file, 'before');
    const {handle, messages} = observe(engine, root);
    await handle.ready;
    fs.appendFileSync(file, 'after');
    await pause(200);
    assert.deepEqual(messages, []);
  },
);

test(
  'macOS guard ancestor movement invalidates fixed directory locations',
  {skip: process.platform !== 'darwin'},
  async (t) => {
    const {root, engine} = fixture(t);
    const ancestor = path.join(root, 'ancestor');
    const directory = path.join(ancestor, 'middle', 'guarded');
    fs.mkdirSync(directory, {recursive: true});
    const {handle, messages} = observe(engine, directory);
    await handle.ready;
    fs.renameSync(ancestor, path.join(root, 'moved'));
    await until(
      () =>
        messages.some(
          (message) =>
            message.type === 'invalidate' && message.reason === 'root-changed',
        ),
      'guard ancestor movement',
    );
    await handle.closed;
  },
);

test(
  'macOS guards share ancestor ownership without closing other guards',
  {skip: process.platform !== 'darwin'},
  async (t) => {
    const {root, engine} = fixture(t);
    const first = path.join(root, 'first'),
      second = path.join(root, 'second');
    fs.mkdirSync(first);
    fs.mkdirSync(second);
    const a = observe(engine, first),
      b = observe(engine, second);
    await Promise.all([a.handle.ready, b.handle.ready]);
    a.handle.dispose();
    await a.handle.closed;
    fs.writeFileSync(path.join(second, 'new'), 'x');
    await until(
      () => b.messages.some((message) => message.type === 'guard'),
      'surviving guard',
    );
    assert.deepEqual(a.messages, []);
  },
);

test(
  'macOS guards deliver when their queue is created above descriptor 2048',
  {skip: process.platform !== 'darwin', timeout: 20000},
  async (t) => {
    const {root, engine} = fixture(t);
    const descriptors = [];
    const releaseDescriptors = () => {
      for (const descriptor of descriptors.splice(0)) fs.closeSync(descriptor);
    };
    t.after(releaseDescriptors);
    // The guard queue is lazy. Force its first descriptor above select's usual
    // fd_set range without relying on asynchronous FSEvents teardown timing.
    while ((descriptors.at(-1) ?? -1) < 2048)
      descriptors.push(fs.openSync('/dev/null', 'r'));
    const {handle, messages} = observe(engine, root);
    await handle.ready;
    const child = path.join(root, 'entry');
    fs.mkdirSync(child);
    await until(
      () => messages.some((message) => message.type === 'guard'),
      'membership with a high-numbered guard queue',
    );
    releaseDescriptors();
    messages.length = 0;
    fs.rmdirSync(child);
    await until(
      () => messages.some((message) => message.type === 'guard'),
      'high-numbered guard queue after unrelated descriptors close',
    );
  },
);

test('guards reject recursive or non-boolean guard options', async (t) => {
  const {root, engine} = fixture(t);
  assert.throws(
    () => engine.watchDirectory(root, {guard: true, recursive: true}, () => {}),
    TypeError,
  );
  assert.throws(
    () => engine.watchDirectory(root, {guard: 'yes'}, () => {}),
    TypeError,
  );
});

test(
  'persistent unrelated sources survive repeated guard and content handoffs',
  {timeout: 30000},
  async (t) => {
    const {root, engine} = fixture(t);
    const profile = path.join(root, 'profile');
    fs.mkdirSync(profile);
    const persistent = engine.watchDirectory(
      profile,
      {recursive: true},
      () => {},
    );
    const persistentGuard = observe(engine, profile);
    await Promise.all([persistent.ready, persistentGuard.handle.ready]);
    const parent = path.join(root, 'working');
    fs.mkdirSync(parent);
    const file = path.join(parent, 'file');
    for (let iteration = 0; iteration < 16; ++iteration) {
      fs.writeFileSync(file, 'original');
      let messages = [];
      const initial = engine.watchDirectory(
        parent,
        {recursive: false},
        (message) => messages.push(message),
      );
      await initial.ready;
      fs.unlinkSync(file);
      await until(
        () =>
          messages.some((message) =>
            message.events?.some(
              (event) => event.path === file && event.action === 'deleted',
            ),
          ),
        `iteration ${iteration} deletion`,
      );
      const guard = observe(engine, parent);
      await guard.handle.ready;
      initial.dispose();
      await initial.closed;
      fs.writeFileSync(file, 'recreated');
      await until(
        () => guard.messages.some((message) => message.type === 'guard'),
        `iteration ${iteration} recreation guard`,
      );
      messages = [];
      const restored = engine.watchDirectory(
        parent,
        {recursive: false},
        (message) => messages.push(message),
      );
      await restored.ready;
      guard.handle.dispose();
      await guard.handle.closed;
      fs.writeFileSync(file, `later external write ${iteration}`);
      await until(
        () =>
          messages.some((message) =>
            message.events?.some(
              (event) => event.path === file && event.action === 'updated',
            ),
          ),
        `iteration ${iteration} restored content`,
      );
      restored.dispose();
      await restored.closed;
    }
  },
);
