#!/usr/bin/env python3
"""SONORA v0.2 UI generator.

Single source of truth for the three editor layouts. Writes
  plugin/resource/{sonora,shutter,drowner}.uidesc        (VSTGUI 4, consumed by VST3Editor)
  plugin/resource/{ui,shutter,drowner}-preview.html      (static 1:1 mirrors shown on the website)

The HTML renderer re-implements the drawing code of plugin/src/ui/sonora_ui.h (same geometry,
colours, fonts and maths, incl. the real RBJ biquad EQ response and the shutter gate model), so the
previews stay aligned with the plugin. Edit the layout here, then run:

    python3 plugin/src/ui/tools/gen_ui.py
"""
from __future__ import annotations

import cmath
import html
import math
import os
from dataclasses import dataclass, field
from xml.sax.saxutils import quoteattr

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..", ".."))
RES = os.path.join(ROOT, "plugin", "resource")
VERSION = "v0.2"

# ------------------------------------------------------------------------------------------------
# Theme (keep in sync with sonora_ui.h namespace theme)
# ------------------------------------------------------------------------------------------------
C = dict(
    bg="#0f0d14", panel="#17141f", card="#1e1a29", cardTop="#221e2f", inset="#120f19",
    stroke="#2c2638", strokeHi="#3c344e", grid="#231e2e", text="#f1eef8", muted="#8f88a3",
    label="#aaa3be", accent="#8b5cf6", accent2="#e879f9", warn="#f59e0b", good="#22c55e",
    mid="#eab308", bad="#ef4444",
)
UIDESC_COLORS = [
    ("sonora.bg", C["bg"]), ("sonora.panel", C["panel"]), ("sonora.card", C["card"]),
    ("sonora.inset", C["inset"]), ("sonora.stroke", C["stroke"]), ("sonora.text", C["text"]),
    ("sonora.muted", C["muted"]), ("sonora.label", C["label"]), ("sonora.accent", C["accent"]),
    ("sonora.accent2", C["accent2"]), ("sonora.warn", C["warn"]), ("sonora.badge", "#8b5cf633"),
    ("sonora.badge.frame", "#8b5cf680"), ("sonora.badge.text", "#c9b6ff"), ("sonora.clear", "#00000000"),
]
# name -> (size, bold)
FONTS = {
    "sonora.small": (11, False), "sonora.small.bold": (11, True), "sonora.label": (11, True),
    "sonora.value": (12, True), "sonora.title": (12, True), "sonora.big": (20, True),
    "sonora.huge": (30, True),
}
FONT_FAMILY = "Segoe UI"
FONT_ALTS = "Arial, Helvetica, DejaVu Sans"
CSS_FONT = "'Segoe UI', 'Inter', Arial, Helvetica, sans-serif"


def hex2rgb(h):
    h = h.lstrip("#")
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


def rgba(h, a=255):
    r, g, b = hex2rgb(h)
    return f"rgba({r},{g},{b},{a / 255:.3f})"


def mix(a, b, t):
    ra, rb = hex2rgb(a), hex2rgb(b)
    return "#%02x%02x%02x" % tuple(round(x + (y - x) * t) for x, y in zip(ra, rb))


def fmt(v, d=1, plus=False):
    if abs(v) < 0.5 * 10 ** (-d):
        v = 0.0
    return (f"{v:+.{d}f}" if plus and v != 0 else f"{v:.{d}f}").replace(".", ",")


def esc(s):
    return html.escape(s, quote=True)


# ------------------------------------------------------------------------------------------------
# Layout model
# ------------------------------------------------------------------------------------------------
@dataclass
class V:
    kind: str
    x: float
    y: float
    w: float
    h: float
    p: dict = field(default_factory=dict)
    kids: list = field(default_factory=list)


def card(name, x, y, w, h, title="", style="card", rule=None, steps=2, accent=None, kids=()):
    return V("card", x, y, w, h, dict(name=name, title=title, style=style, rule=rule, steps=steps,
                                     accent=accent), list(kids))


def label(x, y, w, h, text, font="sonora.label", color="label", align="center"):
    return V("label", x, y, w, h, dict(text=text, font=font, color=color, align=align))


def value(tag, x, y, w, h, font="sonora.value", color="text", align="center"):
    return V("value", x, y, w, h, dict(tag=tag, font=font, color=color, align=align))


def knob(tag, x, y, d, bipolar=False, accent="accent"):
    return V("knob", x, y, d, d, dict(tag=tag, bipolar=bipolar, accent=accent))


def knob_cell(name, tag, x, y, w, text, d=44, bipolar=False, accent="accent", rule=None, steps=2):
    """Vertical cell: label / knob / value. Height = 14 + 2 + d + 2 + 16."""
    h = 14 + 2 + d + 2 + 16
    return card(name, x, y, w, h, style="none", rule=rule, steps=steps, kids=[
        label(0, 0, w, 14, text),
        knob(tag, (w - d) / 2, 16, d, bipolar, accent),
        value(tag, 0, 18 + d, w, 16),
    ])


def knob_row(name, tag, x, y, w, text, d=52, bipolar=False, accent="accent", rule=None, steps=2):
    """Horizontal cell: knob left, label + value right."""
    h = d + 24
    return card(name, x, y, w, h, style="none", rule=rule, steps=steps, kids=[
        knob(tag, 14, 12, d, bipolar, accent),
        label(d + 26, 12 + d / 2 - 18, w - d - 30, 16, text, align="left"),
        value(tag, d + 26, 12 + d / 2, w - d - 30, 18, align="left"),
    ])


def toggle(tag, x, y, w, h, title, big=False, on="accent"):
    return V("toggle", x, y, w, h, dict(tag=tag, title=title, big=big, on=on))


def segment(tag, x, y, w, h, names, accent="accent"):
    return V("segment", x, y, w, h, dict(tag=tag, names=names, accent=accent))


def menu(tag, x, y, w, h, entries):
    return V("menu", x, y, w, h, dict(tag=tag, entries=entries))


def custom(view, x, y, w, h, tag=None, **p):
    return V("custom", x, y, w, h, dict(view=view, tag=tag, **p))


def rule_line(x, y, w, h=1):
    return V("line", x, y, w, h, {})


def badge(x, y, w, h, text):
    return V("badge", x, y, w, h, dict(text=text))


def logo(x, y, plugin):
    return [label(x, y, 74, 28, "sonora", font="sonora.big", color="text", align="left"),
            label(x + 74, y, 110, 28, "/ " + plugin, font="sonora.big", color="muted", align="left")]


# ------------------------------------------------------------------------------------------------
# Parameter tables (IDs are binding, see MASTER_PROMPT.md).
# tag -> (control-tag name, example normalized value, example display text, default normalized)
# ------------------------------------------------------------------------------------------------
PRESETS = ["Natural", "Warm", "Punch", "Air", "Bass", "Vocal Clarity", "Trap 808", "Drill", "Boom Bap",
           "Cloud Rap", "Vocal Upfront", "Club Loud"]
DIVISIONS = ["1/1", "1/2", "1/4", "1/8", "1/16", "1/32", "1/8 T", "1/16 T", "1/32 T", "1/4 P", "1/8 P",
             "1/16 P"]


def lin(v, lo, hi):
    return (v - lo) / (hi - lo)


def logn(v, lo, hi):
    return math.log(v / lo) / math.log(hi / lo)


MASTER_PARAMS = {
    0: ("Master::Mode", 1.0, "Master", 1.0),
    1: ("Master::Strength", 0.65, "65 %", 0.5),
    2: ("Master::Bass", lin(2.0, -6, 6), "+2,0 dB", 0.5),
    3: ("Master::Presence", lin(1.5, -4, 4), "+1,5 dB", 0.5),
    4: ("Master::Output", lin(0.0, -6, 6), "0,0 dB", 0.5),
    5: ("Master::Bypass", 0.0, "Aus", 0.0),
    6: ("Master::Preset", 6 / 11, "Trap 808", 0.0),
    7: ("Master::Nasal", 0.35, "35 %", 0.0),
    8: ("Master::Resonance", 0.40, "40 %", 0.0),
    9: ("Master::BeatHighs", lin(1.0, -6, 6), "+1,0 dB", 0.5),
    10: ("Master::Saturation", 0.30, "30 %", 0.0),
    11: ("Master::Eq60", lin(2.5, -8, 8), "+2,5 dB", 0.5),
    12: ("Master::Eq250", lin(-1.5, -8, 8), "-1,5 dB", 0.5),
    13: ("Master::Eq1k", lin(-0.5, -8, 8), "-0,5 dB", 0.5),
    14: ("Master::Eq4k", lin(2.0, -8, 8), "+2,0 dB", 0.5),
    15: ("Master::Eq12k", lin(3.0, -8, 8), "+3,0 dB", 0.5),
    16: ("Master::VocalFocus", 0.50, "50 %", 0.5),
    17: ("Master::Analyze", 0.0, "Aus", 0.0),
    18: ("Master::Loudness", lin(6.5, 0, 18), "6,5 dB", 0.0),
    19: ("Master::Ceiling", lin(-1.0, -3, 0), "-1,0 dBTP", lin(-1.0, -3, 0)),
    20: ("Master::Glue", 0.45, "45 %", 0.0),
    21: ("Master::Clipper", 0.35, "35 %", 0.0),
    22: ("Master::Punch808", 0.60, "60 %", 0.0),
    23: ("Master::BassMono", lin(120, 0, 250), "120 Hz", 0.0),
    24: ("Master::Width", lin(115, 0, 200), "115 %", 0.5),
    25: ("Master::DeEsser", 0.45, "45 %", 0.0),
    26: ("Master::Input", lin(0.0, -12, 12), "0,0 dB", 0.5),
}
SHUTTER_PARAMS = {
    0: ("Shutter::Enable", 1.0, "An", 0.0),
    1: ("Shutter::Sync", 1.0, "An", 1.0),
    2: ("Shutter::Division", 4 / 11, "1/16", 3 / 11),
    3: ("Shutter::Rate", lin(4.0, 0.5, 20), "4,0 Hz", lin(4.0, 0.5, 20)),
    4: ("Shutter::Depth", 0.85, "85 %", 0.85),
    5: ("Shutter::Mix", 1.0, "100 %", 1.0),
    6: ("Shutter::Smoothing", lin(2.0, 0, 12), "2,0 ms", lin(2.0, 0, 12)),
    7: ("Shutter::PulseWidth", lin(55, 10, 90), "55 %", 0.5),
    8: ("Shutter::Swing", lin(20, 0, 75), "20 %", 0.0),
    9: ("Shutter::Form", 0.0, "Hart", 0.0),
    10: ("Shutter::Stereo", 1.0, "Ping-Pong", 0.0),
    11: ("Shutter::GateSound", 0.0, "Stumm", 0.0),
    12: ("Shutter::Offset", 0.0, "0 %", 0.0),
}
DROWNER_PARAMS = {
    0: ("Drowner::Depth", 0.65, "65 %", 0.0),
    1: ("Drowner::Rise", lin(2.0, 0.2, 6), "2,00 s", lin(2.0, 0.2, 6)),
    2: ("Drowner::Dive", logn(0.05, 0.01, 2), "0,05 s", logn(0.05, 0.01, 2)),
    3: ("Drowner::Resonance", 0.30, "30 %", 0.30),
    4: ("Drowner::Wobble", 0.35, "35 %", 0.35),
    5: ("Drowner::Reverb", 0.30, "30 %", 0.30),
    6: ("Drowner::Body", lin(2.5, 0, 6), "2,5 dB", lin(2.5, 0, 6)),
    7: ("Drowner::Mix", 1.0, "100 %", 1.0),
    8: ("Drowner::Output", lin(0, -12, 6), "0,0 dB", lin(0, -12, 6)),
}

