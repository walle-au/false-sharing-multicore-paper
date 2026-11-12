#!/usr/bin/env bash
# Graba perf c2c para cada layout (requiere sudo o perf_event_paranoid <= 0).
# Uso: sudo ./record_c2c.sh                 (P-cores 0,2,4,6)
#      sudo ./record_c2c.sh ecore_same      (E-cores 8-11, L2 compartida)
#      sudo ./record_c2c.sh ecore_cross     (E-cores 8,12,9,13)
# Cada grupo se guarda en ../results/c2c/<grupo>/
set -euo pipefail
cd "$(dirname "$0")"
make -s fs_bench
GROUP=${1:-pcore}
case "$GROUP" in
  pcore)       CPUS=0,2,4,6 ;;
  ecore_same)  CPUS=8,9,10,11 ;;
  ecore_cross) CPUS=8,12,9,13 ;;
  *) echo "grupo desconocido: $GROUP (pcore | ecore_same | ecore_cross)"; exit 1 ;;
esac
OUT=../results/c2c/$GROUP; mkdir -p "$OUT"
echo "grupo $GROUP, CPUs $CPUS -> $OUT"
ITERS=${ITERS:-50000000}

for layout in packed padded local shared; do
  for rmw in atomic volatile; do
    tag=${layout}_${rmw}
    perf c2c record -o "$OUT/$tag.data" -- \
      ./fs_bench -l $layout -n 4 -c "$CPUS" --rmw $rmw -i $ITERS -r 1 > "$OUT/$tag.bench.txt"
    perf c2c report -i "$OUT/$tag.data" --stdio --full-symbols > "$OUT/$tag.report.txt" 2>&1 || true
    # el resumen de HITM queda al inicio del reporte
    grep -E "Load Local HITM|Load Remote HITM|Shared Data Cache Line|Load Local Peer|Store Operations" \
      "$OUT/$tag.report.txt" | head -8 | sed "s/^/[$tag] /"
  done
done
# los .data son grandes y no se suben al repo; los reportes de texto si
chown -R "${SUDO_USER:-$USER}:${SUDO_USER:-$USER}" ../results/c2c 2>/dev/null || true
echo "Reportes en $OUT/*.report.txt"
