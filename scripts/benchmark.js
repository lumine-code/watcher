'use strict';
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const {createEngine} = require('..');
async function main() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'watcher-benchmark-'));
  for (let d = 0; d < 100; ++d) {
    fs.mkdirSync(path.join(root, String(d)));
    for (let f = 0; f < 10; ++f)
      fs.writeFileSync(path.join(root, String(d), String(f)), 'x');
  }
  const rss = process.memoryUsage().rss,
    start = performance.now();
  const engine = createEngine();
  const callbacks = new Map();
  const watch = engine.watchDirectory(root, {recursive: true}, (message) => {
    if (message.type !== 'changes') return;
    for (const event of message.events) {
      const done = callbacks.get(event.path);
      if (done) {
        callbacks.delete(event.path);
        done();
      }
    }
  });
  try {
    await watch.ready;
    const readyMs = performance.now() - start;
    const idleStart = process.cpuUsage();
    await new Promise((resolve) => setTimeout(resolve, 2000));
    const idle = process.cpuUsage(idleStart);
    const latency = [];
    for (let i = 0; i < 20; ++i) {
      const file = path.join(root, `latency-${i}`),
        start = performance.now();
      await new Promise((resolve, reject) => {
        const timer = setTimeout(
          () => reject(new Error('Event delivery timed out')),
          7000,
        );
        callbacks.set(file, () => {
          clearTimeout(timer);
          resolve();
        });
        fs.writeFileSync(file, 'x');
      });
      latency.push(performance.now() - start);
    }
    const closeStart = performance.now();
    watch.dispose();
    await watch.closed;
    await engine.close();
    latency.sort((a, b) => a - b);
    console.log(
      JSON.stringify({
        node: process.version,
        platform: process.platform,
        directories: 100,
        files: 1000,
        readyMs,
        closeMs: performance.now() - closeStart,
        rssDelta: process.memoryUsage().rss - rss,
        idleCpuMs: (idle.user + idle.system) / 1000,
        latencyP50Ms: latency[10],
        latencyP95Ms: latency[19],
      }),
    );
  } finally {
    await engine.close();
    fs.rmSync(root, {recursive: true, force: true});
  }
}
main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