# ------------------------------------------------------------------------------------------------
# Layouts
# ------------------------------------------------------------------------------------------------
VOCAL_ONLY, BEAT_ONLY, MASTER_ONLY = "0=0,2", "0=1,2", "0=2"


def master_layout():
    header = card("header", 0, 0, 960, 56, style="header", kids=[
        *logo(20, 14, "master"),
        badge(196, 19, 40, 18, VERSION),
        label(282, 14, 90, 28, "BUS-MODUS", font="sonora.small.bold", color="muted", align="right"),
        segment(0, 380, 14, 246, 28, ["Vocal", "Beat", "Master"]),
        label(636, 14, 54, 28, "PRESET", font="sonora.small.bold", color="muted", align="right"),
        menu(6, 698, 14, 150, 28, PRESETS),
        toggle(5, 860, 14, 88, 28, "Bypass", on="warn"),
    ])
    klang = card("section-klang", 12, 68, 156, 560, title="Klangformung", kids=[
        label(0, 34, 156, 16, "Stärke"),
        knob(1, 30, 54, 96),
        value(1, 0, 152, 156, 18),
        rule_line(14, 184, 128),
        knob_row("grp-input", 26, 0, 194, 156, "Eingang", bipolar=True),
        knob_row("grp-loudness", 18, 0, 280, 156, "Lautheit", rule=MASTER_ONLY),
        knob_row("grp-ceiling", 19, 0, 366, 156, "Ceiling", rule=MASTER_ONLY),
        knob_row("grp-output", 4, 0, 452, 156, "Ausgang", bipolar=True),
    ])
    eq_kids = [
        card("grp-analyze", 478, 7, 110, 24, style="none", rule=BEAT_ONLY, kids=[
            toggle(17, 0, 0, 110, 24, "Analysieren", on="accent2")]),
        custom("SonoraEqGraph", 12, 36, 354, 200),
    ]
    for i, (lab, tag) in enumerate(zip(["60", "250", "1k", "4k", "12k"], range(11, 16))):
        x = 378 + i * 42
        eq_kids += [label(x, 36, 42, 14, lab, font="sonora.small.bold", color="muted"),
                    custom("SonoraEqFader", x + 6, 52, 30, 160, tag=tag),
                    value(tag, x - 2, 216, 46, 16, font="sonora.small", color="text")]
    eq_kids.append(custom("SonoraAnalysisStatus", 12, 246, 576, 54))
    eq = card("section-eq", 180, 68, 600, 312, title="Equalizer", kids=eq_kids)

    vocal_cells = [("grp-nasal", 7, "Nasalität", False, VOCAL_ONLY),
                   ("grp-resonance", 8, "Resonanzen", False, VOCAL_ONLY),
                   ("grp-presence", 3, "Stimmen-Präsenz", True, VOCAL_ONLY),
                   ("grp-deesser", 25, "De-Esser", False, VOCAL_ONLY),
                   ("grp-vocalfocus", 16, "Vocal-Fokus", False, BEAT_ONLY)]
    vocal = card("section-vocal", 180, 392, 600, 112, title="Vocal", accent="accent2", kids=[
        knob_cell(n, t, 12 + i * 115, 28, 115, txt, bipolar=b, accent="accent2", rule=r)
        for i, (n, t, txt, b, r) in enumerate(vocal_cells)])
    beat_cells = [("grp-bass", 2, "Bass", True, BEAT_ONLY),
                  ("grp-beathighs", 9, "Beat-Höhen", True, BEAT_ONLY),
                  ("grp-punch", 22, "808-Punch", False, BEAT_ONLY),
                  ("grp-glue", 20, "Glue", False, None),
                  ("grp-saturation", 10, "Sättigung", False, None),
                  ("grp-clipper", 21, "Clipper", False, MASTER_ONLY),
                  ("grp-bassmono", 23, "Bass-Mono", False, BEAT_ONLY),
                  ("grp-width", 24, "Stereo-Breite", True, BEAT_ONLY)]
    beat = card("section-beat", 180, 516, 600, 112, title="Beat & Bus", kids=[
        knob_cell(n, t, 12 + i * 72, 28, 72, txt, bipolar=b, rule=r)
        for i, (n, t, txt, b, r) in enumerate(beat_cells)])

    meters = card("section-meters", 792, 68, 156, 226, title="Pegel", kids=[
        custom("SonoraLevelMeters", 10, 30, 136, 186)])
    lufs = card("section-loudness", 792, 306, 156, 160, title="LUFS", accent="accent2", kids=[
        custom("SonoraLoudness", 10, 30, 136, 124)])
    dyn = card("section-dynamics", 792, 478, 156, 150, title="Dynamik", kids=[
        custom("SonoraGainReduction", 10, 32, 136, 76),
        custom("SonoraCorrelation", 10, 114, 136, 30)])
    return V("root", 0, 0, 960, 640, dict(name="view"), [header, klang, eq, vocal, beat, meters, lufs, dyn])


def shutter_layout():
    header = card("header", 0, 0, 640, 52, style="header", kids=[
        *logo(20, 12, "shutter"),
        badge(204, 17, 40, 18, VERSION),
        toggle(0, 478, 10, 150, 32, "Shutter aktiv", big=True),
    ])
    scope = card("section-scope", 12, 64, 616, 148, title="Gate-Muster · 2 Takte", kids=[
        custom("SonoraShutterScope", 12, 32, 592, 104)])
    timing = card("section-timing", 12, 224, 300, 184, title="Timing", kids=[
        toggle(1, 206, 7, 82, 24, "Sync"),
        card("grp-division", 12, 38, 276, 26, style="none", rule="1=1", steps=1, kids=[
            label(0, 0, 72, 26, "Notenwert", align="left"),
            menu(2, 76, 0, 200, 26, DIVISIONS)]),
        knob_cell("grp-rate", 3, 12, 78, 69, "Rate", d=48, rule="1=0", steps=1),
        knob_cell("grp-pulsewidth", 7, 81, 78, 69, "Pulsbreite", d=48),
        knob_cell("grp-swing", 8, 150, 78, 69, "Swing", d=48),
        knob_cell("grp-offset", 12, 219, 78, 69, "Versatz", d=48),
    ])
    sound = card("section-sound", 324, 224, 304, 184, title="Klang", accent="accent2", kids=[
        segment(9, 12, 38, 280, 26, ["Hart", "Weich", "Pumpe"], accent="accent2"),
        knob_cell("grp-depth", 4, 12, 78, 58, "Tiefe", d=48, accent="accent2"),
        knob_cell("grp-mix", 5, 70, 78, 58, "Mix", d=48, accent="accent2"),
        knob_cell("grp-smoothing", 6, 128, 78, 58, "Glättung", d=48, accent="accent2"),
        label(192, 78, 100, 14, "Stereo"),
        menu(10, 192, 94, 100, 24, ["Mono", "Ping-Pong"]),
        label(192, 126, 100, 14, "Gate-Klang"),
        menu(11, 192, 142, 100, 24, ["Stumm", "Telefon", "Dumpf"]),
    ])
    return V("root", 0, 0, 640, 420, dict(name="view"), [header, scope, timing, sound])


def drowner_layout():
    header = card("header", 0, 0, 640, 52, style="header", kids=[
        *logo(20, 12, "drowner"),
        badge(210, 17, 40, 18, VERSION),
        label(380, 12, 248, 28, "Vocals unter Wasser ziehen", font="sonora.small", color="muted",
              align="right"),
    ])
    water = custom("SonoraWaterView", 12, 64, 400, 220)
    depth = card("section-depth", 424, 64, 204, 220, title="Unterwasser-Anteil", accent="accent2", kids=[
        knob(0, 42, 38, 120, accent="accent2"),
        value(0, 0, 162, 204, 26, font="sonora.big"),
        label(0, 190, 204, 16, "automatisieren zum Abtauchen", font="sonora.small", color="muted"),
    ])
    cells = [("grp-dive", 2, "Abtauchzeit"), ("grp-rise", 1, "Auftauchzeit"), ("grp-resonance", 3, "Resonanz"),
             ("grp-wobble", 4, "Wobble"), ("grp-reverb", 5, "Wasserhall"), ("grp-body", 6, "Körper"),
             ("grp-mix", 7, "Mix"), ("grp-output", 8, "Ausgang")]
    sound = card("section-sound", 12, 296, 616, 112, title="Wasser & Bewegung", kids=[
        knob_cell(n, t, 12 + i * 74, 28, 74, txt) for i, (n, t, txt) in enumerate(cells)])
    return V("root", 0, 0, 640, 420, dict(name="view"), [header, water, depth, sound])


