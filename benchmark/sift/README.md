# Sifting benchmark

The harness and scripts behind the sifting experiments: Sylvan's
`sylvan_sift` against CUDD's `Cudd_ReduceHeap(dd, CUDD_REORDER_SIFT, 0)`, on
the same forests, built gate for gate from the same starting variable order.
Everything needed to rerun them is in this directory, except CUDD itself and
the input circuit, which the instructions below fetch at pinned revisions.

| file | what it is |
|---|---|
| `sylvan_bench.c` | the Sylvan driver: build a forest, sift it once, print one `RESULT {...}` JSON line |
| `cudd_bench.c` | the CUDD driver: the same forest, `Cudd_ReduceHeap` with `CUDD_REORDER_SIFT` |
| `blif.c`, `blif.h` | the BLIF reader both drivers share, and the shared starting-order permutation |
| `fetch-inputs.sh` | installs `C880.blif` (checksummed); `--families` documents the other input families |
| `scripts/run-*.sh` | one script per experiment; each appends JSONL rows |
| `scripts/summarize.py` | Markdown and CSV tables from those rows |
| `scripts/build-matrix.sh`, `scripts/manifest.py` | every build configuration the paper's headlines need, and `manifest.json` |
| `scripts/run-headlines.sh`, `scripts/headlines.py` | the headline experiments on those builds, their tables and the reference check (see `EXPERIMENTS.md` at the repository root) |

## Building

### CUDD, pinned

