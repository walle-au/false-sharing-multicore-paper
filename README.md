# False Sharing in Multicore Processors

An experimental study of **false sharing**: what happens to a parallel program
when threads on different cores write to *independent* variables that live in
the *same cache line*.

Caches stay coherent per line (64 B on x86-64), not per variable. When two cores
keep writing to the same line, the coherence protocol (MESI) keeps invalidating
it, and the line bounces between cores. No data is actually shared, yet the
program stops scaling.

## Research question

> How does the placement of variables across cache lines affect the performance
> and scalability of a parallel program?

## Approach

- **Platform:** Intel Core i7-1260P (Alder Lake, hybrid P/E cores), Linux, with
  each thread pinned to its own core.
- **Microbenchmark:** per-thread counters in three layouts:
  1. packed together (8 B per counter, same line)
  2. padded/aligned to their own cache line (64 B per counter)
  3. per-thread local accumulation
- **Analysis:** execution time and scaling vs. thread count, hardware counters,
  and `perf c2c` to locate contended lines through HITM events.
- **Trade-off:** memory, cache and TLB cost of padding vs. coherence traffic
  saved.

## Repository layout

| Path | Contents |
|---|---|
| `paper/` | IEEE Transactions paper (`paper.tex`, `references.bib`) |
| `benchmarks/` | C microbenchmarks (real hardware) |
| `sim/` | Trace-driven MESI coherence simulator in C (`make`, `./sweep.sh`) |
| `results/` | Raw measurements, `perf c2c` reports and simulator CSVs (`results/sim/`) |
| `docs/` | Course guidelines and notes |

## Build & run

```bash
# Paper (needs IEEEtran, e.g. texlive-publishers)
cd paper && pdflatex paper && bibtex paper && pdflatex paper && pdflatex paper

# Simulator
cd sim && make && ./sweep.sh            # CSVs in results/sim/

# Benchmark (one thread per physical P-core)
cd benchmarks && make
./fs_bench -l packed -n 4 --cpus 0,2,4,6 -r 10
./run_bench.sh                          # full sweep -> results/hw/bench.csv

# Contended cache lines (perf c2c, one recording per layout)
sudo ./record_c2c.sh                    # reports in results/c2c/
```

## Authors

Walter Alfaro · Allan Arrieta

---

*Developed as part of EL-4314 Computer Architecture I, Instituto Tecnológico de
Costa Rica, 2026.*
