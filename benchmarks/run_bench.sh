#!/usr/bin/env bash
# Barrido del benchmark en hardware real. Genera ../results/hw/bench.csv
# Uso: ./run_bench.sh            (todo)
#      REPS=10 ITERS=50000000 ./run_bench.sh
set -euo pipefail
cd "$(dirname "$0")"
make -s fs_bench
OUT=../results/hw; mkdir -p "$OUT"
F=$OUT/bench.csv
REPS=${REPS:-10}
ITERS=${ITERS:-50000000}

# Grupos de CPUs del i7-1260P:
#   P-cores, un hilo por nucleo fisico (L2 privada, la linea viaja por la L3)
#   E-cores de un mismo cluster (comparten L2)
#   E-cores de clusters distintos (la linea viaja por la L3)
declare -A GROUP=( [pcore]="0,2,4,6" [ecore_same]="8,9,10,11" [ecore_cross]="8,12,9,13" )

# Cabecera con la columna de grupo. Cada fila se escribe directo en el CSV,
# asi una corrida interrumpida deja datos parciales validos y una nueva corrida
# empieza de cero (no acumula filas viejas).
echo "group,$(./fs_bench --header)" > "$F"
for g in pcore ecore_same ecore_cross; do
  for rmw in atomic volatile; do
    for n in 1 2 3 4; do
      for layout in packed padded local shared; do
        ./fs_bench -l $layout -n $n -c "${GROUP[$g]}" --rmw $rmw -i $ITERS -r $REPS --csv \
          | sed "s/^/$g,/" >> "$F"
      done
      # padding a 128 B (prefetch de linea adyacente)
      ./fs_bench -l padded -s 128 -n $n -c "${GROUP[$g]}" --rmw $rmw -i $ITERS -r $REPS --csv \
        | sed "s/^/$g,/" >> "$F"
    done
  done
  echo "listo: $g"
done

# Metadatos de la corrida
rm -f "$F.tmp"   # restos de la version anterior del script
{
  date -Iseconds; uname -r; lscpu | grep -E "Model name|MHz"
  echo "governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)"
  echo "no_turbo: $(cat /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null || echo ?)"
  gcc --version | head -1
} > "$OUT/bench_meta.txt"
echo "CSV en $F"
