// Browser smoke for the built WASM bundle. Run with Node 20's
// --experimental-websocket (or Node 22+) and a Chrome/Chromium executable.
// It uses Chrome DevTools directly, so no browser automation package is needed.
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { access, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
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
    const locations = candidate.includes('/') ? [candidate]
      : (process.env.PATH || '').split(path.delimiter).map(dir => path.join(dir, candidate));
    for (const location of locations) {
      try { await access(location); return location; } catch { /* next */ }
    }
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
  // waitFor swallows a throw from its probe, so a Chrome that died used to
  // surface as a bare timeout with its stderr lost. Check the exit here, and
  // give a cold start on a loaded CI runner more than 15 s.
  let debugPort;
  try {
    debugPort = await waitFor(async () => {
      if (browser.exitCode !== null) return -1;
      return Number((await readFile(portFile, 'utf8')).split('\n')[0]);
    }, 'Chrome DevTools', 60000);
  } catch (error) {
    throw new Error(`${error.message}\nChrome stderr:\n${browserErrors}`);
  }
  if (debugPort === -1)
    throw new Error(`Chrome exited (${browser.exitCode}) before DevTools:\n${browserErrors}`);
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
  const base = `http://127.0.0.1:${server.address().port}/`;
  const navigate = async (suffix, disk, preset) => {
    const loaded = event('Page.loadEventFired');
    await send('Page.navigate', { url: `${base}?${disk ? 'disk=' + encodeURIComponent(disk) : 'nodisk=1'}&visit=${suffix}${preset ? '&preset=' + encodeURIComponent(preset) : ''}` });
    await loaded;
    await waitFor(() => evaluate(
      "document.getElementById('bootSplash')?.classList.contains('hidden')"),
    `POM2 first frame (${suffix})`);
  };

  // ImGui's coordinates are canvas pixels; CSS scales the canvas on small
  // displays. Include motion before buttons so the GLFW backend sees hover.
  const mouse = async (type, x, y) => {
    const rect = await evaluate(`(() => {
      const c = document.querySelector('canvas'), r = c.getBoundingClientRect();
      return {x:r.x, y:r.y, scale:r.width/c.width};
    })()`);
    await send('Input.dispatchMouseEvent', {type, x:rect.x+x*rect.scale,
      y:rect.y+y*rect.scale, button:'left', clickCount:1});
  };
  const click = async (x, y) => {
    for (const type of ['mouseMoved', 'mousePressed', 'mouseReleased']) await mouse(type, x, y);
    await sleep(150);
  };
  const checkpoint = () => evaluate(`(() => {
    const fs = Module.FS;
    const marker = fs.readFile('/persistent/browser-resume.enabled', {encoding:'utf8'});
    const index = marker.trim().split('\\n')[1];
    const file = index === '0' ? 'browser-resume.pom2session' : 'browser-resume-next.pom2session';
    const bytes = fs.readFile('/persistent/' + file);
    return {marker, version:bytes[8], bytes:Array.from(bytes)};
  })()`);

  await navigate('write');
  assert.equal(await evaluate("typeof Module.FS?.syncfs"), 'function');
  // Exercise a real browser chooser and File.arrayBuffer -> MEMFS -> C++
  // callback. DevTools supplies a local fixture instead of user interaction.
  await send('Page.setInterceptFileChooserDialog', { enabled: true });
  const listing = path.join(profile, 'listing.txt');
  await writeFile(listing, '10 PRINT "POM2"\r\n20 END\r\n');
  const opened = event('Page.fileChooserOpened');
  await send('Runtime.evaluate', { expression: 'Module.pom2ChooseFile(0)', userGesture: true });
  const chooser = await opened;
  await send('DOM.setFileInputFiles', { files: [listing], backendNodeId: chooser.backendNodeId });
  await waitFor(() => evaluate('!Module.pom2FilePickerActive'), 'text file upload');
  assert.equal(await evaluate("Module.FS.readFile('/uploads/listing.txt', {encoding:'utf8'})"),
    '10 PRINT "POM2"\r\n20 END\r\n');
  assert.equal(await evaluate("document.querySelectorAll('input[type=file]').length"), 0);
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
  // Enable the real UI option on the browser's default //c + 4c topology.
  await click(20, 10); // File
  await mouse('mouseMoved', 90, 78); // Automatic resume
  await sleep(600);
  await click(400, 78); // Resume this machine on reload
  await waitFor(() => evaluate("Module.FS.analyzePath('/persistent/browser-resume.enabled').exists"),
    '//c automatic checkpoint');
  assert.equal((await checkpoint()).version, 7);
  // A manually paused session must stay paused after a real tab reload.
  await click(110, 10); // Machine
  await click(150, 58); // Pause
  await waitFor(() => evaluate('Module._pom2_browser_running() === 0'), 'manual pause');
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

  const expectedCheckpoint = await checkpoint();
  assert.match(expectedCheckpoint.marker, /^stopped\n[01]\n$/);
  // Start a different profile: automatic resume must reconstruct the //c.
  await navigate('read', null, 'ii+');
  await waitFor(() => evaluate('Module._pom2_browser_running() === 0'), '//c paused session restore');
  await evaluate('Module._pom2_persist_now()');
  const restoredCheckpoint = await checkpoint();
  assert(Buffer.from(restoredCheckpoint.bytes).equals(Buffer.from(expectedCheckpoint.bytes)),
    '//c CPU/RAM/card state changed across cross-profile automatic resume');
  const saved = await evaluate(
    "Module.FS.readFile('/persistent/state.cfg', { encoding: 'utf8' })");
  assert(saved.includes(`wasm_smoke_setting=${marker}`),
    'setting disappeared after page reload');
  // Exercise a writable physical floppy, then restore solely from IndexedDB:
  // the original image is deleted before reloading the paused checkpoint.
  await click(20, 10);
  await mouse('mouseMoved', 90, 78);
  await sleep(600);
  await click(400, 78); // disable the previous empty-machine checkpoint
  assert.equal(await evaluate("Module.FS.analyzePath('/persistent/browser-resume.enabled').exists"), false);
  await evaluate(`(() => {
    Module.FS.writeFile('/persistent/session-writable.nib', new Uint8Array(35*6656).fill(0xAA));
    Module.pom2StateDirty = true;
  })()`);
  await waitFor(() => evaluate('!Module.pom2FlushInFlight && !Module.pom2StateDirty'), 'writable fixture persistence');
  await navigate('writable', '/persistent/session-writable.nib');
  // Positional media boot is deferred for 30 frames after the splash.
  await waitFor(() => evaluate(`(() => {
    Module._pom2_persist_now();
    return Module.FS.readFile('/persistent/state.cfg', {encoding:'utf8'}).includes('/persistent/session-writable.nib');
  })()`), 'positional floppy mount');
  await click(110, 10); await click(150, 58);
  await waitFor(() => evaluate('Module._pom2_browser_running() === 0'), 'writable floppy pause');
  await click(20, 10); await mouse('mouseMoved', 90, 78); await sleep(600); await click(400, 78);
  await waitFor(() => evaluate("Module.FS.analyzePath('/persistent/browser-resume.enabled').exists"),
    'writable floppy checkpoint');
  await evaluate('Module._pom2_persist_now()');
  const writable = await checkpoint();
  assert.match(writable.marker, /^stopped\n[01]\n$/);
  const archive = Buffer.from(writable.bytes);
  let pos = 20;
  const skip = () => { const length = archive.readUInt32LE(pos); pos += 4; const start = pos; pos += length; return {start, length}; };
  skip(); skip(); // configuration and CPU/RAM core
  if (archive.readUInt32LE(8) >= 7) skip(); // embedded cassette
  let media;
  for (let slot = 0; slot < 8; ++slot) {
    const kind = archive[pos++]; skip(); const state = skip();
    if (kind === 1) {
      const runtimeLength = archive.readUInt32LE(state.start);
      const start = state.start + 4 + runtimeLength + 4;
      media = archive.subarray(start, start + 5);
    }
  }
  assert(media && media.subarray(0, 4).equals(Buffer.from([68,77,83,1])),
    'the writable Disk II did not enter the session archive');
  assert.equal(media[4], 4, 'the checkpoint did not preserve writable media policy');
  await evaluate(`(() => {
    Module.FS.unlink('/persistent/session-writable.nib');
    Module.pom2StateDirty = true;
  })()`);
  await waitFor(() => evaluate('!Module.pom2FlushInFlight && !Module.pom2StateDirty'), 'source removal persistence');
  await navigate('writable-resume');
  try {
    await waitFor(() => evaluate('Module._pom2_browser_running() === 0'), 'writable floppy restore', 15000);
  } catch (error) {
    await click(20, 10); await mouse('mouseMoved', 90, 78); await sleep(600);
    const shot = await send('Page.captureScreenshot', {format:'png'});
    await writeFile(path.join(os.tmpdir(), 'pom2-writable-resume-failure.png'), Buffer.from(shot.data, 'base64'));
    console.error(await evaluate("Module.FS.readFile('/persistent/browser-resume.enabled',{encoding:'utf8'})"));
    throw error;
  }
  assert.equal(await evaluate("Module.FS.analyzePath('/persistent/session-writable.nib').exists"), false);
  await evaluate('Module._pom2_persist_now()');
  assert(Buffer.from((await checkpoint()).bytes).equals(archive),
    'writable disk/CPU state changed across source-free browser resume');
  // Repeat with a SmartPort HDV. Its source is removed before reload too.
  await click(20, 10); await mouse('mouseMoved', 90, 78); await sleep(600); await click(400, 78);
  await evaluate(`(() => {
    const bytes = new Uint8Array(4*1024*1024); bytes[bytes.length-1] = 0xA6;
    Module.FS.writeFile('/persistent/session-block.hdv', bytes);
    Module.pom2StateDirty = true;
  })()`);
  await waitFor(() => evaluate('!Module.pom2FlushInFlight && !Module.pom2StateDirty'), 'HDV fixture persistence');
  await navigate('hdv', '/persistent/session-block.hdv');
  await waitFor(() => evaluate(`(() => {
    Module._pom2_persist_now();
    return Module.FS.readFile('/persistent/state.cfg', {encoding:'utf8'}).includes('/persistent/session-block.hdv');
  })()`), 'positional HDV mount');
  await click(110, 10); await click(150, 58);
  await waitFor(() => evaluate('Module._pom2_browser_running() === 0'), 'HDV pause');
  await click(20, 10); await mouse('mouseMoved', 90, 78); await sleep(600); await click(400, 78);
  await waitFor(() => evaluate("Module.FS.analyzePath('/persistent/browser-resume.enabled').exists"), 'HDV checkpoint');
  await evaluate('Module._pom2_persist_now()');
  const hdvCheckpoint = await checkpoint();
  assert.match(hdvCheckpoint.marker, /^stopped\n[01]\n$/);
  const hdvArchive = Buffer.from(hdvCheckpoint.bytes);
  assert(hdvArchive.includes(Buffer.from([66,77,83,1])), 'HDV sparse blocks missing from checkpoint');
  assert(hdvArchive.length < 1024*1024, 'sparse HDV was archived at full size');
  await evaluate(`(() => {
    Module.FS.unlink('/persistent/session-block.hdv'); Module.pom2StateDirty = true;
  })()`);
  await waitFor(() => evaluate('!Module.pom2FlushInFlight && !Module.pom2StateDirty'), 'HDV source removal');
  await navigate('hdv-resume');
  await waitFor(() => evaluate('Module._pom2_browser_running() === 0'), 'source-free HDV restore', 15000);
  await evaluate('Module._pom2_persist_now()');
  assert(Buffer.from((await checkpoint()).bytes).equals(hdvArchive),
    'HDV/CPU/SmartPort state changed across source-free browser resume');
  // Import through the actual File menu, not a preloaded virtual path.
  const importedDisk = path.join(profile, 'import.nib');
  await writeFile(importedDisk, Buffer.alloc(35 * 6656, 0xAA));
  await click(20, 10);
  const diskOpened = event('Page.fileChooserOpened');
  await click(115, 98); // Import disk and boot...
  const diskChooser = await diskOpened;
  await send('DOM.setFileInputFiles', { files: [importedDisk], backendNodeId: diskChooser.backendNodeId });
  await waitFor(() => evaluate("!Module.pom2FilePickerActive && Module.FS.analyzePath('/uploads/disk-1/import.nib').exists"),
    'browser disk upload');
  await waitFor(() => evaluate('Module._pom2_browser_running() === 1'), 'imported disk boot');
  assert.equal(await evaluate('Module._pom2_browser_running()'), 1, 'import did not boot the disk');
  assert.equal(await evaluate("Module.FS.stat('/uploads/disk-1/import.nib').size"), 35 * 6656);
  assert.equal(await evaluate("document.querySelectorAll('input[type=file]').length"), 0);
  await click(110, 10); await click(150, 58); // Pause before inserting drive 2.
  await waitFor(() => evaluate('Module._pom2_browser_running() === 0'), 'imported disk pause');
  await writeFile(importedDisk, Buffer.alloc(35 * 6656, 0xFF));
  const driveOpened = event('Page.fileChooserOpened');
  await click(850, 360); // Empty second Disk //c drive picture.
  const driveChooser = await driveOpened;
  await send('DOM.setFileInputFiles', { files: [importedDisk], backendNodeId: driveChooser.backendNodeId });
  await waitFor(() => evaluate(`(() => {
    Module._pom2_persist_now();
    return Module.FS.readFile('/persistent/state.cfg', {encoding:'utf8'}).includes('disk_path_slot6_drive2=/uploads/disk-2/import.nib');
  })()`), 'browser drive-2 import and mount');
  assert.equal(await evaluate('Module._pom2_browser_running()'), 0, 'drive import rebooted a paused machine');
  assert.equal(await evaluate("Module.FS.readFile('/uploads/disk-1/import.nib')[0]"), 0xAA,
    'the second upload replaced the first disk');
  assert.equal(await evaluate("Module.FS.readFile('/uploads/disk-2/import.nib')[0]"), 0xFF);
  await evaluate('Module._pom2_persist_now()');
  const importedCheckpoint = Buffer.from((await checkpoint()).bytes);
  assert(importedCheckpoint.includes(Buffer.alloc(1024, 0xAA)), 'boot disk missing from session');
  assert(importedCheckpoint.includes(Buffer.alloc(1024, 0xFF)), 'second disk missing from session');
  console.log('WASM browser: disk import/boot, text upload, //c Mockingboard, tab pause/resume, persistence and exact cross-profile //c/floppy/HDV checkpoint reload passed');
} finally {
  socket?.close();
  if (browser && browser.exitCode === null && browser.signalCode === null) {
    const exited = new Promise(resolve => browser.once('exit', resolve));
    browser.kill();
    const forceExit = setTimeout(() => browser.kill('SIGKILL'), 2000);
    forceExit.unref();
    await exited;
    clearTimeout(forceExit);
  }
  server.close();
  await rm(profile, { recursive: true, force: true, maxRetries: 5,
    retryDelay: 200 });
}
