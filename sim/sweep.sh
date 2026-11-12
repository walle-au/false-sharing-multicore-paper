#!/usr/bin/env bash
# Barridos del simulador MESI. Genera CSV en ../results/sim/.
set -euo pipefail
cd "$(dirname "$0")"
OUT=../results/sim
mkdir -p "$OUT"
SIM=./mesi_sim
ITERS=${ITERS:-100000}

# 1) Escalabilidad: tiempo vs nucleos para cada layout (linea 64 B)
f=$OUT/scaling.csv; $SIM --header > "$f"
for rmw in atomic loadstore; do
  for layout in packed padded local shared; do
    for n in 1 2 3 4 6 8 12 16; do
      $SIM -n $n -l $layout -r $rmw -i $ITERS --csv >> "$f"
    done
  done
done

# 2) Tamano de linea vs stride (argumento de Torrellas: linea mas grande -> mas false sharing)
f=$OUT/line_vs_stride.csv; $SIM --header > "$f"
for line in 32 64 128 256; do
  for stride in 8 16 32 64 128 256; do
    $SIM -n 4 -l padded -s $stride -L $line -i $ITERS --csv >> "$f"
  done
done

# 3) Prefetch de linea adyacente: padding 64 vs 128 B (recomendacion de Intel)
f=$OUT/prefetch.csv; $SIM --header > "$f"
for pf in "" "-p"; do
  for stride in 64 128; do
    for n in 2 4 8; do
      $SIM -n $n -l padded -s $stride $pf -i $ITERS --csv >> "$f"
    done
  done
done

# 4) Sensibilidad a la latencia HITM (el modelo depende de una estimacion)
f=$OUT/hitm_sensitivity.csv; $SIM --header > "$f"
for h in 30 50 70 100 150; do
  for layout in packed padded; do
    $SIM -n 4 -l $layout --lat-hitm $h -i $ITERS --csv >> "$f"
  done
done

echo "CSV generados en $OUT:"; ls -1 "$OUT"
