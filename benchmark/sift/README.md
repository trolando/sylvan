# Sifting benchmark

The harness and scripts behind the sifting experiments: Sylvan's
`sylvan_sift` against CUDD's `Cudd_ReduceHeap(dd, CUDD_REORDER_SIFT, 0)`, on
the same forests, built gate for gate from the same starting variable order.
Everything needed to rerun them is in this directory, except CUDD itself and
the input circuits, which the instructions below fetch at pinned revisions.

| file | what it is |
|---|---|
| `sylvan_bench.c` | the Sylvan driver: build a forest, sift it once, print one `RESULT {...}` JSON line |
| `cudd_bench.c` | the CUDD driver: the same forest, `Cudd_ReduceHeap` with `CUDD_REORDER_SIFT` |
| `blif.c`, `blif.h` | the BLIF reader both drivers share, and the shared starting-order permutation |
| `fetch-inputs.sh` | installs `C880.blif`, or with `--all` every circuit of `cases.tsv` (all checksummed); `--families` documents the other input families |
| `cases.tsv` | the case table: every case of the main grid, with its family, size class, quick flag and timeout (see [The case table](#the-case-table)) |
| `scripts/run-*.sh` | one script per experiment; each appends JSONL rows |
| `scripts/summarize.py` | Markdown and CSV tables from those rows |
| `scripts/build-matrix.sh`, `scripts/manifest.py` | every build configuration the paper's headlines need, and `manifest.json` |
| `scripts/run-headlines.sh`, `scripts/headlines.py` | the headline experiments on those builds, their tables and the reference check (see `EXPERIMENTS.md` at the repository root) |
| `cudd-swap-trace.patch` | a per-swap trace for CUDD 3.0.0, which has no swap hook (see [Swap traces](#swap-traces)) |
| `scripts/compare-traces.py` | aligns a Sylvan and a CUDD swap trace and explains where they differ |
| `sift-cudd-rules.patch` | a diagnostic Sylvan patch that switches to CUDD's rules one at a time, for the attribution in [Swap traces](#swap-traces) |

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
benchmark/sift/fetch-inputs.sh inputs                 # C880.blif, from GitHub
benchmark/sift/fetch-inputs.sh --cudd-src /path/to/cudd inputs   # or offline
benchmark/sift/fetch-inputs.sh --all inputs           # every circuit of cases.tsv
benchmark/sift/fetch-inputs.sh --check --all inputs   # verify an installed tree
benchmark/sift/fetch-inputs.sh --list                 # each file's source URL and MD5s
```

`C880.blif` must come out with MD5 `82323a515652b47cdbff16cb2e0bc27e`
(18428 bytes, LF line endings), as before; every other file has its own
recorded MD5 (`--list`), and an existing file is never overwritten.
`fetch-inputs.sh --families` gives the sources, checksums and commands for
the McNet, relprod and QBF families, and records the EPFL arithmetic circuits
as infeasible: neither tool can build `adder` or `bar` under their file order
with an 8 GB budget, so there is nothing to sift.

`--all` installs 60 files, every circuit the [case table](#the-case-table)
uses plus the ones it screened out, so that each can still be run by name:

| set | installed as | source, pinned | licence | reference |
|---|---|---|---|---|
| `c880` | `C880.blif` | CUDD 3.0.0, `nanotrav/C880.blif`, cuddorg/cudd `f54f533` | CUDD's BSD-style licence (University of Colorado) | ISCAS'85 |
| `iscas85` | `iscas85/C*.blif`: C432 C499 C1355 C1908 C2670 C3540 C5315 C6288 C7552 | `mcnc/Combinational/blif/` of [ispras/hdl-benchmarks](https://github.com/ispras/hdl-benchmarks) `10c3fb5` | none stated on the circuits, public benchmarks since 1985 (the repository is Apache-2.0) | F. Brglez, H. Fujiwara, ISCAS 1985 |
| `lgsynth91` | `lgsynth91/*.blif`, 36 circuits | the same `mcnc/Combinational/blif/`, and `lgsynth91/blif/` for the sequential ones | none stated; MCNC gave them out by anonymous FTP and tape for benchmarking (`lgsynth91/LGSynth91.readme` there) | S. Yang, *Logic Synthesis and Optimization Benchmarks User Guide, Version 3.0*, MCNC, 1991 |
| `iscas89` | `iscas89/s*.blif`, 10 circuits | `lgsynth91/blif/` of the same | as `lgsynth91` | F. Brglez, D. Bryan, K. Kozminski, ISCAS 1989 |
| `epfl` | `epfl/*.blif`: arbiter mem_ctrl priority voter | `random_control/` of [lsils/benchmarks](https://github.com/lsils/benchmarks) `0060e15` | MIT | L. Amaru, P.-E. Gaillardon, G. De Micheli, IWLS 2015 |

The ispras files are Petr Fiser's collection
(<https://ddd.fit.cvut.cz/www/prj/Benchmarks/>) with LF line endings: all 219
files of `MCNC/Combinational/blif` in `MCNC.7z` (MD5
`cbb76c767f9b6050bddf0359a1ce67ae`) and all 116 of `LGSynth91/blif` in
`LGSynth91.7z` (`91529cd8fcb1dbe0d94f917fdc1f7fb3`), both as downloaded on
2026-10-09, equal their ispras copies once CRs are stripped, so `--src
fiser=DIR` (the two archives extracted into DIR) installs the same bytes.
`--src ispras=DIR` and `--src lsils=DIR` take local checkouts instead.

**What `blif.c` reads.** `.model .inputs .outputs .latch .names .end`,
comments and continuations; a latch output becomes a free variable and only
`.outputs` are built. So a sequential circuit's next-state functions would not
be built, and some files use what it does not read. Files marked `comb` in
`--list` are therefore converted, by an awk program in `fetch-inputs.sh`,
into their combinational part: `.latch IN OUT` becomes `.inputs OUT` and
`.outputs IN` at the same place (the variable order stays the one `blif.c`
gives the original: inputs, then latch outputs), and `.wire_load_slope`, an
`.exdc` don't-care network with everything after it, and `.outputs` names
nothing drives are dropped. What the screening found it could not read
as published:

| what | where | handled |
|---|---|---|
| `.wire_load_slope` (unsupported directive) | every ISCAS'89 file of LGSynth'91 | dropped by the conversion |
| `.exdc` (unsupported; it also redefines every output) | 28 two-level MCNC circuits: alu3 apla b3 b4 b7 b10 b11 bca bcb bcc bcd bw dekoder dk17 dk27 dk48 ex1010 exep exp exps inc mark1 misex3c pdc spla t2 t4 wim | dropped by the conversion: the care network is kept |
| outputs nothing drives | s13207.1 (g1193 g1195 g1197 g1201 g1205), s15850.1 (g1957) | dropped by the conversion |
| a gate input nothing drives (`96`) | mult32b | not readable; left out |
| a cube of 10 literals in a 9-input `.names` (line 283) | CUDD's `nanotrav/adj49.blif` | not readable; left out |

`.subckt`, `.gate`, `.mlatch` and the KISS/PLA formats are not read either;
none of the files above uses them. A `.names` over more than 255 signals would
be cut short silently (`tok[256]` in `blif_read`); the widest here has 188
inputs (k2).

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
                            and after the sift; its checksum, the double
                            sum of count(f.g) + 2 count(f^g), is the same
                            number on both drivers
--order                     print the final variable order
--trace FILE                write one line per swap of the sift to FILE
                            (see Swap traces; cudd_bench needs a CUDD built
                            with cudd-swap-trace.patch)
--no-sift                   build and measure only
--growth G (1.2)            maximum growth while moving one variable
--memory BYTES (8 GiB)
```

`sylvan_bench` adds `--workers N`, `--threshold N` (default 0, the library
default), `--max-swaps N`, `--max-vars N`, `--time-limit S` (0 is unlimited
for all three), `--full-legs`, which walks every leg to the end of the order
instead of stopping at the interaction bound, `--group-pairs`, which sifts
every pair of identities (2k, 2k+1) as one block, and `--auto`, which
installs the configuration with `sylvan_gc_set_auto_sift` and calls
`sylvan_gc()` where the sift would run, so that the growth-recommendation
path is what is measured.
Under `--auto` the row's `sifts` field says whether a sift happened, and the
`sylvan_sift_result` fields are 0 because `sylvan_gc()` does not return them.
`--recommend-factor F` sets the growth factor that recommendation asks for
(`sylvan_sift_set_recommend_factor`; the library default is 2.0, 0 disables
it).
`--stats` resets the library's counters before the build and before the
sift, and prints `sylvan_stats_report` after each on stderr, between
`SIFT-STATS-BEGIN build|sift` and `SIFT-STATS-END ...` lines; the counters
exist only in a `-DSYLVAN_STATS=ON` build, so time such a build separately.
`--no-batch` calls `sylvan_reorder_set_batched(0)`, so every swap removes and
re-indexes its nodes one at a time, and the row's `batched` field records it.
Every row's `split_pairs` is `sylvan_sift_split_pairs()` after the sift: the
pairs (2k, 2k+1) that are not together, even identity first, in the final
order.

`--memstat`, on both drivers, reads the resident set (`VmRSS` of
`/proc/self/status`) and its transparent huge pages (`AnonHugePages` of
`/proc/self/smaps_rollup`) after the build and after the sift, and the peak
(`VmHWM`) at the end, and appends `rss_build_kb`, `rss_sift_kb`, `hwm_kb`,
`thp_build_kb` and `thp_sift_kb` to the row. Linux only: elsewhere they are 0.

`cudd_bench` disables automatic reordering, sets `maxGrowth` to `--growth`
and raises `siftMaxVar` and `siftMaxSwap` to `INT_MAX`, so that it is as
unlimited as Sylvan's defaults; `--cudd-defaults` keeps CUDD's own caps, and
`--max-swaps N` and `--max-vars N` set either cap on top of that (0 is
unlimited, as on Sylvan's side). The row's `unlimited` is 1 only when both
caps end up raised. Its `swaps` is `dd->ddTotalNumberSwapping`, which counts
adjacent swaps on search and return legs alike, as Sylvan's does.

`nodes_before` and `nodes_after` are the shared node count of the output
roots on both sides (`mtbdd_shared_node_count`, `Cudd_SharingSize`); they agree
exactly on every unsifted build, which is what makes them comparable.
Sylvan's `sift_initial`/`sift_final` count the whole reachable forest and are
slightly larger. `eval_hash` is an FNV hash of the evaluation of every output
on the `--verify` vectors (drawn from `srand(--seed)`, 12345 by default) and
is computed identically by both drivers, so equal hashes show that both built
the same functions.

The model counts are each driver's check of itself, not a comparison between
the tools. Sylvan counts every output in a double (`bdd_sat_count_double`), and
exactly as well (`bdd_sat_count_u64`) where the double is under 2^63, so that
the exact count fits in 64 bits, which on a circuit of more than 64 inputs it
rarely does; CUDD counts in a double (`Cudd_CountMinterm`). An output's count
after the sift must equal its count before where both are exact, and agree to
a relative 1e-9 otherwise: doubles are summed in another order once the
variables have moved and may round differently, by at most about one ulp per
variable. A count past the range of a double (2^1024) is infinite in Sylvan
and `DD_PLUS_INF_VAL` in CUDD (infinity, or 10e301 where CUDD's configure did
not find IEEE 754), CUDD gives -1 for any count over more than 2044
variables, and each of these matches only itself. `sound` is 1 when the
evaluation and every count agree, `valid` when the forest passes the
structural check, and whatever fails is named on stderr. `satsum` (Sylvan)
and `mintsum` (CUDD) list the first eight outputs' counts after the sift,
`null` for any of these.

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
`C880-rndK` (random order, seed K) for any circuit `STEM` of the case table
or `STEM.blif` in `--inputs`, `bryantN` and `queensN`; `scripts/cases.sh`
lists them from the table by family, size and quick flag, as in `--cases
"$($S/cases.sh --quick)"` (see [The case table](#the-case-table)).
Each row carries the harness's own fields plus
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

## The case table

`cases.tsv` is the main grid: 141 cases, 48 circuits from the four
circuit sets under one to four starting orders each, queens8 to queens11 and
bryant6 to bryant12. One row per case, tab-separated, with a header line:

| column | |
|---|---|
| `name` | the case name, as everywhere else (`C3540-rnd1`) |
| `mode` | `blif`, `queens` or `bryant` |
| `file` | the circuit, relative to `--inputs` (`fetch-inputs.sh --all`); `-` for queens and bryant, whose size is the number in the name |
| `var_order`, `seed` | `file`, `reverse` or `random` with its `--order-seed` (queens and bryant: `file`, their one order, seed 0) |
| `family` | `iscas85`, `iscas89`, `lgsynth91`, `epfl`, `queens`, `bryant` |
| `size_class` | Sylvan's sift time at P1: `small` under 1 s (122 cases), `medium` 1 to 30 s (17), `large` over 30 s (2: dalu-file, s9234.1-file) |
| `quick` | 1 for nine cases of under 5 s per run on either tool, at least one per family |
| `timeout_s` | a per-run cap: four times the slower tool's P1 run plus 30 s, at least 60 s |
| `notes` | how the file was converted, a duplicate forest, a much slower CUDD, and the number of inputs where it is over 64, so that the model counts of `--verify` are doubles |

`scripts/cases.sh` reads it; the run scripts take its output as `--cases`:

```sh
$S/cases.sh --check --inputs inputs                 # well-formed, every circuit installed
$S/cases.sh --quick                                 # the nine quick cases
$S/cases.sh --family iscas85,iscas89 --size medium,large
$S/cases.sh --order random --table                  # the matching rows
$S/cases.sh --args C3540-rnd1 --inputs inputs       # the driver arguments, one per line
$S/run-timing.sh $B $C --cases "$($S/cases.sh --size small,medium)"
```

A sourced `cases.sh` also gives `sb_cases_select`, `sb_case_row`,
`sb_case_field NAME COLUMN` and `sb_case_timeout NAME` to a runner.

**How the cases were chosen.** Every circuit of the four sets (271 files:
217 of the MCNC directory's 219, all but C17 and its copy of C880; the 40
sequential ones only in the LGSynth'91 directory; the ten EPFL random/control
circuits; CUDD's C880 and three small CUDD examples, ham01, closest and
rcn25) was built and sifted from its file order, its reversed order and the
random order of seed 1, on 2026-10-09 under WSL on the 20-core development
machine, by `sylvan_bench` (list table, P1, threshold 0, growth 1.2,
`--memory` 8 GiB) and by `cudd_bench`, one run at a time while other jobs
held two to four cores. A Sylvan run got 60 s; one that needed more got a
600 s build and a 300 s sift. A CUDD run got 300 s (900 s where Sylvan had
needed more than 60 s) and a 9 GiB address-space limit, since its
`--memory` is only a hint. A case is *feasible* when both tools
build it within 600 s and 8 GiB and Sylvan's sift ends within 300 s. A
circuit is in the table when at least two of its three orders are feasible,
when some order starts with at least 5000 nodes, and when its forests are not
those of a circuit already in (same variables and the same nodes before and
after on every order: C1355 is C499's forest, apex1 is k2's, apex6 is x3's,
s400 is s382's, and so on, 15 pairs). An order of such a circuit is in when
its Sylvan run took at most 60 s at P1, which is what keeps the grid within a
night, and s13207.1-file is left out because it builds for 52 s to sift for
0.9 s.

**What it costs.** The grid this table is for runs every case on the list,
trie, llcht and llchst tables at 1, 2, 4, 8 and 16 workers, 5 repetitions
each, and CUDD at P1, 5 repetitions: 105 runs per case, 14805 in all.
Costing every run at its P1 wall time (conservative: more workers build and
sift faster) plus 0.25 s for the harness gives **9.9 h**: 8.6 h of Sylvan
runs and 1.3 h of CUDD runs. Three cases are 4.4 h of it: s9234.1-file
(1.9 h; CUDD sifts it for 254 s), dalu-file (1.4 h; CUDD 216 s) and queens11
(1.1 h, nearly all build). Trimmed:

| selection | cases | hours |
|---|---:|---:|
| everything | 141 | 9.9 |
| without s9234.1-file, queens11 and C3540-rnd1 (keeps dalu-file as the large case) | 138 | 6.1 |
| `--size small,medium` (no large case) | 139 | 6.6 |
| `--size small,medium` without queens11 | 138 | 5.5 |
| `--quick` | 9 | 0.3 |

Small cases are cheap but say little on their own: a sift has a floor that
grows with the nodes table rather than with the forest, because it begins with
a garbage collection, which passes over the whole table. On bryant6 (127
nodes) at P1 with the trie, the whole sift takes about 2.5 ms with the default
8 GiB tables (2^24 slots), 1.5 ms at `--memory` 1 GiB (2^21) and 1.0 ms at
256 MiB (2^19), of which the collection is 2.0, 1.0 and 0.5 ms and the 179
swaps 0.3 ms; CUDD sifts it in about 0.7 ms. So a forest of a few thousand
nodes measures mostly that floor. (When the session's own opening passes
visited every slot as well, the floor was 39 ms at 8 GiB, 7 ms at 1 GiB and
2 ms at 256 MiB.)

**Left out, and why.** Every order of a non-trivial circuit that is not in
the table, as screened (Sylvan: list, P1; "table full" is Sylvan's nodes
table at its 8 GiB maximum, 2^27 buckets, with a dozen collections in a row
reclaiming under 5%; "not pursued" means the circuit already had two
infeasible orders):

| case | Sylvan | CUDD |
|---|---|---|
| C2670-file | build over 600 s | |
| C2670-rev | build 12.8 s, 18975065 nodes, sift 138 s | build 19.8 s, sift 108 s |
| C2670-rnd1 | table full after 523 s | |
| C3540-rev | table full after 297 s | |
| C5315-file | table full after 175 s | |
| C5315-rev | build 0.1 s, 57585 nodes, sift 0.2 s: in budget, but the circuit's only feasible order | build 0.0 s, sift 0.1 s |
| C5315-rnd1 | build 131 s, 22806108 nodes, sift over 300 s | build 13.6 s, sift 55 s |
| C6288-file | table full within 60 s (the 16x16 multiplier) | |
| C6288-rev, -rnd1 | not done in 60 s, not pursued | |
| C7552-file, -rnd1 | table full after 50 s and within 60 s | |
| C7552-rev | not done in 60 s, not pursued | |
| s838.1-file | table full after 49 s | |
| s5378-file | build over 600 s | |
| s9234.1-rev | build 2.5 s, 10481747 nodes, sift over 300 s | not done in 900 s |
| s9234.1-rnd1 | build 2.7 s, 4731878 nodes, sift 134 s: over the 60 s budget | build 2.9 s, sift 49 s |
| s13207.1-file | build 52 s, 676676 nodes, sift 0.9 s: build-dominated | build 12.4 s, sift 1.5 s |
| s13207.1-rev | build over 600 s | |
| s15850.1-rev | build over 600 s | |
| s38417-file | build 10.8 s, 17811132 nodes, sift over 300 s | not done in 900 s |
| s38417-rev | build over 600 s | |
| s38417-rnd1 | not done in 60 s, not pursued | |
| s38584.1-file | build over 600 s | |
| i10-file | build 4.5 s, 8924136 nodes, sift 86 s; the circuit's only feasible order | build 9.3 s, sift 366 s |
| i10-rev | build over 600 s | |
| i10-rnd1 | table full within 60 s | |
| mm9a-rev | build 22.7 s, 7595159 nodes, sift 203 s: over the 60 s budget | build 21.8 s, sift 7.9 s |
| mm9b-rev | build 17.3 s, 6638670 nodes, sift 359 s (over 300 s) | build 24.3 s, sift 10 s |
| mm30a, o64, mem_ctrl (all three orders) | table full within 60 s | |
| mult32a, voter | file and rnd1: table full within 60 s; rev not pursued | |
| arbiter-rev | build 0.2 s, 715649 nodes, sift 65 s: over the 60 s budget | build 0.6 s, sift 7.0 s |
| queens12 | build 263 s, 435170 nodes, sift 13 s: over the 60 s budget (7.7 h of grid) | build 725 s, sift 34 s |
| queens13 | not built in 1500 s (6.9 GB resident) | |
| queens14 | not built in 600 s (6.9 GB resident) | |

CUDD was run only where Sylvan built. Too small to time: 196 circuits whose
every order starts below 5000 nodes, among them most of the classic MCNC set
(alu4, apex3 to apex7, cordic, misex3, t481, vda, x3, x4, i1, i5 to i7).
mult32b and adj49 cannot be read (see [Inputs](#inputs)).

**`--verify`.** Both drivers verify every case; past 64 inputs Sylvan's
model counts are doubles, as CUDD's always are (see
[The drivers](#the-drivers)). With `--verify 64` (trie, P1), every one of the
60 cases with more than 64 inputs (66 to 1464; the notes column gives the
number) was sound and valid on both tools and gave the same `eval_hash` on
both. Of 51 cases sampled before that, the 40 of at most 64 inputs or with
small counts also gave the same `eval_hash` on both tools, the reference
hashes of the four C880 orders among them.

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
3770 swaps with the bound and 5624 with `--full-legs` (CUDD: 3456). Where
the remaining 314 come from, and why the final sizes differ where they do,
is in [Swap traces](#swap-traces).

## Swap traces

Both drivers can write every adjacent swap their sift makes to a file, so that
the two schedules can be compared swap for swap rather than by their totals.

### The format

`--trace FILE` writes one line per swap, in the order the swaps are made, and
nothing else, so the file has as many lines as the row's `swaps`:

```
POS UPPER LOWER SCORE NODES LEG
```

| field | `sylvan_bench` (`sylvan_sift_set_trace`) | `cudd_bench` (`cudd-swap-trace.patch`) |
|---|---|---|
| POS | rank of the upper of the two variables, 0 at the top | the `x` of `cuddSwapInPlace(dd, x, x+1)` |
| UPPER LOWER | the identities at POS and POS+1 before the swap | `dd->invperm[x]`, `dd->invperm[x+1]` before the swap |
| SCORE | the size the driver compares, after the swap: `sylvan_reorder_forest_size()` | the size `cuddSifting` compares: `dd->keys - dd->isolated`, what `cuddSwapInPlace` returns |
| NODES | the same forest size | `dd->keys - dd->constants.keys`: every live internal node |
| LEG | `u`/`d` a search leg up/down, `b` the return to the best position, `r` reuniting a pair (`--group-pairs`) | `u` from `ddSiftingUp`, `d` from `ddSiftingDown`, `b` from `ddSiftingBackward` |

**NODES counts the same set on both sides.** Sylvan's forest is everything
reachable from its roots, which in this harness are the outputs and the
projection functions `var[i]` the driver holds; CUDD's unique table holds,
after the collection `Cudd_ReduceHeap` starts with, the outputs' nodes, the
projection functions the manager always references, and its four constants
(one, zero, plus and minus infinity), which `constants.keys` takes out. So on
the same order the two NODES are equal, and the traces confirm it: on every
common prefix below they never differ. **SCORE is where the tools differ.**
CUDD leaves out the projection functions nothing else references (`isolated`:
32 to 50 of C880's 60 over the sifts below, 99 of queens10's 100) and keeps
its four constants, so CUDD's SCORE is NODES + 4 - isolated, an offset that
changes with the order. Sylvan scores NODES, projection functions included,
because its harness holds them as roots.

### Building CUDD with the trace

CUDD has no swap hook. `cudd-swap-trace.patch` adds one to `cuddReorder.c` of
CUDD 3.0.0 (f54f533): `cuddSwapInPlace` writes a line to the file the
environment variable `CUDD_SWAP_TRACE` names (looked up once, at the first
swap; one branch per swap after that), and the three sifting routines set
the leg letter. It changes no decision. Apply it before `configure` in the
CUDD build above, in a tree used for traces only:

```sh
patch -p1 < /path/to/sylvan/benchmark/sift/cudd-swap-trace.patch
./configure CFLAGS="-O3 -DNDEBUG" CXXFLAGS="-O3 -DNDEBUG" && make -j
```

`cudd_bench --trace FILE` sets `CUDD_SWAP_TRACE`, and warns when the CUDD it
was built against wrote nothing.

### Comparing

```sh
sylvan_bench --mode blif --file inputs/C880.blif --case C880-file \
  --workers 1 --threshold 0 --verify 64 --trace C880-file.sylvan.trace
cudd_bench --mode blif --file inputs/C880.blif --case C880-file \
  --verify 64 --trace C880-file.cudd.trace
scripts/compare-traces.py C880-file.sylvan.trace C880-file.cudd.trace > C880-file.md
```

`compare-traces.py` cuts each trace into excursions (a variable's two search
legs and its return) and reports, as Markdown: the totals per leg; the
starting order each trace implies, which must agree; the longest common
prefix and the first divergence, with its cause; both candidate orders; where
every leg stopped and why (end of the order, growth bound, interaction
bound); each tool's return rule replayed on both traces, with a check that
every excursion follows its own tool's rule; the difference in swaps split by
leg and stop test; and a per-variable table. `--first-rules cudd` reads a
Sylvan trace made with the rules below.

### What the traces show

trie, P1, `--threshold 0`, growth 1.2, CUDD caps raised, `--verify 64`
(2026-10-09, WSL). Swap counts, sizes and hashes are deterministic.

| case | Sylvan | `--full-legs` | CUDD | common prefix | first divergence | final size Sylvan / CUDD | `eval_hash`, all three runs |
|---|---:|---:|---:|---:|---|---|---|
| C880-file | 3770 | 5624 | 3456 | 88 | return-leg tie | 5234 / 7064 | `b63d2a3eade6548e` |
| C880-rev | 3752 | 5554 | 3362 | 244 | return-leg tie | 36533 / 36533 | `699d25151bb6f5a2` |
| C880-rnd1 | 3870 | 5386 | 3685 | 170 | return-leg tie | 31088 / 23789 | `2cd3a4b5fbcbb236` |
| C880-rnd2 | 3841 | 5405 | 3565 | 795 | return-leg tie | 26972 / 26973 | `ecfd7a40ce4158b9` |
| queens10 | 14157 | 20021 | 13980 | 30 | leg stop (bound) | 25330 / 25331 | `eebe4e4e51c6c683` |

(The queens function is false on all 64 random vectors, so its hash says
only that; `sound` and the model counts check the rest.) Both sides start
from the same order (each trace implies the whole starting order, and they
agree), sift the same variables, and follow their own return rule on every
excursion. What is the same, read off the two sources: the candidate key
(each variable's node count when the sift starts, projection function
included, on the same forest); the interaction matrix (pairs within the
support of some root; Sylvan's projection roots and CUDD's isolated
projection functions have one-variable supports and add no pair, so the
matrices are equal); the leg order (nearer end first); the two interaction
bounds of `ddSiftingUp` and `ddSiftingDown`, term for term; the growth bound
per leg; and no caps. What differs is five rules:

| | rule | Sylvan | CUDD |
|---|---|---|---|
| R | return target | the first level, over both legs, with the smallest size; the start wins a tie | `ddSiftingBackward` on the second leg's moves: the last of them with the smallest of the start size and theirs, else the second leg's start |
| P | projection functions | scored: the harness holds them as roots | `isolated` ones left out of the score and of each level's count in both bounds |
| G | growth test | `size > 1.2f * limit`, sizes without constants | `size > 1.2 * limit` in double, sizes with its 4 constants |
| D | direction on a tie (exact middle) | down first | up first |
| O | candidate order among equal keys | by identity | as `util_qsort` leaves them (`ddUniqueCompare`'s tie-break is compiled out) |

R dominates. A sifted variable crossing a variable it does not interact with
changes no node, so the size curve of an excursion has runs of equal size,
and the minimum is often such a run: the first divergence on the four C880
cases is one (C880-file: variable 38's smallest forest, 50451 nodes, holds on
the 9 levels 18 to 26; Sylvan returns to 26, the first its legs reached, CUDD
to 18, the last its second leg reached: 8 more swaps for Sylvan, and a
different order for every excursion after it). P's first effect is on
queens10, where 99 of the 100 projection functions are isolated: CUDD's bound
stops variable 51's first leg one move before Sylvan's does; that excursion
still ends at the same level, so the orders stay equal for a while.

### Attribution: CUDD's rules one at a time

`sift-cudd-rules.patch` is a diagnostic patch for `src/sift.c` and
`sylvan_bench.c` at this commit, never for a build that measures anything
else. Each rule above becomes an environment variable:

| rule | variable |
|---|---|
| R | `SIFT_CUDD_RETURN=1` |
| P | `SIFT_DROP_VAR_ROOTS=1` (the harness releases its projection functions before the sift, so the forest is what CUDD scores, less its 4 constants) |
| G | `SIFT_CUDD_GROWTH=4` (with P; `0` without) |
| D | `SIFT_CUDD_DIRECTION=1` |
| O | `SIFT_CUDD_ORDER=FILE`, CUDD's candidate order from `compare-traces.py --order-out FILE` |

Adding them in that order to Sylvan's bounded run, on the same cases
(swaps; final size in parentheses where it changes):

| case | Sylvan | +R | +P | +G | +D | +O = CUDD |
|---|---:|---:|---:|---:|---:|---:|
| C880-file | 3770 (5234) | 3442 (7060) | 3451 (7068) | 3451 | 3451 | 3456 (7064) |
| C880-rev | 3752 | 3329 (36535) | 3334 (36533) | 3334 | 3334 | 3362 |
| C880-rnd1 | 3870 (31088) | 3747 (23790) | 3677 (23789) | 3677 | 3677 | 3685 |
| C880-rnd2 | 3841 (26972) | 3556 (26975) | 3563 (26973) | 3563 | 3563 | 3565 |
| queens10 | 14157 (25330) | 14057 (25331) | 13983 | 13983 | 13983 | 13980 |
| queens9 | 9201 (9246) | 9068 (9256) | 8982 | 8982 | 8978 | 8966 (9246) |

With all five, Sylvan's trace is CUDD's, swap for swap, on all six cases
(`compare-traces.py --first-rules cudd` reports the traces identical; SCORE
then differs by exactly CUDD's 4 constants), and so are the final sizes. So
the residual is explained in full. Per cause, in that order: on C880-file
R -328, P +9, G 0, D 0, O +5 (total -314); C880-rev -423, +5, 0, 0, +28;
C880-rnd1 -123, -70, 0, 0, +8; C880-rnd2 -285, +7, 0, 0, +2; queens10 -100,
-74, 0, 0, -3; queens9 (81 variables, so it has a middle) -133, -86, 0, -4,
-12. Each alone, against Sylvan: R -328/-423/-123/-285/-100/-133, P
+15/-180/-22/-32/-74/-92, O 0/-3/0/+4/-2/-2, G 0 everywhere, D 0 except
queens9 (-4). The rules interact (a different choice early changes every
excursion after it), which is why the alone and in-order numbers differ.
The quality differences are tie-breaks too: R alone turns C880-file's 5234
into 7060 and C880-rnd1's 31088 into 23790.

```sh
# in a scratch copy of this tree, never in a build that measures anything else
patch -p1 < benchmark/sift/sift-cudd-rules.patch     # then build sylvan_bench
scripts/compare-traces.py --order-out C880-file.order \
  C880-file.sylvan.trace C880-file.cudd.trace > /dev/null
SIFT_CUDD_RETURN=1 SIFT_DROP_VAR_ROOTS=1 SIFT_CUDD_GROWTH=4 \
SIFT_CUDD_DIRECTION=1 SIFT_CUDD_ORDER=C880-file.order \
  sylvan_bench --mode blif --file inputs/C880.blif --case C880-file \
  --workers 1 --threshold 0 --trace C880-file.rules.trace
scripts/compare-traces.py --first-rules cudd \
  C880-file.rules.trace C880-file.cudd.trace     # "identical"
```

## Measurement protocol

- **Like-for-like settings.** `nodes_threshold` 0 on Sylvan (CUDD has no
  such threshold, and 0 is also the library default), `maxGrowth` 1.2 on both,
  CUDD's `siftMaxVar`/`siftMaxSwap` raised to `INT_MAX` (Sylvan's limits are
  0, unlimited); `run-quality.sh --cudd-defaults` checks that the raised caps
  change nothing. Both tools sift the output cone only: every intermediate
  is released before the sift, and Sylvan's variable-set helper is dropped
  around it.
- **Same schedule, checked swap for swap.** The same candidate key, leg
  order, interaction matrix, interaction bounds and growth bound on both;
  `--trace` and `compare-traces.py` show where the two schedules part, and
  with `sift-cudd-rules.patch` Sylvan's trace is CUDD's (see
  [Swap traces](#swap-traces)). Time with untraced runs.
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