# ------------------------------------------------------------------------------------------------
# uidesc writer
# ------------------------------------------------------------------------------------------------
def color_ref(key):
    return "sonora." + key if key in ("text", "muted", "label", "accent", "accent2", "warn") else C[key]


class UidescWriter:
    def __init__(self, params):
        self.params = params
        self.used_tags = set()
        self.lines = []

    def tagname(self, tag):
        self.used_tags.add(tag)
        return self.params[tag][0]

    @staticmethod
    def a(**kw):
        out = []
        for k, v in kw.items():
            if v is None:
                continue
            k = k.replace("_", "-")
            if isinstance(v, bool):
                v = "true" if v else "false"
            elif isinstance(v, float):
                v = ("%.6f" % v).rstrip("0").rstrip(".")
            out.append(f"{k}={quoteattr(str(v))}")
        return " ".join(out)

    @staticmethod
    def geo(v):
        def n(x):
            return str(int(x)) if float(x).is_integer() else ("%.1f" % x)
        return dict(origin=f"{n(v.x)}, {n(v.y)}", size=f"{n(v.w)}, {n(v.h)}")

    def emit(self, v, depth):
        ind = "  " * depth
        p = v.p
        g = self.geo(v)
        if v.kind in ("root", "card"):
            style = "root" if v.kind == "root" else p["style"]
            attrs = dict(**({"name": p["name"]} if v.kind == "card" else {}),
                         custom_view_name="SonoraRoot" if v.kind == "root" else "SonoraCard",
                         sonora_style=None if v.kind == "root" else style,
                         sonora_title=p.get("title") or None,
                         sonora_accent=color_ref(p["accent"]) if p.get("accent") else None,
                         sonora_active_when=p.get("rule"),
                         sonora_steps=str(p.get("steps")) if p.get("rule") else None,
                         **g,
                         background_color={"root": "sonora.bg", "header": "sonora.panel", "card": "sonora.card",
                                           "inset": "sonora.inset", "none": "sonora.clear"}[style],
                         background_color_draw_style="filled",
                         transparent=style == "none")
            if v.kind == "root":
                attrs.update(minSize=g["size"], maxSize=g["size"])
                head = f'{ind}<template class="CViewContainer" name="view" {self.a(**attrs)}>'
                tail = f"{ind}</template>"
            else:
                head = f'{ind}<view class="CViewContainer" {self.a(**attrs)}>'
                tail = f"{ind}</view>"
            self.lines.append(head)
            for k in v.kids:
                self.emit(k, depth + 1)
            self.lines.append(tail)
            return
        if v.kind == "label":
            self.lines.append(f'{ind}<view class="CTextLabel" ' + self.a(
                **g, title=p["text"], font=p["font"], font_color=color_ref(p["color"]),
                text_alignment=p["align"], transparent=True, style_no_frame=True, mouse_enabled=False) + "/>")
        elif v.kind == "badge":
            self.lines.append(f'{ind}<view class="CTextLabel" ' + self.a(
                **g, title=p["text"], font="sonora.small.bold", font_color="sonora.badge.text",
                back_color="sonora.badge", frame_color="sonora.badge.frame", style_round_rect=True,
                round_rect_radius=9.0, frame_width=1.0, text_alignment="center", transparent=False,
                mouse_enabled=False) + "/>")
        elif v.kind == "line":
            self.lines.append(f'{ind}<view class="CViewContainer" ' + self.a(
                **g, background_color="sonora.stroke", background_color_draw_style="filled",
                transparent=False, mouse_enabled=False) + "/>")
        elif v.kind == "value":
            self.lines.append(f'{ind}<view class="CParamDisplay" ' + self.a(
                control_tag=self.tagname(p["tag"]), **g, font=p["font"], font_color=color_ref(p["color"]),
                back_color="sonora.clear", frame_color="sonora.clear", shadow_color="sonora.clear",
                text_alignment=p["align"], style_no_frame=True, transparent=True, mouse_enabled=False,
                value_precision=1) + "/>")
        elif v.kind == "knob":
            dflt = self.params[p["tag"]][3]
            self.lines.append(f'{ind}<view class="CKnob" custom-view-name="SonoraKnob" ' + self.a(
                control_tag=self.tagname(p["tag"]), **g, min_value=0.0, max_value=1.0,
                default_value=float(dflt), angle_start=135.0, angle_range=270.0, circle_drawing=False,
                corona_drawing=True, corona_from_center=p["bipolar"], corona_color=color_ref(p["accent"]),
                corona_inset=4.0, handle_color="sonora.text", handle_shadow_color="sonora.clear",
                handle_line_width=2.0, value_inset=6.0, zoom_factor=6.0, wheel_inc_value=0.01,
                transparent=True) + "/>")
        elif v.kind == "toggle":
            self.lines.append(f'{ind}<view class="CTextButton" custom-view-name="SonoraToggle" ' + self.a(
                control_tag=self.tagname(p["tag"]), **g, title=p["title"],
                font="sonora.value" if p["big"] else "sonora.small.bold", kick_style=False,
                min_value=0.0, max_value=1.0, default_value=float(self.params[p["tag"]][3]),
                sonora_on_color=color_ref(p["on"]), sonora_big=p["big"] or None,
                text_color="sonora.text", text_color_highlighted="sonora.text", frame_color="sonora.stroke",
                frame_color_highlighted="sonora.accent", frame_width=1.0, round_radius=9.0) + "/>")
        elif v.kind == "segment":
            self.lines.append(f'{ind}<view class="CSegmentButton" custom-view-name="SonoraSegment" ' + self.a(
                control_tag=self.tagname(p["tag"]), **g, style="horizontal", selection_mode="Single",
                segment_names=",".join(p["names"]), font="sonora.small.bold", text_color="sonora.muted",
                text_color_highlighted="sonora.text", frame_color="sonora.stroke", round_radius=9.0,
                frame_width=1.0, sonora_accent=color_ref(p["accent"])) + "/>")
        elif v.kind == "menu":
            self.lines.append(f'{ind}<view class="COptionMenu" custom-view-name="SonoraMenu" ' + self.a(
                control_tag=self.tagname(p["tag"]), **g, menu_popup_style=True, menu_check_style=True,
                font="sonora.small.bold", font_color="sonora.text", back_color="sonora.inset",
                frame_color="sonora.stroke", style_round_rect=True, round_rect_radius=7.0,
                text_inset="10, 0", text_alignment="left") + "/>")
        elif v.kind == "custom":
            view = p["view"]
            if view == "SonoraEqFader":
                self.lines.append(f'{ind}<view class="CSlider" custom-view-name="SonoraEqFader" ' + self.a(
                    control_tag=self.tagname(p["tag"]), **g, orientation="vertical", reverse_orientation=False,
                    mode="free click", min_value=0.0, max_value=1.0, default_value=0.5,
                    draw_frame=False, draw_back=True, draw_value=True, draw_value_from_center=True,
                    back_color="sonora.inset", value_color="sonora.accent", frame_color="sonora.stroke",
                    sonora_accent="sonora.accent", transparent=True, wheel_inc_value=0.01) + "/>")
            else:
                self.lines.append(f'{ind}<view class="CView" custom-view-name="{view}" ' + self.a(**g) + "/>")
        else:
            raise ValueError(v.kind)

    def write(self, layout, path, comment):
        head = ['<?xml version="1.0" encoding="UTF-8"?>', "<!--", comment.rstrip(), "-->",
                '<vstgui-ui-description version="1">']
        self.lines = []
        self.emit(layout, 1)
        body = self.lines
        out = head + ["  <fonts>"]
        for name, (size, bold) in FONTS.items():
            out.append(f"    <font {self.a(name=name, font_name=FONT_FAMILY, size=str(size))}"
                       + (' bold="true"' if bold else "")
                       + f" {self.a(alternative_font_names=FONT_ALTS)}/>")
        out += ["  </fonts>", "  <colors>"]
        for name, col in UIDESC_COLORS:
            col = col if len(col) == 9 else col + "ff"
            out.append(f"    <color {self.a(name=name, rgba=col)}/>")
        out += ["  </colors>", "  <control-tags>"]
        for tag in sorted(self.params):
            out.append(f"    <control-tag {self.a(name=self.params[tag][0], tag=str(tag))}/>")
        out.append("  </control-tags>")
        out += body
        out.append("</vstgui-ui-description>")
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(out) + "\n")


# ------------------------------------------------------------------------------------------------
# HTML renderer (mirrors sonora_ui.h drawing code)
# ------------------------------------------------------------------------------------------------
def svg_arc(cx, cy, r, a0, a1):
    p0 = (cx + r * math.cos(math.radians(a0)), cy + r * math.sin(math.radians(a0)))
    p1 = (cx + r * math.cos(math.radians(a1)), cy + r * math.sin(math.radians(a1)))
    large = 1 if (a1 - a0) > 180 else 0
    return f"M{p0[0]:.2f},{p0[1]:.2f} A{r:.2f},{r:.2f} 0 {large} 1 {p1[0]:.2f},{p1[1]:.2f}"


_uid = [0]


def uid(prefix):
    _uid[0] += 1
    return f"{prefix}{_uid[0]}"


