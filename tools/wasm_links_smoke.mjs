// Browser smoke for the page's link features, in Chrome over DevTools.
// Run with Node 20's --experimental-websocket (or Node 22+):
//   node --experimental-websocket tools/wasm_links_smoke.mjs [bundle-dir]
//
// WASM-004: serves the bundle and two generated 140 KB images, opens
//   ?preset=iie&s6d1=<url 1>&s6d2=<url 2>
// and checks that disk 1 boots with disk 2 in drive 2 (the CLI's own log
// lines), then presses File ▸ Copy link (pom2_copy_setup_link) and checks the
// address bar now holds the same profile and the same two URLs.
//
// WASM-005: a host page frames ?embed=1 and must receive the `boot` message;
// a host page framing the same page WITHOUT embed=1 must receive nothing.
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { access, mkdtemp, readFile, rm } from 'node:fs/promises';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';

const bundle = path.resolve(process.argv[2] || 'wasm');
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
async function waitFor(fn, label, timeout = 90000) {
  const until = Date.now() + timeout;
  while (Date.now() < until) {
    const value = await fn().catch(() => null);
    if (value) return value;
    await sleep(250);
  }
  throw new Error(`Timed out waiting for ${label}`);
}
async function chromeBinary() {
  for (const c of [process.env.CHROME_BIN, '/usr/bin/google-chrome', '/usr/bin/chromium',
                   '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome'].filter(Boolean)) {
    try { await access(c); return c; } catch { /* next */ }
  }
  throw new Error('Set CHROME_BIN to a Chrome or Chromium executable');
}

// Two distinguishable 143 360-byte images (a .dsk of that size is always a
// valid image: the loader nibblizes whatever it holds).
const disk = fill => Buffer.alloc(143360, fill);
const files = new Map([
  ['/', ['index.html', 'text/html']], ['/index.html', ['index.html', 'text/html']],
  ['/POM2.js', ['POM2.js', 'text/javascript']], ['/POM2.wasm', ['POM2.wasm', 'application/wasm']],
  ['/POM2.data', ['POM2.data', 'application/octet-stream']],
]);
const disks = new Map([['/side1.dsk', disk(0x11)], ['/side2.dsk', disk(0x22)]]);
// A host page for the embed check: it frames the emulator and keeps every
// message the frame posts.
const hostPage = query => Buffer.from(`<!doctype html><meta charset="utf-8">
<script>window.events = [];
addEventListener('message', e => { if (e.data && e.data.source === 'pom2') events.push(e.data); });</script>
<iframe id="f" src="/index.html${query}" width="800" height="600"></iframe>`);
const pages = new Map([['/host-embed.html', hostPage('?embed=1&nodisk=1&preset=ii%2B')],
                       ['/host-plain.html', hostPage('?nodisk=1&preset=ii%2B')]]);
