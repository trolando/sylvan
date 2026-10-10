Sylvan
=====================

Sylvan is a parallel (multi-core) MTBDD library written in C. Sylvan
implements parallelized operations on BDDs, MTBDDs and LDDs. Both
sequential and parallel BDD-based algorithms can benefit from
parallelism. Sylvan uses the work-stealing framework Lace and parallel
datastructures to implement scalable multi-core operations on decision
diagrams.

Sylvan is developed (© 2011-2016) by the `Formal Methods and
Tools <http://fmt.ewi.utwente.nl/>`__ group at the University of Twente
as part of the MaDriD project, which is funded by NWO, and (© 2016-2017)
by the `Formal Methods and Verification <http://fmv.jku.at/>`__ group at
the Johannes Kepler University Linz as part of the RiSE project. Sylvan
is licensed with the Apache 2.0 license.
The main author of the project is Tom van Dijk who can be reached via
tom@tvandijk.nl.
Please let us know if you use Sylvan in your projects and if you need
decision diagram operations that are currently not implemented in Sylvan.

The main repository of Sylvan is https://github.com/trolando/sylvan. A
mirror is available at https://github.com/utwente-fmt/sylvan.

Bindings for other languages than C/C++ also exist:

-  Java/JNI bindings: https://github.com/utwente-fmt/jsylvan
-  Haskell bindings: https://github.com/adamwalker/sylvan-haskell
-  Python bindings: https://github.com/johnyf/dd

Dependencies
------------

Sylvan has the following dependencies:

- **CMake** for compiling.
- **gmp** (``libgmp-dev``) for the GMP leaves in MTBDDs.
- **Sphinx** if you want to build the documentation.

Sylvan depends on the `work-stealing framework
Lace <http://fmt.ewi.utwente.nl/tools/lace>`__ for its implementation.
Lace is embedded in the Sylvan distribution.
Lace requires one additional library:

- **hwloc** (``libhwloc-dev``) for pinning worker threads to processors.


Building
--------

It is recommended to build Sylvan in a separate build directory:

.. code:: bash

    mkdir build
    cd build
    cmake ..
    make && make test

It is recommended to use ``ccmake`` to configure the build settings of Sylvan. These include, e.g.,
enabling gathering of statistics and selecting whether you want a ``Debug`` or a ``Release`` build.
The resulting artifact is a static library residing at ``build/src/libsylvan.a``.

Diagnostic builds
-----------------

Sylvan has a few switches that exist only to debug or to measure Sylvan
itself. Each of them defaults to what a production build wants and costs
nothing in that setting, and none of them should be changed in a production
build.

``SYLVAN_CHECK_HANDLES`` (CMake option)
    Check on every node dereference that the decision diagram handle's bucket
    index is inside the nodes table, and abort naming the offending index when
    it is not. An invalid handle otherwise reads memory far outside the table,
    because the index is masked to 40 bits and scaled, so the failure surfaces
    as an unrelated crash somewhere later. Enable this to find the operation
    that produced the bad handle rather than the one that tripped over it.

    .. code:: bash

        cmake .. -DSYLVAN_CHECK_HANDLES=ON

    The reporting and debugging entry points -- the node and leaf counters, the
    ``.dot`` writers and the structural hashes -- already reject an invalid
    root without this option, and return a defined result instead. Operations
    that compute decision diagrams deliberately do not check, so that the check
    is never paid per node on a hot path.

``SYLVAN_GC_DIAGNOSTICS`` (compile definition)
    Write one line to stderr for every garbage collection, reporting the
    trigger site, the number of live nodes, and the table and cache sizes
    before and after the resize hook ran.

    .. code:: bash

        cmake .. -DCMAKE_C_FLAGS=-DSYLVAN_GC_DIAGNOSTICS=1

``SYLVAN_STATS`` (CMake option)
    Collect operation statistics. See ``sylvan_stats_report`` below.

    Among the statistics are the index counters of the nodes table, which say
    how much of its index each operation walks. ``sylvan_stats_report`` prints
    them as ``Unique lookup``, ``Unique remove`` and ``Unique reinsert`` steps
    and calls. A call is one lookup, or one removal or re-indexing of a node,
    which is what a variable swap does to the index. A step is one index word a
    walk loads after its first: a chain link followed (list, llcht, llchst), a
    trie level descended (trie), a further probe (llht). Every walk starts by
    loading one first word -- its chain head, family root or home bucket -- so
    the index loads per operation are ``1 + steps / calls``. On the trie,
    releasing a retired node repeats the descent that removed it, to check that
    it is out of the index, and is counted with the removals; the region a trie
    removal rebuilds below the node is not counted. llht neither removes nor
    re-indexes. The counters are per worker and summed when the report is
    made, so they cost an increment per step, and nothing in a build without
    statistics.

