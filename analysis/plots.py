#!/usr/bin/env python3
"""Genera las figuras del paper a partir de results/hw/bench.csv y del simulador.

Uso:  python3 analysis/plots.py      (desde la raiz del repo; requiere matplotlib
      y sim/mesi_sim compilado: cd sim && make)
Salida: paper/figures/*.pdf (para LaTeX) y *.png (vista previa)
"""
import csv
import statistics as st
import subprocess
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "results/hw/bench.csv"
SIM = ROOT / "sim/mesi_sim"
OUT = ROOT / "paper/figures"

# Paleta categorica validada (orden fijo, nunca ciclado) y tinta de texto.
BLUE, ORANGE, AQUA, YELLOW = "#2a78d6", "#eb6834", "#1baf7a", "#eda100"
INK, INK2, GRID = "#0b0b0b", "#52514e", "#e4e3df"

# Cada layout conserva su color, marcador y linea en todas las figuras.
SERIES = [  # (layout, stride, etiqueta, color, marcador, linea)
    ("packed", 8, "packed (8 B)", BLUE, "o", "-"),
    ("shared", 8, "shared (true sharing)", ORANGE, "s", "--"),
    ("padded", 64, "padded (64 B)", AQUA, "^", "-"),
    ("local", 8, "local accumulation", YELLOW, "D", ":"),
]
GROUPS = [  # (clave, titulo, GHz, latencia HITM calibrada en ciclos)
    ("pcore", "P-cores (via L3)", 4.7, 47),
    ("ecore_same", "E-cores, same cluster (shared L2)", 3.4, 42),
    ("ecore_cross", "E-cores, two clusters (via L3)", 3.4, 50),
]
THREADS = [1, 2, 3, 4]

plt.rcParams.update({
    "font.family": "serif", "font.size": 8, "axes.titlesize": 8,
    "axes.labelsize": 8, "xtick.labelsize": 7, "ytick.labelsize": 7,
    "legend.fontsize": 7, "axes.edgecolor": INK2, "axes.labelcolor": INK,
    "xtick.color": INK2, "ytick.color": INK2, "text.color": INK,
    "axes.spines.top": False, "axes.spines.right": False,
    "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.6,
    "lines.linewidth": 1.4, "lines.markersize": 4.5,
    "pdf.fonttype": 42, "savefig.bbox": "tight", "savefig.pad_inches": 0.02,
})


def load():
    d = {}
    with open(SRC, newline="") as f:
        for r in csv.DictReader(f):
            k = (r["group"], r["rmw"], int(r["threads"]), r["layout"], int(r["stride"]))
            d.setdefault(k, []).append(float(r["ns_per_inc"]))
    return d


def sim_ns(n, ghz, hitm):
    """Prediccion del simulador (layout packed, atomic) en ns por incremento."""
    out = subprocess.run(
        [str(SIM), "-n", str(n), "-l", "packed", "--lat-hitm", str(hitm),
         "--lat-atomic", "18", "--ghz", str(ghz), "--csv"],
        check=True, capture_output=True, text=True).stdout.strip()
    return float(out.split(",")[-1])


def save(fig, name):
    OUT.mkdir(parents=True, exist_ok=True)
    fig.savefig(OUT / f"{name}.pdf")
    fig.savefig(OUT / f"{name}.png", dpi=200)
    plt.close(fig)
    print("  ", (OUT / f"{name}.pdf").relative_to(ROOT))


def fig_scaling(d):
    """Tiempo por incremento vs hilos, atomic, un panel por grupo de nucleos."""
    fig, axes = plt.subplots(1, 3, figsize=(7.16, 2.35), sharey=True)
    for ax, (g, title, ghz, hitm) in zip(axes, GROUPS):
        for lay, s, label, color, mk, ls in SERIES:
            m = [st.mean(d[(g, "atomic", n, lay, s)]) for n in THREADS]
            e = [st.stdev(d[(g, "atomic", n, lay, s)]) for n in THREADS]
            ax.errorbar(THREADS, m, yerr=e, color=color, marker=mk, ls=ls,
                        capsize=2, elinewidth=0.8, markeredgecolor="white",
                        markeredgewidth=0.6, label=label, zorder=3)
        model = [sim_ns(n, ghz, hitm) for n in THREADS]
        ax.plot(THREADS, model, color=INK2, ls=(0, (1, 1.5)), lw=1.0, marker="x",
                markersize=4, label="simulator (packed)", zorder=4)
        ax.set_yscale("log")
        ax.set_xticks(THREADS)
        ax.set_title(title, color=INK)
        ax.set_xlabel("Threads")
        ax.set_ylim(0.1, 100)
    axes[0].set_ylabel("ns per increment (log)")
    h, l = axes[0].get_legend_handles_labels()
    h, l = h[1:] + h[:1], l[1:] + l[:1]          # el simulador al final de la leyenda
    fig.legend(h, l, loc="upper center", ncol=5, frameon=False,
               bbox_to_anchor=(0.5, 1.07), handlelength=2.4)
    save(fig, "fig_scaling")


def fig_slowdown(d):
    """Penalizacion packed/padded con 4 hilos: atomic vs volatile por grupo."""
    fig, ax = plt.subplots(figsize=(3.5, 2.1))
    w, gap = 0.36, 0.04
    labels = ["P-cores", "E-cores\nsame cluster", "E-cores\ntwo clusters"]
    for i, (rmw, color, hatch) in enumerate([("atomic", BLUE, None), ("volatile", ORANGE, "////")]):
        xs, ys = [], []
        for j, (g, *_rest) in enumerate(GROUPS):
            r = st.mean(d[(g, rmw, 4, "packed", 8)]) / st.mean(d[(g, rmw, 4, "padded", 64)])
            xs.append(j + (i - 0.5) * (w + gap))
            ys.append(r)
        bars = ax.bar(xs, ys, width=w, color=color, edgecolor="white", linewidth=0.8,
                      hatch=hatch, label=f"{rmw} increment", zorder=3)
        for b, y in zip(bars, ys):
            ax.text(b.get_x() + b.get_width() / 2, y + 0.25, f"{y:.1f}×",
                    ha="center", va="bottom", fontsize=7, color=INK)
    ax.axhline(1, color=INK2, lw=0.8, zorder=2)
    ax.set_xticks(range(3))
    ax.set_xticklabels(labels)
    ax.set_ylabel("packed / padded time")
    ax.set_ylim(0, 13.5)
    ax.grid(axis="x", visible=False)
    ax.legend(frameon=False, loc="upper left")
    save(fig, "fig_slowdown")


def main():
    if not SIM.exists():
        raise SystemExit("compila el simulador primero: cd sim && make")
    d = load()
    print("Figuras:")
    fig_scaling(d)
    fig_slowdown(d)


if __name__ == "__main__":
    main()
