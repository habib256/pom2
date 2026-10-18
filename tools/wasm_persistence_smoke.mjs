// Browser smoke for the built WASM bundle. Run with Node 20's
// --experimental-websocket (or Node 22+) and a Chrome/Chromium executable.
// It uses Chrome DevTools directly, so no browser automation package is needed.
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { access, mkdtemp, readFile, rm } from 'node:fs/promises';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';

const bundle = path.resolve(process.argv[2] || 'wasm');
const files = new Map([
  ['/', ['index.html', 'text/html']],
  ['/index.html', ['index.html', 'text/html']],
  ['/POM2.js', ['POM2.js', 'text/javascript']],
  ['/POM2.wasm', ['POM2.wasm', 'application/wasm']],
  ['/POM2.data', ['POM2.data', 'application/octet-stream']],
]);
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
  const candidates = [process.env.CHROME_BIN, 'google-chrome', 'chromium',
    '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome'];
  for (const candidate of candidates.filter(Boolean)) {
    if (!candidate.includes('/')) return candidate; // resolved through PATH
    try { await access(candidate); return candidate; } catch { /* next */ }
  }
  throw new Error('Set CHROME_BIN to a Chrome or Chromium executable');
}

const server = http.createServer(async (request, response) => {
  const entry = files.get(new URL(request.url, 'http://localhost').pathname);
  if (!entry) { response.writeHead(404).end(); return; }
  try {
    const contents = await readFile(path.join(bundle, entry[0]));
    response.writeHead(200, { 'Content-Type': entry[1],
      'Content-Length': contents.length, 'Cache-Control': 'no-store' });
    response.end(contents);
  } catch (error) {
    response.writeHead(500).end(String(error));
  }
});

