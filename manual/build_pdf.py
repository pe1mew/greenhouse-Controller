"""Convert boerHandleiding.md and beheerderHandleiding.md to PDF and RTF.

PDF  — Edge headless print-to-PDF.
RTF  — Word COM automation (requires pywin32: pip install pywin32).
"""
import markdown
import subprocess
import os
import tempfile

EDGE = r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"
BASE = os.path.dirname(os.path.abspath(__file__))

CSS = """
@page { size: A4; margin: 2cm 2.2cm 2cm 2.2cm; }
body {
    font-family: Arial, Helvetica, sans-serif;
    font-size: 10.5pt;
    line-height: 1.5;
    color: #111;
    max-width: 100%;
}
h1 { font-size: 18pt; border-bottom: 2px solid #333; padding-bottom: 4px; margin-top: 0; }
h2 { font-size: 14pt; border-bottom: 1px solid #aaa; padding-bottom: 2px; margin-top: 1.4em; }
h3 { font-size: 12pt; margin-top: 1.2em; }
h4 { font-size: 10.5pt; margin-top: 1em; }
table {
    border-collapse: collapse;
    width: 100%;
    margin: 0.8em 0;
    font-size: 9.5pt;
    page-break-inside: avoid;
}
th, td { border: 1px solid #bbb; padding: 5px 8px; text-align: left; vertical-align: top; }
th { background: #e8e8e8; font-weight: bold; }
tr:nth-child(even) td { background: #f7f7f7; }
code {
    background: #f2f2f2;
    padding: 1px 4px;
    font-family: "Courier New", Courier, monospace;
    font-size: 9pt;
    border-radius: 2px;
}
pre {
    background: #f2f2f2;
    padding: 10px 12px;
    font-family: "Courier New", Courier, monospace;
    font-size: 9pt;
    overflow-x: auto;
    border-left: 3px solid #aaa;
    page-break-inside: avoid;
}
pre code { background: none; padding: 0; }
blockquote {
    border-left: 4px solid #bbb;
    margin: 0.6em 0 0.6em 0;
    padding: 0.3em 0.8em;
    color: #444;
    background: #fafafa;
}
blockquote p { margin: 0.2em 0; }
a { color: #1a5276; }
ul, ol { margin: 0.4em 0; padding-left: 1.6em; }
li { margin: 0.15em 0; }
hr { border: none; border-top: 1px solid #ccc; margin: 1.2em 0; }
p { margin: 0.4em 0; }
"""

MANUALS = [
    ("boerHandleiding.md",       "boerHandleiding"),
    ("beheerderHandleiding.md",  "beheerderHandleiding"),
]

WD_FORMAT_RTF = 6


def build_html(md_path):
    with open(md_path, encoding="utf-8") as f:
        text = f.read()
    body = markdown.markdown(
        text,
        extensions=["tables", "fenced_code", "toc", "sane_lists", "nl2br"],
    )
    return f"""<!DOCTYPE html>
<html lang="nl">
<head>
  <meta charset="utf-8">
  <style>{CSS}</style>
</head>
<body>
{body}
</body>
</html>"""


def write_tmp_html(html):
    tmp = tempfile.NamedTemporaryFile(
        suffix=".html", delete=False, mode="w", encoding="utf-8", dir=BASE
    )
    tmp.write(html)
    tmp.close()
    return tmp.name


def make_pdf(tmp_html, pdf_path):
    file_url = "file:///" + tmp_html.replace("\\", "/")
    result = subprocess.run(
        [
            EDGE,
            "--headless",
            "--disable-gpu",
            "--run-all-compositor-stages-before-draw",
            "--virtual-time-budget=5000",
            f"--print-to-pdf={pdf_path}",
            "--no-pdf-header-footer",
            file_url,
        ],
        capture_output=True, text=True,
    )
    if os.path.exists(pdf_path):
        return os.path.getsize(pdf_path) // 1024
    raise RuntimeError(result.stderr[-300:] if result.stderr else "no output")


def make_rtf(tmp_html, rtf_path):
    import win32com.client
    word = win32com.client.Dispatch("Word.Application")
    word.Visible = False
    word.DisplayAlerts = 0          # wdAlertsNone — suppress all dialogs
    try:
        doc = word.Documents.Open(
            tmp_html,
            ConfirmConversions=False,
            ReadOnly=False,
        )
        doc.SaveAs2(rtf_path, FileFormat=WD_FORMAT_RTF)
        doc.Close(SaveChanges=False)
    finally:
        word.Quit(SaveChanges=False)
    if os.path.exists(rtf_path):
        return os.path.getsize(rtf_path) // 1024
    raise RuntimeError("RTF not created")


for md_name, base_name in MANUALS:
    md_path  = os.path.join(BASE, md_name)
    pdf_path = os.path.join(BASE, base_name + ".pdf")
    rtf_path = os.path.join(BASE, base_name + ".rtf")

    html = build_html(md_path)
    tmp  = write_tmp_html(html)

    try:
        kb = make_pdf(tmp, pdf_path)
        print(f"OK  {base_name}.pdf  ({kb} KB)")
    except Exception as e:
        print(f"FAIL  {base_name}.pdf  — {e}")

    try:
        kb = make_rtf(tmp, rtf_path)
        print(f"OK  {base_name}.rtf  ({kb} KB)")
    except Exception as e:
        print(f"FAIL  {base_name}.rtf  — {e}")

    os.unlink(tmp)