const server = http.createServer(async (request, response) => {
  const name = new URL(request.url, 'http://localhost').pathname;
  if (pages.has(name)) {
    response.writeHead(200, { 'Content-Type': 'text/html' });
    response.end(pages.get(name));
    return;
  }
  if (disks.has(name)) {
    response.writeHead(200, { 'Content-Type': 'application/octet-stream',
                              'Content-Length': disks.get(name).length });
    response.end(disks.get(name));
    return;
  }
  const entry = files.get(name);
  if (!entry) { response.writeHead(404).end(); return; }
  const contents = await readFile(path.join(bundle, entry[0]));
  response.writeHead(200, { 'Content-Type': entry[1], 'Content-Length': contents.length });
  response.end(contents);
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
const base = `http://127.0.0.1:${server.address().port}/`;
const profile = await mkdtemp(path.join(os.tmpdir(), 'pom2-link-smoke-'));
let browser, socket;
try {
  browser = spawn(await chromeBinary(), [
    '--headless=new', '--window-size=1280,900', '--no-first-run', '--no-default-browser-check',
    '--no-sandbox', '--enable-unsafe-swiftshader', '--use-gl=angle', '--use-angle=swiftshader',
    '--remote-debugging-port=0', `--user-data-dir=${profile}`, 'about:blank',
  ], { stdio: ['ignore', 'ignore', 'ignore'] });
  const port = await waitFor(async () =>
    Number((await readFile(path.join(profile, 'DevToolsActivePort'), 'utf8')).split('\n')[0]),
    'Chrome DevTools', 60000);
  const page = (await (await fetch(`http://127.0.0.1:${port}/json/list`)).json())
    .find(item => item.type === 'page');
  socket = new WebSocket(page.webSocketDebuggerUrl);
  await new Promise((resolve, reject) => {
    socket.addEventListener('open', resolve, { once: true });
    socket.addEventListener('error', reject, { once: true });
  });
  let nextId = 0;
  const pending = new Map();
  const consoleLines = [];
  socket.addEventListener('message', ({ data }) => {
    const m = JSON.parse(data);
    if (m.id && pending.has(m.id)) {
      const { resolve, reject } = pending.get(m.id);
      pending.delete(m.id);
      m.error ? reject(new Error(m.error.message)) : resolve(m.result);
    } else if (m.method === 'Runtime.consoleAPICalled') {
      consoleLines.push(m.params.args.map(a => a.value ?? '').join(' '));
    }
  });
  const send = (method, params = {}) => new Promise((resolve, reject) => {
    const id = ++nextId;
    pending.set(id, { resolve, reject });
    socket.send(JSON.stringify({ id, method, params }));
  });
  const evaluate = async expression => {
    const r = await send('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true });
    if (r.exceptionDetails) throw new Error(r.exceptionDetails.text);
    return r.result.value;
  };
  await send('Page.enable');
  await send('Runtime.enable');

  const one = base + 'side1.dsk', two = base + 'side2.dsk';
  const query = `?preset=iie&s6d1=${encodeURIComponent(one)}&s6d2=${encodeURIComponent(two)}`;
  await send('Page.navigate', { url: base + query });
  const has = needle => consoleLines.some(l => l.includes(needle));
  await waitFor(async () => has('--mount s6d2:'), 'the drive-2 mount', 120000);
  assert(has('booted disk: /uploads/url/s6d1/side1.dsk'), 'disk 1 is the one booted');
  assert(has('--mount s6d2: /uploads/url/s6d2/side2.dsk'), 'disk 2 is in drive 2');
  assert(!consoleLines.some(l => l.includes('--mount') && l.includes('failed')), 'no mount failed');

  // The Copy link button: the address bar gets the same setup back.
  // The address bar is emptied first: the copied link must put it back,
  // and for this setup it is the very link the page was opened with.
  await evaluate("history.replaceState(null, '', '?cleared=1')");
  await evaluate("Module.ccall('pom2_copy_setup_link', null, [], [])");
  const search = await waitFor(async () => {
    const s = await evaluate('window.location.search');
    return s.includes('s6d2') ? s : null;
  }, 'the copied link', 10000);
  assert.equal(search, query, 'the copied link is the one the page was opened with');
  const params = new URLSearchParams(search);
  assert.equal(params.get('preset'), 'iie');
  assert.equal(params.get('s6d1'), one);
  assert.equal(params.get('s6d2'), two);
  console.log(`wasm_links_smoke: two-disk link booted disk 1, mounted disk 2, Copy link -> ${search}`);

  // WASM-005: the embedded page says it booted; a plain one says nothing.
  const frameRunning = "(() => { const w = document.getElementById('f').contentWindow; " +
                       "return !!(w && w.Module && w.Module._pom2_browser_running && " +
                       "w.Module._pom2_browser_running()); })()";
  await send('Page.navigate', { url: base + 'host-embed.html' });
  const events = await waitFor(async () => {
    const e = await evaluate('JSON.stringify(window.events || [])');
    return JSON.parse(e).some(m => m.event === 'boot') ? JSON.parse(e) : null;
  }, 'the embed boot message', 120000);
  const boot = events.find(m => m.event === 'boot');
  assert.equal(boot.detail.preset, 'ii+');
  assert(!events.some(m => m.event === 'error'), 'no error posted: ' + JSON.stringify(events));
  assert(await evaluate("!!document.getElementById('f').contentWindow.document.getElementById('repoLink').classList.contains('hidden')"),
         'embed hides the page chrome');
  await send('Page.navigate', { url: base + 'host-plain.html' });
  await waitFor(() => evaluate(frameRunning), 'the plain frame running', 120000);
  await sleep(1500);
  assert.equal(await evaluate('window.events.length'), 0, 'nothing is posted without embed=1');
  console.log(`wasm_links_smoke: embed posted ${events.map(m => m.event).join(', ')}; plain frame posted nothing`);
} finally {
  if (socket) socket.close();
  if (browser) browser.kill('SIGKILL');
  server.close();
  await rm(profile, { recursive: true, force: true }).catch(() => {});
}
