# Experiments: parallel variable sifting in Sylvan

How to reproduce the five headline results of the paper on parallel variable
sifting in Sylvan (TACAS 2027) from this tree. Four of them (H1, H2, H3, H5)
run here, through the harness in `benchmark/sift/`; H4 runs in the
bdd-benchmark harness and is documented in its own section. Every conclusion
the paper draws must be reproducible with the commands below; each ablation is
one flag, named in its section.

The whole pipeline, on a Linux machine prepared as in
[the Linux checklist](#linux-checklist):

```sh
S=benchmark/sift/scripts
benchmark/sift/fetch-inputs.sh inputs                            # C880.blif, checksummed
$S/build-matrix.sh --src . --builds builds --cudd-dir /path/to/cudd   # every configuration
$S/run-headlines.sh --builds builds --inputs inputs --out results     # H1, H2, H3, H5
$S/headlines.py --in results --out results/tables                    # tables + reference check
```

`run-headlines.sh --quick` runs the same stanzas with one repetition, two
worker counts and small cases, to check the pipeline before a real batch.
`headlines.py` exits non-zero when a deterministic value differs from the
[reference values](#reference-values); a batch whose check fails is not a
result.

## Machine and OS

Fill in for every batch the paper cites; `run-headlines.sh` records the host,
kernel, CPU count, THP mode and load average itself, in its log and in every
row, but not the hardware.

| field | value | how to get it |
|---|---|---|
| CPU model | | `lscpu \| grep 'Model name'` |
| cores (P + E), threads | | `lscpu -e=CPU,CORE,SOCKET,NODE,MAXMHZ`; on hybrid Intel, `/sys/devices/cpu_core/cpus` and `/sys/devices/cpu_atom/cpus` |
| caches (L1d/L2/L3) | | `lscpu --caches` |
| memory, NUMA nodes | | `free -g`, `numactl --hardware` |
| OS, kernel | | `cat /etc/os-release`, `uname -r` |
| native or virtualised | | must be native, see the checklist |
| frequency governor, turbo | | `cpupower frequency-info`, `/sys/devices/system/cpu/intel_pstate/no_turbo` |
| THP `enabled` / `defrag` | | `cat /sys/kernel/mm/transparent_hugepage/{enabled,defrag}` |
| compiler | | `gcc --version` (recorded in `manifest.json`) |
| CMake, Ninja, Python | | `cmake --version`, `ninja --version`, `python3 --version` |
| perf | | `perf --version`; `perf stat -e cycles true` must show a count |
| workers pinned to | | the `taskset`/`numactl` line used |
| date, batch id | | the `batch` field of the rows |

## Revisions

| component | revision | how it is built |
|---|---|---|
| Sylvan | the commit you run: `git rev-parse HEAD` (the paper's batch: *fill in*) | CMake Release, Ninja, one tree per configuration (`build-matrix.sh`) |
| Lace | 2.4.0 (tag `v2.4.0`, `92b9f99100c87c7ae8895fd16f3552a4649e3b15`) | brought in by Sylvan's build through FetchContent; `--lace-dir` builds from a local copy instead |
| CUDD | 3.0.0, commit `f54f533303640afd5dbe47a05ebeabb3066f2a25` of [cuddorg/cudd](https://github.com/cuddorg/cudd) ("Import cudd-3.0.0.tar.gz") | `./configure CFLAGS="-O3 -DNDEBUG" CXXFLAGS="-O3 -DNDEBUG" && make`, in place |
| bdd-benchmark (H4 only) | a fork carrying branch `sift-hook` at commit `63e0404`; upstream does not carry the sift hook | see [H4](#h4-when-sifting-pays) |

`build-matrix.sh` writes the Sylvan commit, compiler, CMake and Lace versions
and CUDD's version and `CFLAGS` into `builds/manifest.json`, and every row
carries the Sylvan commit (`git`, `src_git`). Built from an exported tree with
no `.git` (`git archive`), give the commit with `--src-rev`; `git
get-tar-commit-id < export.tar` prints it.

### CUDD

```sh
git clone https://github.com/cuddorg/cudd.git && cd cudd
git checkout f54f533303640afd5dbe47a05ebeabb3066f2a25
./configure CFLAGS="-O3 -DNDEBUG" CXXFLAGS="-O3 -DNDEBUG"
make -j
```

Clone it on the Linux machine: a Windows checkout has CRLF line endings, which
break `configure`. `cudd_bench` needs `cuddInt.h` and the generated
`config.h`, so it is built against this tree, not an installed CUDD.

### Sylvan, one tree per configuration

The base configuration of each nodes table is, by hand:

```sh
cmake -S . -B builds/rel-<backend> -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DSYLVAN_NODES_TABLE=<backend> -DSYLVAN_BUILD_SIFT_BENCH=ON \
  -DCUDD_DIR=/path/to/cudd \
  -DBUILD_TESTING=OFF -DSYLVAN_BUILD_EXAMPLES=OFF -DSYLVAN_BUILD_CPP=OFF
cmake --build builds/rel-<backend> --target sylvan_bench --target cudd_bench
```

with `<backend>` one of `list`, `trie`, `llcht`, `llchst`. `build-matrix.sh`
does exactly this for every configuration the headlines need:

```sh
benchmark/sift/scripts/build-matrix.sh --src . --builds builds \
  --cudd-dir /path/to/cudd [--cc gcc|clang] [--only h1,h2 | rel-list,rel-trie-stats]
benchmark/sift/scripts/build-matrix.sh --list      # the matrix, without building
```

| configuration | backends | CMake definition (on top of the base) | headlines |
|---|---|---|---|
| `rel-<backend>` | list, trie, llcht, llchst | none | H1 (all four), H2, H3, H5 (list, trie) |
| `rel-<backend>-stats` | list, trie, llcht, llchst | `-DSYLVAN_STATS=ON` | H1 counters |
| `rel-<backend>-striped` | list, trie | `-DSYLVAN_REGION_STRIPED_START=ON` | H2a |
| `rel-<backend>-phases` | list, trie | `-DSYLVAN_SIFT_PHASE_TIMING=ON` | H2c |
| `rel-<backend>-nohuge` | list, trie | `-DSYLVAN_HUGEPAGES=OFF` | H5 |

The knob names are written in one place, the OPTION TABLE at the top of
`benchmark/sift/scripts/build-matrix.sh` (and its runtime-knob table:
`--no-batch`, `--stats`); the manifest copies them, and every row records the
knob's name and value from the manifest.

**Knob-missing.** If the source tree does not have a variant's CMake option,
CMake reports it as "not used by the project". `build-matrix.sh` then marks
that configuration `knob-missing` in the manifest and does not build it, and
`run-headlines.sh` skips every stanza that needs it with one line (also in
`results/skips.jsonl`, which `headlines.py` lists under the headline). A
skipped ablation is never measured as if the knob were in effect.

## Inputs

```sh
benchmark/sift/fetch-inputs.sh inputs                          # from GitHub
benchmark/sift/fetch-inputs.sh --cudd-src /path/to/cudd inputs # or from the CUDD checkout
benchmark/sift/fetch-inputs.sh --families                      # where the other families come from
```

`C880.blif` must have MD5 `82323a515652b47cdbff16cb2e0bc27e` (18428 bytes, LF);
the script checks it and never overwrites an existing file. N-queens and the
bryant functions are generated by the drivers.

| input | source | licence file in the source |
|---|---|---|
| `C880.blif` (ISCAS-85 C880) | CUDD f54f533, `nanotrav/C880.blif` | **none of its own**: the CUDD repository's `LICENSE` (BSD-style, University of Colorado) is CUDD's; the circuit file carries no licence |
| McNet Petri nets (H4) | Model Checking Contest 2024 PNML archives, mcc.lip6.fr | **none** in the archives |
| relprod BDDs (H4) | Zenodo record 13928216, `2023.zip` | **none** inside the archive; the terms are those stated on the record page |
| QBF instances (H4) | Q-sage, commit `08177212dbca5605f84c86d702fc06e4ecce5cc8` | `LICENSE` at the repository root |
| EPFL arithmetic (documented infeasible, not used) | lsils/benchmarks, commit `0060e156826e733d69bf5b3322d1bdd0d03a1f9a` | `LICENSE` at the repository root |

## Cases

| case | what | used by |
|---|---|---|
| `C880-file`, `C880-rev` | every output of C880, under the file's input order and its reverse | H1, H2, H3, H5 |
| `C880-rnd1`, `C880-rnd2` | the same under random orders, seeds 1 and 2 (`blif_var_perm`, identical in both drivers) | H1, H2, H3, H5 |
| `C880-rnd1` ... `C880-rnd30` | 30 random orders | H3 seeds |
| `queens12` | the 12-queens board, row-major (`queens10` in `--quick` and H3) | H1, H3, H5 |
| `bryant6` ... `bryant14` | x0.y0 + ... + x(N-1).y(N-1) under the split order; optimum 2N+1 | H3 |

`benchmark/sift/scripts/cases.sh` says why these, and why no other circuit
qualifies.

## Settings

| setting | value |
|---|---|
| Sylvan `nodes_threshold` | 0 (the library default; `--threshold 0`) |
| maximum growth | 1.2 on both tools |
| CUDD caps | `siftMaxVar` and `siftMaxSwap` raised to `INT_MAX` (unlimited, like Sylvan's 0); `--cudd-defaults` re-runs with CUDD's own caps (H3) |
| Sylvan limits | none: `max_swaps`, `max_vars`, `time_limit` 0 |
| interaction bound | on (the default); `--full-legs` is the H3 ablation |
| workers | 1, 2, 4, 8, 16 (counts above `nproc` are dropped); CUDD is single-threaded and runs at P1 |
| repetitions | n = 7 for C880, n = 5 for queens12 and soundness, n = 3 for full legs; the deterministic passes once |
| statistics | median with minimum and maximum, and n, never a mean |
| memory | 8 GiB tables (`--memory`, the driver default) |
| process | one process per run; `sift_s` is `CLOCK_MONOTONIC` around the sift call alone, including the collection it opens with; `build_s` the build alone |
| timeout | 1800 s per run (`--timeout`) |
| interleaving | within each repetition every variant runs once, in a fixed order (A,B,A,B) |

## Metrics

Each run is one JSONL row: the driver's `RESULT` object (see
`benchmark/sift/README.md`) prefixed with what the scripts know.

| field | meaning |
|---|---|
| `sift_s`, `build_s` | sift and build wall time (s) |
| `nodes_before`, `nodes_after` | shared node count of the output roots (`mtbdd_shared_node_count`, `Cudd_SharingSize`); identical on both tools before the sift |
| `swaps`, `refusals`, `vars_sifted` | adjacent swaps (search and return legs, as CUDD counts them), swaps the table refused, variables moved |
| `eval_hash`, `sound`, `valid` | see [soundness](#soundness-protocol) |
| `exp`, `cfg`, `rep`, `build`, `git`, `host`, `load1/5/15`, `t_start`, `t_end`, `wall_ms` | the scripts' own fields (README) |
| `headline`, `stanza`, `batch`, `quick` | which headline and stanza of which `run-headlines.sh` batch |
| `config_name`, `config_variant`, `config_status`, `config_knob`, `knobs`, `src_git`, `cc_version`, `lace_version` | the configuration and its knob settings, from the manifest (`cudd_version`, `cudd_git`, `cudd_cflags` on CUDD rows) |
| `variant`, `variant_args` | the runtime setting, in `run-ablation.sh` rows |
| `thp`, `thp_defrag` | the transparent huge page modes when the run started |
| `argv`, `wrap` | the driver's arguments; the `SB_WRAP` command it ran under |
| `stats_report` | `sylvan_bench --stats`: the `sylvan_stats_report` text for the build and for the sift |
| `sift_phases` | the `SIFT-PHASES {json}` object(s) of a phase-timing build |
| `perf` | `{event: count}` when run under `SB_WRAP="perf stat -x, ..."` |

## Soundness protocol

- `--verify N` evaluates every output on N random input vectors (drawn from
  `srand(--seed)`, 12345 by default, indexed by variable identity) and counts
  its models, before and after the sift. Both are invariant under any correct
  reordering; `sound` is 1 when both agree. `valid` is `mtbdd_is_valid` on
  every root (Sylvan) or `Cudd_DebugCheck` (CUDD).
- `eval_hash` is an FNV-1a hash of that evaluation, computed identically by
  both drivers. Equal hashes on the same vectors show that both tools built
  the same functions; H3 quality rows all run with `--verify 64`, soundness
  with 5000 vectors, seeds with 2000.
- `headlines.py` fails the batch when any row that verified is not sound and
  valid, when two rows of a case in one stanza with the same vectors have
  different hashes (across tools or backends), or when C880-file's hash on 64
  vectors of the default seed is not `b63d2a3eade6548e`.

## Reference values

Deterministic, so `headlines.py` checks them on every row whose settings are
the ones above (threshold 0, growth 1.2, no limits, CUDD caps raised), at
every worker count and on every backend:

| case | initial | Sylvan final | CUDD final |
|---|---:|---:|---:|
| C880-file | 346660 | 7064 | 7064 |
| C880-rev | 470046 | 36533 | 36533 |
| C880-rnd1 | 844442 | 23789 | 23789 |
| C880-rnd2 | 1066147 | 26973 | 26973 |
| queens10 | 25945 | 25331 | 25331 |
| queens12 | 435170 | 427043 | 427043 |
| bryantN | | 2N+1 | 2N+1 |

Swap counts are deterministic too, but not reference values: they change with
the schedule (C880-file takes 3469 swaps with the interaction bound and 5279
with `--full-legs`, which ends at 7063 nodes; CUDD 3456). H1 checks instead that every backend and
worker count does the identical sift (same final size and swap count).

## Linux checklist

- **Native Linux, not WSL2.** Under WSL2 `perf` has no hardware PMU counters,
  and the VM presents a fake homogeneous topology: P-cores and E-cores are
  indistinguishable, so workers cannot be pinned to P-cores. A timing taken in
  such a VM on a shared desktop is not a quiet-machine timing, which is what
  this protocol is for.
- **Pin the workers to P-cores** (or to one NUMA node on a server). Affinity is
  inherited, so pin the whole batch:
  `taskset -c <P-core list> benchmark/sift/scripts/run-headlines.sh ...` or
  `numactl --physcpubind=<cpus> --membind=0 ...`. On hybrid Intel the P-cores
  are `/sys/devices/cpu_core/cpus`. Give as many CPUs as the largest worker
  count and no fewer.
- **Hardware counters**: rerun the cells the paper discusses under
  `SB_WRAP="perf stat -x, -e cycles,instructions,cache-references,cache-misses,LLC-load-misses,dTLB-loads,dTLB-load-misses,iTLB-load-misses"`
  (`run-headlines.sh --only h5`, for instance); the counters land in each
  row's `perf`. Check first that `perf stat -e cycles true` counts, and that
  `/proc/sys/kernel/perf_event_paranoid` allows it.
- **THP mode**: `madvise` for H5 (under `always` every mapping gets huge pages,
  under `never` none does, and the ablation measures nothing); every row
  records it, and `run-headlines.sh` warns when H5 runs under another mode.
- **A quiet box**: no other jobs, no desktop, no backups or indexers; the
  `performance` governor, and turbo either off or recorded. `load1` in every
  row and the busiest processes before every case in each `.log` show how
  quiet it was; rerun a batch whose load average was not near zero.
- **Interleave variants** (the scripts do: A,B,A,B within every repetition)
  and report medians with min-max and n.
- **Record the load average** (the scripts do, per row and per case).

## The headlines

Every stanza below is one call of a `run-*.sh` script made by
`run-headlines.sh`; `--only` takes a headline (`h1`) or a stanza (`h2b`).
Each writes `results/<stanza>*.jsonl` and `.log`; `headlines.py` turns them
into `results/tables/hN.md` (the tables) and `hN.csv` (the main table in long
form, median/min/max/n per timed quantity).

### H1 Which unique table

```sh
benchmark/sift/scripts/run-headlines.sh --builds builds --inputs inputs --out results --only h1
```

- `h1-timing`: `rel-list`, `rel-trie`, `rel-llcht`, `rel-llchst` and CUDD,
  P1/2/4/8/16, the four C880 orders (n=7) and queens12 (n=5), interleaved:
  sift seconds, swaps, final size, refusals. The ablation is the nodes table,
  `-DSYLVAN_NODES_TABLE=<backend>`; CUDD at P1 is the external reference.
- `h1-stats`: the `rel-*-stats` builds (`-DSYLVAN_STATS=ON`) with
  `sylvan_bench --stats`, at the lowest and highest worker count, once: the
  per-step index counters of the build and of the sift, as
  `sylvan_stats_report` prints them. These builds are slower; never time them.
- Tables: per case and backend, median sift time per worker count and the
  speed-up over P1 and against CUDD; whether all backends and worker counts
  did the identical sift; the `LLMSSET_*` lines of `sylvan_stats_report`
  ("Unique lookup", "Unique remove" and "Unique reinsert", each in steps and
  calls), with "X steps per call" for every "X steps"/"X calls" pair.

### H2 What made parallel sifting scale

```sh
benchmark/sift/scripts/run-headlines.sh --builds builds --inputs inputs --out results --only h2
```

An ablation on `list` and `trie`, the four C880 orders, every worker count,
n=7, each pair interleaved:

- `h2a` the shared region cursor against striped per-worker starts: `rel-B`
  against `rel-B-striped`, the CMake option `-DSYLVAN_REGION_STRIPED_START=ON`.
- `h2b` batched table primitives against single-node ones: the same `rel-B`
  with and without `sylvan_bench --no-batch` (`sylvan_reorder_set_batched(0)`),
  through `run-ablation.sh --variant batched= --variant nobatch=--no-batch`.
  Skipped, with one line, when the build's `sylvan_bench --help` does not list
  `--no-batch`.
- `h2c` where the time goes: `rel-B-phases` (`-DSYLVAN_SIFT_PHASE_TIMING=ON`)
  prints one `SIFT-PHASES {json}` line per sift, kept in the row as
  `sift_phases`; the table is the median of every numeric field.
- Table: median sift time with and without each change, and the delta.

### H3 Schedule fidelity and quality against CUDD

```sh
benchmark/sift/scripts/run-headlines.sh --builds builds --inputs inputs --out results --only h3
```

- `h3-quality`: `run-quality.sh --cudd-defaults` on `rel-trie` and `rel-list`
  at P1 and P4 and CUDD, with `--verify 64` on both tools so that every row
  has an `eval_hash`: bryant6..14, the four C880 orders, queens10, queens12.
- `h3-soundness`: `run-soundness.sh`, 5000 vectors, the downstream pairwise
  operation phase before and after, n=5, plus a no-sift control.
- `h3-seeds`: `run-seeds.sh` on `rel-list`, C880 under 30 random orders,
  2000 vectors; some orders take CUDD minutes to sift.
- `h3-fulllegs`: `run-ablation.sh --variant bounded= --variant fulllegs=--full-legs`,
  the interaction bound against walking every leg (the ablation flag is
  `--full-legs`), with CUDD's swap count alongside.
- Tables: final size and swap ratios against CUDD, the same order at P4 as at
  P1, cross-tool hash agreement, CUDD's own caps, the pruning of the bound,
  soundness, and the 30-seed distribution.

### H4 When sifting pays

Runs in the bdd-benchmark harness, which drives Sylvan through a sift hook on
real workloads (relational products, McNet reachability, QBF), not in this
repository. Pin both sides:

```sh
# The sift hook lives on branch sift-hook of a fork of bdd-benchmark, at
# commit 63e0404. Upstream SSoelvsten/bdd-benchmark does not carry it.
git clone <a fork of bdd-benchmark carrying branch sift-hook> bdd-benchmark
git -C bdd-benchmark checkout 63e0404
cmake -S bdd-benchmark -B build-list -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBDD_BENCHMARK_SYLVAN2_API=ON -DBDD_BENCHMARK_SYLVAN2_PROTECTED_API=ON \
  -DBDD_BENCHMARK_SYLVAN2_SIFT_API=ON -DBDD_BENCHMARK_LACE2_API=ON \
  -DBDD_BENCHMARK_SYLVAN_SOURCE_DIR=<this Sylvan tree at the commit above> \
  -DSYLVAN_NODES_TABLE=list -DADIAR=OFF -DBUDDY=OFF -DCAL=OFF -DCUDD=OFF -DLIBBDD=OFF -DOXIDD=OFF
cmake --build build-list --target sylvan_relprod_bcdd sylvan_mcnet_bcdd sylvan_qbf_bcdd sylvan_picotrav_bcdd
# a second build with -DSYLVAN_NODES_TABLE=trie for the table comparison
```

The hook sifts when the workload is started with `-R`. Two environment variables select the
variant and are recorded in every `SIFT {json}` line the adapter prints to stderr:
`SYLVAN_SIFT_MODE=off|upfront|auto|both` (default `upfront`: one `sylvan_sift_report` before
the main computation; `auto`: `sylvan_gc_set_auto_sift` plus explicit
`sylvan_gc()` reorder points after each image, so a sift happens only when a collection
recommends it) and `SYLVAN_SIFT_PAIRS=0|1` (default 0; 1 sets `group_pairs`, so the pairs
(2k, 2k+1) move as blocks and `split_pairs` stays 0). Optional caps: `SYLVAN_SIFT_MAX_SWAPS`,
`SYLVAN_SIFT_MAX_VARS`, `SYLVAN_SIFT_TIME_LIMIT`, `SYLVAN_SIFT_FACTOR`. On CUDD, `-R` enables dynamic reordering: `Cudd_AutodynEnable` with sifting, or group sifting over the pairs (2k, 2k+1) with `CUDD_REORDER_METHOD=group`; the adapter prints `CUDD {json}` lines with the method, the number of reorderings and their time, and builds nodes through ITE while reordering is on, since a direct node builder assumes the initial order. The study driver
`tools/sylvan-study.py --sift-mode off upfront auto --sift-pairs 0 1 --workers 1 8` runs the
matrix and `tools/sylvan-summarize.py` groups by mode and pair setting (`median_sifts`,
`median_sift_s`, `median_split_pairs`).

The inputs are the McNet, relprod and QBF families of
`benchmark/sift/fetch-inputs.sh --families`, with the checksums given there.
The study compares every instance unsifted (`SYLVAN_SIFT_MODE=off`, no `-R`) against the
four sifted variants (upfront/auto x independent/paired), on `list` and `trie`, at P1 and P8,
`-M 4096`, n=3 interleaved, and reports the sift time, swaps, `split_pairs`, the result sizes
(relation after the sift, product, reachable set) and the end-to-end time. Correctness
protocol: every sifted run's answer (`relprod/satcount` and `states/satcount`, McNet
`reachability/satcount` and `deadlock/satcount`, QBF `satisfiable`, Picotrav `equal/result`)
must equal the unsifted run's; the summarizer flags any difference. The fused relational
product must not assume that the variable pairs stay adjacent; a tree whose relational product
does fails this protocol.

### H5 Memory-system lessons

```sh
benchmark/sift/scripts/run-headlines.sh --builds builds --inputs inputs --out results --only h5
```

`rel-B` (huge-page advice, the default) against `rel-B-nohuge`
(`-DSYLVAN_HUGEPAGES=OFF`), B in `list` and `trie`, interleaved, at the lowest
and highest worker count: the build time of queens12 and of the C880 forests,
and the sift time, with the THP mode in every row. Run it under THP `madvise`
(see the checklist), and with `SB_WRAP="perf stat ..."` for the dTLB counters.
The table gives build and sift medians with and without the advice and the
deltas, and says when the rows were measured under another THP mode.

## Dependency surface of the reordering stack

How much of Sylvan the reordering stack rests on, measured on this tree against its base
`608011f`:

- Required code: 5,897 library lines. 3,309 are in six new files (`reorder.c` 2,025, `sift.c`
  431, `ranks.c` and the reorder headers 853); 2,588 are added inside 22 base files, three
  quarters of them in the node-table layer (`nodes.h` 285, `nodes_trie.c` 1,057, `nodes_list.c`
  618), the rest being 69 rank comparisons in the kernels (162 lines), the root visitors (285)
  and the session guards (103).
- The sift core imports 28 base symbols: 13 record-layout accessors and types, 6 node-table
  queries, 3 status codes, 3 allocation helpers and 3 statistics counters, plus two base tasks
  (`sylvan_gc_go`, `nodes_count_nodes`) and the Lace 2.4 work-stealing primitives.
- Four base decisions carry the design: a handle is a stable 40-bit index of a 16-byte record
  that is also the unique-table key; one global unique table with max-child-anchored backends
  and a 512-slot region allocator that gives each worker one owned region; stop-the-world
  collection in a Lace frame from roots enumerable as values, with no permanent reference
  counts; and a variable order that appears only through explicit comparisons in the kernels.
  Changing any of them means redesigning the swap, not rebasing it.
- Separable: the chained tables `llcht` and `llchst` (about 5,200 lines, kept for the backend
  comparison of H1), and about 3,000 lines of Sylvan 2.0 changes that sifting does not need
  (traversal-mark bitmap, handle checks, collection sizing, C++ exceptions, initialisation
  status returns, removal of by-value references).
