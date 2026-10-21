#!/usr/bin/env python3
"""Checks every relative link and #anchor in the Markdown files of the repository (README.md, docs/, the project READMEs).

    python tools/check-doc-links.py

Exit code 0 = everything resolves. Anchors follow GitHub's rules (lower case, punctuation removed, spaces become hyphens).
"""
import pathlib
import re
import sys
import unicodedata

root = pathlib.Path(__file__).resolve().parent.parent
skip_parts = {".pio", "bin", "obj", "node_modules", ".git", "legacy"}

files = [p for p in root.rglob("*.md") if not (set(p.relative_to(root).parts) & skip_parts)]


def slug(heading: str) -> str:
    # strip inline markup and links
    text = re.sub(r"`([^`]*)`", r"\1", heading)
    text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", text)
    text = re.sub(r"[*_~]", "", text).strip().lower()
    out = []
    for ch in text:
        cat = unicodedata.category(ch)
        if ch in " -":
            out.append("-" if ch == " " else "-")
        elif cat[0] in "LN" or ch == "_":
            out.append(ch)
        # everything else (punctuation, symbols, dots, dashes like —) is dropped
    return "".join(out)


anchor_cache: dict[pathlib.Path, set[str]] = {}


def anchors(path: pathlib.Path) -> set[str]:
    if path not in anchor_cache:
        found: dict[str, int] = {}
        result = set()
        in_code = False
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.lstrip().startswith("```"):
                in_code = not in_code
                continue
            if in_code:
                continue
            m = re.match(r"^#{1,6}\s+(.*?)\s*#*\s*$", line)
            if m:
                s = slug(m.group(1))
                n = found.get(s, 0)
                found[s] = n + 1
                result.add(s if n == 0 else f"{s}-{n}")
        anchor_cache[path] = result
    return anchor_cache[path]


link_re = re.compile(r"(?<!\!)\[[^\]]*\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)|!\[[^\]]*\]\(([^)\s]+)\)|<img[^>]+src=\"([^\"]+)\"")
problems = 0
checked = 0
for md in sorted(files):
    in_code = False
    for lineno, line in enumerate(md.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
        if line.lstrip().startswith("```"):
            in_code = not in_code
            continue
        if in_code:
            continue
        for m in link_re.finditer(line):
            target = next(g for g in m.groups() if g)
            if re.match(r"^(https?:|mailto:|data:)", target):
                continue
            path_part, _, frag = target.partition("#")
            dest = md if path_part == "" else (md.parent / path_part).resolve()
            checked += 1
            if not dest.exists():
                print(f"{md.relative_to(root)}:{lineno}: missing file {target}")
                problems += 1
                continue
            if frag and dest.suffix.lower() == ".md":
                if frag.lower() not in anchors(dest):
                    print(f"{md.relative_to(root)}:{lineno}: no heading for #{frag} in {dest.relative_to(root)}")
                    problems += 1

print(f"{checked} links checked in {len(files)} files, {problems} problem(s)")
sys.exit(1 if problems else 0)
