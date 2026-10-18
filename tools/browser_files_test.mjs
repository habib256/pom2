// POM2 — GPL-3.0-or-later. Deterministic DOM/FS boundary checks.
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import vm from 'node:vm';

const created = [], calls = [], files = new Map(), revoked = [];
let pickerFailure = false;
const context = {
  Module: { ccall: (...args) => calls.push(Array.from(args.at(-1))) },
  document: {
    body: { appendChild() {} },
    createElement(tag) {
      const listeners = {};
      const element = { tag, style: {}, files: [], removed: false,
        addEventListener(event, handler) { listeners[event] = handler; },
        async fire(event) { await listeners[event](); },
        click() { if (pickerFailure) throw new Error('picker denied'); },
        remove() { this.removed = true; },
      };
      created.push(element);
      return element;
    },
  },
  FS: { mkdirTree() {}, writeFile(path, bytes) { files.set(path, [...bytes]); },
    readFile(path) { if (!files.has(path)) throw new Error('missing file'); return new Uint8Array(files.get(path)); },
  },
  URL: { createObjectURL() { return 'blob:test'; }, revokeObjectURL(url) { revoked.push(url); } },
  Blob, Uint8Array, console: { warn() {} }, setTimeout(fn) { fn(); },
};
vm.runInNewContext(await readFile(new URL('../wasm/browser_files.js', import.meta.url), 'utf8'), context);
const choose = context.Module.pom2ChooseFile;
const last = () => created.at(-1);
choose(0);
assert.equal(last().accept, '.txt,.bas,.a,text/plain');
const first = last();
choose(1);
assert.equal(last(), first, 'a second picker replaced an active selection');
first.files = [{ name: '../../escape', size: 3, async arrayBuffer() { return new Uint8Array([65, 13, 66]).buffer; } }];
await first.fire('change');
assert.deepEqual(files.get('/uploads/listing.txt'), [65, 13, 66]);
assert.deepEqual(calls.at(-1), [0, '/uploads/listing.txt', '']);
assert(first.removed && !context.Module.pom2FilePickerActive);
choose(1);
assert.equal(last().accept, '.pom2session');
last().files = [{ size: 32 * 1024 * 1024 + 1 }];
await last().fire('change');
assert.match(calls.at(-1)[2], /32 MiB/);
assert(!files.has('/uploads/import.pom2session'));
choose(0);
last().files = [{ size: 1024 * 1024 + 1 }];
await last().fire('change');
assert.match(calls.at(-1)[2], /1 MiB/);
choose(0);
await last().fire('cancel');
assert.deepEqual(calls.at(-1), [0, '', '']);
assert(!context.Module.pom2FilePickerActive);
choose(1);
last().files = [{ size: 1, async arrayBuffer() { throw new Error('unreadable'); } }];
await last().fire('change');
assert.match(calls.at(-1)[2], /unreadable/);
choose(1);
last().files = [{ size: 4, async arrayBuffer() { return new Uint8Array([1, 2, 3, 4]).buffer; } }];
await last().fire('change');
assert.deepEqual(calls.at(-1), [1, '/uploads/import.pom2session', '']);
assert.deepEqual(files.get('/uploads/import.pom2session'), [1, 2, 3, 4]);
pickerFailure = true;
choose(0);
assert.match(calls.at(-1)[2], /picker denied/);
assert(last().removed && !context.Module.pom2FilePickerActive);
pickerFailure = false;
assert(context.Module.pom2DownloadFile('/uploads/import.pom2session', 'session.pom2session'));
assert.equal(last().download, 'session.pom2session');
assert(last().removed);
assert.deepEqual(revoked, ['blob:test']);
assert.equal(context.Module.pom2DownloadFile('/missing', 'session.pom2session'), false);
console.log('browser files: import/export, bounds, cancellation and failures passed');
