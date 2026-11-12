#!/usr/bin/env python3
"""Resume results/hw/bench.csv: media, desviacion y CV por configuracion,
razon packed/padded y ajuste del modelo lineal ns(n) = n * t_transfer.

Uso:  python3 analysis/summarize.py            (desde la raiz del repo)
Salida: tablas en pantalla y results/hw/summary.csv
"""
import csv
import statistics as st
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "results/hw/bench.csv"
OUT = ROOT / "results/hw/summary.csv"

GROUPS = {"pcore": 4.7, "ecore_same": 3.4, "ecore_cross": 3.4}  # GHz maxima
LAYOUTS = [("packed", 8), ("padded", 64), ("padded", 128), ("local", 8), ("shared", 8)]


def load(path):
    data, bad = {}, set()
    with open(path, newline="") as f:
        rd = csv.DictReader(f)
        for r in rd:
            k = (r["group"], r["rmw"], int(r["threads"]), r["layout"], int(r["stride"]))
            data.setdefault(k, []).append(float(r["ns_per_inc"]))
            if r["count_ok"] != "1":
                bad.add(k)
    return data, bad


def main():
    if not SRC.exists():
        sys.exit(f"no existe {SRC}; corre benchmarks/run_bench.sh")
    data, bad = load(SRC)
    mean = lambda *k: st.mean(data[k])

    rows = []
    for k, v in sorted(data.items()):
        m, sd = st.mean(v), st.stdev(v) if len(v) > 1 else 0.0
        rows.append(dict(group=k[0], rmw=k[1], threads=k[2], layout=k[3], stride=k[4],
                         reps=len(v), mean_ns=round(m, 4), sd_ns=round(sd, 4),
                         cv_pct=round(100 * sd / m, 2), count_ok=int(k not in bad)))
    with open(OUT, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)

    for g in GROUPS:
        for rmw in ("atomic", "volatile"):
            print(f"\n== {g} / {rmw}: ns por incremento (media ± sd)")
            print("n  " + "".join(f"{l}{s if l == 'padded' else '':<14}"[:16].ljust(16) for l, s in LAYOUTS))
            for n in range(1, 5):
                cells = []
                for l, s in LAYOUTS:
                    v = data.get((g, rmw, n, l, s))
                    cells.append(f"{st.mean(v):7.3f}±{st.stdev(v):5.2f}".ljust(16) if v else "-".ljust(16))
                print(f"{n}  " + "".join(cells))

    print("\n== packed / padded64 (veces mas lento)")
    for g in GROUPS:
        for rmw in ("atomic", "volatile"):
            r = [mean(g, rmw, n, "packed", 8) / mean(g, rmw, n, "padded", 64) for n in range(1, 5)]
            print(f"{g:12} {rmw:8} " + "  ".join(f"n={n}: {x:5.2f}" for n, x in enumerate(r, 1)))

    print("\n== Modelo lineal (atomic, packed): ns(n) = n * t_transfer, n = 2..4")
    for g, ghz in GROUPS.items():
        lock = mean(g, "atomic", 1, "padded", 64)
        ys = [(n, mean(g, "atomic", n, "packed", 8)) for n in (2, 3, 4)]
        t = sum(n * y for n, y in ys) / sum(n * n for n, _ in ys)
        err = "  ".join(f"n={n}: {100 * (n * t - y) / y:+.0f}%" for n, y in ys)
        print(f"{g:12} lock add {lock:.2f} ns ({lock * ghz:.1f} ciclos) | "
              f"transferencia {t:.2f} ns ({t * ghz:.1f} ciclos) | error {err}")

    if bad:
        print("\nSuma incorrecta (actualizaciones perdidas):",
              ", ".join(f"{k[0]}/{k[1]}/n={k[2]}/{k[3]}" for k in sorted(bad)))
    print(f"\nResumen escrito en {OUT.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