def knob_svg(d, v, bipolar, accent):
    cx = cy = d / 2
    stroke = max(3.0, d * 0.075)
    radius = d * 0.5 - stroke * 1.4
    body_r = radius - stroke * 1.25
    acc = C[accent]
    gid = uid("kg")
    out = [f'<svg width="{d}" height="{d}" viewBox="0 0 {d} {d}">',
           f'<defs><linearGradient id="{gid}" x1="0" y1="0" x2="0" y2="1">'
           f'<stop offset="0" stop-color="#342d46"/><stop offset="1" stop-color="#181521"/></linearGradient></defs>',
           f'<path d="{svg_arc(cx, cy, radius, 135, 405)}" fill="none" stroke="{C["stroke"]}" '
           f'stroke-width="{stroke:.2f}" stroke-linecap="round"/>']
    a_from, a_to = 135.0, 135.0 + 270.0 * v
    if bipolar:
        a = 270.0 + 270.0 * (v - 0.5)
        a_from, a_to = min(270.0, a), max(270.0, a)
    if a_to - a_from >= 0.05:
        for w, al in ((2.6, 40), (1.7, 70), (1.0, 255)):
            out.append(f'<path d="{svg_arc(cx, cy, radius, a_from, a_to)}" fill="none" '
                       f'stroke="{rgba(acc, al)}" stroke-width="{stroke * w:.2f}" stroke-linecap="round"/>')
    out.append(f'<circle cx="{cx}" cy="{cy}" r="{body_r:.2f}" fill="url(#{gid})" stroke="#463d5c" stroke-width="1"/>')
    ang = math.radians(135.0 + 270.0 * v)
    p0 = (cx + math.cos(ang) * body_r * 0.30, cy + math.sin(ang) * body_r * 0.30)
    p1 = (cx + math.cos(ang) * body_r * 0.78, cy + math.sin(ang) * body_r * 0.78)
    out.append(f'<line x1="{p0[0]:.2f}" y1="{p0[1]:.2f}" x2="{p1[0]:.2f}" y2="{p1[1]:.2f}" stroke="{C["text"]}" '
               f'stroke-width="{max(2.0, d * 0.04):.2f}" stroke-linecap="round"/>')
    tip = (cx + math.cos(ang) * radius, cy + math.sin(ang) * radius)
    out.append(f'<circle cx="{tip[0]:.2f}" cy="{tip[1]:.2f}" r="{stroke * 0.95:.2f}" fill="{C["text"]}"/>')
    out.append("</svg>")
    return "".join(out)


def font_css(font):
    size, bold = FONTS[font]
    return f"font-size:{size}px;font-weight:{700 if bold else 400};"


def text_div(x, y, w, h, s, font, color, align="center", extra=""):
    jc = {"left": "flex-start", "center": "center", "right": "flex-end"}[align]
    return (f'<div class="t" style="left:{x}px;top:{y}px;width:{w}px;height:{h}px;justify-content:{jc};'
            f'{font_css(font)}color:{color};{extra}">{esc(s)}</div>')


# ---- EQ maths (dsp_common.h Biquad, RBJ cookbook) ----------------------------------------------
def norm_bq(b0, b1, b2, a0, a1, a2):
    return (b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0)


def bq_peak(sr, f, q, g):
    A = 10 ** (g / 40)
    w = 2 * math.pi * f / sr
    c, a = math.cos(w), math.sin(w) / (2 * q)
    return norm_bq(1 + a * A, -2 * c, 1 - a * A, 1 + a / A, -2 * c, 1 - a / A)


def bq_shelf(sr, f, g, high):
    A = 10 ** (g / 40)
    w = 2 * math.pi * f / sr
    c, s = math.cos(w), math.sin(w)
    alpha = s / 2 * math.sqrt((A + 1 / A) * (1 / 1.0 - 1) + 2)
    k = 2 * math.sqrt(A) * alpha
    if not high:
        return norm_bq(A * ((A + 1) - (A - 1) * c + k), 2 * A * ((A - 1) - (A + 1) * c),
                       A * ((A + 1) - (A - 1) * c - k), (A + 1) + (A - 1) * c + k,
                       -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - k)
    return norm_bq(A * ((A + 1) + (A - 1) * c + k), -2 * A * ((A - 1) + (A + 1) * c),
                   A * ((A + 1) + (A - 1) * c - k), (A + 1) - (A - 1) * c + k,
                   2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - k)


def bq_db(q, f, sr):
    w = 2 * math.pi * f / sr
    z1, z2 = cmath.exp(-1j * w), cmath.exp(-2j * w)
    num = q[0] + q[1] * z1 + q[2] * z2
    den = 1 + q[3] * z1 + q[4] * z2
    return 20 * math.log10(max(1e-9, abs(num) / max(1e-12, abs(den))))


EQ_F = [60.0, 250.0, 1000.0, 4000.0, 12000.0]


def eq_response(g, f, sr=48000.0):
    db = bq_db(bq_shelf(sr, EQ_F[0], g[0], False), f, sr)
    for i in (1, 2, 3):
        db += bq_db(bq_peak(sr, EQ_F[i], 0.65, g[i]), f, sr)
    return db + bq_db(bq_shelf(sr, EQ_F[4], g[4], True), f, sr)


def iec(db):
    if db < -70: p = 0.0
    elif db < -60: p = (db + 70) * 0.25
    elif db < -50: p = (db + 60) * 0.5 + 2.5
    elif db < -40: p = (db + 50) * 0.75 + 7.5
    elif db < -30: p = (db + 40) * 1.5 + 15
    elif db < -20: p = (db + 30) * 2.0 + 30
    elif db < 0: p = (db + 20) * 2.5 + 50
    else: p = 100.0
    return p / 100


