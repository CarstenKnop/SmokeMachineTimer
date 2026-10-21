#!/usr/bin/env python3
"""Builds docs/MANUAL.html and docs/FOG_CONSOLE_MANUAL.html: self-contained files (images embedded) that open in any browser and print to PDF.

    pip install markdown
    python tools/make-manual-html.py
"""
import base64
import mimetypes
import pathlib
import re
import sys

try:
    import markdown
except ImportError:
    sys.exit("This script needs the 'markdown' package:  pip install markdown")

root = pathlib.Path(__file__).resolve().parent.parent
docs = root / "docs"

css = """
:root { color-scheme: light dark; }
body { font: 16px/1.55 'Segoe UI', system-ui, sans-serif; max-width: 980px; margin: 2rem auto; padding: 0 1.2rem; color: #1c2630; background: #fff; }
h1, h2, h3 { line-height: 1.2; color: #12324f; }
h1 { border-bottom: 3px solid #d9822b; padding-bottom: .3rem; }
h2 { margin-top: 2.4rem; border-bottom: 1px solid #cdd6df; padding-bottom: .2rem; }
table { border-collapse: collapse; margin: 1rem 0; width: 100%; font-size: .95rem; }
th, td { border: 1px solid #cdd6df; padding: .4rem .6rem; text-align: left; vertical-align: top; }
th { background: #eef3f8; }
img { max-width: 100%; height: auto; border-radius: 6px; margin: .4rem .2rem; }
img[src*="oled"] { max-width: 400px; }
code { background: #f1f4f7; padding: .1rem .3rem; border-radius: 4px; }
pre { background: #f1f4f7; padding: .8rem; border-radius: 6px; overflow: auto; }
blockquote { border-left: 4px solid #d9822b; margin: 1rem 0; padding: .2rem 1rem; background: #fff8ef; }
@media (prefers-color-scheme: dark) {
  body { background: #11161c; color: #d8e0e8; } h1, h2, h3 { color: #cfe3f7; }
  th { background: #1c2530; } th, td { border-color: #33414f; } code, pre { background: #1c2530; } blockquote { background: #241d14; }
}
@media print { body { max-width: none; margin: 0; } h2 { break-after: avoid; } img { break-inside: avoid; } }
"""


def embed(match):
    path = match.group(1)
    if path.startswith(("http://", "https://", "data:", "#")):
        return match.group(0)
    file = (docs / path).resolve()
    if not file.exists():
        print("  missing image:", path)
        return match.group(0)
    mime = mimetypes.guess_type(file.name)[0] or "application/octet-stream"
    return f'src="data:{mime};base64,{base64.b64encode(file.read_bytes()).decode("ascii")}"'


def build(name, title):
    src = (docs / f"{name}.md").read_text(encoding="utf-8")
    body = markdown.markdown(src, extensions=["tables", "toc", "fenced_code", "sane_lists"])
    body = re.sub(r'src="([^"]+)"', embed, body)
    # links between the two manuals stay inside the HTML versions
    body = body.replace('href="FOG_CONSOLE_MANUAL.md"', 'href="FOG_CONSOLE_MANUAL.html"').replace('href="MANUAL.md', 'href="MANUAL.html')
    page = f"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>{title}</title><style>{css}</style></head>
<body>{body}</body></html>"""
    out = docs / f"{name}.html"
    out.write_text(page, encoding="utf-8")
    print(f"wrote {out} ({out.stat().st_size // 1024} KB)")


build("MANUAL", "FogMachine Remote System — User Manual")
build("FOG_CONSOLE_MANUAL", "Fog Console — User Manual")
