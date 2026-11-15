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
Lace <https://github.com/trolando/lace>`__ for its implementation.
CMake uses the Lace that a parent project provides or that is installed, and
otherwise fetches it. Lace needs no other libraries: it pins its worker
threads to processors itself on Linux, FreeBSD and Windows.


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

``SYLVAN_INSTALL`` controls whether CMake generates Sylvan's installation rules.
It defaults to ``ON`` for a top-level build and ``OFF`` as a subproject. Set it
before adding Sylvan to a parent project to override the default.

``SYLVAN_HUGEPAGES`` defaults to ``OFF`` until its effect is measured on
master. When it and ``SYLVAN_USE_MMAP`` are enabled on Linux, Sylvan requests
transparent huge pages for mappings of at least 2 MiB through
``madvise(MADV_HUGEPAGE)``, if available. This includes the node data, hash
table and operation cache. Traversal side bitmap allocations instead request
base pages through ``MADV_NOHUGEPAGE`` when available, regardless of the
option, to limit sparse memory use. Both requests are best effort and may
fail. With the option ``OFF``, the kernel may still use huge pages under its
``always`` policy, including for side bitmaps if their opt-out is unavailable
or fails. Other platforms are unchanged.

To measure the advice, compare Release builds with
``-DSYLVAN_HUGEPAGES=ON`` and ``-DSYLVAN_HUGEPAGES=OFF``, keeping worker
count, table limits and input fixed. For example, run
``./build-release/examples/nqueens -w 4 12`` repeatedly in each build and
compare its computation time and elapsed time. On Linux, use the ``madvise``
transparent huge page policy for this comparison: ``OFF`` omits the request,
but does not prevent the kernel from using huge pages under ``always``.

While the process runs, sample ``AnonHugePages`` and ``Rss`` in
``/proc/<pid>/smaps_rollup`` before and after collections. For shrinking,
use a workload that drops a large diagram while keeping a smaller protected
one, selects ``SYLVAN_GC_LOW_MEMORY``, calls ``sylvan_gc()``, and pauses
before and after that call for sampling.
Compare current RSS at those points, since peak RSS cannot show reclamation.
Also time the collection and subsequent operations. On Linux, aligned clears
first try ``MADV_DONTNEED`` when available; success retains the mapping and
its advice. If it is unavailable or fails, Sylvan tries remapping the cleared
range and requests huge pages again if the option is enabled, the advice is
available and the range is at least 2 MiB. Only if remapping also fails does
it zero the range with ``memset``. Master's operation cache clears by freeing
and reallocating its mappings, so each new allocation gets fresh advice under
the conditions above. Huge pages may need to be allocated again when tables
are touched. Low-memory collections reclaim empty arena stretches at 64 KiB
boundaries, which can split huge pages, so the huge-page share can decrease
after a shrink.
``SYLVAN_GC_DIAGNOSTICS`` below helps identify collections and their table
sizes.

Diagnostic builds
-----------------

The following CMake options are off by default and add no runtime cost when
disabled. They are intended for debugging Sylvan.

``SYLVAN_CHECK_HANDLES``
    Check every node dereference through the MTBDD, ZDD and LDD helpers, or
    ``llmsset_index_to_ptr``, and abort with the offending index if it is
    outside ``llmsset_get_data_size()``. This is the data range, which can be
    larger than the hash table after ``sylvan_gc()`` shrinks it: live nodes
    keep their indices. The check also works in Release builds. It only
    checks bounds; it does not detect an unused bucket within the data range.

``SYLVAN_GC_DIAGNOSTICS``
    Write one line to stderr per collection, identifying ``sylvan_gc()`` or
    ``sylvan_gc_collect()`` and reporting occupied buckets before marking,
    live and freed buckets, the time spent counting, marking, resizing and
    rehashing, and table, data range and cache sizes before and after resizing.
    Table and cache sizes include their maximum and whether they grew, shrank
    or stayed unchanged. Occupancy includes the two reserved terminal buckets;
    the live percentage uses the hash table size before resizing.

For example:

.. code:: bash

    cmake .. -DSYLVAN_CHECK_HANDLES=ON -DSYLVAN_GC_DIAGNOSTICS=ON

