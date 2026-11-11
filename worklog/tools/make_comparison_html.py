#!/usr/bin/env python3
"""Build docs/board-comparison.html from docs/board-comparison.md.

The Markdown file is the source of truth for every number; this script converts the
subset of Markdown it uses (headings, paragraphs, lists, tables, bold, code, links) and
adds four charts after the tables they illustrate. The chart data below is copied from
those tables - change both together.

usage: worklog/tools/make_comparison_html.py
"""

import html
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "docs" / "board-comparison.md"
OUT = ROOT / "docs" / "board-comparison.html"

# ---- chart data (from the tables in board-comparison.md) ----------------------------

MISS_CHART = {
    "id": "chart-miss",
    "title": "Cost of one flash cache miss",
    "subtitle": "Time to fetch one 8-byte line from flash, by read mode and flash clock. Lower is better.",
    "unit": "ns",
    "series": [{"name": "Board's boot default", "slot": 1}, {"name": "Tested setting (PicoBench 431-433)", "slot": 2}],
    "rows": [
        {"label": "RP2350A boot: dual + command, 50 MHz", "value": 1153, "series": 0, "note": "173 cycles @ 150 MHz"},
        {"label": "RP2350 dual continuous, 50 MHz", "value": 993, "series": 1, "note": "149 cycles"},
        {"label": "RP2350 quad + command, 37.5 MHz", "value": 993, "series": 1, "note": "149 cycles"},
        {"label": "RP2350B boot: quad + command, 50 MHz", "value": 753, "series": 0, "note": "113 cycles @ 150 MHz"},
        {"label": "RP2350 dual continuous, 75 MHz", "value": 680, "series": 1, "note": "102 cycles"},
        {"label": "RP2350 quad continuous, 50 MHz", "value": 593, "series": 1, "note": "89 cycles"},
        {"label": "RP2350 quad + command, 75 MHz", "value": 520, "series": 1, "note": "78 cycles"},
        {"label": "RP2040 boot: quad continuous, 62.5 MHz", "value": 424, "series": 0, "note": "53 cycles @ 125 MHz"},
        {"label": "RP2350 quad continuous, 75 MHz", "value": 413, "series": 1, "note": "62 cycles"},
        {"label": "RP2350 quad continuous, 96 MHz (clk 96)", "value": 354, "series": 1, "note": "34 cycles @ 96 MHz"},
        {"label": "RP2350 quad continuous, 133 MHz (clk 133)", "value": 256, "series": 1, "note": "34 cycles @ 133 MHz"},
    ],
}

HOTPATH_CHART = {
    "id": "chart-hotpath",
    "title": "Cartridge hot path, worst case",
    "subtitle": "Maximum time for one emulated bus cycle at each board's default clock (Cortex-M33 on the RP2350). Lower is better.",
    "unit": "ns",
    "series": [{"name": "RP2040 @ 125 MHz", "slot": 1}, {"name": "RP2350A @ 150 MHz", "slot": 2},
               {"name": "RP2350B @ 150 MHz", "slot": 3}],
    "groups": [
        {"label": "Table in SRAM", "values": [320, 153, 153], "notes": ["40 cycles", "23 cycles", "23 cycles"]},
        {"label": "Table in flash, cold cache", "values": [720, 1333, 933], "notes": ["90 cycles", "200 cycles", "140 cycles"]},
        {"label": "Table in flash, uncached", "values": [704, 773, 660], "notes": ["88 cycles", "116 cycles", "99 cycles"]},
        {"label": "Table in PSRAM, uncached", "values": [None, None, 400], "notes": ["no PSRAM", "no PSRAM", "60 cycles"]},
    ],
}

SCALING_CHART = {
    "id": "chart-scaling",
    "title": "Hot path with the table in flash, cold cache",
    "subtitle": "Typical (p50) time per bus cycle against the system clock. The cycle count is constant, so the time scales as 1/f.",
    "unit": "ns",
    "x_label": "clk_sys (MHz)",
    "x_domain": [40, 210], "x_ticks": [48, 96, 120, 150, 200], "x_div": 1, "x_suffix": " MHz",
    "series": [
        {"name": "RP2040 (84 cycles)", "slot": 1,
         "points": [[48, 1750], [96, 875], [120, 700], [125, 672], [133, 632], [150, 560], [200, 420]]},
        {"name": "RP2350A (200 cycles)", "slot": 2, "points": [[48, 4167], [96, 2083], [120, 1667], [133, 1504], [150, 1333]]},
        {"name": "RP2350B (140 cycles)", "slot": 3, "points": [[48, 2917], [96, 1458], [120, 1167], [133, 1053], [150, 933]]},
    ],
}

