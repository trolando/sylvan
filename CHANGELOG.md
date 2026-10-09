# Change Log
All notable changes to Sylvan will be documented in this file.

## [Unreleased]

### Added
- **Breaking (C).** Added root visitors, `sylvan_gc_add_roots` and
  `sylvan_gc_remove_roots`, replacing the marking hooks they remove (see
  Removed). A consumer whose roots have no stable address registers a hook that
  is called with a yield callback and hands its roots back as arrays of MTBDD
  *values*; Sylvan calls it once per garbage collection and once more when a
  reordering session opens. The point of yielding rather than marking is that
  roots can now be enumerated: a session pins what a visitor yields instead of
  refusing to open, so a consumer that owns roots this way can reorder. Sylvan's
  own two MTBDD root sources, the `mtbdd_protect` table and the per-worker
  temporary reference stacks, are registered through the same interface.
- Added `sylvan_sift_recommended` and `sylvan_gc_set_auto_sift`, an opt-in
  automatic reordering. The recommendation is one comparison per garbage
  collection against a number the collection already computes: the live forest
  has grown by `sylvan_sift_set_recommend_factor` (2.0 by default, 0 disables)
  since the last sift, or since startup. With a configuration installed, an
  explicit `sylvan_gc()` that ends recommended is followed by a sift in a frame
  of its own. It is off by default. `sylvan_sift_count` reports how many sifts
  have run, the way `sylvan_gc_count` reports collections.
- Added `sylvan_gc_collect`, the collection-only entry, and made it the one the
  internal table-full path uses. An automatic sift may follow `sylvan_gc()` and
  may never follow `sylvan_gc_collect()`, because the latter is called from the
  middle of an operation's recursion: that operation would resume and combine
  sub-results it computed under the previous rank order. The split makes the
  distinction a property of the code rather than of a comment.
- Added `sylvan_sift`, dynamic variable reordering for the BDD/MTBDD forest.
  It reorders when called, or after an explicit `sylvan_gc()` once the
  automatic sift above is switched on, and it stops the world for the
  duration, exactly like garbage collection, which it begins with. Every
  protected diagram survives, denotes the same function, and keeps its handle.
  `sylvan_sift_config` bounds what the search may spend and
  `sylvan_sift_report` says what it did; see `sylvan/reorder.h`. Sifting is
  refused, changing nothing, while the ZDD or ListDD subsystem is initialised,
  while garbage collection is disabled (a sift begins by collecting, which
  would drop handles a caller disabled collection to keep; the new
  `sylvan_gc_is_enabled` reports the setting), or on a nodes table without
  the reorder primitives: `llht` has none, while `trie`, `list`, `llcht` and
  `llchst` can reorder.
- Added `group_pairs` to `sylvan_sift_config`, which sifts every pair of
  identities (2k, 2k+1) as one block, so that each stays together with the even
  identity first: the layout `bdd_rel_next` and `bdd_rel_prev` are fastest
  under. A pair found apart is brought back together before anything else
  moves, only whole moves of a block are scored, and each swap of a move is
  made only while the table can promise the room to undo the move, so a
  refused swap does not leave a pair apart. `sylvan_sift_split_pairs` and the
  `split_pairs` field of `sylvan_sift_result` count the pairs that are apart.
  It is off by default, and an ungrouped sift is unchanged.
- Added the CMake option `SYLVAN_CHECK_HANDLES` (default off). With it, every
  decision diagram node dereference checks that the handle's bucket index is
  inside the nodes table and aborts naming the index when it is not, which
  points at the operation that produced a bad handle rather than the one that
  tripped over it. It is a debugging aid: builds without it are byte-for-byte
  identical to builds from before the option existed.
- Added ablation and instrumentation knobs for the reordering experiments, so
  that the measurements behind parallel sifting can be reproduced from this
  tree. Every default is the existing behaviour. The CMake option
  `SYLVAN_HUGEPAGES` (default on) can turn off the transparent huge page
  advice for large tables, to measure what the pages are worth. The internal
  `sylvan_reorder_set_batched(0)` makes every swap remove and re-index its
  nodes one at a time even where the table has the batched primitives, to
  measure what batching is worth. The CMake option `SYLVAN_SIFT_PHASE_TIMING`
  (default off) times every phase of every swap and of the session around
  them, and has every sift print the sums to stderr as one `SIFT-PHASES` line
  of JSON. With `SYLVAN_STATS`, every nodes table now counts the index steps
  and calls of its lookups, and of the removals and re-indexings a swap makes,
  so that `sylvan_stats_report` shows how much index each operation walks.
  The CMake option `SYLVAN_REGION_STRIPED_START` (default off) brings back the
  allocator the shared region cursor replaced, each worker filling a stripe of
  the table of its own, to measure what the cursor is worth. The CMake cache
  variable `SYLVAN_SIFT_RECOMMEND_FLOOR` (default 4096) sets the live-node
  floor of the sift recommendation, which was a compile-time constant, to
  measure the recommendation at other floors.

