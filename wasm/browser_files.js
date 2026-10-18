// POM2 — GPL-3.0-or-later. Linked with --pre-js; no host paths are exposed.
Module.pom2ChooseFile = function(kind) {
  if (Module.pom2FilePickerActive) return;
  Module.pom2FilePickerActive = true;
  const input = document.createElement('input');
  input.type = 'file';
  input.accept = kind ? '.pom2session' : '.txt,.bas,.a,text/plain';
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
    const limit = kind ? 32 * 1024 * 1024 : 1024 * 1024;
    if (file.size > limit) { complete('', kind ? 'Session exceeds 32 MiB.' : 'Text file exceeds 1 MiB.'); return; }
    try {
      const bytes = new Uint8Array(await file.arrayBuffer());
      FS.mkdirTree('/uploads');
      // Fixed, separate destinations prevent filenames from becoming paths.
      const path = kind ? '/uploads/import.pom2session' : '/uploads/listing.txt';
      FS.writeFile(path, bytes);
      complete(path, '');
    } catch (error) { complete('', 'File import failed: ' + error.message); }
  });
  try { input.click(); }
  catch (error) { complete('', 'File picker failed: ' + error.message); }
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
