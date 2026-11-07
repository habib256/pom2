#!/usr/bin/env python3
"""Check explicit local backlog links and canonical backlog task identities.

History is evidence, not an active task definition: IDs mentioned in CHANGELOG
remain valid citations after their backlog entry is retired. Free prose about
ctests/cards is deliberately not interpreted as a typed reference.
"""
import argparse
import collections
import html
from pathlib import Path
import re
import tempfile
import unicodedata
from urllib.parse import unquote

TASK = re.compile(r"\b(?:ARCH|AUDIO|CARDS|CASSETTE|CPU|DISPLAY|INPUT|MEMORY|NETWORK|RELEASE|STORAGE|UI|VALIDATION)-\d{3}\b")
HEADING = re.compile(r"^#{1,6}\s+(.+?)\s*#*\s*$")
LINK = re.compile(r"(?<!!)\[[^\]\n]*\]\(<?([^\s)>]+)>?(?:\s+\"[^\"]*\")?\)")


def prose(text):
    """Exclude fenced examples from definitions, references and headings."""
    fence = None
    for number, line in enumerate(text.splitlines(), 1):
        match = re.match(r"^\s{0,3}(`{3,}|~{3,})", line)
        if match:
            token = match[1]
            if fence is None:
                fence = token
            elif token[0] == fence[0] and len(token) >= len(fence):
                fence = None
            continue
        if fence is None:
            yield number, line


def anchors(text):
    result = set()
    generated = set()
    for _, line in prose(text):
        result.update(re.findall(r'<(?:a|[^ >]+)\b[^>]*\bid=["\']([^"\']+)["\']', line))
        match = HEADING.match(line)
        if not match:
            continue
        title = html.unescape(re.sub(r"<[^>]*>", "", match[1]))
        slug = "".join(c for c in title.lower()
                       if c in " _-" or unicodedata.category(c)[0] in "LNM")
        slug = slug.replace(" ", "-")
        candidate = slug
        suffix = 0
        while candidate in generated:
            suffix += 1
            candidate = f"{slug}-{suffix}"
        generated.add(candidate)
        result.add(candidate)
    return result


def check(root):
    root = root.resolve()
    if not root.is_dir():
        return [f"{root}: documentation root is missing"]
    backlog = root / "docs/backlog"
    if not backlog.is_dir() or not any(backlog.rglob("*.md")):
        return [f"{backlog}: backlog Markdown tree is missing or empty"]
    files = sorted(root.glob("*.md"))
    for directory in ("backlog", "decisions", "audits"):
        files.extend(sorted((root / "docs" / directory).rglob("*.md")))
    errors = []
    definitions = collections.defaultdict(list)
    texts = {p: p.read_text(encoding="utf-8") for p in files}
    for path, text in texts.items():
        if root / "docs/backlog" not in path.parents:
            continue
        for number, line in prose(text):
            heading = HEADING.match(line)
            if heading:
                # Only a leading ID defines an entry; index rows and mentions don't.
                task = TASK.match(heading[1])
                if task:
                    definitions[task[0]].append(f"{path.relative_to(root)}:{number}")
    for task, locations in definitions.items():
        if len(locations) > 1:
            errors.append(f"{locations[0]}: duplicate task {task}: {', '.join(locations[1:])}")
    history = root / "CHANGELOG.md"
    historical = set(TASK.findall(texts.get(history, "")))
    known = set(definitions) | historical
    cache = {}
    for path, text in texts.items():
        for number, line in prose(text):
            location = f"{path.relative_to(root)}:{number}"
            # Historical records may describe IDs retired since publication.
            if path != history:
                for task in sorted(set(TASK.findall(line)) - known):
                    errors.append(f"{location}: unknown task {task}")
            for match in LINK.finditer(line):
                target, _, fragment = unquote(match[1]).partition("#")
                if re.match(r"[a-zA-Z][a-zA-Z0-9+.-]*:", target):
                    continue
                dest = (path.parent / target).resolve() if target else path
                backlog_link = root / "docs/backlog" in dest.parents and dest.suffix == ".md"
                historical_fragment = dest == history and bool(fragment)
                if not backlog_link and not historical_fragment:
                    continue
                if not dest.is_file():
                    errors.append(f"{location}: missing backlog file {match[1]}")
                elif fragment:
                    if dest not in cache:
                        cache[dest] = anchors(dest.read_text(encoding="utf-8"))
                    if fragment not in cache[dest]:
                        errors.append(f"{location}: missing anchor {match[1]}")
    return errors