``SYLVAN_HUGEPAGES`` (CMake option, on by default)
    Ask the kernel to back every allocation of 2 MiB or more -- the nodes table
    and the operation cache above all -- with transparent huge pages, through
    ``madvise(MADV_HUGEPAGE)``. Linux only, and the kernel is free to decline
    (it does when ``transparent_hugepage`` is ``never``). The hash-keyed tables
    are addressed at random over their whole extent, so once a table outgrows
    the TLB every lookup would otherwise pay a page walk on top of its cache
    miss. Turn it off to measure what the pages are worth:

    .. code:: bash

        cmake .. -DSYLVAN_HUGEPAGES=OFF

``SYLVAN_SIFT_PHASE_TIMING`` (CMake option)
    Time every phase of every variable swap, and of the reorder session around
    them, with the monotonic clock, and have every sift that opens a session
    print the sums to stderr as one line of JSON:

    .. code:: text

        SIFT-PHASES {"swaps":...,"gc":...,"seed":...,"scatter":...,"pin":...,
                     "begin_total":...,"classify":...,"intern":...,"rewrite":...,
                     "reinsert":...,"retire":...,"release":...,"swap_total":...,
                     "end":...,"interact":...,"popinit":...,"sift_total":...,
                     "remove_batch":...,"reinsert_batch":...}

    (on one line). Every value but ``swaps`` is wall-clock seconds summed over
    the sift. ``gc``, ``seed``, ``scatter`` and ``pin`` open the session and
    ``begin_total`` is all of the opening; ``classify``, ``intern``,
    ``remove_batch``, ``rewrite``, ``reinsert_batch``, ``reinsert``, ``retire``
    and ``release`` are the phases of a swap in the order they run, the two
    batch fields staying near zero when the swaps are not batched, and
    ``swap_total`` is the swaps end to end; ``end`` closes the session;
    ``popinit`` and ``interact`` are the driver counting populations and
    building the interaction matrix, and ``sift_total`` is the whole sift.
    Inside the library the session's share is ``sylvan_reorder_phase_timing``
    in ``sylvan/internal.h``. The clock is read twice per phase per swap, so
    take the breakdown from this build and the totals from one without it.

    .. code:: bash

        cmake .. -DSYLVAN_SIFT_PHASE_TIMING=ON

``SYLVAN_REGION_STRIPED_START`` (CMake option)
    Allocate node slots in per-worker stripes instead of from the shared
    region cursor, in the list, trie, llcht and llchst tables. A worker's
    first search for an allocation region, after the table is created and
    after every garbage collection, starts in that worker's own stripe of the
    table, and every later search goes on past the region the worker has just
    used up, so each worker fills a stripe of its own and the cursor is not
    consulted at all. Node indices then do not follow creation order across
    workers. It changes where nodes are placed in the table and nothing else,
    and exists to measure what the shared cursor is worth. The shape tests of
    ``test_table`` need indices in creation order, and skip themselves with a
    notice when they find a table without it.

    .. code:: bash

        cmake .. -DSYLVAN_REGION_STRIPED_START=ON

Using Sylvan
------------

To use Sylvan, the library and its dependency Lace must be initialized:

.. code:: c

    #include <sylvan.h>

    main() {
        int n_workers = 0; // auto-detect
        lace_start(n_workers, 0);

        // use at most 512 MB, nodes:cache ratio 2:1, initial size 1/32 of maximum
        sylvan_set_limits(512*1024*1024, 1, 5);
        sylvan_init_package();
        sylvan_init_mtbdd();

        /* ... do stuff ... */

        sylvan_stats_report(stdout);
        sylvan_quit();
        lace_stop();
    }

The call to ``lace_start`` initializes the Lace framework, setting up the necessary data structures
for work-stealing. The function also spawns ``n_workers`` worker threads that will execute all
Sylvan tasks. Setting ``n_workers`` to 0 means that the number of workers will be auto-detected.
Lace must be started before Sylvan can be initialized. The worker threads will busy-wait until a
task is offered, e.g. `mtbdd_applyp` is called, from a non-worker (main) thread. The worker threads
can be suspended by calling ``lace_suspend``, preventing high CPU utilization when the application
using Sylvan has no \*BDD-related work to do. The worker threads can be later resumed by calling
``lace_resume``.  Calling ``lace_stop`` terminates all the worker threads.

