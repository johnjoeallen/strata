#!/usr/bin/env python3
"""strata2html.py - render a Strata source file as a standalone, syntax-highlighted HTML page.
usage: strata2html.py server.stra [out.html]     (default out: <name>.strata.html next to the source)
No dependencies. The page follows the system light/dark setting. The text of the page is exactly the source (checked with --verify)."""
import html, re, sys, os

KEYWORDS = set("fn let var struct enum exception const package import static if else while for in switch default return break continue try catch throw spawn scope "
               "true false null self assert impl as".split())
EFFECTS = set("extern parks blocking nopark noyield throws".split())             # the places a program states an effect
BUILTIN_TYPES = set("u8 u16 u32 u64 usize bool string Duration Instant Exception".split())
NOT_CALLS = KEYWORDS | EFFECTS | {"fn"}

TOKEN = re.compile(r"""
    (?P<comment>//[^\n]*|/\*.*?\*/)
  | (?P<string>"(?:\\.|[^"\\\n])*")
  | (?P<char>'(?:\\.|[^'\\\n])')
  | (?P<number>\d+(?:\.\d+)?(?:\.(?:kb|mb|gb|ms|us|ns|s)\b|(?:ms|us|ns|s)\b)?)
  | (?P<ident>[A-Za-z_][A-Za-z0-9_]*)
  | (?P<arrow>->)
  | (?P<op>\?\?|\?:|&&|\|\||==|!=|<=|>=|\.\.\.?|\+\+|--|[+\-*/%=<>!?:.,;&|(){}\[\]])
  | (?P<ws>\s+)
  | (?P<other>.)
""", re.X | re.S)

def classify(text):
    """yield (css class or None, text) for every token of the whole source."""
    pos = 0; prev_sig = ""; prev_prev = ""
    toks = []
    for m in TOKEN.finditer(text):
        toks.append((m.lastgroup, m.group()))
    n = len(toks)
    def next_sig(i):
        j = i + 1
        while j < n and toks[j][0] == "ws": j += 1
        return toks[j] if j < n else (None, "")
    last_sig = ("", ""); before_last = ("", "")
    in_import = False
    for i, (kind, tx) in enumerate(toks):
        cls = None
        if kind == "comment": cls = "c"
        elif kind == "string": cls = "s"
        elif kind == "char": cls = "s"
        elif kind == "number": cls = "n"
        elif kind == "arrow": cls = "ar"
        elif kind == "ident":
            nk, nt = next_sig(i)
            after_dot = last_sig[1] == "."
            if tx in EFFECTS: cls = "e"
            elif tx in KEYWORDS: cls = "k"
            elif last_sig[1] == "import" or (in_import and (after_dot or last_sig[1] == "import")): cls = "mod"
            elif tx in BUILTIN_TYPES: cls = "t"
            elif last_sig[1] in ("break", "continue") : cls = "lab"
            elif nt == ":" and nk == "op" and last_sig[1] in ("", "{", "}", ";") and tx.islower(): cls = "lab" if _is_label(toks, i) else None
            elif last_sig[1] == "fn" or (nt == "(" and tx not in NOT_CALLS): cls = "f" if not tx[0].isupper() else "t"
            elif tx.isupper() and len(tx) > 1: cls = "const"
            elif tx[0].isupper(): cls = "t"
            elif after_dot: cls = "p"
        elif kind == "op" and tx in ("?", "!"): cls = "op"
        if kind == "ident" and tx == "import": in_import = True
        if kind == "ws" and "\n" in tx: in_import = False
        yield cls, tx
        if kind not in ("ws", "comment"):
            before_last, last_sig = last_sig, (kind, tx)

def _is_label(toks, i):
    # `name:` at the start of a statement followed by a loop keyword is a label (e.g. `admission: while (...)`)
    j = i + 1
    while j < len(toks) and toks[j][0] == "ws" : j += 1       # at ':'
    j += 1
    while j < len(toks) and toks[j][0] == "ws": j += 1
    return j < len(toks) and toks[j][1] in ("while", "for")