Both switches can also be enabled as compile definitions with value ``1``.
When setting ``SYLVAN_CHECK_HANDLES`` that way, compile the library and code
using its internal headers with the same definition. The CMake option
propagates it through the ``sylvan::sylvan`` target.

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
By default, every automatic garbage collection doubles the table sizes;
an explicit ``sylvan_gc()`` can shrink them (see Table resizing).

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

Garbage collection can be disabled with ``sylvan_gc_disable`` and enabled again with ``sylvan_gc_enable``.
Call ``sylvan_gc`` to manually trigger garbage collection; this also shrinks the tables
to fit the live nodes (see Table resizing).

If a node still cannot be created after garbage collection has failed to free
enough nodes, Sylvan prints a table-full message to standard error and calls
``exit(1)``. This is an out-of-memory condition. An application can report it in
its own way by installing a handler with ``sylvan_set_table_full_handler``:

.. code:: c

    static void table_full(const char *kind, size_t filled, size_t total)
    {
        fprintf(stderr, "Out of nodes for %s: %zu of %zu buckets used\n",
                kind, filled, total);
        exit(30);
    }

    /* Before starting decision diagram operations: */
    sylvan_set_table_full_handler(table_full);

The callback receives ``"BDD"`` (including MTBDD), ``"MDD"`` (LDD), or ``"ZDD"``
and the occupied and total bucket counts. It runs on a Lace worker in the middle
of an operation. It must not call Sylvan operations or return; it should exit or
abort. If it returns anyway, Sylvan prints its usual message and calls
``exit(1)``. The handler also applies when garbage collection is disabled.
It cannot recover from a full table.

Other workers may be waiting for the operation that ran out of nodes, so neither
the handler nor anything ``exit()`` runs (``atexit`` handlers, static
destructors) may stop Lace (``lace_stop`` waits for every worker), quit Sylvan,
or wait for other workers; the program could hang. If the application's cleanup
cannot avoid that, write and flush the report and call ``_Exit()`` instead.
Several workers can find the table full at the same time, so the handler can be
called concurrently: a handler that must report once should guard its report,
for example with an atomic flag.

The handler is global, and its pointer is stored atomically. It can be set from
any thread; install it before starting operations that may exhaust the table.
Replacing it does not wait for a call that is already running. Passing ``NULL``
restores the default handler. The default output is unchanged,
including the historical ``"BDD"`` label for ZDD failures.

Choose the balance between memory use and collection frequency with
``sylvan_gc_set_policy``, after ``sylvan_init_package``:

* ``SYLVAN_GC_FEW_COLLECTIONS`` is the default. Automatic collections double
  the tables up to their maximum sizes. An explicit ``sylvan_gc()`` shrinks
  the hash table and the operation cache in proportion, but retains the node
  arena's data range and pages. Reusing those pages avoids faulting them in
  again after a collection.
* ``SYLVAN_GC_LOW_MEMORY`` grows the tables only when more than half the
  nodes table remains in use after collection. An explicit ``sylvan_gc()``
  also reduces the arena's data range and gives its empty stretches back to
  the operating system in 64 KiB chunks, when virtual memory allocation is
  enabled (``SYLVAN_USE_MMAP``).

For example, to select low memory use:

.. code:: c

    sylvan_init_package();
    sylvan_gc_set_policy(SYLVAN_GC_LOW_MEMORY);

You can change the policy between operations, with no concurrent Sylvan
calls. Setting a policy does not run a collection. Each
``sylvan_init_package()`` resets it to the default; builds with
``SYLVAN_AGGRESSIVE_RESIZE=0`` instead default to low memory.
The few-collections default uses aggressive growth and retains unused
arena memory on explicit collections.
Select low memory to release that memory.

Setting a policy installs its growth hook, replacing any custom main hook.
A later ``sylvan_gc_hook_main`` call overrides growth while retaining the
policy's shrink mode. ``sylvan_gc_get_policy()`` returns the last selected
policy even when a custom hook is installed. Setting the policy again
restores its growth hook. Explicit ``sylvan_gc()`` collections bypass the
main hook under both policies.
To combine aggressive growth with arena release,
select ``SYLVAN_GC_LOW_MEMORY``, then call
``sylvan_gc_hook_main(sylvan_gc_aggressive_resize_CALL)``.

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