Sylvan is initialized with a call to ``sylvan_init_package``. Before this call, Sylvan needs to know
how much memory to allocate for the nodes table and the operation cache. In this example, we use the
``sylvan_set_limits`` function to tell Sylvan that it may allocate at most 512 MB for these tables.
The second parameter indicates the ratio of the nodes table and the operation cache, with each
higher number doubling the size of the nodes table. Negative numbers double the size of the operation
cache instead. In the example, we want the nodes table to be twice as big as the operation cache.
The third parameter controls how often garbage collection doubles the table sizes before
their maximum size is reached. The value 5 means that the initial tables are 32x as small as the maximum size.
By default, every execution of garbage collection doubles the table sizes.

After ``sylvan_init_package``, subpackages like ``mtbdd`` and ``ldd`` can be initialized with
``sylvan_init_mtbdd`` and ``sylvan_init_ldd``. This allocates auxiliary datastructures.

If you enabled statistics generation (via CMake), then you can use ``sylvan_stats_report`` to report
the obtained statistics to a given ``FILE*``.

The Lace framework
~~~~~~~~~~~~~~~~~~

Sylvan uses the Lace framework to offer 'automatic' parallelization of decision diagram operations.
Many functions in Sylvan are Lace tasks that can be executed only from lace (worker) threads.
Functions provided by Sylvan are internally wrapped in a ``RUN`` macro that will offer a task to
lace workers and block the execution of the thread calling the function, thus, no special handling
is required when calling Sylvan functions.

Garbage collection and referencing nodes
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Like all decision diagram implementations, Sylvan performs garbage collection.
Garbage collection is triggered when trying to insert a new node and no
empty space can be found in the table within a reasonable upper bound.

Garbage collection can be disabled with ``sylvan_gc_disable`` and enabled again with ``sylvan_gc_enable``;
``sylvan_gc_is_enabled`` reports which. Sifting is refused while collection is disabled.
Call ``sylvan_gc`` to manually trigger garbage collection.

To ensure that no decision diagram nodes are overwritten, you must ensure that
Sylvan knows which decision diagrams you care about.
Each subpackage implements mechanisms to store references to decision diagrams that must be kept.
For example, the *mtbdd* subpackage implements ``mtbdd_protect`` and ``mtbdd_unprotect`` to store pointers to
MTBDD variables.

.. code:: c

    MTBDD* allocate_var() {
        MTBDD* my_var = (MTBDD*)calloc(sizeof(MTBDD), 1);
        mtbdd_protect(my_var);
        return my_var;
    }

    free_var(MTBDD* my_var) {
        mtbdd_unprotect(my_var);
        free(my_var);
    }

If you use ``mtbdd_protect`` you do not need to update the reference every time the value changes.

The *mtbdd* subpackage also implements a thread-local stack to temporarily store pointers.
Result-producing tasks write to protected destinations, so spawned results are rooted before the task can run:

.. code:: c

    MTBDD some_thing = ...;
    mtbdd_refs_pushptr(&some_thing);
    MTBDD result_param1 = mtbdd_invalid, result_param2 = mtbdd_invalid;
    mtbdd_refs_pushptr(&result_param1);
    mtbdd_refs_pushptr(&result_param2);
    while (some_condition) {
        an_operation_SPAWN(lace, &result_param1, some_thing, param1);
        int status = an_operation_CALL(lace, &result_param2, some_thing, param2);
        int spawned_status = an_operation_SYNC(lace);
        if (status != SYLVAN_OK || spawned_status != SYLVAN_OK) break;
        status = another_operation_CALL(lace, &some_thing, result_param1, result_param2);
        if (status != SYLVAN_OK) break;
    }
    mtbdd_refs_popptr(3);
    return some_thing;

It is recommended to use the thread-local pointer stack for local variables, and to use the ``protect`` and
``unprotect`` functions for other variables.

Bindings without stable addresses
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Both mechanisms above take the *address* of the variable holding the decision diagram, so they
need that address to stay put. A language binding whose values live in a moving or copying heap,
or in a container that reallocates, has no such address to hand over. For those, register a
**root visitor** with ``sylvan_gc_add_roots`` and yield whatever you consider a root. Sylvan calls
the hook whenever it needs to know what your roots are -- once per garbage collection and once
more when a reordering session opens -- with all workers stopped.

The hook yields *values*, in arrays, so a binding that already holds its roots contiguously hands
over the whole block in one call:

.. code:: c

    static MTBDD *handles;      // your own storage, may move or grow
    static size_t handle_count;

    static void visit_handles(void *context, sylvan_roots_yield yield, void *arg)
    {
        (void)context;
        yield(arg, handles, handle_count);
    }

    // once, after mtbdd_init()
    sylvan_gc_add_roots(visit_handles, NULL);

A binding whose roots are not contiguous fills a small buffer of its own and flushes it, calling
``yield`` as often as it likes; ``count`` may be zero. The array is borrowed for the duration of
the call. The hook may do nothing but yield: no creating, protecting or unprotecting diagrams, and
no registering or removing visitors. ``sylvan_gc_remove_roots`` takes a registration back.

Garbage collection keeps the roots a visitor yields and ``sylvan_sift`` pins them, so a consumer
with a handle table can reorder. Sylvan's own two root sources -- the ``mtbdd_protect`` table and
the per-worker temporary stacks -- are registered through the same interface.

The values are MTBDD (and therefore BDD) handles. ZDD and ListDD roots are not covered; use
``zdd_protect`` and ``listdd_protect``, which those families need anyway.

References to decision diagrams must be added before a worker may cooperate on garbage collection.
Workers can cooperate on garbage collection during ``SYNC`` and when functions create nodes or use ``sylvan_gc_test`` to test whether to assist in garbage collection.
Functions for adding or removing references never perform garbage collection.
Furthermore, only the ``mtbdd_makenode`` function (and other node making primitives) implicitly reference their parameters; all other functions do not reference their parameters.
Nesting Sylvan functions (including ``sylvan_ithvar``) is bad practice and should be avoided.

**Warning**: Sylvan is a multi-threaded library and all workers must cooperate for garbage collection. If you use locking mechanisms in your code, beware of deadlocks!
You can explicitly cooperate on garbage collection with ``sylvan_gc_test()``.

Basic BDD/MTBDD functionality
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

In Sylvan, BDDs are special cases of MTBDDs.
Several functions are specific for BDDs and they start with ``sylvan_``, whereas generic MTBDD functions start
with ``mtbdd_``.

To create new BDDs, you can use:

- ``mtbdd_true``: representation of constant ``true``.
- ``mtbdd_false``: representation of constant ``false``.
- ``bdd_var(&result, var)``: the variable with identity <var> (negate it with ``bdd_not``).
- ``bdd_var_at_level(&result, level)``: the variable currently at position <level> in the variable order.
- ``bdd_new_var(&result)``: a variable with a new identity, placed last in the order.

A variable has an *identity*, the number it was created with, which every node over it
stores and which never changes, and a *position* in the variable order, which dynamic
reordering changes. Everything that takes a variable -- ``bdd_var``, variable sets, maps,
the relational products -- takes identities, so what a program means does not depend on
the order; only ``bdd_var_at_level`` takes a position. Arrays of values that go with a
variable set, such as cubes and assignments, are packed in the set's current order, which
``bdd_set_to_array`` writes out. Without reordering, identity and position coincide.

To follow the BDD edges and obtain the variable at the root of a BDD,
you can use (only for internal nodes, not for leaves ``mtbdd_true`` and ``mtbdd_false``):

- ``mtbdd_getvar(bdd)``: obtain the variable of the root node of <bdd>.
- ``mtbdd_gethigh(bdd)``: follow the high edge of <bdd>.
- ``mtbdd_getlow(bdd)``: follow the low edge of <bdd>.

You need to manually reference BDDs that you want to keep during garbage
collection (see the above explanation):

- ``mtbdd_protect(bddptr)``: add a pointer reference to <bddptr>.
- ``mtbdd_unprotect(bddptr)``: remove a pointer reference to <bddptr>.
- ``mtbdd_refs_pushptr(bddptr)``: add a local pointer reference to <bddptr>.
- ``mtbdd_refs_popptr(amount)``: remove the last <amount> local pointer references.

It is recommended to use ``mtbdd_protect`` and ``mtbdd_unprotect``.
The C++ objects (defined in ``sylvan_obj.hpp``) handle this automatically.
For local variables, we recommend ``mtbdd_refs_pushptr`` and ``mtbdd_refs_popptr``.

The following basic BDD operations are implemented:

- ``sylvan_not(bdd)``: compute the negation of <bdd>.
- ``sylvan_ite(a,b,c)``: compute 'if <a> then <b> else <c>'.
- ``sylvan_and(a, b)``: compute '<a> and <b>'.
- ``sylvan_or(a, b)``: compute '<a> or <b>'.
- ``sylvan_nand(a, b)``: compute 'not (<a> and <b>)'.
- ``sylvan_nor(a, b)``: compute 'not (<a> or <b>)'.
- ``sylvan_imp(a, b)``: compute '<a> then <b>'.
- ``sylvan_invimp(a, b)``: compute '<b> then <a>'.
- ``sylvan_xor(a, b)``: compute '<a> xor <b>'.
- ``sylvan_equiv(a, b)``: compute '<a> = <b>'.
- ``sylvan_diff(a, b)``: compute '<a> and not <b>'.
- ``sylvan_less(a, b)``: compute '<b> and not <a>'.
- ``sylvan_exists(bdd, vars)``: existential quantification of <bdd> with respect to variables <vars>.
- ``sylvan_forall(bdd, vars)``: universal quantification of <bdd> with respect to variables <vars>.
- ``sylvan_project(bdd, vars)``: the dual of ``sylvan_exists``, projects the <bdd> to the variable domain <vars>.

A set of variables (like <vars> above) is a BDD representing the conjunction of the variables.
A number of convencience functions are defined to manipulate sets of variables:

- ``mtbdd_set_empty()``: obtain an empty set.
- ``mtbdd_set_isempty(set)``: compute whether the set is empty.
- ``mtbdd_set_first(set)``: obtain the first variable of the set.
- ``mtbdd_set_next(set)``: obtain the subset without the first variable.
- ``mtbdd_set_from_array(arr, len)``: create a set from a given array.
- ``mtbdd_set_to_array(set, arr)``: write the set to the given array.
- ``mtbdd_set_add(set, var)``: compute the set plus the variable.
- ``mtbdd_set_union(set1, set2)``: compute the union of two sets.
- ``mtbdd_set_remove(set, var)``: compute the set minus the variable.
- ``mtbdd_set_minus(set1, set2)``: compute the set <set1> minus the variables in <set2>.
- ``mtbdd_set_count(set)``: compute the number of variables in the set.
- ``mtbdd_set_contains(set, var)``: compute whether the set contains the variable.

Sylvan also implements composition and substitution/variable renaming using a "MTBDD map". An MTBDD map is a special structure
implemented with special MTBDD nodes to store a mapping from variables (uint32_t) to MTBDDs. Like sets of variables and MTBDDs, MTBDD maps must
also be referenced for garbage collection. The following functions are related to MTBDD maps:

- ``mtbdd_compose(dd, map)``: apply the map to the given decision diagram, transforming every node with a variable that is associated with some function F in the map by ``if <F> then <high> else <low>``.
- ``sylvan_compose(dd, map)``: same as ``mtbdd_compose``, but assumes the decision diagram only has Boolean leaves.
- ``mtbdd_map_empty()``: obtain an empty map.
- ``mtbdd_map_isempty(map)``: compute whether the map is empty.
- ``mtbdd_map_key(map)``: obtain the key of the first pair of the map.
- ``mtbdd_map_value(map)``: obtain the value of the first pair of the map.
- ``mtbdd_map_next(map)``: obtain the submap without the first pair.
- ``mtbdd_map_add(map, key, value)``: compute the map plus the given key-value pair.
- ``mtbdd_map_update(map1, map2)``: compute the union of two maps, with priority to map2 if both maps share variables.
- ``mtbdd_map_remove(map, var)``: compute the map minus the variable.
- ``mtbdd_map_removeall(map, set)``: compute the map minus the given variables.
- ``mtbdd_map_count(set)``: compute the number of pairs in the map.
- ``mtbdd_map_contains(map, var)``: compute whether the map contains the variable.

Sylvan implements a number of counting operations:

- ``mtbdd_satcount(bdd, number_of_vars)``: compute the number of minterms (assignments that lead to True) for a function with <number_of_vars> variables; we don't need to know the exact variables that may be in the BDD, just how many there are.
- ``sylvan_pathcount(bdd)``: compute the number of distinct paths to True.
- ``mtbdd_nodecount(bdd)``: compute the number of nodes (and leaves) in the BDD.
- ``mtbdd_nodecount_more(array, length)``: compute the number of nodes (and leaves) in the array of BDDs.

Sylvan implements various advanced operations:

- ``sylvan_and_exists(bdd_a, bdd_b, vars)``: compute ``sylvan_exists(sylvan_and(bdd_a, bdd_b), vars)`` in one step.
- ``sylvan_and_project(bdd_a, bdd_b, vars)``: compute ``sylvan_project(sylvan_and(bdd_a, bdd_b), vars)`` in one step.
- ``sylvan_cube(vars, values)``: compute a cube (to leaf True) of the given variables, where the array values indicates for each variable whether to use it in negative form (value 0) or positive form (value 1) or to skip it (as dont-care, value 2).
- ``sylvan_union_cube(set, vars, values)``: compute ``sylvan_or(set, sylvan_cube(vars, values))`` in one step.
- ``sylvan_constrain(bdd_f, bdd_c)``: compute the generic cofactor of F constrained by C, i.e, set F to False for all assignments not in C.
- ``sylvan_restrict(bdd_f, bdd_c)``: compute Coudert and Madre's restrict algorithm, which tries to minimize bdd_f according to a care set C using sibling substitution; the invariant is ``restrict(f, c) \and c == f \and c``; the result of this algorithm is often but not always smaller than the original.
- ``sylvan_pick_cube(bdd)`` or ``sylvan_sat_one_bdd(bdd)``: extract a single path to True from the BDD (returns the BDD of this path)
- ``sylvan_pick_single_cube(bdd, vars)`` or ``sylvan_sat_single(bdd, vars)`` extracts a single minterm from the BDD (returns the BDD of this assignment)
- ``sylvan_sat_one(bdd, vars, array)``: extract a single minterm from the BDD given the set of variables and write the values of the variables in order to the given array, with 0 when it is negative, 1 when it is positive, and 2 when it is dontcare.

Sylvan implements several operations for transition systems. These operations pair the variables by identity: the *source* or *unprimed* variable 2k with the *target* or *primed* variable 2k+1, so unprimed variables have even parity (0, 2, 4...) and their primed partners odd parity (1, 3, 5...).
They are fastest when each pair in play is adjacent in the variable order with its unprimed variable first, which holds without reordering and after a reordering that keeps pairs together; under any other order they compute the same result by a slower route.
The transition relations may be partial transition relations that only manipulate a subset of variables; hence, the operations also require the set of variables.

- ``sylvan_relnext(set, relation, vars)``: apply the (partial) relation on the given variables to the set.
- ``sylvan_relprev(relation, set, vars)``: apply the (partial) relation in reverse to the set; this computes predecessors but can also concatenate relations as follows: ``sylvan_relprev(rel1, rel2, rel1_vars)``.
- ``sylvan_closure(relation)``: compute the transitive closure of the given set recursively (see Matsunaga et al, DAC 1993)

See ``src/sylvan_bdd.h`` and ``src/mtbdd.h`` for other operations on BDDs and MTBDDs.

Custom leaves
~~~~~~~~~~~~~

See ``src/sylvan_mt.h`` and the example in ``src/sylvan_gmp.h`` and ``src/sylvan_gmp.c`` for custom leaves in MTBDDs.

Custom decision diagram operations
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Adding custom decision diagram operations is easy. Include ``sylvan/internal.h`` for the advanced functions. See ``sylvan/cache.h``
for how to use the operation cache.

List decision diagrams
~~~~~~~~~~~~~~~~~~~~~~

See ``src/sylvan_ldd.h`` for operations on list decision diagrams.

File I/O
~~~~~~~~

You can store and load BDDs using a number of methods, which are documented in the header files ``sylvan_mtbdd.h`` and ``sylvan_ldd.h``.

Support for C++
~~~~~~~~~~~~~~~

See ``src/sylvan_obj.hpp`` for the C++ interface.

Table resizing
~~~~~~~~~~~~~~

During garbage collection, it is possible to resize the nodes table and
the cache. By default, Sylvan doubles the table sizes during every garbage
collection until the maximum table size has been reached. There is also a
less aggressive version that only resizes when at least half the table is
full. This can be configured in ``src/sylvan_config.h``. It is not
possible to decrease the size of the nodes table and the cache.

Dynamic reordering
~~~~~~~~~~~~~~~~~~

``sylvan_sift`` reorders the variables of the BDD/MTBDD forest by sifting: every
variable is moved through the order one adjacent swap at a time, the size of the
forest is measured at each position, and the variable is left where the forest was
smallest. Variables are taken most populous first. The declarations are in
``sylvan/reorder.h``.

.. code:: c

    sylvan_sift_result report;
    if (sylvan_sift_report(NULL, &report) == SYLVAN_OK) {
        printf("%llu -> %llu nodes\n",
               (unsigned long long)report.initial_size,
               (unsigned long long)report.final_size);
    }

