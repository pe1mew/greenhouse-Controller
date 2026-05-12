#!/usr/bin/env python3
"""
md2pdf.py — convert a markdown file to PDF with branded header/footer.

Pipeline:
    *.md  ──▶ pre-process (Figuur # numbering, version extraction)
          ──▶ markdown (Python) ──▶ *.html
          ──▶ Microsoft Edge headless via DevTools Protocol ──▶ *.pdf

Each page in the resulting PDF carries:
    Header (left)  : Kas Controller - Herenboeren Wenumseveld
    Header (right) : v<version>             (auto-extracted from "**Versie:** X.Y")
    Footer (left)  : Een RFSee product - http://www.rfsee.nl
    Footer (right) : pagina <n>

Every occurrence of "Figuur #:" in the source markdown is rewritten to
"Figuur 1:", "Figuur 2:", ... in document order.

Usage:
    python md2pdf.py <input.md> [<output.pdf>]
"""

from __future__ import annotations

import base64
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import markdown
import requests
from simple_websocket import Client as WSClient


EDGE_CANDIDATES = [
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
]


CSS = r"""
@page {
    size: A4;
    /* Top/bottom margins are wider than 20 mm to leave room for the
       header/footer templates rendered by Chromium (these templates live
       INSIDE the page margins, not in the body area). */
    margin: 22mm 18mm 22mm 20mm;
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
h1, h2, h3, h4, h5, h6 {
    page-break-after: avoid;
    break-after: avoid;
}
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
nav.toc, .toc {
    background: #fafbfc;
    border: 1px solid #ddd;
    padding: 8pt 14pt;
    margin: 6pt 0 12pt 0;
}
a { color: #1a4ea0; text-decoration: none; }
a:hover { text-decoration: underline; }
hr {
    border: 0;
    border-top: 1px solid #ccc;
    margin: 10pt 0;
}
img { max-width: 100%; }
/* Figure captions emitted as italic paragraphs immediately after a placeholder
   image line — give them a slightly muted colour. */
em { color: #333; }
"""


# Chromium header/footer templates.  Limitations to be aware of:
#  * The template's default font-size is 0 — must set it explicitly.
#  * No external resources (no @font-face, no external CSS) — inline only.
#  * Specific span classes (`date`, `title`, `url`, `pageNumber`, `totalPages`,
#    `time`) are substituted by Chromium at print time.
#  * The template renders INSIDE the page margin, so the page CSS @page margin
#    must be wide enough to leave room.
HEADER_TEMPLATE = """<div style="font-family: Segoe UI, Arial, sans-serif; font-size: 8.5pt; color: #555; width: 100%; padding: 0 20mm 0 20mm; display: flex; justify-content: space-between; align-items: center;">
  <span>Kas Controller - Herenboeren Wenumseveld</span>
  <span>{version_label}</span>
</div>"""

FOOTER_TEMPLATE = """<div style="font-family: Segoe UI, Arial, sans-serif; font-size: 8.5pt; color: #555; width: 100%; padding: 0 20mm 0 20mm; display: flex; justify-content: space-between; align-items: center;">
  <span>Een RFSee product - http://www.rfsee.nl</span>
  <span>pagina <span class="pageNumber"></span></span>
</div>"""


# ---------------------------------------------------------------------------
# Markdown pre-processing
# ---------------------------------------------------------------------------

_FIGUUR_PLACEHOLDER_RE = re.compile(r"Figuur\s+#\s*:")
_VERSION_RE = re.compile(
    r"^\s*\*\*Versie:\*\*\s*([^\s—\-]+)",
    re.MULTILINE,
)


def renumber_figures(text: str) -> tuple[str, int]:
    """Replace each 'Figuur #:' with 'Figuur N:' in document order.

    Returns (new_text, count_replaced).
    """
    counter = {"n": 0}

    def _sub(_match: re.Match) -> str:
        counter["n"] += 1
        return f"Figuur {counter['n']}:"

    new_text = _FIGUUR_PLACEHOLDER_RE.sub(_sub, text)
    return new_text, counter["n"]


def extract_version(text: str) -> str | None:
    """Return the bare version token from '**Versie:** X.Y — concept', or None."""
    match = _VERSION_RE.search(text)
    if match:
        return match.group(1).strip()
    return None