class Html:
    def __init__(self, params, demo):
        self.params = params
        self.demo = demo

    def norm(self, tag):
        return self.params[tag][1]

    def render(self, v):
        p = v.p
        box = f"left:{v.x}px;top:{v.y}px;width:{v.w}px;height:{v.h}px;"
        if v.kind in ("root", "card"):
            style = "root" if v.kind == "root" else p["style"]
            cls = {"root": "root", "header": "hdr", "card": "card", "inset": "inset", "none": "grp"}[style]
            rule = ""
            if p.get("rule"):
                pid, allowed = p["rule"].split("=")
                idx = round(self.norm(int(pid)) * p["steps"])
                active = str(idx) in allowed.split(",")
                rule = f' data-rule="{p["rule"]}" data-steps="{p["steps"]}"' + ("" if active else " data-dim")
            inner = []
            if style == "card" and p.get("title"):
                acc = C[p["accent"]] if p.get("accent") else C["accent"]
                inner.append(f'<i class="dot" style="background:{acc};box-shadow:0 0 8px {rgba(acc, 140)}"></i>')
                inner.append(text_div(25, 9, v.w - 35, 16, p["title"], "sonora.title", C["text"], "left"))
            inner += [self.render(k) for k in v.kids]
            name = f' data-name="{p["name"]}"' if p.get("name") else ""
            return f'<div class="{cls}"{name}{rule} style="{box}">' + "".join(inner) + "</div>"
        if v.kind == "label":
            return text_div(v.x, v.y, v.w, v.h, p["text"], p["font"], C[p["color"]], p["align"])
        if v.kind == "badge":
            return text_div(v.x, v.y, v.w, v.h, p["text"], "sonora.small.bold", "#c9b6ff", "center",
                            "background:rgba(139,92,246,.2);box-shadow:inset 0 0 0 1px rgba(139,92,246,.5);"
                            "border-radius:9px;")
        if v.kind == "line":
            return f'<div class="abs" style="{box}background:{C["stroke"]}"></div>'
        if v.kind == "value":
            return text_div(v.x, v.y, v.w, v.h, self.params[p["tag"]][2], p["font"], C[p["color"]], p["align"],
                            "font-variant-numeric:tabular-nums;")
        if v.kind == "knob":
            return (f'<div class="abs knob" title="{esc(self.params[p["tag"]][0])}" style="{box}">'
                    + knob_svg(v.w, self.norm(p["tag"]), p["bipolar"], p["accent"]) + "</div>")
        if v.kind == "toggle":
            return self.toggle(v)
        if v.kind == "segment":
            return self.segment(v)
        if v.kind == "menu":
            return self.menu(v)
        if v.kind == "custom":
            return getattr(self, "v_" + p["view"])(v)
        raise ValueError(v.kind)

    # -- controls --------------------------------------------------------------------------------
    def toggle(self, v):
        p = v.p
        on = self.norm(p["tag"]) >= 0.5
        oc = C[p["on"]]
        radius = min(v.h / 2, 9)
        if on:
            bg = f"linear-gradient({mix(oc, C['text'], .12)},{mix(oc, C['bg'], .25)})"
            border = mix(oc, C["text"], .3)
        else:
            bg = "linear-gradient(#252032,#1b1725)"
            border = C["strokeHi"]
        led = 5 if p["big"] else 4
        lx = 12 + led
        glow = f"box-shadow:0 0 0 {led}px {rgba(C['text'], 50)};" if on else ""
        ledc = C["text"] if on else "#48405c"
        font = "sonora.value" if p["big"] else "sonora.small.bold"
        return (f'<div class="abs tog" style="left:{v.x}px;top:{v.y}px;width:{v.w}px;height:{v.h}px;'
                f'border-radius:{radius}px;background:{bg};box-shadow:inset 0 0 0 1px {border}">'
                f'<i class="led" style="left:{lx - led}px;top:{v.h / 2 - led}px;width:{2 * led}px;'
                f'height:{2 * led}px;background:{ledc};{glow}"></i>'
                + text_div(lx + led + 6, 0, v.w - (lx + led + 6) - 8, v.h, p["title"], font,
                           C["text"] if on else C["muted"])
                + "</div>")

    def segment(self, v):
        p = v.p
        n = len(p["names"])
        sel = round(self.norm(p["tag"]) * (n - 1))
        radius = min(v.h / 2, 9)
        acc = C[p["accent"]]
        w = v.w / n
        out = [f'<div class="abs seg" style="left:{v.x}px;top:{v.y}px;width:{v.w}px;height:{v.h}px;'
               f'border-radius:{radius}px;background:{C["inset"]};box-shadow:inset 0 0 0 1px {C["stroke"]}">']
        for i, name in enumerate(p["names"]):
            x = w * i
            if i == sel:
                out.append(f'<div class="abs" style="left:{x + 2}px;top:2px;width:{w - 4}px;height:{v.h - 4}px;'
                           f'border-radius:{radius - 2}px;background:linear-gradient({mix(acc, C["text"], .1)},'
                           f'{mix(acc, C["bg"], .3)});box-shadow:0 2px 10px {rgba(acc, 70)}"></div>')
            elif i > 0 and i != sel + 1:
                out.append(f'<div class="abs" style="left:{x}px;top:7px;width:1px;height:{v.h - 14}px;'
                           f'background:{C["stroke"]}"></div>')
            out.append(text_div(x, 0, w, v.h, name, "sonora.small.bold", C["text"] if i == sel else C["muted"]))
        out.append("</div>")
        return "".join(out)

    def menu(self, v):
        p = v.p
        cur = self.params[p["tag"]][2]
        return (f'<div class="abs menu" style="left:{v.x}px;top:{v.y}px;width:{v.w}px;height:{v.h}px;'
                f'border-radius:7px;background:linear-gradient(#211c2d,#181521);'
                f'box-shadow:inset 0 0 0 1px {C["strokeHi"]}">'
                + text_div(10, 0, v.w - 34, v.h, cur, "sonora.small.bold", C["text"], "left")
                + f'<svg class="abs" style="left:{v.w - 18}px;top:{v.h / 2 - 4}px" width="10" height="8">'
                  f'<path d="M1,2 L5,6.5 L9,2" fill="none" stroke="{C["muted"]}" stroke-width="1.6" '
                  f'stroke-linecap="round" stroke-linejoin="round"/></svg></div>')

    # -- master views ----------------------------------------------------------------------------
    def v_SonoraEqFader(self, v):
        p = v.p
        w, h = v.w, v.h
        acc = C["accent"]
        cx, top, bot = w / 2, 7, h - 7
        mid = (top + bot) / 2
        y = bot - self.norm(p["tag"]) * (bot - top)
        gid = uid("fg")
        s = [f'<svg class="abs" style="left:{v.x}px;top:{v.y}px" width="{w}" height="{h}">',
             f'<defs><linearGradient id="{gid}" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#f6f3fc"/>'
             f'<stop offset="1" stop-color="#c4bcd8"/></linearGradient></defs>',
             f'<rect x="{cx - 2.5}" y="{top}" width="5" height="{bot - top}" rx="2.5" fill="{C["inset"]}" '
             f'stroke="{C["stroke"]}"/>']
        for i in range(5):
            ty = top + (bot - top) * i / 4
            col = C["muted"] if i == 2 else C["strokeHi"]
            l = 9 if i == 2 else 6
            s.append(f'<path d="M{cx - l},{ty} H{cx - 4.5} M{cx + 4.5},{ty} H{cx + l}" stroke="{col}"/>')
        a, z = min(y, mid), max(y, mid)
        if z - a > 0.5:
            s.append(f'<rect x="{cx - 4.5}" y="{a - 1}" width="9" height="{z - a + 2}" rx="3" fill="{rgba(acc, 60)}"/>')
            s.append(f'<rect x="{cx - 2}" y="{a}" width="4" height="{z - a}" rx="2" fill="{acc}"/>')
        s.append(f'<rect x="{cx - 14}" y="{y - 9}" width="28" height="18" rx="8" fill="{rgba(acc, 45)}"/>')
        s.append(f'<rect x="{cx - 11}" y="{y - 6}" width="22" height="12" rx="5" fill="url(#{gid})"/>')
        s.append(f'<line x1="{cx - 6}" y1="{y}" x2="{cx + 6}" y2="{y}" stroke="{acc}" stroke-width="2"/>')
        s.append("</svg>")
        return "".join(s)

    def v_SonoraEqGraph(self, v):
        w, h = v.w, v.h
        user = [self.norm(t) * 16 - 8 for t in range(11, 16)]
        auto = self.demo.get("autoEq", [0] * 5)
        total = [u + a for u, a in zip(user, auto)]
        pl, pt, pr_, pb = 30.0, 10.0, w - 12.0, h - 20.0
        pw, ph = pr_ - pl, pb - pt
        xo = lambda f: pl + pw * math.log(f / 20) / math.log(1000)
        yo = lambda db: pt + ph * (0.5 - max(-13, min(13, db)) / 24)
        gid = uid("eg")
        s = [f'<svg class="abs" style="left:0;top:0" width="{w}" height="{h}">',
             f'<rect x=".5" y=".5" width="{w - 1}" height="{h - 1}" rx="8" fill="{C["inset"]}" stroke="{C["stroke"]}"/>']
        txt = []
        for db in (-12, -6, 0, 6, 12):
            y = yo(db)
            s.append(f'<line x1="{pl}" y1="{y}" x2="{pr_}" y2="{y}" stroke="{C["strokeHi"] if db == 0 else C["grid"]}"/>')
            txt.append(text_div(4, y - 7, pl - 9, 14, "0" if db == 0 else fmt(db, 0, True), "sonora.small",
                                C["muted"], "right"))
        for f, lab in ((50, "50"), (100, "100"), (250, "250"), (500, "500"), (1000, "1k"), (2000, "2k"),
                       (5000, "5k"), (10000, "10k"), (20000, "20k")):
            x = xo(f)
            if f < 20000:
                s.append(f'<line x1="{x:.1f}" y1="{pt}" x2="{x:.1f}" y2="{pb}" stroke="{C["grid"]}"/>')
            txt.append(text_div(x - 20 if f < 20000 else x - 30, pb + 2, 40 if f < 20000 else 34, 14, lab,
                                "sonora.small", C["muted"], "center" if f < 20000 else "right"))
        n = max(16, int(pw / 2))
        pts, apts = [], []
        for k in range(n):
            t = k / (n - 1)
            f = 20 * 1000 ** t
            x = pl + pw * t
            pts.append((x, yo(eq_response(total, f))))
            apts.append((x, yo(eq_response(auto, f))))
        zero = yo(0)
        line = " ".join(f"{x:.1f},{y:.1f}" for x, y in pts)
        min_y = min(min(y for _, y in pts), zero - 1)
        s.append(f'<defs><linearGradient id="{gid}" gradientUnits="userSpaceOnUse" x1="0" y1="{min_y:.1f}" '
                 f'x2="0" y2="{zero + 30:.1f}"><stop offset="0" stop-color="{rgba(C["accent"], 90)}"/>'
                 f'<stop offset="1" stop-color="{rgba(C["accent"], 10)}"/></linearGradient></defs>')
        s.append(f'<polygon points="{pts[0][0]:.1f},{zero:.1f} {line} {pts[-1][0]:.1f},{zero:.1f}" fill="url(#{gid})"/>')
        if any(abs(a) > 0.05 for a in auto):
            s.append(f'<polyline points="{" ".join(f"{x:.1f},{y:.1f}" for x, y in apts)}" fill="none" '
                     f'stroke="{rgba(C["accent2"], 200)}" stroke-width="1.4" stroke-dasharray="3 3" '
                     f'stroke-linecap="round"/>')
            txt.append(text_div(pr_ - 90, pt + 2, 86, 14, "- - Auto-EQ", "sonora.small", C["accent2"], "right"))
        s.append(f'<polyline points="{line}" fill="none" stroke="{rgba(C["accent"], 60)}" stroke-width="6" '
                 f'stroke-linejoin="round" stroke-linecap="round"/>')
        s.append(f'<polyline points="{line}" fill="none" stroke="#b08fff" stroke-width="2" stroke-linejoin="round" '
                 f'stroke-linecap="round"/>')
        for i in range(5):
            hx, hy = xo(EQ_F[i]), yo(eq_response(total, EQ_F[i]))
            ring = C["accent2"] if abs(user[i]) > 0.05 else C["accent"]
            s.append(f'<circle cx="{hx:.1f}" cy="{hy:.1f}" r="9" fill="{rgba(C["accent"], 50)}"/>')
            s.append(f'<circle cx="{hx:.1f}" cy="{hy:.1f}" r="6.5" fill="{C["inset"]}" stroke="{ring}" stroke-width="2"/>')
            txt.append(text_div(hx - 6, hy - 7, 12, 14, str(i + 1), "sonora.small.bold", C["text"],
                                extra="font-size:10px;"))
        s.append("</svg>")
        return (f'<div class="abs" style="left:{v.x}px;top:{v.y}px;width:{w}px;height:{h}px">'
                + "".join(s) + "".join(txt) + "</div>")

    def v_SonoraAnalysisStatus(self, v):
        d = self.demo["analysis"]
        w, h = v.w, v.h
        out = [f'<div class="abs" style="left:{v.x}px;top:{v.y}px;width:{w}px;height:{h}px;border-radius:8px;'
               f'background:{C["inset"]};box-shadow:inset 0 0 0 1px {C["stroke"]}">',
               f'<i class="led" style="left:9px;top:9px;width:7px;height:7px;background:{C["good"]};'
               f'box-shadow:0 0 6px {rgba(C["good"], 160)}"></i>']
        msg = (f"Analyse fertig · RMS Beat {fmt(d['beatRms'])} / Vocal {fmt(d['vocalRms'])} dBFS · "
               f"Crest {fmt(d['crest'])} dB")
        out.append(text_div(22, 5, w - 32, 16, msg, "sonora.small", C["text"], "left"))
        out.append(f'<div class="abs" style="left:10px;top:25px;width:{w - 20}px;height:3px;border-radius:1.5px;'
                   f'background:{C["accent"]}"></div>')
        names = ["60 Hz", "250 Hz", "1 kHz", "4 kHz", "12 kHz"]
        cw = (w - 20) / 5
        for i in range(5):
            x = 10 + cw * i
            out.append(text_div(x, 31, 44, h - 35, names[i], "sonora.small", C["muted"], "left"))
            out.append(text_div(x + 42, 31, 34, h - 35, "B " + fmt(d["beat"][i], 0), "sonora.small", C["text"], "left"))
            out.append(text_div(x + 76, 31, cw - 76, h - 35, "V " + fmt(d["vocal"][i], 0), "sonora.small",
                                C["accent2"], "left"))
        out.append("</div>")
        return "".join(out)

    def v_SonoraLevelMeters(self, v):
        w, h = v.w, v.h
        d = self.demo["levels"]
        lane_t, lane_b = 24.0, h - 18.0
        lh = lane_b - lane_t
        bw, gap = 13.0, 4.0
        cx = w / 2
        sl, sr = cx - 18, cx + 18
        xs = [sl - 2 * bw - gap - 4, sl - bw - 4, sr + 4, sr + bw + gap + 4]
        out = [f'<div class="abs" style="left:{v.x}px;top:{v.y}px;width:{w}px;height:{h}px">']
        for val, cxx in ((max(d["hold"][0:2]), xs[0] + bw + gap / 2), (max(d["hold"][2:4]), xs[2] + bw + gap / 2)):
            out.append(text_div(cxx - 30, 0, 60, 16, fmt(val, 1), "sonora.small.bold",
                                C["bad"] if val > -0.1 else C["text"], "center",
                                f"background:{C['inset']};border-radius:5px;"))
        last = -1000
        for db in (0, -3, -6, -9, -12, -18, -24, -30, -40, -50, -60):
            y = lane_b - lh * iec(db)
            if y - last < 12:
                continue
            last = y
            out.append(text_div(sl, y - 7, 36, 14, "0" if db == 0 else str(db), "sonora.small", C["muted"]))
            out.append(f'<div class="abs" style="left:{sl - 3}px;top:{y - .5:.1f}px;width:3px;height:1px;background:{C["strokeHi"]}"></div>')
            out.append(f'<div class="abs" style="left:{sr}px;top:{y - .5:.1f}px;width:3px;height:1px;background:{C["strokeHi"]}"></div>')
        stops = (f"{C['good']} 0%,{C['good']} {iec(-18) * 100:.1f}%,{C['mid']} {iec(-9) * 100:.1f}%,"
                 f"{C['warn']} {iec(-3) * 100:.1f}%,{C['bad']} {iec(-0.5) * 100:.1f}%,{C['bad']} 100%")
        for ch in range(4):
            lvl, hold = d["level"][ch], d["hold"][ch]
            frac = iec(lvl)
            anim = f"mtr{ch}"
            out.append(f'<div class="abs bar" style="left:{xs[ch]}px;top:{lane_t}px;width:{bw}px;height:{lh}px">'
                       f'<div class="lit" style="height:{frac * 100:.1f}%;background:linear-gradient(to top,{stops});'
                       f'background-size:100% {lh}px;animation:{anim} {1.3 + ch * 0.17:.2f}s ease-in-out infinite alternate"></div>'
                       f'<div class="seg3"></div>'
                       f'<div class="hold" style="bottom:{iec(hold) * lh - 1:.1f}px;background:{C["text"]}"></div></div>')
            out.append(f"<style>@keyframes {anim}{{0%{{height:{frac * 100:.1f}%}}55%{{height:{iec(lvl - 4.5) * 100:.1f}%}}"
                       f"100%{{height:{iec(lvl - 1.5) * 100:.1f}%}}}}</style>")
        out.append(text_div(xs[0] - 6, lane_b + 3, xs[1] + bw + 6 - (xs[0] - 6), h - lane_b - 3, "IN",
                            "sonora.small.bold", C["muted"]))
        out.append(text_div(xs[2] - 6, lane_b + 3, xs[3] + bw + 6 - (xs[2] - 6), h - lane_b - 3, "OUT",
                            "sonora.small.bold", C["muted"]))
        out.append("</div>")
        return "".join(out)

    def v_SonoraGainReduction(self, v):
        w = v.w
        d = self.demo["gr"]
        out = [f'<div class="abs" style="left:{v.x}px;top:{v.y}px;width:{w}px;height:{v.h}px">']

        def row(y, lab, db, col, anim):
            frac = math.sqrt(max(0, min(1, db / 12)))
            return (text_div(0, y, 60, 14, lab, "sonora.small.bold", C["muted"], "left")
                    + text_div(w - 70, y, 70, 14, "0,0 dB" if db < 0.05 else "-" + fmt(db) + " dB", "sonora.small",
                               C["text"], "right")
                    + f'<div class="abs" style="left:0;top:{y + 17}px;width:{w}px;height:8px;border-radius:4px;'
                      f'background:{C["inset"]};box-shadow:inset 0 0 0 1px {C["stroke"]};overflow:hidden">'
                      f'<div class="abs" style="right:0;top:0;bottom:0;width:{frac * 100:.1f}%;background:{col};'
                      f'border-radius:4px;transform-origin:right;animation:{anim} 1.7s ease-in-out infinite alternate">'
                      f'</div></div>')
        out.append(row(0, "GLUE", d["glue"], C["accent"], "grA"))
        out.append(row(30, "LIMITER", d["lim"], C["accent2"], "grB"))
        y = 62
        clip = d["clip"]
        out.append(text_div(0, y, 70, 14, "CLIPPER", "sonora.small.bold", C["muted"], "left"))
        out.append(f'<i class="led clipled" style="left:{w - 7 - 4.5}px;top:{y + 7 - 4.5}px;width:9px;height:9px;'
                   f'background:{mix("#48405c", C["warn"], clip)};box-shadow:0 0 8px {rgba(C["warn"], int(150 * clip))}"></i>')
        out.append(text_div(w - 70, y, 54, 14, fmt(clip * 100, 0) + " %", "sonora.small", C["text"], "right"))
        out.append("</div>")
        return "".join(out)

    def v_SonoraLoudness(self, v):
        w, h = v.w, v.h
        d = self.demo["lufs"]
        out = [f'<div class="abs" style="left:{v.x}px;top:{v.y}px;width:{w}px;height:{h}px;border-radius:8px;'
               f'background:linear-gradient(#181422,{C["inset"]});box-shadow:inset 0 0 0 1px {C["stroke"]}">',
               text_div(10, 8, w - 20, 14, "INTEGRIERT", "sonora.small.bold", C["muted"], "left"),
               text_div(w - 60, 8, 50, 14, "RESET", "sonora.small", rgba(C["muted"], 170), "right"),
               text_div(9, 22, w - 53, 34, fmt(d["i"]), "sonora.huge", C["text"], "left", "letter-spacing:-.5px;"),
               text_div(w - 46, 38, 36, 16, "LUFS", "sonora.small.bold", C["accent2"], "right"),
               f'<div class="abs" style="left:10px;top:62px;width:{w - 20}px;height:1px;background:{C["stroke"]}"></div>']
        for i, (lab, val) in enumerate((("Momentan", fmt(d["m"])), ("Kurzzeit", fmt(d["s"])),
                                        ("True Peak", fmt(d["tp"])))):
            y = 68 + 17 * i
            out.append(text_div(10, y, w - 70, 16, lab, "sonora.small", C["muted"], "left"))
            out.append(text_div(w - 70, y, 60, 16, val, "sonora.value", C["text"], "right",
                                "font-variant-numeric:tabular-nums;"))
        out.append("</div>")
        return "".join(out)

    def v_SonoraCorrelation(self, v):
        w = v.w
        val = self.demo["corr"]
        cx = w * (val + 1) / 2
        return (f'<div class="abs" style="left:{v.x}px;top:{v.y}px;width:{w}px;height:{v.h}px">'
                + text_div(0, 0, w - 40, 14, "KORRELATION", "sonora.small.bold", C["muted"], "left")
                + text_div(w - 50, 0, 50, 14, fmt(val, 2, True), "sonora.small", C["text"], "right")
                + f'<div class="abs" style="left:0;top:19px;width:{w}px;height:8px;border-radius:4px;'
                  f'box-shadow:inset 0 0 0 1px {C["stroke"]};background:linear-gradient(90deg,{rgba(C["bad"], 150)} 0%,'
                  f'{rgba(C["warn"], 110)} 45%,{rgba(C["good"], 110)} 60%,{rgba(C["good"], 170)} 100%)"></div>'
                + f'<div class="abs" style="left:{w / 2 - .5}px;top:17px;width:1px;height:12px;background:{C["strokeHi"]}"></div>'
                + f'<div class="abs corr" style="left:{cx - 3}px;top:16px;width:6px;height:14px;border-radius:2px;'
                  f'background:{C["text"]}"></div></div>')

    # -- shutter -----------------------------------------------------------------------------------
    def v_SonoraShutterScope(self, v):
        w, h = v.w, v.h
        P = self.demo["shutter"]
        pl, pt, pr_, pb = 22.0, 24.0, w - 10.0, h - 10.0
        pw = pr_ - pl
        steps_beats = [4.0, 2.0, 1.0, 0.5, 0.25, 0.125, 1 / 3, 1 / 6, 1 / 12, 1.5, 0.75, 0.375]
        L = steps_beats[P["division"]] if P["sync"] else 1.0

        def gate(pos, ch):
            steps = pos / L if P["sync"] else pos
            position = steps - P["offset"]
            pair = position - 2 * math.floor(position * 0.5)
            delay = min(max(P["swing"], 0), 0.75) * 2 / 3
            if pair < 1 + delay:
                parity, ln = 0, 1 + delay
                u = min(max(pair / ln, 0), 1)
            else:
                parity, ln = 1, 1 - delay
                u = min(max((pair - 1 - delay) / ln, 0), 1)
            form = P["form"]
            if P["stereo"] == 1 and parity != ch:
                return 1.0 if form == 2 else 0.0
            sps = 2.0 / L if P["sync"] else max(0.5, P["rate"])
            step_s = ln / sps
            duty = P["width"]
            minimum = max(P["smooth"], 0.6) * 0.001 / max(step_s, 1e-9)
            if form == 2:
                edge = min(minimum, 0.5 * duty)
            else:
                max_edge = 0.5 * min(duty, 1 - duty)
                edge = max_edge if form == 1 else min(minimum, max_edge)
            rc = lambda t: 0.5 - 0.5 * math.cos(math.pi * t)
            if form == 2:
                if edge > 0 and u < edge:
                    return 1 - rc(u / edge)
                re = max(duty, edge + 1e-6)
                if u >= re:
                    return 1.0
                t = min(max((u - edge) / (re - edge), 0), 1)
                return t * t * (3 - 2 * t)
            if edge > 0 and u < edge:
                return rc(u / edge)
            if u < duty:
                return 1.0
            if edge > 0 and u < duty + edge:
                return 1 - rc((u - duty) / edge)
            return 0.0

        n = int(max(32, pw)) * 2
        eff = P["depth"] * P["mix"]
        s = [f'<svg class="abs" style="left:0;top:0" width="{w}" height="{h}">',
             f'<rect x=".5" y=".5" width="{w - 1}" height="{h - 1}" rx="8" fill="{C["inset"]}" stroke="{C["stroke"]}"/>']
        if pw / 8 * L > 5:
            b = P["offset"] * L
            while b < 8:
                x = pl + pw * b / 8
                s.append(f'<line x1="{x:.1f}" y1="{pt}" x2="{x:.1f}" y2="{pb}" stroke="{rgba(C["grid"], 160)}"/>')
                b += L
        for b in range(9):
            x = pl + pw * b / 8
            s.append(f'<line x1="{x:.1f}" y1="{pt - (4 if b % 4 == 0 else 0)}" x2="{x:.1f}" y2="{pb}" '
                     f'stroke="{C["strokeHi"] if b % 4 == 0 else C["stroke"]}"/>')
        dual = P["stereo"] == 1
        midy = (pt + pb) / 2
        lanes = ([((pt, midy - 3), 0, C["accent"]), ((midy + 3, pb), 1, C["accent2"])] if dual
                 else [((pt, pb), 0, C["accent"])])
        for (lt, lb), ch, col in lanes:
            pts = []
            for i in range(n):
                pos = 8 * i / (n - 1)
                g = 1 - eff * (1 - gate(pos, ch))
                pts.append((pl + pw * i / (n - 1), lb - (lb - lt - 2) * g))
            line = " ".join(f"{x:.1f},{y:.1f}" for x, y in pts)
            gid = uid("sg")
            s.append(f'<defs><linearGradient id="{gid}" gradientUnits="userSpaceOnUse" x1="0" y1="{lt}" x2="0" y2="{lb}">'
                     f'<stop offset="0" stop-color="{rgba(col, 120)}"/><stop offset="1" stop-color="{rgba(col, 18)}"/>'
                     f'</linearGradient></defs>')
            s.append(f'<polygon points="{pl},{lb} {line} {pr_},{lb}" fill="url(#{gid})"/>')
            s.append(f'<polyline points="{line}" fill="none" stroke="{rgba(col, 70)}" stroke-width="5" '
                     f'stroke-linejoin="round" stroke-linecap="round"/>')
            s.append(f'<polyline points="{line}" fill="none" stroke="{mix(col, C["text"], .25)}" stroke-width="1.8" '
                     f'stroke-linejoin="round" stroke-linecap="round"/>')
        s.append("</svg>")
        out = [f'<div class="abs scope" style="left:{v.x}px;top:{v.y}px;width:{w}px;height:{h}px">', "".join(s),
               text_div(pl + 6, 4, 114, 14, "TAKT 1", "sonora.small.bold", C["muted"], "left"),
               text_div((pl + pr_) / 2 + 6, 4, 114, 14, "TAKT 2", "sonora.small.bold", C["muted"], "left")]
        info = DIVISIONS[P["division"]] + (" · Ping-Pong" if dual else "")
        out.append(text_div(pr_ - 200, 4, 104, 14, info, "sonora.small", C["muted"], "right"))
        if dual:
            out.append(text_div(6, pt, 12, 14, "L", "sonora.small.bold", C["accent"], "left"))
            out.append(text_div(6, midy + 3, 12, 14, "R", "sonora.small.bold", C["accent2"], "left"))
        for ch, col in ((0, C["accent"]), (1, C["accent2"])):
            out.append(f'<div class="abs" style="left:{w - 86}px;top:{6 + ch * 6}px;width:78px;height:4px;'
                       f'border-radius:2px;background:{C["grid"]}"><div class="abs gg{ch}" style="left:0;top:0;'
                       f'height:4px;border-radius:2px;background:{col}"></div></div>')
        out.append(f'<div class="abs playhead" style="left:{pl - 4}px;top:{pt - 2}px;height:{pb - pt + 4}px;'
                   f'--travel:{pw}px"><i></i></div>')
        out.append("</div>")
        return "".join(out)

    # -- drowner -----------------------------------------------------------------------------------
    def v_SonoraWaterView(self, v):
        w, h = v.w, v.h
        d = self.demo["depth"]
        cutoff = self.demo["cutoff"]
        mid_y = h * 0.55
        copies = 1 + round(d * 4)
        gid = uid("wv")
        s = [f'<svg class="abs" style="left:0;top:0" width="{w}" height="{h}">',
             f'<defs><linearGradient id="{gid}s" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#1e1a2c"/>'
             f'<stop offset="1" stop-color="{C["inset"]}"/></linearGradient>'
             f'<linearGradient id="{gid}w" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="rgba(124,82,240,.47)"/>'
             f'<stop offset=".35" stop-color="rgba(76,48,170,.59)"/><stop offset="1" stop-color="rgba(22,16,54,.84)"/>'
             f'</linearGradient><clipPath id="{gid}c"><rect x="1" y="1" width="{w - 2}" height="{h - 2}" rx="7.5"/>'
             f'</clipPath></defs>',
             f'<rect x="0" y="0" width="{w}" height="{h}" rx="8" fill="url(#{gid}s)"/>',
             f'<g clip-path="url(#{gid}c)">']
        t = 0.8
        for k in range(copies - 1, -1, -1):
            off = (k - (copies - 1) * 0.5) * 2.2 * d
            pts = []
            for i in range(121):
                x = w * i / 120
                u = i / 120 * 6 * math.pi + t * 1.6
                env = 0.55 + 0.45 * math.sin(i / 120 * 2 * math.pi * 1.5 + t * 0.7)
                lo = math.sin(u) * 0.6
                hi = (math.sin(u * 4.3) * 0.28 + math.sin(u * 9.1) * 0.16) * (1 - d)
                pts.append((x, mid_y + off - (lo + hi) * env * h * 0.2 * (1 - 0.35 * d)))
            alpha = 230 if (k == 0 and copies == 1) else int(150 / copies + 25)
            s.append(f'<polyline points="{" ".join(f"{x:.1f},{y:.1f}" for x, y in pts)}" fill="none" '
                     f'stroke="{rgba(C["accent2"], alpha)}" stroke-width="{1.8 + d * 1.8:.2f}" stroke-linejoin="round" '
                     f'stroke-linecap="round" class="voice"/>')
        level = h - h * (0.10 + 0.82 * d)
        amp = 2 + 5 * d

        def wave_path(wl, a, y_off, sign2):
            pts = []
            total = w + 2 * wl
            steps = int(total / 4)
            for i in range(steps + 1):
                x = -wl + total * i / steps
                y = (level + y_off + math.sin(x / wl * 2 * math.pi) * a
                     + math.sin(x / (wl * 0.5) * 2 * math.pi * sign2) * a * 0.35)
                pts.append((x, y))
            body = " ".join(f"L{x:.1f},{y:.1f}" for x, y in pts)
            return f"M{-wl},{h} {body} L{w + wl},{h} Z", " ".join(f"{x:.1f},{y:.1f}" for x, y in pts)

        back, _ = wave_path(150, amp * 0.8, -3, 1)
        front, crest = wave_path(110, amp, 0, -1)
        s.append(f'<g class="wave back" style="--wl:150px"><path d="{back}" fill="{rgba(C["accent"], 55)}"/></g>')
        s.append(f'<g class="wave front" style="--wl:-110px"><path d="{front}" fill="url(#{gid}w)"/>'
                 f'<polyline points="{crest}" fill="none" stroke="rgba(214,196,255,.59)" stroke-width="1.4"/></g>')
        span = h - level
        for i in range(round(3 + d * 12)):
            seed = i * 12.9898
            fx = abs(math.sin(seed) * 43758.5453) % 1
            speed = 10 + 14 * (abs(math.sin(seed * 1.7) * 9631.17) % 1)
            rr = 1.5 + 2.2 * (abs(math.sin(seed * 3.1) * 1234.5) % 1)
            x = 12 + (w - 24) * fx
            dur = span / speed
            s.append(f'<circle class="bub" cx="{x:.1f}" cy="{h:.1f}" r="{rr:.2f}" fill="none" '
                     f'stroke="rgba(220,205,255,.47)" style="--rise:-{span:.1f}px;animation-duration:{dur:.2f}s;'
                     f'animation-delay:-{(i * 37 / speed) % dur:.2f}s"/>')
        s.append("</g>")
        s.append(f'<rect x=".5" y=".5" width="{w - 1}" height="{h - 1}" rx="8" fill="none" stroke="{C["stroke"]}"/>')
        s.append("</svg>")
        cut = ((fmt(cutoff / 1000, 0 if cutoff >= 10000 else 1) + " kHz") if cutoff >= 1000
               else fmt(cutoff, 0) + " Hz")
        return (f'<div class="abs water" style="left:{v.x}px;top:{v.y}px;width:{w}px;height:{h}px">' + "".join(s)
                + text_div(10, 8, 100, 18, f"TIEFE {fmt(d * 100, 0)} %", "sonora.small.bold", C["text"], "center",
                           f"background:{rgba(C['bg'], 190)};border-radius:9px;")
                + text_div(w - 150, 8, 140, 18, "TIEFPASS " + cut, "sonora.small.bold", C["accent2"], "center",
                           f"background:{rgba(C['bg'], 190)};border-radius:9px;")
                + "</div>")