A call stops the world for its whole duration, exactly like garbage collection, and
begins by collecting. Call it from inside a Lace worker, with **no decision diagram
operation in flight**. That is the one precondition and the caller owns it: a
suspended operation has already read node fields and would resume to combine
sub-results it computed under the previous order. Its handles stay valid, but its
result is wrong, and nothing detects that afterwards.

Everything a garbage collection would keep, a sift keeps. The session pins every root
it is shown -- the ``mtbdd_protect`` table, every worker's ``mtbdd_refs_push`` and
``mtbdd_refs_pushptr`` stack, and every root a ``sylvan_gc_add_roots`` visitor yields
-- and a pinned node is never retired. All of them denote the same function
afterwards and all of them keep their handles, because nodes are rewritten in place
and never moved.

Reordering moves positions, never identities. ``bdd_var(&v, i)`` returns the same
variable before and after a sift, whereas ``bdd_var_at_level(&v, i)`` returns whichever
variable a sift left at position i; in the C++ bindings ``Bdd::bddVar`` and
``Bdd::bddVarAtLevel`` are the same pair.

Pass a ``sylvan_sift_config`` to bound what the search may spend: a total swap
budget, a number of variables, a growth factor at which one leg of a variable's
excursion is abandoned (1.2 by default), a population below which a variable is
not moved at all (0 by default: skipping small variables costs final size and
saves no time), and a wall clock limit. ``NULL`` selects the defaults.

By default a variable's excursion also stops as soon as an interaction bound proves
that no position further in that direction can beat the best already seen -- two
variables interact when some function depends on both, and a variable can only
exchange nodes with the ones it interacts with. The bound is conservative: it finds
the same positions and the same final size as the exhaustive walk, in about a third
fewer swaps, as in CUDD. Set ``full_legs`` to walk every leg to the end of the order
anyway, for measurement.

An ungrouped sift moves variables one at a time, so it can separate the pairs
(``2k``, ``2k+1``) of the operations for transition systems, or put the primed
variable first; those operations stay correct but take their slower route (see
above). Set ``group_pairs`` to sift each pair as one block: a pair found apart is
brought back together first, blocks are taken in order of their summed population
and moved past one neighbouring block at a time, and only whole moves are scored or
kept, so a grouped sift leaves every pair together unless a refusal makes that
impossible, as ``sylvan/reorder.h`` describes. ``sylvan_sift_split_pairs`` counts the
pairs that are apart in the current order, and ``split_pairs`` in the
``sylvan_sift_result`` of every sift, grouped or not, reports it afterwards. CUDD's
counterpart is group sifting over a tree of variable groups (``Cudd_MakeTreeNode``).

Reordering is refused, changing nothing and returning ``SYLVAN_ERR_INVALID``, while
the ZDD or ListDD subsystem is initialised -- both order their nodes by variable
identity rather than by position, and they share the node arena -- while garbage
collection is disabled, since a sift begins by collecting, or on a nodes table
that does not implement the reorder primitives. Of the nodes tables the CMake option
``SYLVAN_NODES_TABLE`` selects from, ``trie`` (the default), ``list``, ``llcht`` and
``llchst`` can reorder; ``llht``, which is open-addressed, cannot. Running out of
room *during* a sift is not a failure: a swap that cannot reserve what it needs
changes nothing at all, and the driver simply stops moving that variable in that
direction.

When to reorder, and where Sylvan may do it for you
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

``sylvan_sift_recommended`` answers the question "is it worth reordering now" with a
signal that is free to compute: the live forest has grown by a factor (2.0 by
default, set with ``sylvan_sift_set_recommend_factor``, 0 to disable) since it was last
sifted, or since startup. Every garbage collection already counts the nodes that
survive marking, so the recommendation is one comparison per collection and the
predicate is a load. A forest of a few thousand nodes never recommends, whatever the
factor (the floor is ``SYLVAN_SIFT_RECOMMEND_FLOOR``, see `Measuring reordering`_).

``sylvan_gc_set_auto_sift`` hands the decision to Sylvan: pass a ``sylvan_sift_config``
and every **explicit** ``sylvan_gc()`` that ends with a recommendation is followed by a
sift, in a frame of its own. Pass ``NULL``, the default, to switch it off.
``sylvan_sift_count`` reports how many sifts have run, the way ``sylvan_gc_count``
reports collections.

