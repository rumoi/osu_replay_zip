# README bar charts, one fixed layout so the plots line up: python3 charts/make_charts.py writes <mode>_<what>_<theme>.svg here.
import math, os, sys

MODES = {
    "osu":   ([100, 27.33, 29.77, 33.42], [62.1, 27.8, 23.2, 20.4], [3.56, 12.07, 7.16, 4.25]),
    "taiko": ([100, 44.27, 57.88, 74.65], [54.9, 3.1, 1.6, 0.9], [0.85, 2.90, 1.40, 0.56]),
    "mania": ([100, 45.59, 56.16, 65.79], [64.4, 3.4, 1.9, 1.1], [1.48, 3.14, 1.65, 0.70]),
    "fruit": ([100, 44.64, 47.52, 52.82], [45.4, 6.5, 4.3, 2.9], [2.32, 5.75, 3.35, 1.87]),
    "total": ([100, 29.27, 32.32, 36.52], [61.2, 19.1, 15.5, 13.5], [2.81, 8.95, 5.23, 3.02]),
}
NAMES = [".osr", "SIZE", "BALANCED", "SPEED"]
THEMES = {"dark": "#f0f6fc", "light": "#1f2328"}
AXIS = "#8c959f"
BAR = "#3b6ea8"
FONT = '"trebuchet ms", verdana, arial, sans-serif'
W, H = 600, 200
PLOT_X0, PLOT_X1 = 140, 590
AXIS_Y, PLOT_Y1 = 58, 196

def ticks(vmax):
    for step in (0.1, 0.2, 0.25, 0.5, 1, 2, 2.5, 5, 10, 20, 25, 50):
        if vmax / step <= 10:
            return step, math.ceil(vmax / step - 1e-9) * step

def svg(title, vals, unit, text, fixed_max=None):
    step, vmax = (10, fixed_max) if fixed_max else ticks(max(vals))
    sx = lambda v: PLOT_X0 + (PLOT_X1 - PLOT_X0) * v / vmax
    band = (PLOT_Y1 - AXIS_Y) / len(vals)
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" font-family=\'{FONT}\'>',
           f'<text x="{(PLOT_X0 + PLOT_X1) / 2}" y="20" font-size="16" fill="{text}" text-anchor="middle">{title}</text>',
           f'<path d="M{PLOT_X0} {AXIS_Y}H{PLOT_X1}M{PLOT_X0} {AXIS_Y}V{PLOT_Y1}" stroke="{AXIS}" stroke-width="2" fill="none"/>']
    n = round(vmax / step)
    for i in range(n + 1):
        v = i * step
        x = sx(v)
        out.append(f'<path d="M{x:.1f} {AXIS_Y}v-5" stroke="{AXIS}" stroke-width="2"/>')
        out.append(f'<text x="{x:.1f}" y="{AXIS_Y - 10}" font-size="12" fill="{text}" text-anchor="middle">{v:g}</text>')
    for i, (name, v) in enumerate(zip(NAMES, vals)):
        y = AXIS_Y + band * (i + 0.5)
        label = name if i == 0 else f"{name} {v:.1f}{unit}"
        out.append(f'<path d="M{PLOT_X0} {y:.1f}h-5" stroke="{AXIS}" stroke-width="2"/>')
        out.append(f'<text x="{PLOT_X0 - 10}" y="{y:.1f}" font-size="13" fill="{text}" text-anchor="end" dominant-baseline="central">{label}</text>')
        out.append(f'<rect x="{PLOT_X0 + 1}" y="{y - band * 0.3:.1f}" width="{sx(v) - PLOT_X0 - 1:.1f}" height="{band * 0.6:.1f}" fill="{BAR}"/>')
    return "\n".join(out + ["</svg>", ""])

def main(out_dir):
    os.makedirs(out_dir, exist_ok=True)
    for mode, (size, enc, dec) in MODES.items():
        for theme, text in THEMES.items():
            charts = {"size": svg("Size, % of .osr", size, "%", text, 100),
                      "encode": svg("Encode, ms per replay", enc, " ms", text),
                      "decode": svg("Decode, ms per replay", dec, " ms", text)}
            for what, body in charts.items():
                open(os.path.join(out_dir, f"{mode}_{what}_{theme}.svg"), "w", encoding="utf-8", newline="\n").write(body)

main(sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__)))