CSS = """
*{box-sizing:border-box}
html,body{margin:0;height:100%;overflow:hidden;background:transparent}
html.standalone,html.standalone body{background:#e9e7ed}
@media (prefers-color-scheme:dark){html.standalone,html.standalone body{background:#09080c}}
body{font-family:%FONT%;-webkit-font-smoothing:antialiased;text-rendering:geometricPrecision}
.stage{position:absolute;inset:0;display:flex;align-items:center;justify-content:center}
.plugin{position:relative;flex:none;transform-origin:center center;border-radius:12px;overflow:hidden;
  box-shadow:0 12px 28px rgba(20,10,40,.28),0 2px 6px rgba(20,10,40,.22)}
html.standalone .plugin{box-shadow:0 24px 60px rgba(20,10,40,.35),0 2px 8px rgba(20,10,40,.25)}
.abs,.t,.root,.hdr,.card,.grp,.knob,.tog,.seg,.menu,.led,.dot{position:absolute}
.t{display:flex;align-items:center;white-space:nowrap;overflow:hidden;line-height:1;letter-spacing:.1px}
.root{background:%BG%}
.root::before{content:"";position:absolute;inset:0;pointer-events:none;
  background:radial-gradient(circle at 18% 0,rgba(139,92,246,.135) 0,rgba(139,92,246,0) 55%)}
.hdr{background:linear-gradient(#1a1624,%PANEL%);box-shadow:inset 0 -1px 0 %STROKE%}
.card{border-radius:10px;background:linear-gradient(%CARDTOP%,%CARD%);box-shadow:inset 0 0 0 1px %STROKE%}
[data-dim]{opacity:.3}
.led,.dot{display:block;border-radius:50%}
.dot{left:13px;top:14px;width:6px;height:6px}
.knob svg,.abs svg{display:block;overflow:visible}
.bar{border-radius:3px;background:%INSET%;box-shadow:inset 0 0 0 1px %STROKE%;overflow:hidden}
.bar .lit{position:absolute;left:0;right:0;bottom:0;border-radius:2.5px;background-position:bottom!important}
.bar .seg3{position:absolute;inset:0;background:repeating-linear-gradient(to top,transparent 0 2px,rgba(15,13,20,.43) 2px 3px)}
.bar .hold{position:absolute;left:0;right:0;height:2px}
.playhead{width:8px;animation:play 4s linear infinite}
.playhead::before{content:"";position:absolute;inset:2px 0;border-radius:4px;background:rgba(241,238,248,.12)}
.playhead i{position:absolute;left:3.25px;top:0;bottom:0;width:1.5px;background:#f1eef8;box-shadow:0 0 6px rgba(241,238,248,.6)}
@keyframes play{from{transform:translateX(0)}to{transform:translateX(var(--travel))}}
.gg0,.gg1{width:78px;animation:gate .25s steps(1,end) infinite alternate}
.gg1{animation-direction:alternate-reverse}
@keyframes gate{from{width:78px}to{width:12px}}
.wave{animation:wave 6s linear infinite}
.wave.back{animation-duration:9s}
@keyframes wave{from{transform:translateX(0)}to{transform:translateX(var(--wl))}}
.bub{animation:bub 8s linear infinite}
@keyframes bub{from{transform:translateY(0);opacity:.9}to{transform:translateY(var(--rise));opacity:0}}
.voice{animation:voice 3s ease-in-out infinite alternate}
@keyframes voice{from{transform:translateX(-6px)}to{transform:translateX(6px)}}
.corr{animation:corr 2.4s ease-in-out infinite alternate}
@keyframes corr{from{transform:translateX(-5px)}to{transform:translateX(4px)}}
@keyframes grA{from{transform:scaleX(1)}to{transform:scaleX(.72)}}
@keyframes grB{from{transform:scaleX(.8)}to{transform:scaleX(1)}}
.clipled{animation:clip 1.1s ease-in-out infinite alternate}
@keyframes clip{from{opacity:1}to{opacity:.55}}
@media (prefers-reduced-motion:reduce){*{animation:none!important}}
"""

