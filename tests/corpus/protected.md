# Real protected-disk corpus

Three optional games, with commercial images kept outside the checkout and all
mounted media copied by the runner. The recorded checks are change detectors,
not certification of a complete game or hardware accuracy.

| Game | Archive source | Machine | Milestones |
|---|---|---|---|
| Captain Goodnight | [WOZ-a-day item](https://archive.org/details/wozaday_Captain_Goodnight_and_The_Islands_of_Fear) | unenhanced //e, NTSC | Title at 30 s, disk-turn prompt at 40 s, face B inserted without reboot, arrival at 60 s, running at 90 s |
| Prince of Persia | [WOZ-a-day item](https://archive.org/details/wozaday_Prince_of_Persia) | enhanced //e, NTSC | Title at 30 s, SPACE, first dungeon at 60 s, RIGHT then another 2 s |

The SHA-256s in `protected.manifest` match the images used in the
[October audit](../../docs/audits/protected-disks-2026-10-12.md).
Captain A is WOZ2 with half-track positions (49 populated TMAP entries);
Prince A is WOZ1 with 35 whole tracks. These are **bit-cell captures**, not
WOZ 2.1 FLUX chunks. They do not themselves exercise FLUX tracks, prove every
weak-bit case, or test Prince side B and later levels.

Bandits uses [the WOZ-a-day capture](https://archive.org/details/wozaday_Bandits):
WOZ2 INFO v3, a 160-byte FLUX chunk at offset 831488, and 19 mapped
quarter-track positions (6, 10, …, 78). Its title at 30 s and game at 50 s
(after SPACE) are checked on an unenhanced NTSC //e. A separate instruction
probe observed Disk II data-register reads at all 19 positions during boot;
see [the consolidation audit](../../docs/audits/core-consolidation-2026-11-09.md).
This adds one real-FLUX title, not a general claim about every FLUX capture.

## Fetch temporarily and replay

From the repository root; the download destination must be outside the tree.
No firmware is downloaded by this recipe; use the project's existing ROM setup.

```sh
media_dir=$(mktemp -d /tmp/pom2-protected.XXXXXX)
python3 - "$media_dir" <<'PY'
import io, pathlib, sys, urllib.parse, urllib.request, zipfile
root = pathlib.Path(sys.argv[1])
items = [
    ('captain', 'wozaday_Captain_Goodnight_and_The_Islands_of_Fear',
     'Captain Goodnight and the Islands of Fear (woz-a-day collection).zip'),
    ('prince', 'wozaday_Prince_of_Persia',
     'Prince of Persia (woz-a-day collection).zip'),
]
for prefix, item, archive in items:
    url = 'https://archive.org/download/' + item + '/' + urllib.parse.quote(archive)
    with urllib.request.urlopen(url, timeout=60) as response:
        data = response.read()
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        for side in ('A', 'B'):
            suffix = 'side ' + side + '.woz'
            names = [n for n in z.namelist() if n.endswith(suffix)]
            if len(names) != 1:
                raise RuntimeError('Expected exactly one ' + suffix)
            (root / (prefix + '-' + side.lower() + '.woz')).write_bytes(z.read(names[0]))
url = 'https://archive.org/download/wozaday_Bandits/' + urllib.parse.quote('Bandits (woz-a-day collection).zip')
with urllib.request.urlopen(url, timeout=60) as response:
    data = response.read()
with zipfile.ZipFile(io.BytesIO(data)) as z:
    names = [n for n in z.namelist() if n.lower().endswith('.woz')]
    if len(names) != 1:
        raise RuntimeError('Expected exactly one Bandits WOZ')
    (root / 'bandits.woz').write_bytes(z.read(names[0]))
print(root)
PY
cmake --build build-release-tests --target pom2_corpus --parallel 3
build-release-tests/pom2_corpus \
  --manifest tests/corpus/protected.manifest \
  --expected tests/corpus/protected.expected \
  --root "$media_dir" --frames "$media_dir/frames" \
  --report "$media_dir/report.md"
```

The runner checks each selected image's SHA-256 before boot. Missing media,
wrong media or different firmware produce a reported skip; they are not a
passing compatibility result. Keep captures and downloads in the temporary
directory for inspection, then remove that directory when finished.

For a deliberate expectation update, first inspect every `--frames` image,
then use `--bless` with this **separate** expected file and repeat without it.
Never update the default corpus's expectations to accommodate these titles.
