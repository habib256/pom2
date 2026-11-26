// POM2 — GPL-3.0-or-later. Linked with --pre-js (before browser_files.js).
//
// WASM-004 — shareable links: one URL parameter per drive, and the link that
// reproduces the running setup.
//
//   ?preset=iie-pal&s6d1=<disk 1>&s6d2=<disk 2>      two 5.25" disks
//   ?s5d1=bundled_disks/DIX-fix.po&s6d1=https://…/data.dsk
//
// The keys are Apple2TS's (sNdM: slot N, drive M — drive 1 to 14 for a
// SmartPort chain). A value is an http(s) URL (the host must send CORS
// headers, as for ?url=) or a path inside the page's own bundle.
//
// Which disk BOOTS: a DRIVE 1 only — the boot disk is the CLI positional,
// which always lands in drive 1 of the slot its type routes to, so naming
// drive 2 as the boot would put it in drive 1 and refuse the real drive 1
// (bug hunt 2026-11-19). ?boot=sNdM picks among the drive-1 keys; otherwise
// the highest slot's, as the Autostart ROM scans slot 7 down to slot 1. A
// link with no drive-1 key boots nothing from its keys: they are mounted,
// as a disk in drive 2 is on the machine. ?url= / ?disk= take the boot when
// no drive-1 key does (?nodisk= keeps its meaning). Every other drive is
// `--mount sNdM:<path>`, mounted after the boot without booting.
//
// A value that is neither an http(s) URL nor a path inside the bundle is
// refused (`rejected`): a link must not mount a visitor's stored image
// (/persistent/…) or reach outside the bundle (..).
//
// Pure functions first, so tests/wasm_setup_link_test.mjs can run them under
// node; `fetchDiskUrl` needs the browser (fetch) and Emscripten's FS.