CUDD 3.0.0 as imported in commit `f54f533` of
[cuddorg/cudd](https://github.com/cuddorg/cudd), built in place at the same
optimisation level as Sylvan's Release build:

```sh
git clone https://github.com/cuddorg/cudd.git
cd cudd
git checkout f54f533303640afd5dbe47a05ebeabb3066f2a25
./configure CFLAGS="-O3 -DNDEBUG" CXXFLAGS="-O3 -DNDEBUG"
make -j
```

Clone it on the machine that builds it. A checkout made on Windows has CRLF
line endings, which break `configure`; stripping them touches every file, and
autotools then wants to regenerate `configure` from a newer `configure.ac`.

`cudd_bench` needs `cuddInt.h` and the `config.h` that `configure` writes, so
it is built against this source tree, not against an installed CUDD.

### Sylvan, one build tree per nodes table

`-DSYLVAN_BUILD_SIFT_BENCH=ON` adds the harness; `-DCUDD_DIR` points it at the
CUDD tree above, and without it only `sylvan_bench` is built (configure says
which file it missed). From the root of this repository:

```sh
for b in list trie llcht llchst; do
  cmake -S . -B build-$b -DCMAKE_BUILD_TYPE=Release \
    -DSYLVAN_NODES_TABLE=$b -DSYLVAN_BUILD_SIFT_BENCH=ON \
    -DCUDD_DIR=/path/to/cudd \
    -DBUILD_TESTING=OFF -DSYLVAN_BUILD_EXAMPLES=OFF -DSYLVAN_BUILD_CPP=OFF
  cmake --build build-$b -j
done
```

The binaries are `build-$b/benchmark/sift/sylvan_bench` and
`build-$b/benchmark/sift/cudd_bench`. `cudd_bench` does not depend on Sylvan,
so every build's copy is the same program; use any one of them. Lace is
fetched by CMake (v2.4.0); to build offline, add
`-DFETCHCONTENT_SOURCE_DIR_LACE=/path/to/lace`.

### Inputs

```sh
benchmark/sift/fetch-inputs.sh inputs                 # from GitHub
benchmark/sift/fetch-inputs.sh --cudd-src /path/to/cudd inputs   # or offline
```

Either way the file must come out as `C880.blif` with MD5
`82323a515652b47cdbff16cb2e0bc27e` (18428 bytes, LF line endings), and an
existing file is never overwritten. `fetch-inputs.sh --families` gives the
sources, checksums and commands for the McNet, relprod and QBF families, and
records the EPFL arithmetic circuits as infeasible: neither tool can build
`adder` or `bar` under their file order with an 8 GB budget, so there is
nothing to sift.

## The drivers

Both drivers take the same case options and print one `RESULT {...}` line.

```
--mode bryant --pairs N     x0.y0 + ... + x(N-1).y(N-1) under the split order
--mode blif --file F.blif   every .outputs signal of the circuit
       --var-order file|reverse|random  --order-seed S
--mode queens --n N         the N-queens board, row-major
--case NAME                 a label copied into the row
--verify N                  evaluate every output on N random vectors and count
                            its models, before and after; report sound/valid
                            and eval_hash
--ops                       time a pairwise and/xor/model-count phase before
                            and after the sift
--order                     print the final variable order
--no-sift                   build and measure only
--growth G (1.2)            maximum growth while moving one variable
--memory BYTES (8 GiB)
```

`sylvan_bench` adds `--workers N`, `--threshold N` (default 0, the library
default), `--max-swaps N`, `--max-vars N`, `--time-limit S` (0 is unlimited
for all three), `--full-legs`, which walks every leg to the end of the order
instead of stopping at the interaction bound, and `--auto`, which installs the
configuration with `sylvan_gc_set_auto_sift` and calls `sylvan_gc()` where the
sift would run, so that the growth-recommendation path is what is measured.
Under `--auto` the row's `sifts` field says whether a sift happened, and the
`sylvan_sift_result` fields are 0 because `sylvan_gc()` does not return them.
`--stats` resets the library's counters before the build and before the
sift, and prints `sylvan_stats_report` after each on stderr, between
`SIFT-STATS-BEGIN build|sift` and `SIFT-STATS-END ...` lines; the counters
exist only in a `-DSYLVAN_STATS=ON` build, so time such a build separately.

`cudd_bench` disables automatic reordering, sets `maxGrowth` to `--growth`
and raises `siftMaxVar` and `siftMaxSwap` to `INT_MAX`, so that it is as
unlimited as Sylvan's defaults; `--cudd-defaults` keeps CUDD's own caps. Its
`swaps` is `dd->ddTotalNumberSwapping`, which counts adjacent swaps on search
and return legs alike, as Sylvan's does.

`nodes_before` and `nodes_after` are the shared node count of the output
roots on both sides (`mtbdd_shared_node_count`, `Cudd_SharingSize`); they agree
exactly on every unsifted build, which is what makes them comparable.
Sylvan's `sift_initial`/`sift_final` count the whole reachable forest and are
slightly larger. `eval_hash` is an FNV hash of the evaluation of every output
on the `--verify` vectors (drawn from `srand(--seed)`, 12345 by default) and
is computed identically by both drivers, so equal hashes show that both built
the same functions.

## Running

Every script takes the same options and appends to a JSONL file; everything
each command printed on stderr, and the load average before each case, go to
the same name with `.log` appended.

```
--sylvan-build NAME=DIR   a build tree from above; repeat for several backends
--cudd-bench PATH         cudd_bench (omit it to run Sylvan only)
--inputs DIR              the directory fetch-inputs.sh filled
--out FILE                where rows are appended (default ./EXPERIMENT.jsonl)
--reps N  --workers 1,4,8  --timeout SECONDS  --cases "C880-file queens10"
--sylvan-args "..."  --cudd-args "..."   passed to every run of that driver
```

Case names describe the forest completely: `C880-file`, `C880-rev`,
`C880-rndK` (random order, seed K) for any `STEM.blif` in `--inputs`,
`bryantN` and `queensN`. Each row carries the harness's own fields plus
`exp`, `cfg` (the variant), `rep`, `build`, `build_backend`, `git` (the commit
of the Sylvan source tree the build came from), `git_dirty`, `host`,
`load1`/`load5`/`load15`, `t_start`/`t_end` (UTC) and `wall_ms`, the driver's
arguments (`argv`) and the transparent huge page modes (`thp`, `thp_defrag`).
What a run prints on stderr for its row is kept too: `SIFT-PHASES {json}`
lines as `sift_phases`, `--stats` reports as `stats_report`, and `perf stat
-x,` counters as `perf` when `SB_WRAP` runs the drivers under perf. A build
from `build-matrix.sh` adds its configuration name and knob settings
(`config_name`, `knobs`, ...), and `SB_ROW_EXTRA` adds any JSON members to
every row. A run that fails or times out leaves a row with `"failed":1` and
its exit code.

With `S=benchmark/sift/scripts` and the builds above:

```sh
B="--sylvan-build list=build-list --sylvan-build trie=build-trie"
C="--cudd-bench build-trie/benchmark/sift/cudd_bench --inputs inputs"

$S/run-quality.sh   $B $C --out results/quality.jsonl     # Q1, add --thresholds 0,32 --cudd-defaults
$S/run-timing.sh    $B $C --out results/timing.jsonl      # Q2, P1/2/4/8/16, 7 reps
$S/run-soundness.sh $B $C --out results/soundness.jsonl   # Q3, 5000 vectors, ops phase
$S/run-queens.sh    $B $C --out results/queens.jsonl      # queens10 and queens12
$S/run-seeds.sh     $B $C --out results/seeds.jsonl       # 30 random orders of C880
$S/run-backends.sh --sylvan-build list=build-list --sylvan-build trie=build-trie \
    --sylvan-build llcht=build-llcht --sylvan-build llchst=build-llchst \
    --inputs inputs --out results/backends.jsonl

$S/summarize.py all results/*.jsonl                       # Markdown on stdout
$S/summarize.py --csv results/csv timing results/timing.jsonl
```

| script | question | defaults |
|---|---|---|
| `run-quality.sh` | final size against CUDD; same order at P1 and P4 | bryant6..14 and the four C880 orders, `--workers 1,4`, 1 rep |
| `run-timing.sh` | sift time and its scaling | four C880 orders and queens11, `--workers 1,2,4,8,16`, 7 reps |
| `run-soundness.sh` | soundness, cross-tool hashes, downstream ops | four C880 orders, 5000 vectors, P1, 5 reps, plus a no-sift control |
| `run-queens.sh` | a good natural order | queens10 and queens12, P1, 3 reps |
| `run-seeds.sh` | the distribution over random orders | C880-rnd1..30, 2000 vectors, P1, 1 rep |
| `run-backends.sh` | nodes tables: equality, then timing | C880-rnd2 and C880-file, `--workers 1,8`, 5 reps |
| `run-ablation.sh` | one runtime setting against another (`--variant NAME=ARGS`), interleaved | four C880 orders, P1, 5 reps |

`--help` on any script prints its own description and the common options.
The long ones are queens12, which builds for several minutes per run on
either tool, and the seed sweep, where some orders take CUDD minutes to sift.

## Reference values

Final shared node counts at `nodes_threshold` 0, growth 1.2, P1, with CUDD
unlimited, from the evaluation that produced these scripts (sift-eval-20260816,
Sylvan 8358b10, trie). Sizes are deterministic: the evaluation found the same
final size and order at P1 and P4 on every case, and `run-backends.sh` checks
that every nodes table agrees. The four C880 rows and queens10 were re-checked
on the tree that added this directory, on the trie and list backends.

| case | initial | Sylvan | CUDD | ratio |
|---|---:|---:|---:|---:|
| C880-file | 346660 | 5234 | 7064 | 0.741 |
| C880-rev | 470046 | 36533 | 36533 | 1.000 |
| C880-rnd1 | 844442 | 31088 | 23789 | 1.307 |
| C880-rnd2 | 1066147 | 26972 | 26973 | 1.000 |
| queens10 | 25945 | 25330 | 25331 | 1.000 |
| queens12 | 435170 | 427043 | 427043 | 1.000 |
| bryantN | 2^(N+1)-1 | 2N+1 | 2N+1 | 1.000 |

With `--verify 64` (and the default `--seed`), C880-file's `eval_hash` is
`b63d2a3eade6548e` on both drivers.

Swap counts are not reference values. Sylvan's legs stop at the interaction
bound unless `--full-legs` is given; the bound only skips positions that
cannot win, so the final sizes above hold either way, while C880-file takes
3770 swaps with the bound and 5624 with `--full-legs` (CUDD: 3456).

## Measurement protocol

- **Like-for-like settings.** `nodes_threshold` 0 on Sylvan (CUDD has no
  such threshold, and 0 is also the library default), `maxGrowth` 1.2 on both,
  CUDD's `siftMaxVar`/`siftMaxSwap` raised to `INT_MAX` (Sylvan's limits are
  0, unlimited); `run-quality.sh --cudd-defaults` checks that the raised caps
  change nothing. Both tools sift the output cone only: every intermediate
  is released before the sift, and Sylvan's variable-set helper is dropped
  around it.
- **Same flags.** Both libraries and both drivers at `-O3 -DNDEBUG`
  (CMake Release, and CUDD's `configure` line above).
- **One process per run**, so no run inherits another's tables. `sift_s` is
  `CLOCK_MONOTONIC` around the sift call alone; the build is timed separately
  (`build_s`). Both include the garbage collection their sift starts with.
- **Interleaved variants.** Within each repetition every variant runs once,
  in a fixed order, before any runs again (A,B,A,B), so drift over a batch
  falls on all variants alike.
- **Medians with min–max** over the repetitions, never means; the number of
  repetitions is in every table.
- **Load average logged** in every row, and before every case in the `.log`
  together with the busiest processes, so that a suspicious number can be
  checked against how busy the machine was. Run on an otherwise idle machine.
- **Soundness on every row that asks for it.** `--verify` checks evaluation
  and model counts before and after every sift, `valid` checks the structure,
  and `summarize.py` flags any row where either fails, any refused swap, and
  any case whose evaluation hashes disagree across tools or backends.