**Only** ``sylvan_gc()`` triggers it, never the collection an operation performs when
the nodes table has no free bucket. That internal collection runs in the middle of an
operation, which is the operation in flight that sifting forbids (see above), while
``sylvan_gc()`` is called *between* operations. The internal path is a separate
function, ``sylvan_gc_collect()``, which never sifts.

A swap asks the nodes table for the slots it will need plus a margin of one
512-slot allocation region per Lace worker, which is what makes the answer hold
whichever workers end up doing the allocating. On a table that is small relative
to the worker count that margin is a large share of it, so a sift that reports no
swaps on a table which looks half empty usually wants a larger nodes table or
fewer workers.

Measuring reordering
^^^^^^^^^^^^^^^^^^^^

A few switches exist to measure where a sift spends its time and what parts of its
design are worth. None of them changes what a sift computes, only how it gets there
or what it reports. The build-time ones are listed under `Diagnostic builds`_
(``SYLVAN_SIFT_PHASE_TIMING`` for where a sift's time goes, the index counters of
``SYLVAN_STATS`` for how much index each swap walks, ``SYLVAN_REGION_STRIPED_START``
for per-worker allocation stripes); the one runtime switch is internal API, declared
in ``sylvan/internal.h``:

``sylvan_reorder_set_batched(int enable)``
    With ``0``, every swap removes and re-indexes its nodes one at a time, even
    on a nodes table that can take the whole set at once (list, trie, llcht,
    llchst) with more than one worker to share it. ``1``, the default, restores
    batching. Read at the start of every swap; the forest, the scores and the
    order a sift arrives at are the same either way.

The recommendation that drives the automatic sift has one more setting, a build
setting because it is a constant of the library rather than a choice of the
caller:

``SYLVAN_SIFT_RECOMMEND_FLOOR`` (CMake cache variable, 4096 by default)
    The smallest forest a growth is measured from: the recommendation compares
    the live nodes against the factor times the larger of this floor and the
    forest the last sift left behind, so ``sylvan_sift_recommended`` never
    fires below factor times floor. It changes when an automatic sift happens
    on a small forest and nothing about a sift itself. It exists to measure the
    recommendation at other floors; every build records it in its CMake cache.

    .. code:: bash

        cmake .. -DSYLVAN_SIFT_RECOMMEND_FLOOR=16384

Examples
--------

Simple examples can be found in the ``examples`` subdirectory. The file
``simple.cpp`` contains a toy program that uses the C++ objects to
perform basic BDD manipulation. The ``mc.c`` and ``lddmc.c`` programs
are more advanced examples of symbolic model checking (with example
models in the ``models`` subdirectory).

Troubleshooting
---------------

Sylvan may require a larger than normal program stack. You may need to
increase the program stack size on your system using ``ulimit -s``.
Segmentation faults on large computations typically indicate a program
stack overflow.

I am getting the error "unable to allocate memory: ...!"
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Sylvan allocates virtual memory using mmap. If you specify a combined
size for the cache and node table larger than your actual available
memory you may need to set ``vm.overcommit_memory`` to ``1``. E.g.
``echo 1 > /proc/sys/vm/overcommit_memory``. You can make this setting
permanent with
``echo "vm.overcommit_memory = 1" > /etc/sysctl.d/99-sylvan.conf``. You
can verify the setting with ``cat /proc/sys/vm/overcommit_memory``. It
should report ``1``.

I get errors about ``__lace_worker`` and ``__lace_dq_head``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Many Sylvan operations are implemented as Lace tasks. To call a Lace
task, the variables ``__lace_worker`` and ``__lace_dq_head`` must be
initialized. Use the macro ``LACE_ME`` to do this. Only use ``LACE_ME``
locally (in a function), never globally!

Publications
------------

T. van Dijk (2016) `Sylvan: Multi-core Decision
Diagrams <http://dx.doi.org/10.3990/1.9789036541602>`__. PhD Thesis.

T. van Dijk and J.C. van de Pol (2016) `Sylvan: Multi-core Framework
for Decision Diagrams <http://dx.doi.org/10.1007/s10009-016-0433-2>`__.
In: STTT (Special Issue), Springer.

T. van Dijk and J.C. van de Pol (2015) `Sylvan: Multi-core Decision
Diagrams <http://dx.doi.org/10.1007/978-3-662-46681-0_60>`__. In: TACAS
2015, LNCS 9035. Springer.

T. van Dijk and A.W. Laarman and J.C. van de Pol (2012) `Multi-Core BDD
Operations for Symbolic
Reachability <http://eprints.eemcs.utwente.nl/22166/>`__. In: PDMC 2012,
ENTCS. Elsevier.