### Changed
- Sifting no longer destroys handles held only on a worker's temporary
  reference stack, and no longer refuses to run because a consumer owns roots
  of its own. A session pins every root a garbage collection would mark -- the
  same visitor registrations, walked in the same order -- so what a collection
  keeps, a sift keeps, at the same handle denoting the same function. The one
  remaining precondition is unchanged and unchangeable: no decision diagram
  operation may be in flight, because a suspended operation would resume and
  combine sub-results computed under a rank order that no longer holds.
- Parallel sifting now scales instead of losing to its own workers. Three
  changes compose: the reorder-capable tables (`trie`, `list`, `llcht` and
  `llchst`) take a swap's removals and re-indexings as whole batches, run as
  internally parallel phases with barriers where the trie's per-node lock
  used to serialise everything; the
  swap's parallel phases claim shared state per block instead of paying an
  atomic fetch-add per node; and retirement drops its dead edges from a
  parallel task over the deduplicated round. Measured on C880 with the list
  table, an eight-worker sift went from slower than one worker to about
  1.9 times faster, and the trie's eight-worker sift halved. A single-worker
  sift skips the batched calls, which are pure overhead with nothing to
  spread. The chained tables' batches came last and took a further 28%
  (`llcht`) and 18% (`llchst`) off their eight-worker sift.
- A nodes table that has run out of arena now says so on stderr, once per
  exhaustion episode and from every backend, instead of leaving only a
  failed allocation to show for it.
- Sifting now stops a variable's excursion as soon as a variable interaction
  bound proves that no position further in that direction can beat the best
  already seen, the way CUDD's sifting does. The bound is conservative, so the
  final size is the one the exhaustive walk finds, in about a third fewer
  swaps. Set `full_legs` in `sylvan_sift_config` to walk every leg to the end
  of the order anyway.
- The nodes table backends now hand out allocation regions from one shared
  cursor instead of starting each worker in its own stripe of the table, so
  node index order follows creation order across workers. The stripes made a
  heavily shared node created by a high-stripe worker the anchor of its whole
  fan-in in the backends that index by larger child, which every family walk
  then paid for: on C880 with eight workers the largest family was seven times
  longer than the same forest built serially, a serial variable swap on such a
  table ran 3x slower, and parallel sifting lost more time to the layout than
  it gained from the workers. With the cursor, a table built in parallel has
  the family structure of a serial build, and the parallel build itself is
  slightly faster. `llht`, which cannot reorder, keeps its stripes.
- **Breaking (C++).** The C++ bindings in `sylvan/obj.hpp` now report a failed
  operation by throwing `sylvan::sylvan_error`, which carries the negative
  `SYLVAN_ERR_*` status and its `sylvan_status_string` description. Previously
  every failure was silently turned into a `Bdd`/`Mtbdd` holding
  `mtbdd_invalid`, which callers had no way to detect and which crashed when it
  was later dereferenced. No wrapper operation can produce an object holding
  `mtbdd_invalid` any more. The operand of a failed operation is unchanged,
  including for the in-place operators. `SatCountU64` and the static members of
  `Sylvan` still report failure through their return values.

### Fixed
- `bdd_rel_next`, `bdd_rel_prev` and `bdd_transitive_closure` now compute the
  right result after a sift has separated a pair (2k, 2k+1) or ranked its primed
  variable first. Their recursive kernels fuse the product, the quantification
  and the renaming, which is only valid while every pair in play sits on
  adjacent ranks with 2k on top, and they returned wrong results (or diagrams
  out of order) without complaint when it did not. Each call now checks that
  once, against an answer cached per order generation so that the check costs a
  load while the order is unchanged, and uses the kernel when it may; otherwise
  it takes a slower route that does not care where a variable is ranked:
  `bdd_and_exists` followed by a renaming through `bdd_compose` for the image
  and the preimage, a recursion that fixes partners by composition for the
  composition of two relations, and repeated squaring for the closure.
- Variables are now named by identity where callers mean "variable i". Added
  `bdd_var`, which returns the variable with a given identity, the same one
  before and after any reordering. The C++ `Bdd(uint32_t)` and `Bdd::bddVar`,
  documented as taking the variable index, called `bdd_var_at_level` and so
  returned a different variable after a sift, and `BddSet::add`,
  `BddSet::fromArray` and `Bdd::Permute` with them; they now use `bdd_var`, and
  `Bdd::bddVarAtLevel` is the explicit position API, as `bdd_var_at_level`
  remains in C. `Bdd::VariablesCube` is now valid for identities in any order
  and under any variable order.
- The transitive closure was wrong for most relations, independently of the
  variable order: the closure of a relation that flips one bit came out empty.
  The recursion combined its four blocks with the formulas for the reflexive
  closure while computing the transitive one. It now uses the block formulas
  for the transitive closure.
- `bdd_rel_prev` recursed into `bdd_rel_next` for a variable outside the pairs
  that both operands depend on, composing that part the wrong way round.
