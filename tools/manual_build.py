#!/usr/bin/env python3
"""Build the user manual: Markdown -> self-contained HTML (images embedded) -> PDF via headless Chrome.
No third-party modules; covers the Markdown subset the manual uses (headings, paragraphs, bullet lists,
numbered lists, pipe tables, fenced code, images, bold, inline code, links).
Usage: manual_build.py <manual.md> <out.html> [<out.pdf>]"""
import base64, html, pathlib, re, subprocess, sys

CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
CSS = """
@page { size: A4; margin: 18mm 16mm; }
body { font: 11pt/1.45 -apple-system, "Helvetica Neue", Helvetica, Arial, sans-serif; color: #1a1a1a; max-width: 178mm; margin: 0 auto; }
h1 { font-size: 24pt; margin: 0 0 4pt; } h1 + p { color: #555; margin-top: 0; }
h2 { font-size: 16pt; margin: 22pt 0 8pt; page-break-after: avoid; border-bottom: 1px solid #bbb; padding-bottom: 2pt; }
h3 { font-size: 12.5pt; margin: 14pt 0 6pt; page-break-after: avoid; }
p { margin: 6pt 0; } li { margin: 3pt 0; }
img { max-width: 84mm; max-height: 62mm; display: block; margin: 5pt auto; border: 1px solid #ccc; page-break-inside: avoid; }   /* 780x560 shots at 84 mm: three fit on one A4 page (user 2026-11-05) */
table { border-collapse: collapse; width: 100%; margin: 8pt 0; font-size: 9.5pt; page-break-inside: auto; }
th, td { border: 1px solid #bbb; padding: 3pt 5pt; vertical-align: top; text-align: left; }
th { background: #eee; } tr { page-break-inside: avoid; }
code { font: 9.5pt Menlo, monospace; background: #f2f2f2; padding: 0 2pt; }
pre { background: #f2f2f2; border: 1px solid #ddd; padding: 6pt; font: 9pt/1.35 Menlo, monospace; white-space: pre-wrap; page-break-inside: avoid; }
"""

def inline(s, base):
    s = html.escape(s, quote=False)
    s = re.sub(r"`([^`]+)`", r"<code>\1</code>", s)
    s = re.sub(r"\*\*(.+?)\*\*", r"<strong>\1</strong>", s)
    s = re.sub(r"\[([^\]]+)\]\(([^)]+)\)", r'<a href="\2">\1</a>', s)
    return s

def image(alt, src, base):
    p = base / src
    data = base64.b64encode(p.read_bytes()).decode()
    return f'<img src="data:image/png;base64,{data}" alt="{html.escape(alt)}">'

def convert(md, base):
    out, lines, i = [], md.splitlines(), 0
    while i < len(lines):
        ln = lines[i]
        if ln.startswith("```"):
            j = i + 1
            while j < len(lines) and not lines[j].startswith("```"): j += 1
            out.append("<pre>" + html.escape("\n".join(lines[i + 1:j])) + "</pre>"); i = j + 1; continue
        m = re.match(r"^(#{1,3}) (.*)", ln)
        if m: out.append(f"<h{len(m[1])}>{inline(m[2], base)}</h{len(m[1])}>"); i += 1; continue
        m = re.match(r"^!\[([^\]]*)\]\(([^)]+)\)\s*$", ln)
        if m: out.append(image(m[1], m[2], base)); i += 1; continue
        if ln.startswith("|"):
            rows = []
            while i < len(lines) and lines[i].startswith("|"):
                cells = [c.strip() for c in lines[i].strip().strip("|").split("|")]
                if not all(re.fullmatch(r":?-+:?", c) for c in cells): rows.append(cells)
                i += 1
            t = "<table><tr>" + "".join(f"<th>{inline(c, base)}</th>" for c in rows[0]) + "</tr>"
            for r in rows[1:]: t += "<tr>" + "".join(f"<td>{inline(c, base)}</td>" for c in r) + "</tr>"
            out.append(t + "</table>"); continue
        m = re.match(r"^(\d+)\. (.*)", ln)
        if m or ln.startswith("- "):
            tag = "ol" if m else "ul"; items = []
            pat = r"^\d+\. (.*)" if m else r"^- (.*)"
            while i < len(lines) and re.match(pat, lines[i]):
                items.append(re.match(pat, lines[i])[1]); i += 1
            out.append(f"<{tag}>" + "".join(f"<li>{inline(x, base)}</li>" for x in items) + f"</{tag}>"); continue
        if not ln.strip(): i += 1; continue
        para = []
        while i < len(lines) and lines[i].strip() and not re.match(r"^(#|!\[|\||- |\d+\. |```)", lines[i]):
            para.append(lines[i]); i += 1
        out.append("<p>" + inline(" ".join(para), base) + "</p>")
    return "\n".join(out)

def main():
    src = pathlib.Path(sys.argv[1]); out_html = pathlib.Path(sys.argv[2]); out_pdf = pathlib.Path(sys.argv[3]) if len(sys.argv) > 3 else None
    body = convert(src.read_text(), src.parent)
    title = re.search(r"^# (.*)", src.read_text(), re.M)[1]
    out_html.write_text(f"<!doctype html><html><head><meta charset=utf-8><title>{html.escape(title)}</title><style>{CSS}</style></head><body>{body}</body></html>")
    if out_pdf:
        r = subprocess.run([CHROME, "--headless", "--disable-gpu", "--no-pdf-header-footer", f"--print-to-pdf={out_pdf.resolve()}", out_html.resolve().as_uri()], capture_output=True, text=True, timeout=120)
        if not out_pdf.exists() or out_pdf.stat().st_size < 10000: print(r.stderr[-800:]); sys.exit(1)
    print(out_html, out_pdf or "")

if __name__ == "__main__": main()