The *mtbdd* subpackage also implements thread-local stacks to temporarily store pointers and results of tasks:

.. code:: c

    MTBDD some_thing = ...;
    mtbdd_refs_pushptr(&some_thing);
    MTBDD result_param1 = mtbdd_false, result_param2 = mtbdd_false;
    mtbdd_refs_pushptr(&result_param1);
    mtbdd_refs_pushptr(&result_param2);
    while (some_condition) {
        mtbdd_refs_spawn(SPAWN(an_operation, some_thing, param1));
        result_param2 = CALL(an_operation, some_thing, param2);
        result_param1 = mtbdd_refs_sync(SYNC(an_operation));
        some_thing = CALL(another_operation, result1, result2);
    }
    mtbdd_refs_popptr(3);
    return some_thing;

It is recommended to use the thread-local stacks for local variables, and to use the ``protect`` and ``unprotect``
functions for other variables. Every SPAWN and SYNC of a Lace task that returns an MTBDD must be decorated with
``mtbdd_refs_stack`` and ``mtbdd_refs_sync`` as in the above example.

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
- ``sylvan_ithvar(var)``: representation of literal <var> (negated: ``sylvan_nithvar(var)``)

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
- ``mtbdd_refs_spawn(SPAWN(...))``: spawn a task that returns a BDD/MTBDD.
- ``mtbdd_refs_sync(SYNC(...))``: sync a task that returns a BDD/MTBDD.

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
- ``sylvan_satcount_u64(bdd, variables, &result)``: count satisfying assignments exactly over a variable set containing the BDD's support.
- ``mtbdd_satcount_u64(dd, number_of_vars, &result)``: count exactly with the same variable-count argument and leaf semantics as ``mtbdd_satcount``. False and built-in numeric zero leaves do not count; other leaves count.
- ``lddmc_satcount_u64(dd, &result)``: count the tuples in an LDD exactly, with the same semantics as ``lddmc_satcount``.
- ``zdd_satcount_u64(dd, &result)``: count paths to non-False ZDD leaves exactly, with the same semantics as ``zdd_satcount``; skipped variables do not multiply the count.
- ``sylvan_pathcount(bdd)``: compute the number of distinct paths to True.
- ``mtbdd_nodecount(bdd)``: compute the number of nodes (and leaves) in the BDD.
- ``mtbdd_nodecount_more(array, length)``: compute the number of nodes (and leaves) in the array of BDDs.

The four ``_u64`` operations write a ``uint64_t`` result and return 1 on success.
They return 0 on overflow or invalid arguments and leave the result unchanged.
In particular, ``UINT64_MAX`` is a valid result. Existing floating-point count
operations are unchanged.

With ``SYLVAN_GMP=ON``, include ``sylvan_gmp.h`` for
``sylvan_satcount_gmp``, ``mtbdd_satcount_gmp``, ``lddmc_satcount_gmp`` and
``zdd_satcount_gmp``. These take the same arguments as their ``_u64`` counterparts,
with an initialized ``mpz_t`` as the final argument. They support results larger
than 64 bits and return 1 on success, or 0 on invalid arguments or memo-table
allocation failure, leaving the result unchanged on failure. Each call uses a
private memo table and traverses sequentially. Initialize and clear the result
with ``mpz_init`` and ``mpz_clear``; ``gmp_init`` is not needed for counting.
GMP's own allocation failures follow its allocation policy.

Sylvan implements various advanced operations:

- ``sylvan_and_exists(bdd_a, bdd_b, vars)``: compute ``sylvan_exists(sylvan_and(bdd_a, bdd_b), vars)`` in one step.
- ``sylvan_and_project(bdd_a, bdd_b, vars)``: compute ``sylvan_project(sylvan_and(bdd_a, bdd_b), vars)`` in one step.
- ``sylvan_cube(vars, values)``: compute a cube (to leaf True) of the given variables, where the array values indicates for each variable whether to use it in negative form (value 0) or positive form (value 1) or to skip it (as dont-care, value 2).
- ``sylvan_union_cube(set, vars, values)``: compute ``sylvan_or(set, sylvan_cube(vars, values))`` in one step.
- ``sylvan_constrain(bdd_f, bdd_c)``: compute the generic cofactor of F constrained by C, i.e, set F to False for all assignments not in C.
- ``sylvan_restrict(bdd_f, bdd_c)``: compute Coudert and Madre's restrict algorithm, which tries to minimize bdd_f according to a care set C using sibling substitution; the invariant is ``restrict(f, c) \and c == f \and c``; the result of this algorithm is often but not always smaller than the original.
- ``sylvan_cofactor(f, cube)``: check that ``cube`` is a conjunction of positive or negative literals, then restrict ``f`` by it. Returns ``sylvan_invalid`` for a non-cube, including False. True is the empty cube. ``sylvan_restrict`` retains its existing behavior for general care sets.
- ``sylvan_eval(f, variables, values, count)`` and ``mtbdd_eval(dd, variables, values, count)``: follow a complete assignment to a leaf without allocating nodes. The array contains exactly ``count`` values, each 0 or 1, in variable-set order, and ``count`` must equal the set's size. The set must contain the diagram's support; extra variables are ignored. The BDD operation returns True or False; the MTBDD operation returns the reached leaf, including False. Invalid arguments or an encountered unassigned variable yield ``sylvan_invalid`` / ``mtbdd_invalid``. These are sequential traversals of one path; they do not perform the callback-based transformation offered by ``mtbdd_eval_compose``.
- ``sylvan_pick_cube(bdd)`` or ``sylvan_sat_one_bdd(bdd)``: extract a single path to True from the BDD (returns the BDD of this path)
- ``sylvan_pick_single_cube(bdd, vars)`` or ``sylvan_sat_single(bdd, vars)`` extracts a single minterm from the BDD (returns the BDD of this assignment)
- ``sylvan_sat_one(bdd, vars, array)``: extract a single minterm from the BDD given the set of variables and write the values of the variables in order to the given array, with 0 when it is negative, 1 when it is positive, and 2 when it is dontcare.

Sylvan implements several operations for transition systems. These operations assume an interleaved variable ordering, such that *source* or *unprimed* variables have even parity (0, 2, 4...) and matching *target* or *primed* variables have odd parity (1, 3, 5...).
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

Adding custom decision diagram operations is easy. Include ``sylvan_int.h`` for the internal functions. See ``sylvan_cache.h``
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

Automatic garbage collections resize the nodes table and the operation
cache through the main GC hook. ``SYLVAN_GC_FEW_COLLECTIONS`` selects
``sylvan_gc_aggressive_resize``, which doubles each table until its maximum
size is reached. ``SYLVAN_GC_LOW_MEMORY`` selects
``sylvan_gc_normal_resize``, which doubles the nodes table only when more
than half is still in use after marking, and grows the cache along with it.
These hooks can also be installed directly with ``sylvan_gc_hook_main``.

A garbage collection asked for with ``sylvan_gc`` shrinks the tables
instead: the nodes table to the smallest size, from the initial size up,
with room for twice the live nodes, and the operation cache in proportion.
Neither table shrinks below its initial size. Nodes keep their place, so
live node indices may exceed the hash table size after shrinking. Use
``llmsset_get_data_size()`` for node-index bounds, not ``llmsset_get_size()``.
With low memory, the arena keeps the range through the highest live node
(rounded to a 512-node region), or the hash table size if larger, and
releases empty stretches. It can release more arena memory on later explicit
collections even if the hash table is already at its minimum. With few
collections, the arena's data range and pages stay as they were.
``sylvan_gc_collect()``, the internal collection used when insertion fails,
uses the main hook and never shrinks the tables under either policy.

If node creation still fails after collection and resizing, the table-full
handler described under `Garbage collection and referencing nodes`_ is called.

Dynamic reordering
~~~~~~~~~~~~~~~~~~

Dynamic reordening is not yet supported. For now, we suggest users
find a good static variable ordering.

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