SCRIPT = """
(function(){
  if (window.self === window.top) document.documentElement.classList.add('standalone');
  var p = document.querySelector('.plugin'), W = %W%, H = %H%;
  function fit(){
    var pad = window.self === window.top ? Math.max(8, Math.min(24, Math.min(innerWidth, innerHeight) * 0.03)) : 32;
    var s = Math.min((innerWidth - 2 * pad) / W, (innerHeight - 2 * pad) / H, 1.25);
    p.style.transform = 'scale(' + Math.max(0.2, s) + ')';
  }
  addEventListener('resize', fit); fit();
})();
"""


def write_html(layout, renderer, path, title):
    body = renderer.render(layout)
    css = (CSS.replace("%FONT%", CSS_FONT).replace("%BG%", C["bg"]).replace("%PANEL%", C["panel"])
           .replace("%STROKE%", C["stroke"]).replace("%CARD%", C["card"]).replace("%CARDTOP%", C["cardTop"])
           .replace("%INSET%", C["inset"]))
    doc = f"""<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{esc(title)}</title>
<!-- GENERATED by plugin/src/ui/tools/gen_ui.py from the same layout as the .uidesc - do not edit by hand. -->
<style>{css}</style>
</head>
<body>
<div class="stage"><div class="plugin" style="width:{layout.w}px;height:{layout.h}px">{body}</div></div>
<script>{SCRIPT.replace('%W%', str(layout.w)).replace('%H%', str(layout.h))}</script>
</body>
</html>
"""
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(doc)