def render_lines(text):
    lines, cur = [], []
    for cls, tx in classify(text):
        parts = tx.split("\n")
        for k, part in enumerate(parts):
            if k > 0: lines.append("".join(cur)); cur = []
            if part == "": continue
            e = html.escape(part, quote=False)
            cur.append(f'<span class="{cls}">{e}</span>' if cls else e)
    lines.append("".join(cur))
    return lines

PAGE = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{title}</title>
<style>
:root {{
  --bg: #fbfbfa; --panel: #ffffff; --fg: #24292f; --muted: #6e7781; --line: #d8dee4; --hover: #f3f4f6;
  --c: #6a737d; --k: #8250df; --t: #0a6ea8; --f: #6f42c1; --s: #0a7a3d; --n: #b35900; --e: #c4261d; --mod: #1a7f77; --const: #953800; --p: #24292f; --lab: #9a6700; --ar: #8250df; --op: #57606a;
}}
@media (prefers-color-scheme: dark) {{
  :root:not([data-theme="light"]) {{
    --bg: #0d1117; --panel: #161b22; --fg: #d5dbe3; --muted: #8b949e; --line: #30363d; --hover: #1c2230;
    --c: #8b949e; --k: #c297ff; --t: #6cb6ff; --f: #d2a8ff; --s: #7ee787; --n: #ffa657; --e: #ff7b72; --mod: #56d4c8; --const: #ffb27a; --p: #d5dbe3; --lab: #e3b341; --ar: #c297ff; --op: #9aa4b2;
  }}
}}
:root[data-theme="dark"] {{
  --bg: #0d1117; --panel: #161b22; --fg: #d5dbe3; --muted: #8b949e; --line: #30363d; --hover: #1c2230;
  --c: #8b949e; --k: #c297ff; --t: #6cb6ff; --f: #d2a8ff; --s: #7ee787; --n: #ffa657; --e: #ff7b72; --mod: #56d4c8; --const: #ffb27a; --p: #d5dbe3; --lab: #e3b341; --ar: #c297ff; --op: #9aa4b2;
}}
* {{ box-sizing: border-box; }}
html {{ background: var(--bg); }}
body {{ margin: 0; background: var(--bg); color: var(--fg); font: 15px/1.5 system-ui, -apple-system, "Segoe UI", sans-serif; }}
header {{ padding: 18px 16px 12px; max-width: 1200px; margin: 0 auto; }}
h1 {{ font: 600 18px/1.3 ui-monospace, SFMono-Regular, Menlo, Consolas, monospace; margin: 0 0 4px; }}
.sub {{ color: var(--muted); font-size: 13px; }}
.legend {{ display: flex; flex-wrap: wrap; gap: 6px 14px; margin-top: 10px; font: 12px/1.4 ui-monospace, SFMono-Regular, Menlo, Consolas, monospace; }}
.legend span::before {{ content: ""; display: inline-block; width: 8px; height: 8px; border-radius: 2px; margin-right: 5px; background: currentColor; }}
.tools {{ margin-top: 10px; display: flex; gap: 8px; }}
button {{ font: 12px system-ui, sans-serif; padding: 4px 10px; border: 1px solid var(--line); background: var(--panel); color: var(--fg); border-radius: 6px; cursor: pointer; }}
button:hover {{ background: var(--hover); }}
main {{ max-width: 1200px; margin: 0 auto 40px; padding: 0 16px; }}
pre.code {{ margin: 0; padding: 12px 0; background: var(--panel); border: 1px solid var(--line); border-radius: 8px; overflow-x: auto;
  font: 13px/1.55 ui-monospace, SFMono-Regular, Menlo, Consolas, "Liberation Mono", monospace; tab-size: 4; counter-reset: ln; }}
pre.code.wrap .line {{ white-space: pre-wrap; word-break: break-word; }}
.line {{ display: block; padding: 0 16px 0 0; white-space: pre; min-width: max-content; }}
.line::before {{ counter-increment: ln; content: counter(ln); display: inline-block; width: 4.2em; padding-right: 1.2em; margin-right: 1em; text-align: right;
  color: var(--muted); border-right: 1px solid var(--line); user-select: none; -webkit-user-select: none; }}
