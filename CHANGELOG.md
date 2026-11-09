# Change Log
All notable changes to Sylvan will be documented in this file.

## [Unreleased]

### Added
- `sylvan_gc_collect()`, the garbage collection Sylvan starts itself when the
  nodes table is full. It resizes with the main gc hook, as before, and never
  shrinks the tables. Programs that want to collect garbage call `sylvan_gc()`.
- For code that works with the nodes table directly (`sylvan_table.h`):
  `llmsset_shrink()`, and `llmsset_get_data_size()`, the range of node
  indices in use. After a shrink, live nodes can have indices at or above
  `llmsset_get_size()` (the size of the hash table), but always below
  `llmsset_get_data_size()`.

### Changed
- `sylvan_gc()` now shrinks the nodes table to fit the live nodes: to the
  smallest size, from the initial size up, with room for twice the live
  nodes, and the operation cache in proportion. Nodes keep their place, so the
  nodes array keeps the buckets up to the highest live node, but the memory of
  its empty stretches goes back to the operating system. The main gc hook is
  not called for these collections. The automatic collections, when the nodes
  table is full, grow the tables as before. After dropping about a million
  nodes, `sylvan_gc()` took a program from 62 MB to 3 MB resident.
- `mtbdd_protect`, `lddmc_protect`, `zdd_protect`, their `unprotect`
  functions, and `mtbdd_ref`, `lddmc_ref` and their `deref` functions no
  longer make all workers contend on one shared counter. The tables count
  active calls so that a resize can wait for them; each Lace worker now counts
  its calls in a counter of its own, and other threads use the shared counter
  as before. With 8 workers that each protect and unprotect a variable in a
  loop, a pair costs 45 ns of CPU time instead of 3.5 µs; with 1 worker it
  costs the same as before (29 ns). Programs that create and destroy C++
  objects holding decision diagrams on many workers at once gain the most.

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