await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
const profile = await mkdtemp(path.join(os.tmpdir(), 'pom2-wasm-smoke-'));
let browser;
let socket;
try {
  browser = spawn(await chromeBinary(), [
    '--headless=new', '--no-first-run', '--no-default-browser-check',
    '--no-sandbox', '--enable-unsafe-swiftshader', '--use-gl=angle',
    '--use-angle=swiftshader', '--remote-debugging-port=0',
    `--user-data-dir=${profile}`, 'about:blank',
  ], { stdio: ['ignore', 'ignore', 'pipe'] });
  let browserErrors = '';
  browser.stderr.on('data', data => { browserErrors += String(data).slice(-1000); });
  const portFile = path.join(profile, 'DevToolsActivePort');
  const debugPort = await waitFor(async () => {
    if (browser.exitCode !== null) throw new Error(browserErrors);
    return Number((await readFile(portFile, 'utf8')).split('\n')[0]);
  }, 'Chrome DevTools', 15000);
  const pages = await (await fetch(`http://127.0.0.1:${debugPort}/json/list`)).json();
  const page = pages.find(item => item.type === 'page');
  assert(page, 'Chrome did not open a page target');
  socket = new WebSocket(page.webSocketDebuggerUrl);
  await new Promise((resolve, reject) => {
    socket.addEventListener('open', resolve, { once: true });
    socket.addEventListener('error', reject, { once: true });
  });

  let nextId = 0;
  const pending = new Map();
  const listeners = new Map();
  socket.addEventListener('message', ({ data }) => {
    const message = JSON.parse(data);
    if (message.id && pending.has(message.id)) {
      const { resolve, reject } = pending.get(message.id);
      pending.delete(message.id);
      message.error ? reject(new Error(message.error.message)) : resolve(message.result);
    } else if (listeners.has(message.method)) {
      for (const resolve of listeners.get(message.method)) resolve(message.params);
      listeners.delete(message.method);
    }
  });
  const send = (method, params = {}) => new Promise((resolve, reject) => {
    const id = ++nextId;
    pending.set(id, { resolve, reject });
    socket.send(JSON.stringify({ id, method, params }));
  });
  const event = method => new Promise(resolve => {
    const callbacks = listeners.get(method) || [];
    callbacks.push(resolve);
    listeners.set(method, callbacks);
  });
  const evaluate = async expression => {
    const result = await send('Runtime.evaluate', {
      expression, returnByValue: true, awaitPromise: true,
    });
    if (result.exceptionDetails) throw new Error(result.exceptionDetails.text);
    return result.result.value;
  };
  await send('Page.enable');
  await send('Runtime.enable');
  const base = `http://127.0.0.1:${server.address().port}/?nodisk=1`;
  const navigate = async suffix => {
    const loaded = event('Page.loadEventFired');
    await send('Page.navigate', { url: `${base}&visit=${suffix}` });
    await loaded;
    await waitFor(() => evaluate(
      "document.getElementById('bootSplash')?.classList.contains('hidden')"),
    `POM2 first frame (${suffix})`);
  };

  await navigate('write');
  assert.equal(await evaluate("typeof Module.FS?.syncfs"), 'function');
  assert.equal(await evaluate('Module._pom2_browser_running()'), 1,
    'the browser //c did not start running');
  await evaluate(`(() => {
    window.pom2TestVisibility = 'hidden';
    Object.defineProperty(document, 'visibilityState', {
      configurable: true, get: () => window.pom2TestVisibility,
    });
    document.dispatchEvent(new Event('visibilitychange'));
  })()`);
  assert.equal(await evaluate('Module._pom2_browser_running()'), 0,
    'switching away from the tab kept the guest running');
  await evaluate(`(() => {
    window.pom2TestVisibility = 'visible';
    document.dispatchEvent(new Event('visibilitychange'));
  })()`);
  assert.equal(await evaluate('Module._pom2_browser_running()'), 1,
    'returning to the tab did not resume the guest');
  await evaluate("window.dispatchEvent(new Event('pagehide'))");
  assert.equal(await evaluate('Module._pom2_browser_running()'), 0,
    'the hidden page kept running');
  await evaluate("window.dispatchEvent(new Event('pageshow'))");
  assert.equal(await evaluate('Module._pom2_browser_running()'), 1,
    'the visible page did not resume');
  const initialSettings = await evaluate(
    "Module.FS.readFile('/persistent/state.cfg', { encoding: 'utf8' })");
  assert.match(initialSettings, /^iic_expansion_card=mockingboard$/m,
    'the browser //c has no Mockingboard 4c on its internal connector');
  const marker = `wasm_smoke_${Date.now()}`;
  const beforeFlush = await evaluate(`(() => {
    const fs = Module.FS;
    // The lifecycle hook saves the live Settings map and can replace this
    // file. Finish that save before simulating the next setting write.
    Module._pom2_persist_now();
    const old = fs.readFile('/persistent/state.cfg', { encoding: 'utf8' });
    fs.writeFile('/persistent/state.cfg', old + '\\nwasm_smoke_setting=${marker}\\n');
    Module.pom2StateDirty = true;
    return fs.readFile('/persistent/state.cfg', { encoding: 'utf8' }).includes('wasm_smoke_setting=${marker}');
  })()`);
  assert(beforeFlush, 'setting disappeared before IDBFS flush');
  await waitFor(() => evaluate(
    '!Module.pom2FlushInFlight && !Module.pom2StateDirty && !!Module.pom2LastFlush'),
  'IDBFS flush');
  const afterFlush = await evaluate(
    `Module.FS.readFile('/persistent/state.cfg', { encoding: 'utf8' }).includes('wasm_smoke_setting=${marker}')`);
  assert(afterFlush, 'setting disappeared during IDBFS flush');

  await navigate('read');
  const saved = await evaluate(
    "Module.FS.readFile('/persistent/state.cfg', { encoding: 'utf8' })");
  assert(saved.includes(`wasm_smoke_setting=${marker}`),
    'setting disappeared after page reload');
  console.log('WASM browser: //c Mockingboard, tab pause/resume, persistence and reload passed');
} finally {
  socket?.close();
  if (browser && browser.exitCode === null) {
    browser.kill();
    await new Promise(resolve => browser.once('exit', resolve));
  }
  server.close();
  await rm(profile, { recursive: true, force: true, maxRetries: 5,
    retryDelay: 200 });
}
