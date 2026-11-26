// WASM-004: the browser's per-drive link parameters, planned and rebuilt by
// wasm/setup_link.js — run under node, without a browser or a build.
//   node tests/wasm_setup_link_test.mjs <repo>/wasm/setup_link.js
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import path from 'node:path';

const require = createRequire(import.meta.url);
const link = require(path.resolve(process.argv[2] || 'wasm/setup_link.js'));
const DEF = 'bundled_disks/DIX-fix.po';

// No parameter: the page's default disk boots.
assert.deepEqual(link.planDrives('', DEF),
                 { preset: 'iic-pal', boot: { key: null, value: DEF }, mounts: [], rejected: [] });
assert.deepEqual(link.planDrives('?nodisk=1', DEF).boot, null);

// Two 5.25" disks: drive 1 boots, drive 2 is mounted beside it.
let p = link.planDrives('?preset=iie-pal&s6d2=https://h/side2.dsk&s6d1=https://h/side1.dsk', DEF);
assert.equal(p.preset, 'iie-pal');
assert.deepEqual(p.boot, { key: 's6d1', value: 'https://h/side1.dsk' });
assert.deepEqual(p.mounts, [{ key: 's6d2', value: 'https://h/side2.dsk' }]);

// The highest slot's drive 1 boots (the Autostart scan) unless ?boot= says.
p = link.planDrives('?s6d1=a.dsk&s5d1=b.po&s7d1=c.hdv', DEF);
assert.equal(p.boot.key, 's7d1');
assert.deepEqual(p.mounts.map(m => m.key), ['s6d1', 's5d1']);
p = link.planDrives('?s6d1=a.dsk&s5d1=b.po&boot=S5D1', DEF);
assert.equal(p.boot.key, 's5d1');
// Only a drive 1 boots (the positional lands in drive 1): a drive 2 alone is
// mounted, no default disk is booted over it; ?boot= naming a drive 2 is
// ignored rather than putting that disk in drive 1 (bug hunt 2026-11-19).
p = link.planDrives('?s6d2=x.dsk', DEF);
assert.equal(p.boot, null);
assert.deepEqual(p.mounts, [{ key: 's6d2', value: 'x.dsk' }]);
p = link.planDrives('?s6d1=a.dsk&s6d2=b.dsk&boot=s6d2', DEF);
assert.equal(p.boot.key, 's6d1');
assert.deepEqual(p.mounts, [{ key: 's6d2', value: 'b.dsk' }]);
// The legacy ?url= takes the boot when no drive 1 does; the drives are mounted.
p = link.planDrives('?url=https://h/g.dsk&s6d2=https://h/d.dsk', DEF);
assert.deepEqual(p.boot, { key: null, value: 'https://h/g.dsk' });
assert.deepEqual(p.mounts, [{ key: 's6d2', value: 'https://h/d.dsk' }]);
// …and a drive 1 key wins over it (both would land in drive 1).
assert.equal(link.planDrives('?url=https://h/g.dsk&s6d1=https://h/a.dsk', DEF).boot.key, 's6d1');
// Neither a URL nor a bundle path: refused, never handed to the machine.
p = link.planDrives('?s6d1=/persistent/pom2/x.dsk&s6d2=../../etc/p.dsk&s5d1=javascript:alert(1)', DEF);
assert.equal(p.boot, null);
assert.deepEqual(p.mounts, []);
assert.deepEqual(p.rejected.map(r => r.key), ['s6d1', 's6d2', 's5d1']);
// …and ?url= must be a URL (bug hunt round 2).
p = link.planDrives('?url=/persistent/pom2/x.dsk', DEF);
assert.equal(p.boot, null);
assert.deepEqual(p.rejected, [{ key: 'url', value: '/persistent/pom2/x.dsk' }]);
// ?disk= keeps its meaning: a path in the page's filesystem, stored images
// included (tools/wasm_persistence_smoke.mjs relies on it).
assert.deepEqual(link.planDrives('?disk=/persistent/s.nib', DEF).boot, { key: null, value: '/persistent/s.nib' });
assert.deepEqual(link.planDrives('?disk=bundled_disks/A.po', DEF).boot, { key: null, value: 'bundled_disks/A.po' });
// Keys are case-insensitive, malformed ones ignored, a repeated one taken once.
p = link.planDrives('?S6D1=a.dsk&s6d1=b.dsk&s8d1=c.dsk&s6d15=d.dsk&s6d=e.dsk', DEF);
assert.deepEqual(p.boot, { key: 's6d1', value: 'a.dsk' });
assert.deepEqual(p.mounts, []);
// '+' typed raw in a preset decodes to a space; it is put back.
assert.equal(link.planDrives('?preset=ii+', DEF).preset, 'ii+');