# ------------------------------------------------------------------------------------------------
MASTER_COMMENT = """  SONORA Master v0.2 editor (960 x 640). GENERATED by plugin/src/ui/tools/gen_ui.py - edit there.

  Custom views come from sonora::ui::createMasterView (plugin/src/ui/sonora_ui.h). Without the
  delegate forwarding every view falls back to its "class" (plain VSTGUI controls).

  Named containers: header, section-klang, section-eq, section-vocal, section-beat, section-meters,
  section-loudness, section-dynamics, and per-control groups grp-* (label + knob + value).
  Mode rules (sonora-active-when on param 0 Bus-Modus: 0 = Vocal, 1 = Beat, 2 = Master) dim a group
  to 30 % and disable its mouse input automatically (SonoraCard polls the parameter getter):
    Vocal-only  "0=0,2": grp-nasal, grp-resonance, grp-presence, grp-deesser
    Beat-only   "0=1,2": grp-vocalfocus, grp-bass, grp-beathighs, grp-punch, grp-bassmono, grp-width,
                         grp-analyze
    Master-only "0=2"  : grp-loudness, grp-ceiling, grp-clipper (limiter/clipper meters dim too)
    always             : Stärke, grp-input, grp-output, grp-glue, grp-saturation, EQ faders
"""
SHUTTER_COMMENT = """  SONORA Shutter v0.2 editor (640 x 420). GENERATED by plugin/src/ui/tools/gen_ui.py - edit there.
  Custom views come from sonora::ui::createShutterView (plugin/src/ui/sonora_ui.h).
  Named containers: header, section-scope, section-timing, section-sound, grp-*.
  Rules (param 1 Tempo-Sync): grp-division active when Sync = on ("1=1"), grp-rate when Sync = off ("1=0").
"""
DROWNER_COMMENT = """  SONORA Drowner v0.2 editor (640 x 420). GENERATED by plugin/src/ui/tools/gen_ui.py - edit there.
  Custom views come from sonora::ui::createDrownerView (plugin/src/ui/sonora_ui.h).
  Named containers: header, section-depth, section-sound, grp-*.
"""


def main():
    jobs = [
        ("sonora.uidesc", "ui-preview.html", "SONORA Master – Vorschau", master_layout(), MASTER_PARAMS,
         MASTER_COMMENT, dict(
             autoEq=[1.2, -0.8, -1.5, 0.6, 1.0],
             analysis=dict(beatRms=-14.2, vocalRms=-17.8, crest=11.6, beat=[-18, -21, -24, -27, -33],
                           vocal=[-35, -24, -20, -22, -30]),
             levels=dict(level=[-7.5, -8.2, -2.4, -2.9], hold=[-4.1, -4.6, -1.0, -1.1]),
             gr=dict(glue=2.6, lim=1.4, clip=0.35), lufs=dict(i=-8.9, m=-8.1, s=-8.6, tp=-1.0), corr=0.62)),
        ("shutter.uidesc", "shutter-preview.html", "SONORA Shutter – Vorschau", shutter_layout(), SHUTTER_PARAMS,
         SHUTTER_COMMENT, dict(shutter=dict(sync=True, division=4, rate=4.0, depth=0.85, mix=1.0, smooth=2.0,
                                            width=0.55, swing=0.20, offset=0.0, form=0, stereo=1))),
        ("drowner.uidesc", "drowner-preview.html", "SONORA Drowner – Vorschau", drowner_layout(), DROWNER_PARAMS,
         DROWNER_COMMENT, dict(depth=0.65, cutoff=21000 * (350 / 21000) ** 0.65)),
    ]
    for uidesc, preview, title, layout, params, comment, demo in jobs:
        w = UidescWriter(params)
        w.write(layout, os.path.join(RES, uidesc), comment)
        missing = set(params) - w.used_tags
        if missing:
            raise SystemExit(f"{uidesc}: parameters without control: {sorted(missing)}")
        write_html(layout, Html(params, demo), os.path.join(RES, preview), title)
        print("wrote", uidesc, preview)


if __name__ == "__main__":
    main()
