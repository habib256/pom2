// POM2 — GPL-3.0-or-later. Linked with --pre-js; no host paths are exposed.
Module.pom2ChooseFile = function(kind) {
  if (Module.pom2FilePickerActive || ![0, 1, 2].includes(kind)) return false;
  Module.pom2FilePickerActive = true;
  const input = document.createElement('input');
  input.type = 'file';
  input.accept = kind === 2 ? '.dsk,.do,.d13,.po,.nib,.woz,.moof,.2mg,.hdv,.image'
    : kind === 1 ? '.pom2session' : '.txt,.bas,.a,text/plain';
  input.style.display = 'none';
  document.body.appendChild(input);
  let finished = false;
  const complete = function(path, error) {
    if (finished) return;
    finished = true;
    input.remove();
    Module.pom2FilePickerActive = false;
    Module.ccall('pom2_browser_file_ready', null, ['number', 'string', 'string'], [kind, path, error]);
  };
  input.addEventListener('cancel', () => complete('', ''));
  input.addEventListener('change', async () => {
    const file = input.files && input.files[0];
    if (!file) { complete('', ''); return; }
    const limit = (kind === 2 ? 64 : kind === 1 ? 32 : 1) * 1024 * 1024;
    const tooLarge = kind === 2 ? 'Disk image exceeds 64 MiB.'
      : kind === 1 ? 'Session exceeds 32 MiB.' : 'Text file exceeds 1 MiB.';
    if (file.size > limit) { complete('', tooLarge); return; }
    // Preserve the extension for format detection; never use a supplied path.
    const name = kind === 2 ? (file.name || '').split(/[\\/]/).pop() : '';
    if (kind === 2 && !/\.(dsk|do|d13|po|nib|woz|moof|2mg|hdv|image)$/i.test(name)) {
      complete('', 'Choose a supported disk image (.dsk, .do, .d13, .po, .nib, .woz, .moof, .2mg, .hdv, .image).');
      return;
    }
    try {
      const bytes = new Uint8Array(await file.arrayBuffer());
      if (bytes.length > limit) { complete('', tooLarge); return; }
      FS.mkdirTree('/uploads');
      let path = kind === 1 ? '/uploads/import.pom2session' : '/uploads/listing.txt';
      if (kind === 2) {
        // A fresh directory keeps a second upload from overwriting mounted media,
        // including two disks with the same original filename.
        let directory;
        do {
          Module.pom2DiskUploadId = (Module.pom2DiskUploadId || 0) + 1;
          directory = '/uploads/disk-' + Module.pom2DiskUploadId;
        } while (FS.analyzePath(directory).exists);
        FS.mkdirTree(directory);
        path = directory + '/' + name.replace(/[^a-zA-Z0-9._-]/g, '_').slice(-128);
      }
      FS.writeFile(path, bytes);
      complete(path, '');
    } catch (error) { complete('', 'File import failed: ' + error.message); }
  });
  try { input.click(); }
  catch (error) { complete('', 'File picker failed: ' + error.message); }
  return true;
};
Module.pom2DownloadFile = function(path, name) {
  let url;
  let link;
  try {
    const bytes = FS.readFile(path);
    url = URL.createObjectURL(new Blob([bytes], {type: 'application/octet-stream'}));
    link = document.createElement('a');
    link.href = url;
    link.download = name;
    document.body.appendChild(link);
    link.click();
    return true;
  } catch (error) {
    console.warn('POM2 download failed:', error);
    return false;
  } finally {
    if (link) link.remove();
    if (url) setTimeout(() => URL.revokeObjectURL(url), 30000);
  }
};