var pom2SetupLink = (function () {
  'use strict';
  const DRIVE_KEY = /^s([1-7])d([1-9]|1[0-4])$/;
  // A .zip, .7z or .gz is downloaded as it is and unpacked by the C++ side
  // (MediaSource.h): the boot positional and --mount both resolve archives.
  const DISK_EXT = /\.(dsk|do|d13|po|nib|woz|moof|2mg|hdv|image|zip|7z|gz)$/i;
  const LIMIT = 64 * 1024 * 1024;

  function isRemote(v) { return /^https?:\/\//i.test(v || ''); }

  // A path inside the bundle: relative, no scheme, no '..'.
  function isBundledPath(v) {
    return !!v && v[0] !== '/' && !/^[a-z][a-z0-9+.-]*:/i.test(v) &&
           !/(^|\/)\.\.(\/|$)/.test(v);
  }

  // A '+' typed unencoded (?preset=ii+) decodes to a space; no preset name
  // contains one.
  function presetOf(qs) { return (qs.get('preset') || 'iic-pal').replace(/ /g, '+'); }

  // search → { preset, boot: {key, value} | null, mounts: [{key, value}],
  //            rejected: [{key, value}] }.
  // `key` is null for the legacy ?url= / ?disk= / default boot disk.
  function planDrives(search, defaultDisk) {
    const qs = new URLSearchParams(search);
    const drives = [], rejected = [], seen = new Set();
    for (const [k, v] of qs) {
      const key = k.toLowerCase();
      const m = DRIVE_KEY.exec(key);
      if (!m || !v || seen.has(key)) continue;   // first one wins
      seen.add(key);
      if (!isRemote(v) && !isBundledPath(v)) { rejected.push({ key, value: v }); continue; }
      drives.push({ key, slot: Number(m[1]), drive: Number(m[2]), value: v });
    }
    const firsts = drives.filter(d => d.drive === 1);
    let boot = null;
    let legacy = qs.get('url') || (qs.get('nodisk') === '1' ? null : qs.get('disk'));
    // ?url= must be what its name says (round 2): a path given there used to
    // pass as a local file. ?disk= keeps its long-standing meaning — a path
    // in the page's own filesystem, the bundle or a stored image (the
    // persistence smoke boots /persistent/… through it); the worst a link
    // can do with it is boot a disk the visitor already holds.
    if (qs.get('url') && !isRemote(legacy)) {
      rejected.push({ key: 'url', value: legacy });
      legacy = null;
    }
    if (firsts.length) {
      const want = (qs.get('boot') || '').toLowerCase();
      boot = firsts.find(d => d.key === want) || firsts.sort((a, b) => b.slot - a.slot)[0];
    } else if (legacy) {
      boot = { key: null, value: legacy };
    } else if (!drives.length && !rejected.length && qs.get('nodisk') !== '1' && defaultDisk) {
      boot = { key: null, value: defaultDisk };
    }
    const mounts = drives.filter(d => d !== boot).map(d => ({ key: d.key, value: d.value }));
    return { preset: presetOf(qs), boot: boot ? { key: boot.key, value: boot.value } : null,
             mounts, rejected };
  }

  // A mounted MEMFS path → the value a link carries, or null when the disk
  // cannot be shared (imported from this computer, restored from storage).
  function shareValue(path, urlByPath) {
    if (urlByPath && Object.prototype.hasOwnProperty.call(urlByPath, path)) return urlByPath[path];
    if (/^\/(uploads|persistent|tmp|home)\//.test(path)) return null;
    const rel = path.replace(/^\/+/, '');
    return isBundledPath(rel) ? rel : null;
  }

  // The running setup → its query string. `drives` is [{key, path}] in the
  // machine's order; `skipped` names what could not go in the link.
  function setupQuery(preset, drives, urlByPath) {
    const parts = ['preset=' + encodeURIComponent(preset)];
    const skipped = [];
    let shared = 0;
    for (const d of drives) {
      if (!DRIVE_KEY.test(d.key)) continue;
      const v = shareValue(d.path, urlByPath);
      if (v === null) { skipped.push(d.path); continue; }
      parts.push(d.key + '=' + encodeURIComponent(v));
      ++shared;
    }
    if (!shared) parts.push('nodisk=1');   // or the page would boot its default disk
    return { query: '?' + parts.join('&'), skipped };
  }

  // Download an image into MEMFS under `dir`, with ?url='s bounds: http(s)
  // only, a disk extension, 64 MiB, 60 s. Resolves to the MEMFS path.
  async function fetchDiskUrl(url, dir, fs) {
    let target;
    try { target = new URL(url, window.location.href); }
    catch (e) { throw new Error('not a valid URL'); }
    if (target.protocol !== 'https:' && target.protocol !== 'http:')
      throw new Error('only http(s) URLs are accepted');
    const name = decodeURIComponent(target.pathname.split('/').pop() || '');
    if (!DISK_EXT.test(name))
      throw new Error('the file name must end in .dsk, .do, .d13, .po, .nib, ' +
                      '.woz, .moof, .2mg, .hdv or .image, or be a .zip, .7z ' +
                      'or .gz holding one');
    // A server that stalls must not hold the page on its splash forever, and
    // a body with no (or a false) Content-Length is counted as it streams.
    const abort = new AbortController();
    const deadline = setTimeout(function () { abort.abort(); }, 60000);
    try {
      let response;
      try {
        response = await fetch(target.href, { mode: 'cors', credentials: 'omit',
                                              signal: abort.signal });
      } catch (e) {
        if (abort.signal.aborted) throw new Error('download timed out after 60 s');
        throw new Error('download failed (network error, or the server does ' +
                        'not allow cross-origin requests)');
      }
      if (!response.ok) throw new Error('download failed: HTTP ' + response.status);
      if (Number(response.headers.get('Content-Length') || 0) > LIMIT)
        throw new Error('image exceeds 64 MiB');
      const reader = response.body.getReader();
      const chunks = [];
      let total = 0;
      for (;;) {
        let r;
        try { r = await reader.read(); }
        catch (e) {
          throw new Error(abort.signal.aborted ? 'download timed out after 60 s'
                                               : 'download interrupted');
        }
        if (r.done) break;
        total += r.value.length;
        if (total > LIMIT) { abort.abort(); throw new Error('image exceeds 64 MiB'); }
        chunks.push(r.value);
      }
      if (total === 0) throw new Error('the server returned an empty file');
      const bytes = new Uint8Array(total);
      let at = 0;
      for (const c of chunks) { bytes.set(c, at); at += c.length; }
      fs.mkdirTree(dir);
      const path = dir.replace(/\/$/, '') + '/' +
                   name.replace(/[^a-zA-Z0-9._-]/g, '_').slice(-128);
      // canOwn: MEMFS keeps this buffer instead of copying it.
      fs.writeFile(path, bytes, { canOwn: true });
      return path;
    } finally {
      clearTimeout(deadline);
    }
  }

  return { planDrives, shareValue, setupQuery, fetchDiskUrl, isRemote, isBundledPath };
})();

if (typeof Module !== 'undefined') {
  Module.pom2SetupLink = pom2SetupLink;
  // Remote images by the MEMFS path they were saved to: the copy-link
  // button gives the URL back, not a path only this tab has.
  Module.pom2UrlByPath = Module.pom2UrlByPath || {};
  // Called by MainWindow (File ▸ Copy link to this setup). `drivesJson` is
  // [{"key":"s6d1","path":"/…"}]. The link goes to the clipboard and the
  // address bar. Returns how many mounted disks could NOT go in the link
  // (imported from this computer), so the caller can say so; -1 on error.
  Module.pom2CopySetupLink = function (preset, drivesJson) {
    let drives = [];
    try { drives = JSON.parse(drivesJson); } catch (e) { return -1; }
    const built = pom2SetupLink.setupQuery(preset, drives, Module.pom2UrlByPath);
    const link = window.location.origin + window.location.pathname + built.query;
    try { window.history.replaceState(null, '', built.query); } catch (e) { /* file:// */ }
    if (navigator.clipboard && navigator.clipboard.writeText)
      navigator.clipboard.writeText(link).catch(function () {
        console.warn('POM2: clipboard refused; the link is in the address bar:', link);
      });
    return built.skipped.length;
  };
}
if (typeof module === 'object' && module.exports) module.exports = pom2SetupLink;
