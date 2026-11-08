# Change Log
All notable changes to Sylvan will be documented in this file.

## [Unreleased]

### Added
- `sylvan_reach` and `lddmc_reach`, which compute all states reachable from a
  set with one transition relation in one recursive operation (REACH, from
  S. Brand, T. Bäck and A. Laarman, FM 2023), instead of a loop of
  `sylvan_relnext` or `lddmc_relprod` calls. They take the variables of
  `sylvan_relnext` and the meta of `lddmc_relprod`, including copy nodes and
  action labels at any level, including between a read and its write. Labels
  are existentially quantified by both LDD operations. `lddmc_extend_rel`
  extends an LDD relation to read and write every variable, so that several
  relations can be merged with `lddmc_union`.
  The `bddmc` and `lddmc` examples have a new strategy `reach`, which merges
  the relations and then calls REACH once.
- `sylvan_unique`, which computes parity quantification (an odd number of
  satisfying assignments, not necessarily exactly one).
- `sylvan_intersection_witness`, which finds a nonempty subset of two BDDs'
  intersection, preferring low branches, or returns false if they are disjoint.
- `sylvan_pick_representatives`, which deterministically selects one satisfying
  assignment to a variable set for each extendible assignment to the remaining
  variables, preferring false in increasing variable order.
- `sylvan_cardinality`, which constructs inclusive lower and upper bounds on
  the number of true variables in a set, including exact-k constraints.
- Boolean ZDD operations `zdd_xor`, `zdd_equiv`, `zdd_nand`, `zdd_nor`,
  `zdd_imp`, `zdd_disjoint` and `zdd_subset`, plus universal (`zdd_forall`)
  and parity (`zdd_unique`) quantification. They use master's ZDD domains
  and return `zdd_invalid` for invalid arguments, except for the predicates
  `zdd_disjoint` and `zdd_subset`, which return `int`: 1 for true, 0 for false
  and -1 for invalid operands. Boolean validation rejects both ZDD and MTBDD
  map nodes, including maps embedded in diagrams.
- `zdd_eval_assignment` for full Boolean ZDD assignments. The existing
  single-variable `zdd_eval` is unchanged.
- `zdd_minimal_sets`, which converts a monotone Boolean BDD to the ZDD family
  of its inclusion-minimal satisfying sets and rejects non-monotone input.
- Exact `uint64_t` model counts: `sylvan_satcount_u64`, `mtbdd_satcount_u64`,
  `lddmc_satcount_u64` and `zdd_satcount_u64`. They report overflow and leave
  the output unchanged on failure. The corresponding `_gmp` operations in
  `sylvan_gmp.h` support arbitrary precision when built with `SYLVAN_GMP`.
  Existing count functions and their semantics are unchanged.
- `sylvan_eval` and `mtbdd_eval`, which follow an assignment to a leaf without
  allocating nodes, and `sylvan_cofactor`, which rejects non-cubes before
  restricting a BDD. `sylvan_restrict` remains unchanged.
- `sylvan_gc_set_policy()` and `sylvan_gc_get_policy()`, with
  `SYLVAN_GC_FEW_COLLECTIONS` for aggressive growth and retaining the node
  arena on explicit collections, and `SYLVAN_GC_LOW_MEMORY` for growth only
  above half occupancy and releasing empty arena stretches. Selecting a
  policy installs its growth hook. A custom main hook installed afterwards
  overrides growth while keeping the policy's shrink mode.
- `SYLVAN_HUGEPAGES`, a CMake option defaulting to OFF until measured on
  master. When enabled with `SYLVAN_USE_MMAP` on Linux, it requests transparent
  huge pages for mappings of at least 2 MiB through `MADV_HUGEPAGE` when
  available, including the node data, hash table and operation cache.
  Traversal side bitmap allocations request `MADV_NOHUGEPAGE` regardless of
  the option to limit sparse memory use. Both requests are best effort: the
  advice may be unavailable or fail. With the option OFF, the kernel may still
  use huge pages under THP `always`, including for side bitmaps if their
  opt-out is unavailable or fails.