def self_test():
    collision = anchors('## A\n## A\n## A-1\n')
    assert collision == {"a", "a-1", "a-1-1"}, collision
    assert "a-2" not in collision, collision
    literal_first = anchors('## A-1\n## A\n## A\n')
    assert literal_first == {"a-1", "a", "a-2"}, literal_first
    explicit = anchors('<a id="a"></a>\n## A\n')
    assert explicit == {"a"}, explicit
    print("  ok: generated slug collisions, literal suffix order, separate HTML IDs")
    with tempfile.TemporaryDirectory(prefix="pom2-doc-refs-") as directory:
        root = Path(directory)
        backlog = root / "docs/backlog"
        assert check(root / "absent"), "missing root must fail"
        assert check(root), "missing backlog tree must fail"
        backlog.mkdir(parents=True)
        assert check(root), "empty backlog tree must fail"
        print("  ok: missing root, missing backlog tree, empty backlog tree")
        task = backlog / "tasks.md"
        readme = root / "README.md"
        (root / "CHANGELOG.md").write_text('Retired ARCH-099.\n- <a id="closed"></a> Historical closure.\n', encoding="utf-8")
        baseline = '## ARCH-001 — Active\n<a id="stable"></a>\n### Repeated\n### Repeated\n### Café &amp; déjà-vu!\n'
        task.write_text(baseline, encoding="utf-8")
        good = '[closed](CHANGELOG.md#closed)\n[task](docs/backlog/tasks.md#stable) ARCH-001 ARCH-099\n[duplicate heading](docs/backlog/tasks.md#repeated-1)\n[Unicode heading](docs/backlog/tasks.md#caf%C3%A9--d%C3%A9j%C3%A0-vu)\n'
        readme.write_text(good, encoding="utf-8")
        assert not check(root), check(root)
        cases = [
            (good + 'ARCH-998\n', baseline, 'unknown task'),
            (good + '[bad](docs/backlog/absent.md)\n', baseline, 'missing backlog file'),
            (good + '[bad](docs/backlog/tasks.md#absent)\n', baseline, 'missing anchor'),
            (good + '[bad](CHANGELOG.md#absent)\n', baseline, 'missing anchor'),
            (good + '[bad](docs/backlog/tasks.md#cafe--deja-vu)\n', baseline, 'missing anchor'),
            (good, baseline + '### ARCH-001 — Other identity\n', 'duplicate task'),
        ]
        for body, entries, diagnostic in cases:
            readme.write_text(body, encoding="utf-8")
            task.write_text(entries, encoding="utf-8")
            assert any(diagnostic in error for error in check(root)), diagnostic
            print(f"  ok: {diagnostic}")
        readme.write_text(good, encoding="utf-8")
        task.write_text(baseline, encoding="utf-8")
        other = backlog / "other.md"
        other.write_text("## ARCH-001 — Cross-file collision\n", encoding="utf-8")
        assert any("duplicate task" in error for error in check(root)), "cross-file duplicate"
        other.unlink()
        print("  ok: duplicate canonical definition across files")
        readme.write_text(good + '```md\nARCH-998 [example](docs/backlog/no.md#bad)\n```\nSHA-256 RS-232\n', encoding="utf-8")
        task.write_text(baseline, encoding="utf-8")
        assert not check(root), check(root)
        print("  ok: historical IDs, inline historical anchors, Unicode slugs, fenced examples, non-task identifiers")
    print("check_doc_refs: self-test passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    errors = check(args.root)
    for error in errors:
        print(error)
    print(f"check_doc_refs: {'FAILED' if errors else 'passed'} ({len(errors)} errors)")
    return bool(errors)


if __name__ == "__main__":
    raise SystemExit(main())
