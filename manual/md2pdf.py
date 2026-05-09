#!/usr/bin/env python3
"""
md2pdf.py — convert a markdown file to PDF.

Pipeline:
    *.md  ──▶ markdown (Python)  ──▶ *.html  ──▶ Microsoft Edge --headless  ──▶ *.pdf

Usage:
    python md2pdf.py <input.md> [<output.pdf>]

Behavior:
    * Each H1 and H2 begins on a new page (CSS: page-break-before: always).
    * Tables, fenced code blocks, and ASCII LCD art are preserved.
    * Body font is a clean sans-serif; code blocks use a monospace font.
    * Output PDF page size: A4 with 20 mm margins.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import markdown


EDGE_CANDIDATES = [
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
]


CSS = r"""
@page {
    size: A4;
    margin: 20mm 18mm 22mm 20mm;
}
html, body {
    font-family: "Segoe UI", "Helvetica Neue", Arial, sans-serif;
    font-size: 10.5pt;
    line-height: 1.45;
    color: #111;
    margin: 0;
    padding: 0;
}
/* Each top-level chapter (H1 + H2) starts on a new page. */
h1, h2 {
    page-break-before: always;
}
/* The very first heading should not push a blank page in front of itself. */
h1:first-of-type, body > h1:first-child, body > h2:first-child {
    page-break-before: auto;
}
h1 {
    font-size: 22pt;
    border-bottom: 2px solid #333;
    padding-bottom: 4px;
    margin-top: 0;
}
h2 {
    font-size: 16pt;
    border-bottom: 1px solid #999;
    padding-bottom: 3px;
    margin-top: 12pt;
}
h3 {
    font-size: 13pt;
    margin-top: 14pt;
    color: #222;
}
h4 {
    font-size: 11.5pt;
    margin-top: 12pt;
    color: #333;
}
h5, h6 {
    font-size: 11pt;
    margin-top: 10pt;
    color: #444;
}
/* Headings should not be orphaned at the bottom of a page. */
h1, h2, h3, h4, h5, h6 {
    page-break-after: avoid;
    break-after: avoid;
}
/* Body text. */
p {
    margin: 0 0 6pt 0;
    orphans: 3;
    widows: 3;
}
ul, ol {
    margin: 4pt 0 8pt 0;
    padding-left: 22pt;
}
li {
    margin: 2pt 0;
}
/* Block quotes. */
blockquote {
    border-left: 3px solid #4a90c0;
    background: #eef5fb;
    margin: 8pt 0;
    padding: 6pt 12pt;
    color: #234;
    page-break-inside: avoid;
    break-inside: avoid;
}
blockquote > p { margin: 0 0 4pt 0; }
blockquote > p:last-child { margin-bottom: 0; }
/* Inline + block code. */
code {
    font-family: "Consolas", "Cascadia Code", "Courier New", monospace;
    font-size: 9.5pt;
    background: #f3f3f3;
    padding: 1px 4px;
    border-radius: 3px;
}
pre {
    font-family: "Consolas", "Cascadia Code", "Courier New", monospace;
    font-size: 9pt;
    line-height: 1.25;
    background: #f7f7f7;
    border: 1px solid #ddd;
    padding: 8pt 10pt;
    border-radius: 4px;
    overflow-x: auto;
    page-break-inside: avoid;
    break-inside: avoid;
}
pre code {
    background: transparent;
    padding: 0;
    border-radius: 0;
}
/* Tables. */
table {
    border-collapse: collapse;
    margin: 8pt 0;
    width: 100%;
    page-break-inside: auto;
}
thead { display: table-header-group; }
tr { page-break-inside: avoid; break-inside: avoid; }
th, td {
    border: 1px solid #bbb;
    padding: 4pt 6pt;
    vertical-align: top;
    font-size: 10pt;
}
th {
    background: #eaeef3;
    text-align: left;
}
/* Table-of-contents (Markdown-rendered). */
nav.toc, .toc {
    background: #fafbfc;
    border: 1px solid #ddd;
    padding: 8pt 14pt;
    margin: 6pt 0 12pt 0;
}
/* Links. */
a { color: #1a4ea0; text-decoration: none; }
a:hover { text-decoration: underline; }
/* Horizontal rule = soft visual separator (page break already handled by H2). */
hr {
    border: 0;
    border-top: 1px solid #ccc;
    margin: 10pt 0;
}
/* Image placeholders (we don't actually have images; this just keeps any future
 * image inline-block aligned). */
img { max-width: 100%; }
"""


def find_edge() -> str:
    for path in EDGE_CANDIDATES:
        if os.path.isfile(path):
            return path
    found = shutil.which("msedge")
    if found:
        return found
    raise FileNotFoundError(
        "Microsoft Edge not found. Tried: " + ", ".join(EDGE_CANDIDATES)
    )


def md_to_html(md_path: Path) -> str:
    text = md_path.read_text(encoding="utf-8")
    md = markdown.Markdown(
        extensions=[
            "extra",          # tables, fenced code, attr_list, def_list, etc.
            "sane_lists",
            "toc",
            "md_in_html",
        ],
        output_format="html5",
    )
    body = md.convert(text)
    title = md_path.stem
    html = f"""<!DOCTYPE html>
<html lang="nl">
<head>
<meta charset="utf-8">
<title>{title}</title>
<style>
{CSS}
</style>
</head>
<body>
{body}
</body>
</html>
"""
    return html


def html_to_pdf(html_path: Path, pdf_path: Path) -> None:
    import time

    edge = find_edge()
    # Edge headless requires absolute file:// URLs; convert path.
    file_url = "file:///" + str(html_path.resolve()).replace("\\", "/")

    # Capture the PDF's mtime before we run Edge so we can detect a fresh write
    # without having to delete the old file (deletion may fail if a viewer has
    # the file locked).
    prev_mtime = pdf_path.stat().st_mtime if pdf_path.exists() else 0.0
    prev_size = pdf_path.stat().st_size if pdf_path.exists() else 0

    cmd = [
        edge,
        "--headless=new",
        "--disable-gpu",
        f"--print-to-pdf={str(pdf_path.resolve())}",
        "--no-pdf-header-footer",
        file_url,
    ]
    print("  Edge command:", " ".join(f'"{c}"' if " " in c else c for c in cmd))
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
    # Edge headless may exit with rc != 0 even on success and may write the PDF
    # asynchronously after the parent process returns. Poll for up to ~30 s.
    deadline = time.time() + 30.0
    while time.time() < deadline:
        if pdf_path.exists():
            cur_size = pdf_path.stat().st_size
            cur_mtime = pdf_path.stat().st_mtime
            # Fresh PDF: either the file was rewritten (newer mtime) or was
            # absent before. Wait until the size has settled.
            if cur_size > 0 and (cur_mtime > prev_mtime or prev_size == 0):
                # Verify size is stable (file finished writing).
                time.sleep(0.5)
                if pdf_path.stat().st_size == cur_size:
                    return
        time.sleep(0.25)
    raise RuntimeError(
        f"Edge headless print failed: PDF not produced after 30 s.\n"
        f"rc={result.returncode}\n"
        f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
    )


def convert(md_path: Path, pdf_path: Path | None = None) -> Path:
    md_path = md_path.resolve()
    if not md_path.is_file():
        raise FileNotFoundError(md_path)
    if pdf_path is None:
        pdf_path = md_path.with_suffix(".pdf")
    pdf_path = pdf_path.resolve()

    print(f"\n=== {md_path.name} ===")
    print(f"  -> {pdf_path}")
    html = md_to_html(md_path)
    # Keep the HTML next to the source so anchor links resolve and the user can
    # inspect / re-print it manually if desired.
    html_path = md_path.with_suffix(".html")
    html_path.write_text(html, encoding="utf-8")
    print(f"  HTML written:    {html_path}")
    html_to_pdf(html_path, pdf_path)
    print(f"  PDF written:     {pdf_path}  ({pdf_path.stat().st_size:,} bytes)")
    return pdf_path


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__)
        return 2
    md_path = Path(argv[1])
    pdf_path = Path(argv[2]) if len(argv) > 2 else None
    try:
        convert(md_path, pdf_path)
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
