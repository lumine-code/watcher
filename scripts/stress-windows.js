'use strict';
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const args = process.argv.slice(2);
const iterations = Number(args[args.indexOf('--iterations') + 1]) || 40;
const {createEngine} = require('..');
async function main() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'watcher-stress-'));
  try {
    for (let iteration = 0; iteration < iterations; ++iteration) {
      const engine = createEngine();
      try {
        const handles = Array.from({length: 4}, (_, i) => {
          const directory = path.join(root, String(i));
          fs.mkdirSync(directory, {recursive: true});
          return engine.watchDirectory(
            directory,
            {recursive: i % 2 === 0},
            () => {},
          );
        });
        await Promise.all(handles.map((handle) => handle.ready));
        for (let i = 0; i < 200; ++i) {
          const file = path.join(root, String(i % 4), String(i));
          fs.writeFileSync(file, 'x');
          if (i % 3 === 0) fs.unlinkSync(file);
        }
        handles[0].dispose();
        await Promise.resolve();
        for (const handle of handles) handle.dispose();
        await Promise.all(handles.map((handle) => handle.closed));
      } finally {
        await engine.close();
      }
    }
  } finally {
    fs.rmSync(root, {recursive: true, force: true});
  }
  console.log(`stress complete: ${iterations} iterations without a crash`);
}
main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