- `SYLVAN_INSTALL`, a CMake option to generate Sylvan's installation rules.
  It defaults to on for a top-level build and off as a subproject.
- `SYLVAN_CHECK_HANDLES`, an opt-in bounds check in the node dereference
  helpers, including in Release builds. It aborts with the offending index
  when a handle is outside the nodes data range, which can exceed the hash
  table size after shrinking. `test_check_handles` checks both rejection and
  valid handles above the hash table size.
- `SYLVAN_GC_DIAGNOSTICS`, an opt-in trace to stderr with one line per
  collection: occupancy, live and freed buckets, time, and table and cache
  sizes before and after growing or shrinking. Both diagnostic options have
  no runtime cost when disabled.
- A Linux CI job that builds and runs the tests in Debug with AddressSanitizer
  and UndefinedBehaviorSanitizer, using the current Ubuntu GCC.
- `sylvan_gc_collect()`, the garbage collection Sylvan starts itself when the
  nodes table is full. It resizes with the main gc hook, as before, and never
  shrinks the tables. Programs that want to collect garbage call `sylvan_gc()`.
- For code that works with the nodes table directly (`sylvan_table.h`):
  `llmsset_shrink()`, and `llmsset_get_data_size()`, the range of node
  indices in use. After a shrink, live nodes can have indices at or above
  `llmsset_get_size()` (the size of the hash table), but always below
  `llmsset_get_data_size()`.

### Changed
- Garbage collection skips the empty parts of the nodes table. The passes that
  rehash the live nodes and destroy dead custom leaves now go through the
  bitmaps 64 buckets at a time instead of one bucket at a time, so empty
  parts of the data range cost one load per 64 buckets. With 2 live nodes and 2 workers (M1 Pro), a collection of a table of
  2^22 buckets takes 0.20 ms instead of 3.2 ms. After `sylvan_gc()` with the
  few-collections policy, which keeps the data range at its peak, a
  collection over 2^20 buckets takes 83 µs instead of 0.83 ms.
- The default GC policy is `SYLVAN_GC_FEW_COLLECTIONS`, with aggressive
  growth. Explicit `sylvan_gc()` collections shrink the hash table and
  operation cache and retain the arena's data range and pages. Select
  `SYLVAN_GC_LOW_MEMORY` to release unused arena memory; it is also the
  default when compiled with `SYLVAN_AGGRESSIVE_RESIZE=0`. Low-memory
  collections can release arena memory even if the hash table is already at
  its minimum, including after switching policies.
- Aligned clears on Linux first try `MADV_DONTNEED` when available, retaining
  the mapping and its advice on success. If unavailable or unsuccessful,
  clears try remapping and request huge pages again for ranges of at least
  2 MiB when the option is enabled and the advice is available, before falling
  back to `memset` if remapping also fails. Master's operation cache clears by
  freeing and reallocating, so new allocations get fresh advice under the
  conditions above. Low-memory shrinking reclaims empty data stretches at
  64 KiB boundaries, which can split huge pages. Other platforms keep their
  existing behavior.
- Text files are stored and checked out with LF line endings on every
  platform. Serialized `.bdd` and `.ldd` models remain binary.
- `sylvan_gc()` now shrinks the nodes table to fit the live nodes: to the
  smallest size, from the initial size up, with room for twice the live
  nodes, and the operation cache in proportion. Nodes keep their place. With
  the low-memory policy, the nodes array keeps the buckets up to the highest
  live node, but the memory of its empty stretches goes back to the operating
  system; the few-collections policy retains the arena. The main gc hook is
  not called for these collections. The automatic collections, when the nodes
  table is full, grow according to the selected hook. After dropping about a
  million nodes, shrinking with arena release took a program from 62 MB to
  3 MB resident.
