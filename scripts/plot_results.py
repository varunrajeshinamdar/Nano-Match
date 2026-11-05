#!/usr/bin/env python3
"""Draws the README charts from the newest citable results, in a light and a dark version.

Values come only from files in results/. With --png, also writes PNG copies to the given directory, which is
useful for checking the layout.
"""

import argparse
import json
import pathlib

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker
from matplotlib.path import Path
from matplotlib.patches import PathPatch

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUTPUT = ROOT / "docs/plots"

THEMES = {
    "light": {
        "surface": "#ffffff",
        "primary": "#0b0b0b",
        "secondary": "#52514e",
        "grid": "#e4e3df",
        "series": ["#2a78d6", "#eb6834", "#1baf7a"],
    },
    "dark": {
        "surface": "#0d1117",
        "primary": "#f0efec",
        "secondary": "#c3c2b7",
        "grid": "#30302e",
        "series": ["#3987e5", "#d95926", "#199e70"],
    },
}

BOOK_NAMES = {"baseline": "Baseline", "baseline_pool": "Baseline, pool allocator", "fast": "Optimized"}
PERCENTILES = (("p50", "p50_ns"), ("p90", "p90_ns"), ("p99", "p99_ns"), ("p99.9", "p99_9_ns"), ("p99.99", "p99_99_ns"))


def newest(suite, label_prefix=None, turbo="0"):
    match = None
    for path in sorted((ROOT / "results").glob("*.json")):
        data = json.loads(path.read_text())
        if not data.get("citable") or data.get("suite", "book") != suite or data["machine"]["no_turbo"] != turbo:
            continue
        if label_prefix and data["label"].split("_turbo")[0] != label_prefix:
            continue
        match = data
    if match is None:
        raise SystemExit(f"no citable {suite} results")
    return match


def style_axes(axes, theme):
    axes.set_facecolor("none")
    for side in ("top", "right", "left"):
        axes.spines[side].set_visible(False)
    axes.spines["bottom"].set_color(theme["grid"])
    axes.tick_params(colors=theme["secondary"], length=0, labelsize=10)
    axes.yaxis.grid(True, color=theme["grid"], linewidth=1, zorder=0)
    axes.set_axisbelow(True)


def add_titles(figure, theme, title, subtitle):
    figure.text(0.02, 0.95, title, color=theme["primary"], fontsize=13, fontweight="bold", va="top")
    figure.text(0.02, 0.885, subtitle, color=theme["secondary"], fontsize=10, va="top")


def save(figure, name, mode, png_dir):
    OUTPUT.mkdir(parents=True, exist_ok=True)
    figure.savefig(OUTPUT / f"{name}_{mode}.svg", transparent=True, metadata={"Date": None})
    if png_dir:
        figure.savefig(pathlib.Path(png_dir) / f"{name}_{mode}.png", dpi=110, facecolor=THEMES[mode]["surface"])
    plt.close(figure)


def plot_latency(data, mode, png_dir):
    theme = THEMES[mode]
    entries = {entry["book"]: entry for entry in data["entries"] if entry["profile"] == "realistic"}
    figure, axes = plt.subplots(figsize=(8, 4.4))
    figure.subplots_adjust(left=0.1, right=0.72, top=0.77, bottom=0.11)
    style_axes(axes, theme)

    positions = list(range(len(PERCENTILES)))
    ends = []
    for slot, book in enumerate(("baseline", "baseline_pool", "fast")):
        latency = entries[book]["summary"]["latency"]
        values = [latency[key]["median"] for _, key in PERCENTILES]
        color = theme["series"][slot]
        axes.plot(positions, values, color=color, linewidth=2, solid_capstyle="round", solid_joinstyle="round",
                  label=BOOK_NAMES[book], zorder=3, marker="o", markersize=8,
                  markeredgecolor=theme["surface"], markeredgewidth=2)
        ends.append((values[-1], BOOK_NAMES[book]))

    axes.set_yscale("log")
    axes.set_xticks(positions, [name for name, _ in PERCENTILES])
    axes.set_xlim(-0.2, len(PERCENTILES) - 0.8)
    axes.set_ylabel("Nanoseconds, log scale", color=theme["secondary"], fontsize=10)
    axes.yaxis.set_major_formatter(matplotlib.ticker.FuncFormatter(lambda value, _: f"{value:,.0f}"))
    axes.yaxis.set_minor_formatter(matplotlib.ticker.NullFormatter())
    axes.tick_params(which="minor", length=0)

    # End labels name the lines directly, nudged apart if two would collide.
    ends.sort()
    previous = None
    for value, name in ends:
        target = value if previous is None else max(value, previous * 1.75)
        axes.annotate(f"{name}\n{value:,.0f} ns", xy=(len(PERCENTILES) - 1, target), xytext=(10, 0),
                      textcoords="offset points", annotation_clip=False, color=theme["primary"], fontsize=9.5, va="center")
        previous = target

    legend = axes.legend(loc="upper left", frameon=False, fontsize=9.5, handlelength=1.6)
    for text in legend.get_texts():
        text.set_color(theme["secondary"])

    add_titles(figure, theme, "Latency per command",
               "realistic workload, 25M commands, median of runs, turbo on, includes timer cost")
    save(figure, "latency", mode, png_dir)