.line:hover {{ background: var(--hover); }}
.line:target {{ background: color-mix(in srgb, var(--lab) 18%, transparent); }}
.line a {{ position: absolute; opacity: 0; }}
.c {{ color: var(--c); font-style: italic; }} .k {{ color: var(--k); }} .t {{ color: var(--t); }} .f {{ color: var(--f); }} .s {{ color: var(--s); }}
.n {{ color: var(--n); }} .e {{ color: var(--e); font-weight: 600; }} .mod {{ color: var(--mod); }} .const {{ color: var(--const); }} .p {{ color: var(--p); }}
.lab {{ color: var(--lab); }} .ar {{ color: var(--ar); font-weight: 600; }} .op {{ color: var(--op); }}
.legend .c {{ font-style: italic; }}
@media (max-width: 600px) {{ pre.code {{ font-size: 12px; }} .line::before {{ width: 3em; margin-right: .6em; padding-right: .6em; }} }}
</style>
</head>
<body>
<header>
  <h1>{name}</h1>
  <div class="sub">{lines} lines &middot; pseudo-Strata &middot; generated from <code>{src}</code> by <code>strata2html.py</code> &middot; the text on this page is the source, unchanged</div>
  <div class="legend">
    <span class="k">keyword</span><span class="t">type</span><span class="f">function</span><span class="e">effect keyword (extern, parks, blocking, nopark, noyield, throws)</span>
    <span class="s">string</span><span class="n">number</span><span class="mod">module</span><span class="const">CONSTANT</span><span class="lab">label</span><span class="c">comment</span>
  </div>
  <div class="tools"><button id="wrap" type="button">Wrap lines</button><button id="theme" type="button">Light / dark</button></div>
</header>
<main>
<pre class="code" id="code" aria-label="{name} source code">{body}</pre>
</main>
<script>
(function () {{
  var code = document.getElementById('code');
  document.getElementById('wrap').addEventListener('click', function () {{ code.classList.toggle('wrap'); }});
  document.getElementById('theme').addEventListener('click', function () {{
    var r = document.documentElement, dark = matchMedia('(prefers-color-scheme: dark)').matches;
    var cur = r.getAttribute('data-theme') || (dark ? 'dark' : 'light');
    r.setAttribute('data-theme', cur === 'dark' ? 'light' : 'dark');
  }});
}})();
</script>
</body>
</html>
"""

def build(src_path, out_path):
    text = open(src_path).read()
    lines = render_lines(text.rstrip("\n"))
    body = "".join(f'<span class="line" id="L{i}">{ln}</span>' for i, ln in enumerate(lines, 1))
    name = os.path.basename(src_path).replace(".stra", ".strata")
    page = PAGE.format(title=name, name=name, lines=len(lines), src=os.path.basename(src_path), body=body)
    open(out_path, "w").write(page)
    return text.rstrip("\n").split("\n"), page

def verify(src_lines, page):
    body = re.search(r'<pre class="code"[^>]*>(.*)</pre>', page, re.S).group(1)
    got = [html.unescape(re.sub(r"<[^>]+>", "", l)) for l in re.findall(r'<span class="line" id="L\d+">(.*?)</span>(?=<span class="line"|\Z)', body, re.S)]
    # nested spans: re-extract more robustly by splitting on line markers
    chunks = re.split(r'<span class="line" id="L\d+">', body)[1:]
    got = []
    for c in chunks:
        c = re.sub(r"</span>$", "", c.rstrip())            # closing tag of the .line span
        got.append(html.unescape(re.sub(r"<[^>]+>", "", c)))
    return got == src_lines, len(got), len(src_lines)

if __name__ == "__main__":
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 and not sys.argv[2].startswith("--") else re.sub(r"\.stra$", ".strata.html", src)
    src_lines, page = build(src, out)
    ok, a, b = verify(src_lines, page)
    print(f"wrote {out}: {b} lines; text identical to the source: {ok}")
    if not ok: sys.exit(1)