// What a mounted path becomes in a link.
const urls = { '/uploads/url/s6d1/side1.dsk': 'https://h/side1.dsk' };
assert.equal(link.shareValue('/uploads/url/s6d1/side1.dsk', urls), 'https://h/side1.dsk');
assert.equal(link.shareValue('/bundled_disks/A2FileCmd.po', urls), 'bundled_disks/A2FileCmd.po');
assert.equal(link.shareValue('bundled_disks/DIX-fix.po', urls), 'bundled_disks/DIX-fix.po');
assert.equal(link.shareValue('/uploads/disk-3/mine.dsk', urls), null);
assert.equal(link.shareValue('/persistent/session/x.dsk', urls), null);

// The copy-link button: the running setup round-trips through planDrives.
const q = link.setupQuery('iie-pal', [
    { key: 's6d1', path: '/uploads/url/s6d1/side1.dsk' },
    { key: 's6d2', path: '/uploads/url/s6d2/side2.dsk' },
    { key: 's5d1', path: '/uploads/disk-1/private.po' },
], { '/uploads/url/s6d1/side1.dsk': 'https://h/side1.dsk',
     '/uploads/url/s6d2/side2.dsk': 'https://h/a b&c.dsk' });
assert.deepEqual(q.skipped, ['/uploads/disk-1/private.po']);
p = link.planDrives(q.query, DEF);
assert.equal(p.preset, 'iie-pal');
assert.deepEqual(p.boot, { key: 's6d1', value: 'https://h/side1.dsk' });
assert.deepEqual(p.mounts, [{ key: 's6d2', value: 'https://h/a b&c.dsk' }]);
// Nothing shareable: the link says so rather than booting the default disk.
assert.equal(link.setupQuery('iic-pal', [], {}).query, '?preset=iic-pal&nodisk=1');
assert.equal(link.planDrives('?preset=iic-pal&nodisk=1', DEF).boot, null);

// fetchDiskUrl: a disk or an archive of one (.zip / .7z / .gz, unpacked by
// the C++ side, MediaSource.h) is downloaded into MEMFS under its own name;
// any other name is refused before a byte is fetched.
globalThis.window = { location: { href: 'https://pom2.test/' } };
let fetched = 0;
globalThis.fetch = async () => { ++fetched; return new Response(new Uint8Array([0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C])); };
const memfs = new Map();
const fakeFs = { mkdirTree() {}, writeFile(p, bytes) { memfs.set(p, bytes); } };
for (const name of ['Game%20Side%20A.7z', 'GAME.ZIP', 'game.dsk.gz', 'game.woz']) {
  const at = await link.fetchDiskUrl('https://h/' + name, '/uploads/url/s6d1', fakeFs);
  assert.equal(at, '/uploads/url/s6d1/' + decodeURIComponent(name).replace(/ /g, '_'));
  assert(memfs.has(at));
}
assert.equal(fetched, 4);
await assert.rejects(link.fetchDiskUrl('https://h/readme.txt', '/uploads/url/s6d1', fakeFs),
                     /\.7z/);
assert.equal(fetched, 4);

console.log('wasm_setup_link: all passed');