- `mtbdd_protect`, `lddmc_protect`, `zdd_protect`, their `unprotect`
  functions, and `mtbdd_ref`, `lddmc_ref` and their `deref` functions no
  longer make all workers contend on one shared counter. The tables count
  active calls so that a resize can wait for them; each Lace worker now counts
  its calls in a counter of its own, and other threads use the shared counter
  as before. With 8 workers that each protect and unprotect a variable in a
  loop, a pair costs 45 ns of CPU time instead of 3.5 µs; with 1 worker it
  costs the same as before (29 ns). Programs that create and destroy C++
  objects holding decision diagrams on many workers at once gain the most.

### Fixed
- MTBDD comparisons could cache false answers from subtasks aborted by another
  worker's early stop, making later comparisons return incorrect results. All
  six comparisons now distinguish aborted tasks from completed results and use
  an atomic stop flag. The new `test_mtbdd_compare` checks comparisons against
  an assignment oracle and queries subdiagrams after parallel early stops.
- `mtbdd_less` and `mtbdd_greater` returned false where both operands were
  undefined. They now ignore those assignments, as documented for partial
  domains, including when both operands are entirely undefined.
- `mtbdd_equal_norm_d` swapped operands after reading their node metadata,
  which could compare the wrong cofactors when their top variables differed.
  It now orders operands before reading their nodes.

## [1.12.8] - 2026-11-15

This release updates Lace to version 1.11.1, which makes frequent garbage
collections with many workers fast again, and adds a handler for a full nodes
table.

### Fixed
- Updated Lace to version 1.11.1. With Lace 1.11.0, workers that fell asleep
  during a garbage collection were not woken when it finished, so every
  collection waited for their sleep to time out: programs that collect often
  with many workers ran much slower (Sylvan's test_gc at 8 workers took 4.1 s
  instead of 1.5 s on a 16-core Linux machine). On Windows, every collection
  waited about 11 ms for a napping worker, also with earlier Lace versions.
  Lace 1.11.1 wakes them.

### Added
- `sylvan_set_table_full_handler` lets applications report nodes-table exhaustion
  after garbage collection and exit with their own code. The callback receives
  the diagram kind and bucket counts. Passing `NULL`, or returning from a handler,
  uses the existing error message and `exit(1)`.

## [1.12.7] - 2026-11-15

This release fixes the transitive closure, `sylvan_relprev`, duplicate nodes
while counting or printing decision diagrams on another worker, a crash in
garbage collection, and an abort when collecting with very many protected
variables.

### Fixed
- `sylvan_closure` combined recursively computed transitive closures with
  formulas that required reflexive closures. It could omit reachable pairs,
  including every pair in a relation that flips one bit; this has been so
  since 2015. It now includes paths of one or more steps. The new `test_relprod` checks the flip relation
  and compares closure and relational products with an explicit-state oracle.
- `sylvan_relprev` called `sylvan_relnext` when both operands depended on a
  variable outside the specified pairs, composing in the wrong direction.
  It now recurses into `sylvan_relprev`. `test_relprod` checks the shared
  outside-variable case as well as partial relations and relation composition.
- Node and leaf counters, `.dot` writers and structural hashes marked visited
  nodes in the word hashed by the unique table. A concurrent node insertion
  could miss an existing node and create a duplicate, breaking canonical
  handles. Traversals now use lazily allocated side bitmaps, with a separate
  bitmap for each concurrent or nested traversal. The new `test_markbit`
  checks node lookup while a walk is marked, repeated and concurrent MTBDD,
  ZDD and LDD traversals, and GC and table growth inside a leaf printer.
- GC treated a protected `mtbdd_invalid` or `zdd_invalid` as a node index and
  could crash, for example after `mtbdd_fraction(1, 0)`. It now ignores these
  sentinels. The new `test_gc_invalid` and `test_gc_invalid_zdd` collect with
  protected invalid results and check that live nodes survive too. LDD has
  no invalid sentinel.
- Six-key cache entries used the wrong status bit to identify their two
  buckets. A three-key lookup could mistake half of such an entry for its
  own result. The pair flag now agrees with the reader's mask;
  `test_table_cache` checks colliding keys and replacement of a paired entry.