# ---------------------------------------------------------------------------
# Edge / DevTools Protocol plumbing
# ---------------------------------------------------------------------------


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


def _pick_free_port() -> int:
    """Return a free localhost TCP port that we can hand to Edge."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def _wait_for_devtools(port: int, timeout_s: float = 15.0) -> str:
    """Poll http://127.0.0.1:<port>/json/version until Edge is ready.

    Returns the webSocketDebuggerUrl of the browser endpoint."""
    deadline = time.time() + timeout_s
    last_err: str = "(no response)"
    while time.time() < deadline:
        try:
            r = requests.get(f"http://127.0.0.1:{port}/json/version", timeout=1.5)
            if r.status_code == 200:
                data = r.json()
                return data["webSocketDebuggerUrl"]
        except Exception as exc:
            last_err = str(exc)
        time.sleep(0.25)
    raise RuntimeError(
        f"Edge DevTools endpoint not reachable on port {port} within "
        f"{timeout_s}s ({last_err})."
    )


def _get_first_page_ws(port: int) -> str:
    """Return the webSocketDebuggerUrl of the first page tab."""
    r = requests.get(f"http://127.0.0.1:{port}/json", timeout=3)
    r.raise_for_status()
    tabs = r.json()
    pages = [t for t in tabs if t.get("type") == "page"]
    if not pages:
        raise RuntimeError("Edge has no page tab; cannot drive Page.printToPDF.")
    return pages[0]["webSocketDebuggerUrl"]


class _CDPClient:
    """Minimal Chrome DevTools Protocol client over a single WebSocket.

    Handles request/response correlation by 'id' and ignores unrelated events.
    """

    def __init__(self, ws_url: str):
        self._ws = WSClient.connect(ws_url)
        self._id = 0

    def close(self) -> None:
        try:
            self._ws.close()
        except Exception:
            pass

    def call(self, method: str, params: dict | None = None, timeout: float = 60.0) -> dict:
        self._id += 1
        msg_id = self._id
        payload = {"id": msg_id, "method": method, "params": params or {}}
        self._ws.send(json.dumps(payload))

        deadline = time.time() + timeout
        while time.time() < deadline:
            remaining = max(0.05, deadline - time.time())
            try:
                raw = self._ws.receive(timeout=remaining)
            except TimeoutError:
                continue
            if raw is None:
                raise RuntimeError(f"WebSocket closed waiting for {method}")
            try:
                msg = json.loads(raw)
            except Exception:
                continue
            if msg.get("id") == msg_id:
                if "error" in msg:
                    raise RuntimeError(
                        f"CDP {method} returned error: {msg['error']}"
                    )
                return msg.get("result", {})
            # Otherwise it's an event (e.g. Page.loadEventFired). Discard.
        raise TimeoutError(f"Timeout waiting for CDP response to {method}")

    def wait_event(self, method: str, timeout: float = 30.0) -> dict:
        deadline = time.time() + timeout
        while time.time() < deadline:
            remaining = max(0.05, deadline - time.time())
            try:
                raw = self._ws.receive(timeout=remaining)
            except TimeoutError:
                continue
            if raw is None:
                raise RuntimeError(f"WebSocket closed waiting for event {method}")
            try:
                msg = json.loads(raw)
            except Exception:
                continue
            if msg.get("method") == method:
                return msg.get("params", {})
        raise TimeoutError(f"Timeout waiting for CDP event {method}")


def html_to_pdf_with_headerfooter(
    html_path: Path,
    pdf_path: Path,
    version_label: str,
) -> None:
    """Render the HTML to PDF via Edge headless DevTools Protocol.

    The page is rendered with a custom header (Kas Controller branding +
    version) and footer (RFSee + page number) on every page.
    """
    edge = find_edge()
    file_url = "file:///" + str(html_path.resolve()).replace("\\", "/")
    port = _pick_free_port()

    # A throwaway user-data-dir keeps this Edge instance fully isolated from
    # the user's normal Edge profile (so we never compete with an interactive
    # Edge session and we always get a fresh start).
    with tempfile.TemporaryDirectory(prefix="md2pdf_edge_") as user_data_dir:
        cmd = [
            edge,
            "--headless=new",
            "--disable-gpu",
            "--no-first-run",
            "--no-default-browser-check",
            f"--user-data-dir={user_data_dir}",
            f"--remote-debugging-port={port}",
            # Start Edge on about:blank; we navigate ourselves so we can wait
            # cleanly for the load event.
            "about:blank",
        ]
        print(f"  Launching Edge headless on port {port} ...")
        proc = subprocess.Popen(
            cmd,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
            _wait_for_devtools(port)
            page_ws = _get_first_page_ws(port)
            cdp = _CDPClient(page_ws)
            try:
                cdp.call("Page.enable")
                # Navigate to the local HTML file and wait for it to load.
                cdp.call("Page.navigate", {"url": file_url})
                cdp.wait_event("Page.loadEventFired", timeout=45.0)
                # Give the renderer a beat to lay out tables/images.
                time.sleep(0.6)

                header_html = HEADER_TEMPLATE.format(version_label=version_label)
                footer_html = FOOTER_TEMPLATE

                # A4 portrait in inches (1 in = 25.4 mm): 8.27 × 11.69.
                # Page margins here are in inches and correspond loosely to
                # the CSS @page margin so the body text is not clipped.
                params = {
                    "landscape": False,
                    "displayHeaderFooter": True,
                    "headerTemplate": header_html,
                    "footerTemplate": footer_html,
                    "printBackground": True,
                    "preferCSSPageSize": False,
                    "paperWidth": 8.27,
                    "paperHeight": 11.69,
                    "marginTop": 0.7,     # ~18 mm — room for header
                    "marginBottom": 0.7,  # ~18 mm — room for footer
                    "marginLeft": 0.79,   # ~20 mm
                    "marginRight": 0.71,  # ~18 mm
                    "transferMode": "ReturnAsBase64",
                }
                result = cdp.call("Page.printToPDF", params, timeout=120.0)
                pdf_b64 = result.get("data")
                if not pdf_b64:
                    raise RuntimeError("Page.printToPDF returned no 'data'.")
                pdf_bytes = base64.b64decode(pdf_b64)
                pdf_path.write_bytes(pdf_bytes)
            finally:
                cdp.close()
        finally:
            try:
                # Ask Edge to quit; if it hangs, kill.
                proc.terminate()
                proc.wait(timeout=5)
            except Exception:
                try:
                    proc.kill()
                except Exception:
                    pass


# ---------------------------------------------------------------------------
# Markdown → HTML
# ---------------------------------------------------------------------------


def md_to_html(md_text: str, title: str) -> str:
    md = markdown.Markdown(
        extensions=[
            "extra",          # tables, fenced code, attr_list, def_list, etc.
            "sane_lists",
            "toc",
            "md_in_html",
        ],
        output_format="html5",
    )
    body = md.convert(md_text)
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


# ---------------------------------------------------------------------------
# Top-level conversion
# ---------------------------------------------------------------------------


def convert(md_path: Path, pdf_path: Path | None = None) -> Path:
    md_path = md_path.resolve()
    if not md_path.is_file():
        raise FileNotFoundError(md_path)
    if pdf_path is None:
        pdf_path = md_path.with_suffix(".pdf")
    pdf_path = pdf_path.resolve()

    print(f"\n=== {md_path.name} ===")
    print(f"  -> {pdf_path}")

    text = md_path.read_text(encoding="utf-8")
    version = extract_version(text) or "?"
    version_label = f"v{version}" if not version.startswith("v") else version
    text_renumbered, n_figs = renumber_figures(text)
    print(f"  Version detected:   {version_label}")
    print(f"  Figures renumbered: {n_figs}")

    html = md_to_html(text_renumbered, md_path.stem)
    # Keep the HTML next to the source so anchor links resolve and so the
    # user can inspect / re-print it manually if desired.
    html_path = md_path.with_suffix(".html")
    html_path.write_text(html, encoding="utf-8")
    print(f"  HTML written:       {html_path}")

    html_to_pdf_with_headerfooter(html_path, pdf_path, version_label)
    print(
        f"  PDF written:        {pdf_path}  "
        f"({pdf_path.stat().st_size:,} bytes)"
    )
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
