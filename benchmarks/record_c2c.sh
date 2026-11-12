#!/usr/bin/env bash
# Graba perf c2c para cada layout (requiere sudo o perf_event_paranoid <= 0).
# Uso: sudo ./record_c2c.sh
set -euo pipefail
cd "$(dirname "$0")"
make -s fs_bench
OUT=../results/c2c; mkdir -p "$OUT"
CPUS=${CPUS:-0,2,4,6}
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
chown -R "${SUDO_USER:-$USER}" "$OUT" 2>/dev/null || true
echo "Reportes en $OUT/*.report.txt"