- Table rehashing inserted reserved node indices 0 and 1, which could make
  lookups return a terminal or the failure sentinel instead of a real node.
  Rehashing now skips both indices; `test_table` checks the rebuilt table.
- The table size setters allowed up to 2^42 buckets although handles only
  hold 40-bit indices, so sufficiently large tables could produce truncated
  handles. Both size setters now stop at 2^40 buckets. `test_table_limit`
  checks that the explicit setter accepts 2^40 and rejects 2^41 without
  allocating the table.
- Removed an unreachable `madvise` block from the node table allocator. Its
  preprocessor condition tested a function name as a macro, so the block
  never ran; this does not enable `MADV_RANDOM` or change allocation behavior.
- Garbage collection marked the variables protected with `mtbdd_protect`,
  `lddmc_protect` and `zdd_protect`, and the values referenced with
  `mtbdd_ref` and `lddmc_ref`, with one task per root, all spawned by the
  worker that collected. With more roots than its task deque holds (1M tasks
  by default), Lace aborted with "Task stack overflow"; with fewer, that one
  worker still pushed every task. Collection now splits each table by divide
  and conquer into ranges of 256 buckets, and marks the roots of a range in
  one task. The new test `test_gc_roots` collects garbage with 40000 roots of
  each kind and a deque of 8192 tasks. The per-worker stacks of temporary
  references, already marked by divide and conquer, now stop splitting at 64
  entries instead of 32.

## [1.12.6] - 2026-11-09

This release updates Lace to version 1.11.0, whose idle workers scale to many
cores.

### Changed
- Updated Lace to version 1.11.0, which makes idle workers scale to many
  cores. Each sleeping worker waits on its own futex word, so that waking
  workers no longer contends on one kernel lock. Idle workers search only
  while that pays off, and with pinned workers on Linux they steal mostly
  within their own socket. On a 4-socket machine with 96 cores, the changes
  beyond the per-worker futex words make mCRL2's symbolic reachability 5-28%
  faster, with the same or less CPU time, than the per-worker futex words
  alone.
- Sylvan now asks for Lace 1.11.0 when it looks for an installed Lace, and
  requires at least Lace 1.11.0. An installed Lace accepts only requests for
  its own minor version.

## [1.12.5] - 2026-11-08

This release fixes a bug in which the tables of protected variables could
lose entries, so that garbage collection deleted decision diagrams that were
still protected.

### Fixed
- The tables of protected variables and of external references (behind
  `mtbdd_protect`, `lddmc_protect`, `zdd_protect`, `mtbdd_ref` and
  `lddmc_ref`) could lose entries when they grew twice in quick succession:
  a thread that helped with one resize, if it was delayed at the wrong
  moment, could claim a part of the next resize before that resize had its
  new table, and rehash that part into the old table. Garbage collection
  could then delete decision diagrams that were still protected. Only the
  thread that resizes a table now rehashes it; the others wait. Every earlier
  version has this bug. The new test `test_refs` adds and removes protected
  values on many workers while the tables grow.
- Unprotecting a variable that was not protected (`mtbdd_unprotect`,
  `lddmc_unprotect`, `zdd_unprotect`) left the protection table's count of
  active calls raised (in a build without assertions), so the next resize of
  the table waited forever.
- The value of an LDD node was read and written as a `uint32_t` at byte
  offset 6 of the node, which is misaligned: undefined behaviour, reported by
  UndefinedBehaviorSanitizer (`-fsanitize=undefined`) in every program that
  uses LDDs. Sylvan now uses `memcpy`, which compilers turn into the same
  single load or store, so the generated code is as before.

## [1.12.4] - 2026-11-08

This release updates Lace to version 1.10.0, which fixes a bug that could
corrupt memory during garbage collection.

### Fixed
- Updated Lace to version 1.10.0, which fixes a bug that could corrupt memory
  during garbage collection. A worker that went idle during a collection
  could reset the scratch memory of the task that it had interrupted, while
  that task still used it. Sylvan uses scratch memory in its enumeration
  functions, such as `sylvan_enum_par` and `lddmc_sat_all_par`. Lace then
  printed a "leaked scratch" warning, followed by crashes or wrong results.
  Lace 1.7.0 to 1.9.0 have this bug, so Sylvan 1.11.0 to 1.12.3 have it too.

