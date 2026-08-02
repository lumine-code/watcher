'use strict';
// Windows subscribe/unsubscribe churn stress for the teardown use-after-free
// fixed in this fork. On a vulnerable build the process dies with
// 0xC0000005 (exit code 3221225477); on a fixed build it exits 0 after all
// iterations. A JavaScript failure exits 2 so the two are distinguishable.
//
// Usage:
//   node scripts/stress-windows.js [--target <package-dir>] [--iterations N]
//
// Drive it repeatedly, counting crashes:
//   for i in $(seq 1 15); do node scripts/stress-windows.js || echo "DIED ($?)"; done
//
// --target lets the same harness run against another checkout (e.g. the
// upstream prebuilt in a consumer's node_modules) for a baseline.

const fs = require('fs');
const os = require('os');
const path = require('path');

const args = process.argv.slice(2);
function arg(name, dflt) {
  const at = args.indexOf(name);
  return at === -1 ? dflt : args[at + 1];
}

const target = arg('--target', path.join(__dirname, '..'));
const iterations = Number(arg('--iterations', 40));
const watcher = require(target);

const DIRS_PER_ITER = 4;

function makeTree(root, i) {
  const dir = fs.mkdtempSync(path.join(root, `stress-${i}-`));
  for (let d = 0; d < 3; d++) {
    const sub = path.join(dir, `sub-${d}`);
    fs.mkdirSync(sub);
    for (let f = 0; f < 5; f++)
      fs.writeFileSync(path.join(sub, `f${f}.txt`), 'x');
  }
  return dir;
}

// Synchronous write burst: queues ReadDirectoryChangesW completions that race
// the CancelIo issued during teardown.
function burst(dir, n) {
  for (let i = 0; i < n; i++) {
    const p = path.join(dir, `sub-${i % 3}`, `burst-${i}.txt`);
    try {
      fs.writeFileSync(p, String(i));
      if (i % 3 === 0) fs.unlinkSync(p);
    } catch {
      // The directory may be mid-teardown in a later phase; irrelevant here.
    }
  }
}

async function main() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'watcher-stress-'));
  const noop = () => {};

  for (let iter = 0; iter < iterations; iter++) {
    const dirs = [];
    for (let i = 0; i < DIRS_PER_ITER; i++) dirs.push(makeTree(root, i));

    const subs = await Promise.all(
      dirs.map((dir) => watcher.subscribe(dir, noop, {backend: 'windows'})),
    );

    // Keep events flowing while tearing down.
    for (const dir of dirs) burst(dir, 25);

    // Mixed teardown timing: immediate, after a microtask, after short
    // delays — all while bursts are still completing in the kernel. The last
    // unsubscribe drops the backend's watcher count to zero, destroying the
    // backend and joining its thread: the exact shutdown-storm window that
    // crashed the Lumine watcher worker.
    await subs[0].unsubscribe();
    const rest = subs.slice(1).map(async (sub, i) => {
      if (i % 2) await new Promise((resolve) => setTimeout(resolve, i * 3));
      burst(dirs[i + 1], 10);
      await sub.unsubscribe();
    });
    await Promise.all(rest);

    for (const dir of dirs) fs.rmSync(dir, {recursive: true, force: true});
    if (iter % 10 === 9) console.log(`iteration ${iter + 1}/${iterations}`);
  }

  fs.rmSync(root, {recursive: true, force: true});
  console.log('stress complete: no crash');
}

main().catch((err) => {
  console.error(err);
  process.exit(2);
});
