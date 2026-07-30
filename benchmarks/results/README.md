# Committed benchmark baselines

`reference_i7-13650HX.{txt,csv}` is the run quoted throughout `docs/PERFORMANCE.md`.
It is committed so that a later run can be diffed against it without having to
find and rebuild an old commit.

Reproduce with:

```powershell
powershell -ExecutionPolicy Bypass -File tools/build.ps1 -Config RelWithDebInfo -Target voxl_bench
./build/RelWithDebInfo/bin/voxl_bench.exe --quiet --csv benchmarks/results/mine.csv
```

then

```powershell
git diff --no-index benchmarks/results/reference_i7-13650HX.csv benchmarks/results/mine.csv
```

The CSV is long-format - one row per (case, metric) - specifically so that this
diff shows only the numbers that moved, instead of every column shifting because
one case gained a counter.

**Do not compare across machines.** The reference was taken on the hardware
described in `docs/PERFORMANCE.md` §2, on a machine carrying 10-15% background
load; absolute times are meaningful only relative to another run on the same
box. The `derived` rows (LOD speedups, job-system scaling) and the `counter`
rows (quad counts, payload sizes, heap bytes) ARE portable - the counters are
deterministic functions of the seed and should be byte-identical anywhere.