### Changed
- Sylvan now asks for Lace 1.10.0 when it looks for an installed Lace, and
  requires at least Lace 1.10.0. Lace 1.10.0 removes the option
  `LACE_USE_HWLOC`, which Sylvan never set.

## [1.12.3] - 2026-11-07

This release updates Lace to version 1.9.0.

### Changed
- Updated Lace to version 1.9.0. Lace now pins its workers on FreeBSD and
  Windows too (it already did on Linux), pins each worker before the worker
  sets up its memory, so that the memory is on the worker's NUMA node, and
  starts its workers faster. Lace deprecates its option `LACE_USE_HWLOC`,
  which Sylvan never set.
- Sylvan now asks for Lace 1.9.0 when it looks for an installed Lace, and
  requires at least Lace 1.9.0. An installed Lace accepts only requests for
  its own minor version, so a request for 1.8.0 would not find Lace 1.9.0.

## [1.12.2] - 2026-11-07

This release fixes a hang in garbage collection.

### Fixed
- A worker that called `sylvan_gc` just after another worker's garbage
  collection had finished could wait forever for a collection that had
  already run. Programs that start garbage collection often from several
  workers at once could hang. Every earlier version has this bug. The new test
  `test_gc` checks that this no longer hangs.

## [1.12.1] - 2026-11-07

This release makes Sylvan easier to use as a subproject, as mCRL2 does, and
updates Lace to version 1.8.2, which makes the same changes on the Lace side.

### Changed
- Updated Lace to version 1.8.2. As a subproject, Lace no longer builds with
  `-march=native` by default; when Sylvan fetches Lace itself, Lace uses it
  only if `SYLVAN_NATIVE_OPT` is `ON`. Lace 1.8.2 also uses `rdtsc` instead
  of `rdtscp` on MSVC (some virtual CPUs lack `rdtscp`), builds with
  ThreadSanitizer on Clang, and allows building in the source directory.
- When Sylvan is a subproject (`add_subdirectory` or `FetchContent`), the C++
  bindings are now off by default, so a C project can use Sylvan without
  enabling C++. Set `SYLVAN_BUILD_CPP` to `ON` to build them.
- The tests are now controlled by `SYLVAN_BUILD_TESTS` (on by default only
  when Sylvan is the top-level project) instead of `BUILD_TESTING`. As a
  subproject, Sylvan no longer includes CTest and no longer adds its tests when
  the parent enables testing; set `SYLVAN_BUILD_TESTS` to `ON` to add them.

### Fixed
- Adding Sylvan as a subproject before the parent's `include(CTest)` no longer
  turns off the parent's tests (Sylvan cached `BUILD_TESTING` as `OFF`).
- Configuring no longer fails when the build directory is inside Sylvan's
  `src` directory: the generated `sylvan_config.h` now has its own file set.

## [1.12.0] - 2026-11-07

This release updates Lace to version 1.8.1, which backports the scheduling
changes of Lace 2.5.0 and, on ARM, no longer runs a memory fence on every
spawn. On the bddmc and lddmc benchmarks (Intel Xeon, 16 cores per socket),
Sylvan is 3-4% faster with 1 worker. With 8 to 16 workers it is 10-13% faster
with the saturation strategy and 2-4% faster with the par strategy, and with
24 to 32 workers on two sockets about 18% and 8%. Workers that have no work
now use much less CPU.

### Changed
- Updated Lace to version 1.8.1. An installed Lace must now be version 1.8.0
  or newer. This also fixes building Lace with newer Clang versions.
- On Linux, Lace now pins its workers to CPUs by default when they fill the
  CPUs the program may use. Set `LACE_PIN=off` to turn this off, for example
  when several Sylvan programs share a machine.