OC_CHART = {
    "id": "chart-overclock",
    "title": "Highest clock that passed, by core voltage",
    "subtitle": "Outside specification. Highest 10 MHz step that passed 250 ms of checks on both cores "
                "(the lowest of the runs that reached it); Cortex-M33 on the RP2350. Higher is better.",
    "unit": "MHz",
    "x_label": "core voltage",
    "x_domain": [1080, 1320], "x_ticks": [1100, 1150, 1200, 1250, 1300], "x_div": 1000, "x_dec": 2, "x_suffix": " V",
    "series": [
        {"name": "RP2040", "slot": 1, "points": [[1100, 309], [1150, 351], [1200, 380], [1250, 412], [1300, 432]]},
        {"name": "RP2350A (to 1.25 V; hangs at 1.30 V)", "slot": 2,
         "points": [[1100, 320], [1150, 320], [1200, 320], [1250, 320]]},
        {"name": "RP2350B", "slot": 3, "points": [[1100, 309], [1150, 330], [1200, 360], [1250, 390], [1300, 412]]},
    ],
}

CHARTS_AFTER = {   # heading text -> chart placed after the first table in that section
    "Cost of one flash cache miss": MISS_CHART,
    "Cartridge hot path": HOTPATH_CHART,
    "Scaling with the system clock": SCALING_CHART,
    "Overclocking (outside specification)": OC_CHART,
}

# ---- minimal Markdown -----------------------------------------------------------------


def inline(text: str) -> str:
    text = text.replace("\\*", "\x00")
    out = html.escape(text, quote=False)
    out = re.sub(r"`([^`]+)`", r"<code>\1</code>", out)
    out = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", out)
    out = re.sub(r"(?<![\w*])\*([^*\s][^*]*)\*(?![\w*])", r"<em>\1</em>", out)

    def link(m):
        label, url = m.group(1), m.group(2)
        return f'<a href="{html.escape(url)}">{label}</a>'
    out = re.sub(r"\[([^\]]+)\]\(([^)]+)\)", link, out)
    return out.replace("\x00", "*")


def slug(text: str) -> str:
    return re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")


def table(lines):
    rows = [[c.strip() for c in ln.strip().strip("|").split("|")] for ln in lines]
    head, body = rows[0], rows[2:]
    h = "".join(f"<th>{inline(c)}</th>" for c in head)
    b = "".join("<tr>" + "".join(f"<td>{inline(c)}</td>" for c in r) + "</tr>" for r in body)
    return f'<div class="table-wrap"><table><thead><tr>{h}</tr></thead><tbody>{b}</tbody></table></div>'


def convert(md: str):
    lines = md.split("\n")
    out, charts, i = [], [], 0
    section, section_has_table = None, False
    while i < len(lines):
        ln = lines[i]
        if not ln.strip():
            i += 1
            continue
        m = re.match(r"^(#{1,3}) (.*)$", ln)
        if m:
            level, text = len(m.group(1)), m.group(2)
            if level == 1:
                out.append(f"<h1>{inline(text)}</h1>")
            else:
                out.append(f'<h{level} id="{slug(text)}">{inline(text)}</h{level}>')
            section, section_has_table = text, False
            i += 1
            continue
        if ln.startswith("|"):
            block = []
            while i < len(lines) and lines[i].startswith("|"):
                block.append(lines[i])
                i += 1
            out.append(table(block))
            if section in CHARTS_AFTER and not section_has_table:
                c = CHARTS_AFTER[section]
                charts.append(c)
                out.append(chart_block(c))
            section_has_table = True
            continue
        if re.match(r"^\d+\. ", ln) or ln.startswith("- "):
            ordered = bool(re.match(r"^\d+\. ", ln))
            items = []
            while i < len(lines) and lines[i].strip():
                cur = lines[i]
                if re.match(r"^\d+\. ", cur) or cur.startswith("- "):
                    items.append(re.sub(r"^(\d+\. |- )", "", cur))
                else:
                    items[-1] += " " + cur.strip()
                i += 1
            tag = "ol" if ordered else "ul"
            out.append(f"<{tag}>" + "".join(f"<li>{inline(t)}</li>" for t in items) + f"</{tag}>")
            continue
        para = []
        while i < len(lines) and lines[i].strip() and not lines[i].startswith(("|", "#", "- ")) \
                and not re.match(r"^\d+\. ", lines[i]):
            para.append(lines[i].strip())
            i += 1
        text = " ".join(para)
        if text.startswith("An HTML version with charts is in"):
            text = "The same comparison as Markdown: [board-comparison.md](board-comparison.md)."
        out.append(f"<p>{inline(text)}</p>")
    return "\n".join(out), charts