- `bdd_compose` built a diagram out of order when a replacement function
  depended on a variable ranked above an unreplaced node of the input, for
  example when renaming z to x in y & z. It now places such a node with ITE.
- A reorder session no longer runs out of node slots on a table that is almost
  entirely free, and no longer aborts on an allocation `nodes_reserve` had
  promised it. Both came from region ownership. A worker claims a 512-slot
  region and used to keep it marked owned for good, so the free slots inside it
  were reachable by that one worker and by nobody else, which is why
  `nodes_reserve` deliberately does not count them. A garbage collection resets
  every region, but a session may not collect and is the only thing that
  releases individual slots, so region by region a long session made its own
  released slots invisible to the count and unreachable by the allocator: with
  20 workers, reserve came to refuse a request for a hundred slots on a table
  with four million free ones. The swap then reported `SYLVAN_ERR_OOM` and
  `sylvan_sift` left a variable at a position it had already measured and
  rejected, so the forest could come out bigger than it went in. The same
  ownership made the bound optimistic in the other direction, because it assumed
  every counted slot could be reached: W workers can each claim a region and
  leave it partly used, stranding up to a region apiece, which is how an
  allocation reserve had promised could still be refused. A worker now gives a
  region back once it has used the last free slot in it -- at its next
  allocation, which then claims another -- so an owned region is exactly one
  worker's current region, and `nodes_reserve` asks for `count` plus one whole
  region per worker, which is what makes the promise hold however the workers
  divide the allocations between them. Sifting with a single worker was never
  affected. `llht`, which has no `nodes_reserve`, is unchanged.
- A cleared operation cache no longer answers a query with a result from before
  the clear. `cache_clear` zeroes only the status words -- the operands and
  results stay in place, which is what lets those pages go back to the operating
  system -- and `cache_get3` accepted a bucket on its hash bits alone. A zeroed
  status word carries zero hash bits, so the one query in 16384 whose own top
  hash bits are zero passed that test, compared the surviving key, and on a
  match was answered with a result computed before the last garbage collection,
  naming a handle that had since been reused for another node. The status word
  now carries a live bit that every put sets and every accessor requires, so an
  emptied bucket matches nothing; the hash field narrows from 14 to 13 bits to
  make room. `cache_get4` was already immune, because it also requires the bit
  that marks a two-bucket entry.
- The node and leaf counters, the `.dot` writers and the structural hashes now
  check each root they are given once, on entry, and return a defined result --
  0 for counts, no output for the writers, an empty string for the hashes --
  when a root's index is outside the nodes table. Previously such a root was
  dereferenced anyway: `mtbdd_invalid` masks to index `0xffffffffff`, which is
  terabytes past the table. Operations that compute decision diagrams keep the
  "garbage in, undefined out" contract, so nothing is checked per node; see
  "Root validation at traversal entry points" in `sylvan/mtbdd.h`.
- Recursive traversals -- the node and leaf counters, the `.dot` writers and
  the structural hashes -- no longer store their visited mark in the node's own
  record. That bit lived in the word the unique table hashes and compares, so
  for the length of a traversal the key of every visited node was wrong and a
  lookup on another worker could fail to find a node that was there and intern
  a duplicate of it. The mark now lives in a dense scratch bitmap beside the
  node table. Results are unchanged; only the mechanism is.

### Removed
- **Breaking (C).** Removed the garbage collection marking hooks,
  `sylvan_gc_add_mark` and `sylvan_gc_mark_hook_count`. There is no deprecation
  shim. A callback that marks can keep roots alive but cannot say what they
  are, which is why a consumer that used one could never be reordered around;
  register a `sylvan_gc_add_roots` visitor instead and yield the values the
  callback used to mark. A visitor that yields exactly what the old callback
  marked is behaviourally identical for garbage collection, and additionally
  works with `sylvan_sift`. Consumers whose roots are ZDD or ListDD handles
  have no visitor equivalent -- the values are MTBDD handles -- and must use
  `zdd_protect` or `listdd_protect`; neither family can be reordered, so the
  enumeration the visitor exists for would buy them nothing.
- Removed the internal node accessors `mtbddnode_getmark`/`setmark`,
  `zddnode_getmark`/`setmark` and `mddnode_getmark`/`setmark` (and the
  `bddnode_` aliases). The record bits they used are now unused: bit 61 for
  MTBDD, bit 60 for ZDD and bit 62 for ListDD.
- Removed the by-value external reference API: `mtbdd_ref`, `mtbdd_deref`,
  `mtbdd_ref_count`, `listdd_ref`, `listdd_deref` and `listdd_ref_count`, along
  with the two garbage collection mark passes that scanned their tables. Use
  `mtbdd_protect`/`mtbdd_unprotect` and `listdd_protect`/`listdd_unprotect` for
  roots with a stable address, the per-worker `mtbdd_refs_pushptr`/`popptr`
  stacks for local variables, or a `sylvan_gc_add_roots` visitor for roots
  that have neither. The per-worker temporary reference stacks are a separate
  mechanism and are unaffected.

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