- Worker stacks are larger by default (a 1 GB reservation per worker on 64-bit
  systems), so deep recursions are less likely to overflow. The Lace CMake
  option `LACE_DONT_OVERCOMMIT` keeps the old size.
- The installed CMake package now requires Lace 1.8 (`find_dependency(lace
  1.8.0)`), like the pkg-config file. Configuring Sylvan with an unsupported
  Lace (older than 1.8.0, or 2.x) now stops with a clear message, also when a
  parent project provides Lace.

### Fixed
- Fixed `mtbdd_union_cube` dropping the existing MTBDD on the branch that the
  cube does not cover, when the cube's variable comes before the MTBDD's top
  variable or the MTBDD is a terminal.
- The help of the lddmc example now gives its actual default strategy (sat).

## [1.11.0] - 2026-07-17

### Added
- Added support for building and running Sylvan on Windows with MSVC, alongside
  the existing MSYS2 builds. Windows library builds are currently static.

### Changed
- Large internal allocations are cleared using virtual memory operations on
  supported platforms.
- Improved CMake integration, including true C-only builds, cleaner subproject
  behavior, and stronger installed-package tests.
- Cleaned up portability issues and compiler warnings across MSVC, GCC, and
  Clang.

### Fixed
- Fixed garbage collection on Windows.
- Fixed edge cases in MTBDD integer and fraction handling.
- Fixed ZDD reference handling and domain operations.

## [1.10.0] - 2026-03-31

This release contains a small API change in Lace, which will break things! If you use
the `TASK(foo)` macro, you will need to change it to `foo_CALL`. Lace 1.6.0 contains
backports from the new Lace version 2.3.0, which is the current development version.
It has some improvements with better support for Windows MSVC.

### Changed
- Updated Lace to version 1.6.0.


## [1.9.4] - 2026-03-17
### Changed
- Updated Lace to version 1.5.3

### Fixed
- Removed a few compiler warnings and build errors


## [1.9.3] - 2026-02-25
### Changed
- By default, Sylvan now sets a default build type (Release) when it is the top project
- Sylvan build options are now marked as advanced when Sylvan is not the top project


## [1.9.2] - 2026-02-24
### Changed
- The C++ wrapper is now optional and can be excluded from the library
- Update Lace to version 1.5.1 with some improved support for other platforms

### Fixed
- Added forgotten copy constructor to MtbddMap to ensure correct garbage collection
- Fix two memory leaks in the LDD implementation


## [1.9.1] - 2025-08-07
### Changed
- Improved implementation of `cache_get` and `cache_get` for systems with weak
  memory.
- The example programs that are only compiled if MEDDLY is installed are no
  longer supported and are removed from the library. They were only there for
  benchmarking and by now MEDDLY has changed a lot and maintaining these
  benchmark examples is not currently a priority.

### Fixed
- The `zdd_isop` test was faulty and is now functioning correctly.


## [1.9.0] - 2025-08-03
### Added
- New BDD operations: fast tests for `disjoint` and `subset`.
- Test scripts to validate the result of `make install`

### Changed
- Update Lace to version 1.5.0
- Slightly more efficient hash function for the cache.
- Various changes to improve the CMake build script.

### Fixed
- Fixed major concurrency bug in the cache.


## [1.8.1] - 2023-11-17
### Added
- Added CMake scripts for installing again.


## [1.8.0] - 2023-03-31
### Changed
- Now supports Windows (via MSYS2) and OSX.
- CI builds for Linux, OSX and Windows.
- Better support for systems that don't have mmap.
- Removed dependency on argp, by using `getopt_long` instead.
- Using a new version of Lace, with better cross-platform support.


## [1.7.1] - 2022-09-04
### Added
- Now complains when certain Sylvan functions are used outside a Lace worker
  thread.

### Changed
- Slightly streamlined `mtbdd_makenode` to generate more optimal code.
- Lace is now a dependency of Sylvan instead of included in the source code.


## [1.7.0] - 2022-09-03
### Added
- Rudimentary support for zero-suppressed decision diagrams (ZDDs). 
- Implemented irredundant sum-of-products `ISOP` that computes the ZDD cover of
  a BDD.