def chart_block(c):
    legend = "".join(f'<li><span class="swatch s{s["slot"]}"></span>{html.escape(s["name"])}</li>'
                     for s in c["series"])
    return (f'<figure class="chart" id="{c["id"]}">'
            f'<figcaption><span class="chart-title">{html.escape(c["title"])}</span>'
            f'<span class="chart-sub">{html.escape(c["subtitle"])}</span></figcaption>'
            f'<ul class="legend">{legend}</ul><div class="plot" role="img" aria-label="{html.escape(c["title"])}; '
            f'the values are in the table above"></div></figure>')


# ---- page -----------------------------------------------------------------------------

PAGE = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Board Comparison</title>
<style>
:root {
  color-scheme: light;
  --surface: #fcfcfb; --surface-2: #f3f2ef; --grid: #e4e3df; --border: #dddcd7;
  --text: #0b0b0b; --text-2: #52514e; --text-3: #75746f; --accent: #1c5cab;
  --s1: #2a78d6; --s2: #eb6834; --s3: #1baf7a;
}
@media (prefers-color-scheme: dark) {
  :root:not([data-theme="light"]) {
    color-scheme: dark;
    --surface: #1a1a19; --surface-2: #232321; --grid: #2f2f2c; --border: #3a3a37;
    --text: #f0efec; --text-2: #c3c2b7; --text-3: #9a998f; --accent: #86b6ef;
    --s1: #3987e5; --s2: #d95926; --s3: #199e70;
  }
}
:root[data-theme="dark"] {
  color-scheme: dark;
  --surface: #1a1a19; --surface-2: #232321; --grid: #2f2f2c; --border: #3a3a37;
  --text: #f0efec; --text-2: #c3c2b7; --text-3: #9a998f; --accent: #86b6ef;
  --s1: #3987e5; --s2: #d95926; --s3: #199e70;
}
* { box-sizing: border-box; }
html { -webkit-text-size-adjust: 100%; }
body {
  margin: 0; background: var(--surface); color: var(--text);
  font: 16px/1.55 system-ui, -apple-system, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif;
}
main { max-width: 1040px; margin: 0 auto; padding: 32px 16px 64px; }
h1 { font-size: 2rem; line-height: 1.2; margin: 0 0 12px; }
h2 { font-size: 1.4rem; margin: 48px 0 12px; padding-top: 8px; border-top: 1px solid var(--border); }
h3 { font-size: 1.1rem; margin: 32px 0 8px; }
p, li { color: var(--text); max-width: 78ch; }
p { margin: 0 0 14px; }
a { color: var(--accent); }
code { font: 0.9em ui-monospace, SFMono-Regular, Menlo, Consolas, monospace; background: var(--surface-2);
       padding: 1px 4px; border-radius: 4px; }
strong { font-weight: 650; }
ol, ul { padding-left: 1.4em; margin: 0 0 16px; }
li { margin: 4px 0; }
.table-wrap { overflow-x: auto; margin: 0 0 18px; border: 1px solid var(--border); border-radius: 8px; }
table { border-collapse: collapse; width: 100%; font-size: 0.875rem; font-variant-numeric: tabular-nums; }
th, td { text-align: left; vertical-align: top; padding: 7px 10px; border-bottom: 1px solid var(--grid); }
thead th { background: var(--surface-2); color: var(--text-2); font-weight: 600; }
tbody tr:last-child td { border-bottom: 0; }
td:first-child { color: var(--text); font-weight: 500; }
.chart { margin: 8px 0 28px; padding: 16px; border: 1px solid var(--border); border-radius: 8px; }
figcaption { display: flex; flex-direction: column; gap: 2px; margin-bottom: 8px; }
.chart-title { font-weight: 650; }
.chart-sub { color: var(--text-2); font-size: 0.875rem; }
.legend { list-style: none; display: flex; flex-wrap: wrap; gap: 6px 18px; padding: 0; margin: 0 0 8px;
          font-size: 0.85rem; color: var(--text-2); }
