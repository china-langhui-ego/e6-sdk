#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Generate a self-contained HTML documentation page from a Markdown file.

Features:
  * markdown-it-py rendering (GitHub-flavoured tables / fenced code)
  * Pygments syntax highlighting for fenced code blocks
  * Sticky sidebar table-of-contents with scrollspy + live filter
  * Copy-to-clipboard buttons on code blocks
  * Light / dark theme toggle, responsive layout, back-to-top
  * GitHub-style anchor ids so in-doc links keep working

Usage:
    python3 tools/md2html.py [input.md] [output.html]

Defaults: Readme.md -> Readme.html (repo root).
"""
from __future__ import annotations

import re
import sys
from datetime import datetime, timezone
from pathlib import Path

from markdown_it import MarkdownIt
from markdown_it.token import Token
from pygments import highlight
from pygments.formatters import HtmlFormatter
from pygments.lexers import ClassNotFound, get_lexer_by_name, guess_lexer

ROOT = Path(__file__).resolve().parent.parent
SRC = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "Readme.md"
OUT = Path(sys.argv[2]) if len(sys.argv) > 2 else SRC.with_suffix(".html")
DOC_LANG = "en" if "_en" in SRC.stem else "zh-CN"

# --------------------------------------------------------------------------
# Heading slugification (GitHub-compatible)
# --------------------------------------------------------------------------

def github_slug(text: str) -> str:
    """GitHub-compatible anchor slug.

    Lowercase, drop punctuation, then turn each space into a hyphen —
    but DO NOT collapse consecutive hyphens (removed '&'/'/' leave two
    spaces, which become '--', exactly like GitHub).
    """
    s = text.lower()
    s = re.sub(r"[^\w -]", "", s, flags=re.UNICODE)  # keep word chars, space, hyphen
    s = s.replace(" ", "-").strip("-")
    return s or "section"


def make_unique(slug: str, seen: dict) -> str:
    if slug not in seen:
        seen[slug] = 0
        return slug
    seen[slug] += 1
    return f"{slug}-{seen[slug]}"


def heading_text(tokens: list[Token], idx: int) -> str:
    """Plain-text content of the inline token following a heading_open."""
    inline = tokens[idx + 1]
    return "".join(getattr(c, "content", "") for c in inline.children)


# --------------------------------------------------------------------------
# Pygments highlighting hook for markdown-it fences
# --------------------------------------------------------------------------

_formatter = HtmlFormatter(nowrap=True, style="default")


def _pygments_highlight(code: str, lang: str, attrs: str) -> str:
    if not lang:
        return ""
    try:
        lexer = get_lexer_by_name(lang)
    except ClassNotFound:
        try:
            lexer = guess_lexer(code)
        except Exception:
            return ""
    return highlight(code, lexer, _formatter)


# --------------------------------------------------------------------------
# Render
# --------------------------------------------------------------------------

md = MarkdownIt("commonmark", {"html": True, "highlight": _pygments_highlight})
md.enable(["table", "strikethrough", "fence"])
# 'fence' is part of commonmark; enabling table/strikethrough for GFM extras.

src = SRC.read_text(encoding="utf-8")
tokens = md.parse(src)

# Pass 1: assign unique heading ids, collect TOC entries.
toc = []          # (level, title, slug)
used = {}
heading_ids = {}  # token index -> slug (for pass 2)
for i, tok in enumerate(tokens):
    if tok.type == "heading_open":
        level = int(tok.tag[1])
        title = heading_text(tokens, i)
        slug = make_unique(github_slug(title), used)
        tok.attrSet("id", slug)
        heading_ids[i] = slug
        toc.append((level, title, slug))

# Pass 2: insert a hover-anchor link inside each heading, right before its close.
anchors: list[tuple[int, str]] = []
for i, slug in heading_ids.items():
    anchors.append((i + 2, slug))  # heading_open = i, inline = i+1, close = i+2
for close_idx, slug in sorted(anchors, reverse=True):
    a = Token("html_inline", "", 0)
    a.content = f'<a class="anchor-link" href="#{slug}" aria-hidden="true" title="链接到本小节"></a>'
    tokens.insert(close_idx, a)

body = md.renderer.render(tokens, md.options, {})

# --------------------------------------------------------------------------
# TOC tree HTML
# --------------------------------------------------------------------------

class _Node:
    __slots__ = ("level", "title", "slug", "children")

    def __init__(self, level, title, slug):
        self.level, self.title, self.slug = level, title, slug
        self.children = []


def render_toc(items: list[tuple[int, str, str]]) -> str:
    root = _Node(0, None, None)
    stack = [root]
    for level, title, slug in items:
        node = _Node(level, title, slug)
        while stack and stack[-1].level >= level:
            stack.pop()
        stack[-1].children.append(node)
        stack.append(node)

    def render(node: _Node) -> str:
        if node.slug is None:  # root
            return '<ul class="toc-root">' + "".join(render(c) for c in node.children) + "</ul>"
        s = f'<li class="toc-l{node.level}" data-title="{node.title}">' \
            f'<a href="#{node.slug}">{node.title}</a>'
        if node.children:
            s += "<ul>" + "".join(render(c) for c in node.children) + "</ul>"
        return s + "</li>"

    return render(root)


toc_html = render_toc(toc)

# Title = first h1, fall back to file name.
title = toc[0][1] if toc and toc[0][0] == 1 else SRC.stem

generated_at = datetime.now(timezone.utc).astimezone().strftime("%Y-%m-%d %H:%M")

# --------------------------------------------------------------------------
# Template (CSS + JS)
# --------------------------------------------------------------------------

HTML_TEMPLATE = """<!DOCTYPE html>
<html lang="{{DOC_LANG}}" data-theme="auto">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="generator" content="tools/md2html.py">
<title>{{TITLE}}</title>
<style>
/* ---------- tokens ---------- */
:root {
  --bg: #ffffff;
  --bg-sidebar: #f7f7f9;
  --bg-hover: #ececf1;
  --bg-code: #f6f8fa;
  --bg-code-header: #eceff3;
  --border: #e3e6eb;
  --text: #1f2328;
  --text-muted: #57606a;
  --accent: #2563eb;
  --accent-soft: rgba(37, 99, 235, .10);
  --shadow: 0 1px 3px rgba(31,35,40,.08);
  --radius: 8px;
  --header-h: 52px;
  --sidebar-w: 300px;
}
[data-theme="dark"] {
  --bg: #0d1117;
  --bg-sidebar: #161b22;
  --bg-hover: #1f2630;
  --bg-code: #161b22;
  --bg-code-header: #1d2430;
  --border: #30363d;
  --text: #e6edf3;
  --text-muted: #9198a1;
  --accent: #58a6ff;
  --accent-soft: rgba(88,166,255,.12);
  --shadow: 0 1px 3px rgba(0,0,0,.4);
}
/* ---------- base ---------- */
* { box-sizing: border-box; }
html { scroll-behavior: smooth; -webkit-text-size-adjust: 100%; }
body {
  margin: 0;
  font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto,
    "PingFang SC", "Hiragino Sans GB", "Microsoft YaHei", "Noto Sans CJK SC",
    "Noto Sans SC", sans-serif;
  font-size: 15px; line-height: 1.75; color: var(--text); background: var(--bg);
}
h1,h2,h3,h4,h5,h6 {
  line-height: 1.4; margin: 1.6em 0 .6em; font-weight: 650;
  scroll-margin-top: calc(var(--header-h) + 16px);
}
h1 { font-size: 1.75em; border-bottom: 1px solid var(--border); padding-bottom: .35em; }
h2 { font-size: 1.45em; border-bottom: 1px solid var(--border); padding-bottom: .3em; }
h3 { font-size: 1.2em; } h4 { font-size: 1.05em; } h5,h6 { font-size: 1em; }
p { margin: .7em 0; }
a { color: var(--accent); text-decoration: none; }
a:hover { text-decoration: underline; }
hr { border: none; border-top: 1px solid var(--border); margin: 2em 0; }
code {
  font-family: ui-monospace, SFMono-Regular, "SF Mono", Menlo, Consolas,
    "Liberation Mono", monospace;
  font-size: .88em; background: var(--bg-code); padding: .15em .35em;
  border-radius: 4px;
}
pre {
  position: relative; margin: .9em 0; background: var(--bg-code);
  border: 1px solid var(--border); border-radius: var(--radius);
  overflow: auto; padding: 12px 14px; line-height: 1.55;
}
pre code { background: none; padding: 0; font-size: .86em; }
pre .hljs,
pre .code-content { display: block; }
blockquote {
  margin: .9em 0; padding: .1em 1em; color: var(--text-muted);
  border-left: 4px solid var(--accent); background: var(--accent-soft);
  border-radius: 0 var(--radius) var(--radius) 0;
}
table {
  border-collapse: collapse; width: 100%; margin: 1em 0; display: block;
  overflow-x: auto; font-size: .92em;
}
th, td { border: 1px solid var(--border); padding: 7px 11px; text-align: left; }
th { background: var(--bg-sidebar); font-weight: 600; white-space: nowrap; }
tbody tr:nth-child(even) { background: var(--bg-sidebar); }
ul, ol { padding-left: 1.6em; }
li { margin: .22em 0; }
img { max-width: 100%; }
/* pygments token colors (light / dark) */
[data-theme="dark"] .code-content { color: #c9d1d9; }
[data-theme="dark"] .code-content .k { color: #ff7b72; }
[data-theme="dark"] .code-content .kc { color: #79c0ff; }
[data-theme="dark"] .code-content .kd { color: #ff7b72; }
[data-theme="dark"] .code-content .kt { color: #ff7b72; }
[data-theme="dark"] .code-content .s, [data-theme="dark"] .code-content .s1,
[data-theme="dark"] .code-content .s2, [data-theme="dark"] .code-content .sb,
[data-theme="dark"] .code-content .sc, [data-theme="dark"] .code-content .sd,
[data-theme="dark"] .code-content .se, [data-theme="dark"] .code-content .sh,
[data-theme="dark"] .code-content .si, [data-theme="dark"] .code-content .sx { color: #a5d6ff; }
[data-theme="dark"] .code-content .m, [data-theme="dark"] .code-content .mi,
[data-theme="dark"] .code-content .mf, [data-theme="dark"] .code-content .mo,
[data-theme="dark"] .code-content .mh, [data-theme="dark"] .code-content .mn,
[data-theme="dark"] .code-content .mx, [data-theme="dark"] .code-content .il { color: #79c0ff; }
[data-theme="dark"] .code-content .c, [data-theme="dark"] .code-content .c1,
[data-theme="dark"] .code-content .cm, [data-theme="dark"] .code-content .cc,
[data-theme="dark"] .code-content .cs { color: #8b949e; font-style: italic; }
[data-theme="dark"] .code-content .n, [data-theme="dark"] .code-content .na,
[data-theme="dark"] .code-content .nb { color: #c9d1d9; }
[data-theme="dark"] .code-content .nf, [data-theme="dark"] .code-content .fm { color: #d2a8ff; }
[data-theme="dark"] .code-content .nc, [data-theme="dark"] .code-content .nd,
[data-theme="dark"] .code-content .nn { color: #ffa657; }
[data-theme="dark"] .code-content .o, [data-theme="dark"] .code-content .ow { color: #ff7b72; }
[data-theme="dark"] .code-content .p { color: #c9d1d9; }
[data-theme="dark"] .code-content .bp { color: #ffa657; }
[data-theme="dark"] .code-content .ne { color: #ffa657; }
[data-theme="dark"] .code-content .nv { color: #ff7b72; }
[data-theme="dark"] .code-content .nt { color: #7ee787; }
[data-theme="dark"] .code-content .no { color: #79c0ff; }
[data-theme="light"] .code-content { color: #24292f; }
[data-theme="light"] .code-content .k { color: #cf222e; }
[data-theme="light"] .code-content .kc, [data-theme="light"] .code-content .kt { color: #0550ae; }
[data-theme="light"] .code-content .kd { color: #cf222e; }
[data-theme="light"] .code-content .s, [data-theme="light"] .code-content .s1,
[data-theme="light"] .code-content .s2, [data-theme="light"] .code-content .sb,
[data-theme="light"] .code-content .sc, [data-theme="light"] .code-content .sd,
[data-theme="light"] .code-content .se, [data-theme="light"] .code-content .sh,
[data-theme="light"] .code-content .si, [data-theme="light"] .code-content .sx { color: #0a3069; }
[data-theme="light"] .code-content .m, [data-theme="light"] .code-content .mi,
[data-theme="light"] .code-content .mf, [data-theme="light"] .code-content .mo,
[data-theme="light"] .code-content .mh, [data-theme="light"] .code-content .mn,
[data-theme="light"] .code-content .mx, [data-theme="light"] .code-content .il { color: #0550ae; }
[data-theme="light"] .code-content .c, [data-theme="light"] .code-content .c1,
[data-theme="light"] .code-content .cm, [data-theme="light"] .code-content .cc,
[data-theme="light"] .code-content .cs { color: #6e7781; font-style: italic; }
[data-theme="light"] .code-content .nf, [data-theme="light"] .code-content .fm { color: #8250df; }
[data-theme="light"] .code-content .nc, [data-theme="light"] .code-content .nd,
[data-theme="light"] .code-content .nn { color: #953800; }
[data-theme="light"] .code-content .o, [data-theme="light"] .code-content .ow { color: #cf222e; }
[data-theme="light"] .code-content .bp { color: #953800; }
[data-theme="light"] .code-content .ne { color: #953800; }
[data-theme="light"] .code-content .nv { color: #cf222e; }
[data-theme="light"] .code-content .nt { color: #116329; }
[data-theme="light"] .code-content .no { color: #0550ae; }
/* ---------- layout ---------- */
#header {
  position: fixed; top: 0; left: 0; right: 0; height: var(--header-h);
  display: flex; align-items: center; gap: 12px; padding: 0 16px;
  background: var(--bg); border-bottom: 1px solid var(--border);
  z-index: 40;
}
#brand { font-weight: 700; font-size: 1.02em; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
#header .spacer { flex: 1; }
#hamburger { display: none; }
#sidebar {
  position: fixed; top: var(--header-h); left: 0; bottom: 0; width: var(--sidebar-w);
  background: var(--bg-sidebar); border-right: 1px solid var(--border);
  overflow-y: auto; z-index: 30; padding: 12px 0 24px;
}
#search {
  display: block; width: calc(100% - 24px); margin: 4px 12px 10px;
  padding: 7px 10px; font-size: .88em; color: var(--text);
  background: var(--bg); border: 1px solid var(--border); border-radius: 6px;
}
#search:focus { outline: 2px solid var(--accent); outline-offset: -1px; }
#toc { padding: 0 8px; font-size: .88em; }
#toc ul { list-style: none; margin: 0; padding: 0; }
#toc ul ul { padding-left: 14px; }
#toc a {
  display: block; color: var(--text-muted); padding: 3px 8px; border-radius: 6px;
  border-left: 2px solid transparent; text-decoration: none;
}
#toc a:hover { color: var(--text); background: var(--bg-hover); }
#toc a.active { color: var(--accent); background: var(--accent-soft); border-left-color: var(--accent); font-weight: 600; }
#toc .toc-l1 { margin-top: 2px; }
#toc .toc-l1 > a { font-weight: 600; color: var(--text); }
#toc li.hide { display: none; }
#toc-empty { display: none; color: var(--text-muted); padding: 8px 12px; font-size: .88em; }
#sidebar-foot {
  margin: 16px 12px 0; padding-top: 12px; border-top: 1px solid var(--border);
  color: var(--text-muted); font-size: .78em;
}
#main {
  margin-left: var(--sidebar-w); padding: calc(var(--header-h) + 24px) 24px 80px;
}
article { max-width: 860px; margin: 0 auto; }
/* anchor link on headings */
.anchor-link {
  opacity: 0; margin-left: 6px; color: var(--text-muted); text-decoration: none;
  transition: opacity .15s;
}
h1:hover .anchor-link, h2:hover .anchor-link, h3:hover .anchor-link,
h4:hover .anchor-link, h5:hover .anchor-link, h6:hover .anchor-link { opacity: 1; }
.anchor-link:hover { color: var(--accent); text-decoration: none; }
/* code block header + copy */
.code-head {
  display: flex; align-items: center; justify-content: space-between;
  background: var(--bg-code-header); border: 1px solid var(--border);
  border-bottom: none; border-radius: var(--radius) var(--radius) 0 0;
  padding: 4px 10px; font-size: .78em; color: var(--text-muted);
  font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
}
.code-head + pre { border-top-left-radius: 0; border-top-right-radius: 0; margin-top: 0; }
.copy-btn {
  background: transparent; border: 1px solid var(--border); color: var(--text-muted);
  font-size: .82em; padding: 2px 10px; border-radius: 5px; cursor: pointer;
  font-family: inherit;
}
.copy-btn:hover { color: var(--text); border-color: var(--text-muted); }
.copy-btn.ok { color: #1a7f37; border-color: #1a7f37; }
/* buttons */
.btn {
  display: inline-flex; align-items: center; justify-content: center;
  gap: 6px; border: 1px solid var(--border); background: var(--bg);
  color: var(--text); border-radius: 6px; cursor: pointer; height: 30px;
  padding: 0 12px; font-size: .86em; font-family: inherit;
}
.btn:hover { background: var(--bg-hover); }
#backtop {
  position: fixed; right: 20px; bottom: 20px; z-index: 50; opacity: 0;
  pointer-events: none; transition: opacity .2s; box-shadow: var(--shadow);
}
#backtop.show { opacity: 1; pointer-events: auto; }
@media (max-width: 900px) {
  #hamburger { display: inline-flex; }
  #sidebar {
    transform: translateX(-100%); transition: transform .2s ease;
    box-shadow: 2px 0 12px rgba(0,0,0,.18);
  }
  #sidebar.open { transform: translateX(0); }
  #main { margin-left: 0; padding-left: 14px; padding-right: 14px; }
  #scrim { display: none; position: fixed; inset: var(--header-h) 0 0 0;
    background: rgba(0,0,0,.35); z-index: 25; }
  #scrim.show { display: block; }
}
@media print {
  #sidebar, #header, #backtop, .copy-btn { display: none !important; }
  #main { margin: 0; padding: 0; }
  pre, code, blockquote, table { break-inside: avoid; }
}
</style>
</head>
<body>
<div id="header">
  <button id="hamburger" class="btn" aria-label="目录" aria-expanded="false">☰</button>
  <span id="brand">{{TITLE}}</span>
  <span class="spacer"></span>
  <button id="themeBtn" class="btn" aria-label="切换主题">🌙</button>
</div>
<div id="scrim"></div>
<aside id="sidebar">
  <input id="search" type="search" placeholder="搜索目录…" autocomplete="off">
  <nav id="toc">
{{TOC}}
  </nav>
  <div id="toc-empty">未找到匹配条目</div>
  <div id="sidebar-foot">由 <code>{{SOURCE_NAME}}</code> 自动生成<br>generated by <code>tools/md2html.py</code> · {{GENERATED_AT}}</div>
</aside>
<main id="main">
<article>
{{CONTENT}}
</article>
</main>
<button id="backtop" class="btn">↑ 顶部</button>
<script>
(function () {
  "use strict";

  /* ---------- theme ---------- */
  var themeBtn = document.getElementById("themeBtn");
  var root = document.documentElement;
  function applyTheme(t) {
    root.setAttribute("data-theme", t === "dark" ? "dark" : "light");
    themeBtn.textContent = t === "dark" ? "☀️" : "🌙";
  }
  var stored = null;
  try { stored = localStorage.getItem("docs-theme"); } catch (e) {}
  var initial = stored || (window.matchMedia && window.matchMedia("(prefers-color-scheme: dark)").matches ? "dark" : "light");
  applyTheme(initial);
  themeBtn.addEventListener("click", function () {
    var next = root.getAttribute("data-theme") === "dark" ? "light" : "dark";
    applyTheme(next);
    try { localStorage.setItem("docs-theme", next); } catch (e) {}
  });

  /* ---------- mobile sidebar ---------- */
  var sidebar = document.getElementById("sidebar");
  var scrim = document.getElementById("scrim");
  var hamburger = document.getElementById("hamburger");
  function toggleSidebar(open) {
    sidebar.classList.toggle("open", open);
    scrim.classList.toggle("show", open);
    hamburger.setAttribute("aria-expanded", String(open));
  }
  hamburger.addEventListener("click", function () {
    toggleSidebar(!sidebar.classList.contains("open"));
  });
  scrim.addEventListener("click", function () { toggleSidebar(false); });

  /* ---------- copy buttons ---------- */
  var heads = document.querySelectorAll("article h2, article h3, article h4, article h5");
  function splitCodeBlocks() {
    document.querySelectorAll("article pre").forEach(function (pre) {
      if (pre.dataset.wrapped) return;
      pre.dataset.wrapped = "1";
      var head = document.createElement("div");
      head.className = "code-head";
      var lang = "";
      var code = pre.querySelector("code");
      if (code) {
        var m = (code.className || "").match(/language-([\w-]+)/);
        if (m) lang = m[1];
      }
      var label = document.createElement("span");
      label.textContent = lang || "code";
      var btn = document.createElement("button");
      btn.type = "button"; btn.className = "copy-btn"; btn.textContent = "复制";
      btn.addEventListener("click", function () {
        var text = pre.innerText.replace(/^[\s\S]*?$/, pre.innerText); // whole block
        copyText(text, btn);
      });
      head.appendChild(label); head.appendChild(btn);
      pre.parentNode.insertBefore(head, pre);
    });
  }
  function copyText(text, btn) {
    function done() {
      btn.textContent = "已复制";
      btn.classList.add("ok");
      setTimeout(function () { btn.textContent = "复制"; btn.classList.remove("ok"); }, 1600);
    }
    if (navigator.clipboard && navigator.clipboard.writeText) {
      navigator.clipboard.writeText(text).then(done).catch(function () { fallbackCopy(text, done); });
    } else {
      fallbackCopy(text, done);
    }
  }
  function fallbackCopy(text, done) {
    var ta = document.createElement("textarea");
    ta.value = text; ta.style.position = "fixed"; ta.style.opacity = "0";
    document.body.appendChild(ta); ta.select();
    try { document.execCommand("copy"); } catch (e) {}
    document.body.removeChild(ta);
    done();
  }

  /* ---------- scrollspy ---------- */
  var tocLinks = Array.prototype.slice.call(document.querySelectorAll("#toc a"));
  var headings = Array.prototype.slice.call(document.querySelectorAll("article h2, article h3, article h4, article h5, article h6"))
    .filter(function (h) { return h.id; });
  var offset = 90;
  function onScroll() {
    var pos = window.scrollY;
    var current = null;
    for (var i = 0; i < headings.length; i++) {
      if (headings[i].getBoundingClientRect().top + window.scrollY <= pos + offset) current = headings[i];
    }
    var id = current ? current.id : null;
    tocLinks.forEach(function (a) {
      a.classList.toggle("active", a.getAttribute("href") === "#" + id);
    });
    document.getElementById("backtop").classList.toggle("show", pos > 600);
  }
  window.addEventListener("scroll", onScroll, { passive: true });
  onScroll();

  /* ---------- search / filter ---------- */
  var search = document.getElementById("search");
  var empty = document.getElementById("toc-empty");
  search.addEventListener("input", function () {
    var q = search.value.trim().toLowerCase();
    var visible = 0;
    tocLinks.forEach(function (a) {
      var li = a.parentNode;
      var title = (li.getAttribute("data-title") || a.textContent).toLowerCase();
      var hit = !q || title.indexOf(q) !== -1;
      li.classList.toggle("hide", !hit);
      if (hit) visible++;
    });
    empty.style.display = visible ? "none" : "block";
  });

  /* ---------- back to top ---------- */
  document.getElementById("backtop").addEventListener("click", function () {
    window.scrollTo({ top: 0, behavior: "smooth" });
  });

  splitCodeBlocks();
})();
</script>
</body>
</html>
"""

html = (
    HTML_TEMPLATE.replace("{{DOC_LANG}}", DOC_LANG)
    .replace("{{TITLE}}", title)
    .replace("{{TOC}}", toc_html)
    .replace("{{CONTENT}}", body)
    .replace("{{GENERATED_AT}}", generated_at)
    .replace("{{SOURCE_NAME}}", SRC.name)
)

OUT.parent.mkdir(parents=True, exist_ok=True)
OUT.write_text(html, encoding="utf-8")
print(f"OK: {SRC} -> {OUT}  ({len(html)} bytes, {len(toc)} headings)")