### Changed
- Another new version of Lace, using C11 atomics instead of the old abuse of
  the volatile keyword. This is a fairly major change and so far it even
  appears to improve performance a little bit.
- The name of Sylvan in CMake is now `sylvan` or `sylvan::sylvan`
- Possibly breaking change: `mtbdd_enum_first` and `mtbdd_enum_next` no longer
  check if `variables` covers all variables encountered in the decision
  diagram, allowing partial evaluation.
- Various small changes to the CMake scripts.

### Removed
- Support for Sylvan as a shared library is removed.

### Fixed
- Fixed OSX build
- Fixed warnings on modern gcc in `sha2.c`
- Fixed test and example programs that use Sylvan wrongly. Sylvan functions
  should always be executed from a Lace worker.
- A major performance regression introduced in version 1.6.0 has been fixed.


## [1.6.1] - 2021-06-20
### Changed
- New version of Lace with slightly better support for OSX and a few minor changes.

### Fixed
- A bug with `sylvan_set_limits` has been fixed.


## [1.6.0] - 2021-04-19
### Changed
- Major API change in Lace. This will break things!
- Lace is now started with `lace_start` and stopped with `lace_stop`. The
  initializing thread is no longer a Lace thread. Instead, starting Lace with N
  workers will simply start N threads. To temporarily halt the workers (because
  they use 100% CPU time), use `lace_suspend` and `lace_resume`.
- The macros `SPAWN`, `SYNC` and `CALL` only work from Lace tasks. The
  `LACE_ME` macro is gone. If you are not inside a Lace task, use the `RUN`
  macro to run a task.
- If you use `RUN` from a Lace task, this will automatically fallback to `CALL`
  with a little bit of overhead.
- CAREFUL: using `RUN` from an external thread is not safe w.r.t. garbage
  collection! If you invoke Sylvan operations from multiple external threads,
  then it may be possible that BDDs are not referenced (and put back into the
  table) in rare cases. The solution to this is to wrap each operation in a Lace
  task that stores the result in a properly referenced variable before leaving
  the Lace environment. The reason for this is that garbage collection will not
  run until all Lace workers cooperate.
- You can now run Sylvan from any thread. This means that in the future, it is
  easier to support bridges to Python and Java.


## [1.5.0] - 2019-09-05
### Added
- New tools `ldd2meddly` and `medmc` as part of research comparing the
  saturation algorithm in Sylvan to the one in Meddly.

### Changed
- Renamed the `mc` example program to `bccmc`.

### Fixed
- A bug in the GMP handling was fixed.


## [1.4.1] - 2018-06-14
### Changed
- We now implement twisted tabulation as the hash function for the nodes table.
  The old hash function is still available and the default behavior can be
  changed in `sylvan_table.h`.

### Removed
- Removed dependency on hwloc.


## [1.4.0] - 2017-07-12
### Added
- Function `mtbdd_cmpl` that computes the complement for MTBDDs. (0 becomes 1,
  non-0 becomes 0)

### Changed
- Changed file formats used by the examples to match the changes in LTSmin.
- Function `mtbdd_satcount` now does not count assignments leading to 0.
  Perhaps in the future we make this configurable. (Like in CuDD.)
- Slightly improved C++ support by wrapping header files in the namespace
  sylvan.

### Fixed
- There was a bug where Lace tasks are overwritten during SYNC, which causes
  problems during garbage collection. Lace reusing the bucket during SYNC is by
  design and is difficult to change. We fix the issue by checking during garbage
  collection if the stored task is still the same function, which in the worst
  case marks more nodes than needed.
- Band-aid patch for hashing; very similar nodes were hashing to similar
  positions and strides, causing early garbage collections and full tables. The
  patch works for now, but we need a more robust solution.

### Removed
- Removed support for HWLOC (pinning on NUMA machines). Planning to bring this
  back as an option, but in its current form it prevents multiple Sylvan
  programs from running simultaneously on the same machine.