.legend li { display: flex; align-items: center; gap: 6px; margin: 0; }
.swatch { width: 12px; height: 12px; border-radius: 3px; display: inline-block; }
.s1 { background: var(--s1); } .s2 { background: var(--s2); } .s3 { background: var(--s3); }
.plot { position: relative; width: 100%; }
.plot svg { display: block; width: 100%; overflow: visible; }
.plot text { font-family: inherit; }
.mark { cursor: default; outline: none; }
.mark:focus-visible .hit { stroke: var(--text); stroke-width: 1.5; }
.tip { position: absolute; pointer-events: none; background: var(--surface); color: var(--text);
       border: 1px solid var(--border); border-radius: 6px; padding: 6px 9px; font-size: 0.8rem;
       box-shadow: 0 2px 10px rgba(0,0,0,.12); white-space: nowrap; display: none; z-index: 2; }
.tip b { font-weight: 650; }
.foot { color: var(--text-3); font-size: 0.8rem; margin-top: 40px; }
</style>
</head>
<body>
<main>
__CONTENT__
<p class="foot">Generated from board-comparison.md by worklog/tools/make_comparison_html.py.</p>
</main>
<script>
const CHARTS = __CHARTS__;
const css = n => getComputedStyle(document.documentElement).getPropertyValue(n).trim();
const NS = 'http://www.w3.org/2000/svg';
function el(tag, attrs, parent) {
  const e = document.createElementNS(NS, tag);
  for (const k in attrs) e.setAttribute(k, attrs[k]);
  if (parent) parent.appendChild(e);
  return e;
}
function niceMax(v) {
  const p = Math.pow(10, Math.floor(Math.log10(v)));
  for (const m of [1, 2, 2.5, 5, 10]) if (m * p >= v) return m * p;
  return 10 * p;
}
let UNIT = 'ns';   // set per chart by renderAll()
function fmt(v) {
  if (UNIT === 'MHz') return v + ' MHz';
  return v >= 1000 ? (v / 1000).toFixed(v % 1000 ? 2 : 0).replace(/\\.?0+$/, '') + ' µs' : v + ' ns';
}
function tipFor(plot) {
  let t = plot.querySelector('.tip');
  if (!t) { t = document.createElement('div'); t.className = 'tip'; plot.appendChild(t); }
  return t;
}
function attachTip(plot, node, html, cx, cy) {
  const tip = tipFor(plot);
  const show = () => {
    tip.innerHTML = html; tip.style.display = 'block';
    const w = plot.clientWidth, tw = tip.offsetWidth;
    tip.style.left = Math.max(0, Math.min(cx - tw / 2, w - tw)) + 'px';
    tip.style.top = Math.max(0, cy - tip.offsetHeight - 10) + 'px';
  };
  const hide = () => { tip.style.display = 'none'; };
  node.addEventListener('pointerenter', show); node.addEventListener('pointerleave', hide);
  node.addEventListener('focus', show); node.addEventListener('blur', hide);
}
function gridX(svg, x0, x1, top, bottom, max, step) {
  for (let v = 0; v <= max + 1e-9; v += step) {
    const x = x0 + (x1 - x0) * v / max;
    el('line', {x1: x, x2: x, y1: top, y2: bottom, stroke: css('--grid'), 'stroke-width': 1}, svg);
    const t = el('text', {x: x, y: bottom + 16, 'text-anchor': 'middle', 'font-size': 11, fill: css('--text-3')}, svg);
    t.textContent = fmt(v);
  }
}
function hbars(c, plot) {
  const W = plot.clientWidth, narrow = W < 560;
  const labelW = narrow ? 0 : Math.min(300, W * 0.42), rowH = narrow ? 44 : 28, bar = 14;
  const top = 4, H = top + c.rows.length * rowH + 24;
  const svg = el('svg', {viewBox: `0 0 ${W} ${H}`, height: H});
  const x0 = labelW + 4, x1 = W - 70;
  const max = niceMax(Math.max(...c.rows.map(r => r.value)));
  const step = max / 4;
  gridX(svg, x0, x1, top, top + c.rows.length * rowH, max, step);
  c.rows.forEach((r, i) => {
    const yRow = top + i * rowH, y = yRow + (narrow ? 22 : (rowH - bar) / 2);
    const lab = el('text', {x: narrow ? x0 : labelW - 8, y: narrow ? yRow + 14 : y + bar / 2 + 4,
      'text-anchor': narrow ? 'start' : 'end', 'font-size': 12, fill: css('--text')}, svg);
    lab.textContent = r.label;
    const w = Math.max(2, (x1 - x0) * r.value / max);
    const g = el('g', {class: 'mark', tabindex: 0, 'aria-label': `${r.label}: ${fmt(r.value)}, ${r.note}`}, svg);
    el('rect', {class: 'hit', x: x0, y: y - 6, width: Math.max(w, 24), height: bar + 12, fill: 'transparent'}, g);
    const slot = c.series[r.series].slot;
    el('path', {d: `M${x0},${y} h${w - 4} a4,4 0 0 1 4,4 v${bar - 8} a4,4 0 0 1 -4,4 h${-(w - 4)} z`,
      fill: css('--s' + slot)}, g);
    const v = el('text', {x: x0 + w + 6, y: y + bar / 2 + 4, 'font-size': 12, fill: css('--text-2')}, svg);
    v.textContent = fmt(r.value);
    attachTip(plot, g, `<b>${r.label}</b><br>${fmt(r.value)} · ${r.note}<br>${c.series[r.series].name}`, x0 + w / 2, y);
  });
  plot.appendChild(svg);
}
function groupedBars(c, plot) {
  const W = plot.clientWidth, narrow = W < 560;
  const n = c.series.length, bar = 12, gap = 2, groupH = n * (bar + gap) + (narrow ? 34 : 18);
  const labelW = narrow ? 0 : Math.min(230, W * 0.32), top = 4;
  const H = top + c.groups.length * groupH + 24;
  const svg = el('svg', {viewBox: `0 0 ${W} ${H}`, height: H});
  const x0 = labelW + 4, x1 = W - 70;
  const all = c.groups.flatMap(g => g.values.filter(v => v != null));
  const max = niceMax(Math.max(...all));
  gridX(svg, x0, x1, top, top + c.groups.length * groupH, max, max / 4);
  c.groups.forEach((gr, gi) => {
    const gy = top + gi * groupH + (narrow ? 20 : 6);
    const lab = el('text', {x: narrow ? x0 : labelW - 8, y: narrow ? gy - 6 : gy + (n * (bar + gap)) / 2 + 4,
      'text-anchor': narrow ? 'start' : 'end', 'font-size': 12, fill: css('--text')}, svg);
    lab.textContent = gr.label;
    gr.values.forEach((v, si) => {
      const y = gy + si * (bar + gap);
      const s = c.series[si];
      if (v == null) {
        const t = el('text', {x: x0 + 4, y: y + bar - 2, 'font-size': 11, fill: css('--text-3')}, svg);
        t.textContent = s.name.split(' ')[0] + ': ' + gr.notes[si];
        return;
      }
      const w = Math.max(2, (x1 - x0) * v / max);
      const g = el('g', {class: 'mark', tabindex: 0, 'aria-label': `${gr.label}, ${s.name}: ${fmt(v)}, ${gr.notes[si]}`}, svg);
      el('rect', {class: 'hit', x: x0, y: y - 1, width: Math.max(w, 24), height: bar + 2, fill: 'transparent'}, g);
      el('path', {d: `M${x0},${y} h${w - 4} a4,4 0 0 1 4,4 v${bar - 8} a4,4 0 0 1 -4,4 h${-(w - 4)} z`,
        fill: css('--s' + s.slot)}, g);
      const tv = el('text', {x: x0 + w + 6, y: y + bar - 2, 'font-size': 11, fill: css('--text-2')}, svg);
      tv.textContent = fmt(v);
      attachTip(plot, g, `<b>${gr.label}</b><br>${s.name}: ${fmt(v)} · ${gr.notes[si]}`, x0 + w / 2, y);
    });
  });
  plot.appendChild(svg);
}
function lines(c, plot) {
  const W = plot.clientWidth, narrow = W < 560;
  const H = narrow ? 260 : 320, left = 56, right = narrow ? 16 : 150, top = 12, bottom = H - 40;
  const svg = el('svg', {viewBox: `0 0 ${W} ${H}`, height: H});
  const xs = c.x_domain, ymax = niceMax(Math.max(...c.series.flatMap(s => s.points.map(p => p[1]))));
  const xl_ = v => (v / c.x_div).toFixed(c.x_dec || 0) + c.x_suffix;
  const X = v => left + (W - left - right) * (v - xs[0]) / (xs[1] - xs[0]);
  const Y = v => bottom - (bottom - top) * v / ymax;
  for (let v = 0; v <= ymax + 1e-9; v += ymax / 4) {
    el('line', {x1: left, x2: W - right, y1: Y(v), y2: Y(v), stroke: css('--grid'), 'stroke-width': 1}, svg);
    const t = el('text', {x: left - 8, y: Y(v) + 4, 'text-anchor': 'end', 'font-size': 11, fill: css('--text-3')}, svg);
    t.textContent = fmt(v);
  }
  c.x_ticks.forEach(v => {
    const t = el('text', {x: X(v), y: bottom + 18, 'text-anchor': 'middle', 'font-size': 11, fill: css('--text-3')}, svg);
    t.textContent = c.x_div === 1 ? v : xl_(v);
  });
  const xl = el('text', {x: (left + W - right) / 2, y: H - 4, 'text-anchor': 'middle', 'font-size': 11, fill: css('--text-3')}, svg);
  xl.textContent = c.x_label;
  c.series.forEach(s => {
    const col = css('--s' + s.slot);
    el('path', {d: s.points.map((p, i) => (i ? 'L' : 'M') + X(p[0]) + ',' + Y(p[1])).join(''), fill: 'none',
      stroke: col, 'stroke-width': 2, 'stroke-linejoin': 'round', 'stroke-linecap': 'round'}, svg);
    s.points.forEach(p => {
      const g = el('g', {class: 'mark', tabindex: 0, 'aria-label': `${s.name} at ${xl_(p[0])}: ${fmt(p[1])}`}, svg);
      el('circle', {class: 'hit', cx: X(p[0]), cy: Y(p[1]), r: 12, fill: 'transparent'}, g);
      el('circle', {cx: X(p[0]), cy: Y(p[1]), r: 4, fill: col, stroke: css('--surface'), 'stroke-width': 2}, g);
      attachTip(plot, g, `<b>${s.name}</b><br>${xl_(p[0])}: ${fmt(p[1])}`, X(p[0]), Y(p[1]));
    });
    if (!narrow) {
      const last = s.points[s.points.length - 1];
      const t = el('text', {x: X(last[0]) + 10, y: Y(last[1]) + 4, 'font-size': 12, fill: css('--text-2')}, svg);
      t.textContent = s.name.split(' ')[0] + ' ' + fmt(last[1]);
    }
  });
  plot.appendChild(svg);
}
function renderAll() {
  for (const c of CHARTS) {
    const plot = document.querySelector('#' + c.id + ' .plot');
    if (!plot) continue;
    plot.innerHTML = '';
    UNIT = c.unit || 'ns';
    if (c.rows) hbars(c, plot); else if (c.groups) groupedBars(c, plot); else lines(c, plot);
  }
}
let rt;
window.addEventListener('resize', () => { clearTimeout(rt); rt = setTimeout(renderAll, 120); });
matchMedia('(prefers-color-scheme: dark)').addEventListener('change', renderAll);
renderAll();
</script>
</body>
</html>
"""


def main():
    content, charts = convert(SRC.read_text())
    OUT.write_text(PAGE.replace("__CONTENT__", content).replace("__CHARTS__", json.dumps(charts)))
    print(f"wrote {OUT.relative_to(ROOT)} ({len(charts)} charts)")


if __name__ == "__main__":
    main()
