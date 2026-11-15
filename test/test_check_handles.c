/* Handle bounds use the data range, which can outlive a larger hash table. */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sylvan_int.h"
#include "test_assert.h"

static void
expected_abort(int sig)
{
    /* Let CTest distinguish abort() from a crash, without a core dump or a
     * Windows error dialog. The wrapper also checks the diagnostic. */
    (void)sig;
    _Exit(86);
}

TASK_1(int, check_handles, const char*, helper)
{
    /* Fill more than one allocation region, then retain only the last key.
     * We test pointer conversion, so these need not be decision diagram nodes. */
    uint64_t index = 0;
    for (uint64_t i = 0; i < 600; i++) {
        int created;
        index = llmsset_lookup(nodes, i, 0, &created);
        test_assert(index != 0 && created);
    }
    llmsset_clear_data(nodes);
    llmsset_mark(nodes, index);
    llmsset_shrink(nodes, 512);
    test_assert(index >= llmsset_get_size(nodes));
    test_assert(index < llmsset_get_data_size(nodes));
    test_assert(llmsset_get_data_size(nodes) < llmsset_get_max_size(nodes));

    void *expected = nodes->data + index * 16;
    test_assert(llmsset_index_to_ptr(nodes, index) == expected);
    test_assert((void*)MTBDD_GETNODE(index) == expected);
    test_assert((void*)MTBDD_GETNODE(index | mtbdd_complement) == expected);
    test_assert((void*)ZDD_GETNODE(index) == expected);
    test_assert((void*)ZDD_GETNODE(index | zdd_complement) == expected);
    test_assert((void*)LDD_GETNODE(index) == expected);

    if (helper != NULL) {
#if defined(_MSC_VER)
        /* Suppress runtime dialogs before abort() reaches our SIGABRT handler. */
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
        test_assert(signal(SIGABRT, expected_abort) != SIG_ERR);
        index = llmsset_get_data_size(nodes); // first invalid index, below max_size
        if (strcmp(helper, "mtbdd") == 0) (void)MTBDD_GETNODE(index | mtbdd_complement);
        else if (strcmp(helper, "zdd") == 0) (void)ZDD_GETNODE(index | zdd_complement);
        else if (strcmp(helper, "ldd") == 0) (void)LDD_GETNODE(index);
        else if (strcmp(helper, "table") == 0) (void)llmsset_index_to_ptr(nodes, index);
        return 1; // every helper must abort
    }

    return 0;
}

int
main(int argc, char **argv)
{
    lace_start(1, 0);
    sylvan_set_sizes(4096, 8192, 4096, 8192);
    sylvan_init_package();
    int result = RUN(check_handles, argc > 1 ? argv[1] : NULL);
    sylvan_quit();
    lace_stop();
    return result;
}