## [1.3.3] - 2017-06-03
### Changed
- Changed file format for .bdd files in the MC example.

### Fixed
- A major bug in `lddmc_match_sat_par` has been fixed.
- A bug in the saturation algorithm in the model checking example has been fixed.
- A major bug in the hash table rehashing implementation has been fixed.


## [1.3.2] - 2017-05-23
### Added
- Now implements `lddmc_protect` and `lddmc_unprotect` for external pointer
  references.
- Now implements `lddmc_refs_pushptr` and `lddmc_refs_popptr` for internal
  pointer references

### Changed
- New version of Lace has slightly different API for manually created threads.


## [1.3.1] - 2017-05-22
### Fixed
- A bug in `mtbdd_refs_ptrs_up` caused a segfault. This has been fixed.


## [1.3.0] - 2017-05-16 ### Added
- The embedded work-stealing framework now explicitly checks for stack
  overflows and aborts with an appropriate error message written to stderr.
- New functions `sylvan_project` and `sylvan_and_project` for BDDs, a dual of
  existential quantification, where instead of the variables to remove, the
  given set of variables are the variables to keep.
- New functions `mtbdd_refs_pushptr` and `mtbdd_refs_popptr` allow
  thread-specific referencing of pointers.

### Changed
- Rewritten initialization of Sylvan. Before the call to `sylvan_init_package`,
  table sizes must be initialized either using `sylvan_set_sizes` or with the
  new function `sylvan_set_limits`. This new function allows the user to set a
  maximum number of bytes allocated for the nodes table and for the operation
  cache.
- Rewritten MTBDD referencing system.
- Rewritten MTBDD map and set functions (no API change except renaming
  `mtbdd_map_addall` to `mtbdd_map_update` with backward compatibility)
- The lock-free unique table now uses double hashing instead of rehashing. This
  can improve the performance for custom leaves and improves the hash spread.

### Fixed
- A bug in `llmsset_lookup` affecting custom leaves has been fixed.


## [1.2.0] - 2017-02-03
### Added
- Added documentation in the docs directory using Sphinx. Some documentation is
  removed from the README.md file.

### Changed
- The custom terminal/leaf API is slightly modified. The `read_binary_cb` has a
  different signature to remove the dependency upon MTBDD functionality.
- The custom terminal/leaf API functions have been renamed and moved to a
  separate file.
- Lace has been updated with a new version. The new version has rewritten the
  hardware locality code that pins worker threads and memory.

### Fixed
- A bug in `mtbdd_reader_readbinary` has been fixed.


## [1.1.2] - 2017-01-11
### Fixed
- The pkg-config file is slightly improved.
- A critical bug in `sylvan_collect` has been fixed.


## [1.1.1] - 2017-01-10
### Fixed
- The pkg-config file now includes hwloc as a requirement


## [1.1.0] - 2017-01-09
### Added
- This CHANGELOG file.
- Custom leaves can now implement custom callbacks for writing/reading to/from
  files.
- Implemented GMP leaf writing/reading to/from file.
- Method `mtbdd_eval_compose` for proper function composition (after partial
  evaluation).
- Method `mtbdd_enum_par_*` for parallel path enumeration.
- LDD methods `relprod` and `relprev` now support action labels (meta 5).
- Examples program `ldd2bdd` now converts LDD transition systems to BDDs
  transition systems.
- Methods `cache_get6` and `cache_put6` for operation cache entries that
  require two buckets.
- File `sylvan.pc` for pkg-config.

### Changed
- The API to register a custom MTBDD leaf now requires multiple calls, which is
  better design for future extensions.
- When rehashing during garbage collection fails (due to finite length probe
  sequences), Sylvan now increases the probe sequence length instead of
  aborting with an error message. However, Sylvan will probably still abort due
  to the table being full, since this error is typically triggered when garbage
  collection does not remove many dead nodes.

### Fixed
- Methods `mtbdd_enum_all_*` fixed and rewritten.

### Removed
- We no longer use both autoconf makefiles and CMake. Instead, we removed the
  autoconf files and rely solely on CMake now.