def rounded_column(x, width, height, radius_x, radius_y):
    left, right = x, x + width
    points = [
        (left, 0), (left, height - radius_y), (left, height), (left + radius_x, height),
        (right - radius_x, height), (right, height), (right, height - radius_y), (right, 0), (left, 0),
    ]
    codes = [Path.MOVETO, Path.LINETO, Path.CURVE3, Path.CURVE3, Path.LINETO, Path.CURVE3, Path.CURVE3, Path.LINETO, Path.CLOSEPOLY]
    return Path(points, codes)


def plot_ring(data, mode, png_dir):
    theme = THEMES[mode]
    entries = [
        entry for entry in data["entries"]
        if entry["kind"] == "throughput" and entry["payload_bytes"] == 32 and entry["placement"] == "cross_core"
    ]
    figure, axes = plt.subplots(figsize=(8, 4.2))
    figure.subplots_adjust(left=0.1, right=0.97, top=0.74, bottom=0.14)
    style_axes(axes, theme)

    alignments = sorted({entry["index_alignment"] for entry in entries})
    series = ((False, "Uncached indices"), (True, "Cached indices"))
    top = max(entry["summary"]["items_per_second"]["median"] for entry in entries) / 1e6
    axes.set_ylim(0, top * 1.22)
    axes.set_xlim(-0.6, len(alignments) - 0.4)
    # Thin columns with a small gap, sized in pixels so that they don't depend on how many groups there are.
    axes_width = axes.get_window_extent().width
    group_width = axes_width / len(alignments)
    width = 24 / group_width
    gap = 1 / group_width

    box = axes.get_window_extent()
    radius_x = 4 / (box.width / (len(alignments)))
    radius_y = 4 / (box.height / (top * 1.22))

    for slot, (cached, name) in enumerate(series):
        color = theme["series"][slot]
        for index, alignment in enumerate(alignments):
            entry = next(item for item in entries if item["index_alignment"] == alignment and item["cached_indices"] == cached)
            value = entry["summary"]["items_per_second"]["median"] / 1e6
            x = index - width - gap if slot == 0 else index + gap
            patch = PathPatch(rounded_column(x, width, value, radius_x, radius_y), facecolor=color, edgecolor="none",
                              zorder=3, label=name if index == 0 else None)
            axes.add_patch(patch)
            # Each label is anchored to the outer edge of its column, so that neighbours can't collide.
            label_x, align = (x + width, "right") if slot == 0 else (x, "left")
            axes.text(label_x, value + top * 0.02, f"{value:.1f}M", ha=align, va="bottom", color=theme["primary"], fontsize=10)

    axes.set_xticks(range(len(alignments)), [f"{alignment} bytes" for alignment in alignments])
    axes.yaxis.set_major_locator(matplotlib.ticker.MaxNLocator(nbins=4, integer=True))
    axes.set_xlabel("Index alignment", color=theme["secondary"], fontsize=10)
    axes.set_ylabel("Million items per second", color=theme["secondary"], fontsize=10)
    legend = axes.legend(loc="lower left", bbox_to_anchor=(0, 1.0), borderaxespad=0, frameon=False, fontsize=9.5, ncols=2,
                         handlelength=1.2)
    for text in legend.get_texts():
        text.set_color(theme["secondary"])

    add_titles(figure, theme, "SPSC ring throughput by layout",
               "32-byte items between two physical cores, 4,096-item ring, median of runs, turbo on")
    save(figure, "ring", mode, png_dir)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--png", default=None, help="also write PNG copies to this directory")
    args = parser.parse_args()
    books = newest("book", "book")
    ring = newest("ring")
    for mode in THEMES:
        plot_latency(books, mode, args.png)
        plot_ring(ring, mode, args.png)
    print(f"wrote charts to {OUTPUT.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
