// Loads the ten outputs of one mode from a mount and prints a markdown row
// with how long it took. See README.md.
//
// Usage: node load.mjs <mountpoint> <mode>
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { pathToFileURL } from 'node:url';
import { createRequire } from 'node:module';

const [mnt, mode] = process.argv.slice(2);
const ext = mode === 'require' ? 'js' : 'mjs';
const files = Array.from({ length: 10 }, (_, i) => path.join(mnt, `${mode}${i}.${ext}`));
const url = f => pathToFileURL(f).href;
const readAll = () => Promise.all(files.map(f => fs.promises.readFile(f)));
const seconds = (from, to = performance.now()) => `${((to - from) / 1000).toFixed(1)}s`;

const modes = {
  static: ['Static `import` ×10 in one entry module', async () => {
    // Written outside the mount and imported, so all ten are in one graph.
    const entry = path.join(os.tmpdir(), `makebelieve-static-${process.pid}.mjs`);
    fs.writeFileSync(entry, files.map((f, i) => `import m${i} from ${JSON.stringify(url(f))};\n`).join('') +
        `export default [${files.map((_, i) => `m${i}`)}];\n`);
    try { return (await import(url(entry))).default; } finally { fs.rmSync(entry); }
  }],
  dynamic: ['`Promise.all` of 10 `import()` calls', async () =>
    (await Promise.all(files.map(f => import(url(f))))).map(m => m.default)],
  require: ['`require()` ×10', async () => files.map(createRequire(import.meta.url))],
  promises: ['`Promise.all` of 10 `fs.promises.readFile` calls', readAll],
  callback: ['10 callback-style `fs.readFile` calls at once', () => Promise.all(files.map(f =>
    new Promise((resolve, reject) => fs.readFile(f, (e, data) => e ? reject(e) : resolve(data)))))],
  pool16: ['`fs.promises.readFile` ×10 with `UV_THREADPOOL_SIZE=16`', readAll],
};

const start = performance.now();
let got, time;
if (mode === 'prefetch') {
  await readAll();
  const read = performance.now();
  got = (await Promise.all(files.map(f => import(url(f))))).map(m => m.default);
  time = `${seconds(start, read)} read + ${seconds(read)} import`;
  console.log(`| \`fs.promises.readFile\` ×10 first, then \`import()\` them all | ${time} |`);
} else {
  const [description, load] = modes[mode];
  got = await load();
  console.log(`| ${description} | ${seconds(start)} |`);
}
// Every file must have been read in full, not as the 1 byte reported before
// it is built.
const expected = fs.readFileSync(new URL(`./module.${ext}`, import.meta.url));
for (const g of got) {
  if (typeof g === 'number' ? g !== 1 : !expected.equals(g)) throw new Error(`${mode}: wrong content`);
}
