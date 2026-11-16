/* Write example input files with two initial states and no transition groups. */
#include <stdio.h>

#include "sylvan.h"
#include "test_assert.h"

TASK_0(int, write_models)
{
    int variables = 1, bits = 2, zero = 0, projection = -1;
    FILE *f = fopen("empty-groups.bdd", "wb");
    test_assert(f != NULL);
    test_assert(fwrite(&variables, sizeof(int), 1, f) == 1);
    test_assert(fwrite(&bits, sizeof(int), 1, f) == 1);
    test_assert(fwrite(&zero, sizeof(int), 1, f) == 1); // action bits
    test_assert(fwrite(&projection, sizeof(int), 1, f) == 1);
    BDD bdd = sylvan_nithvar(0); // the other state bit (variable 2) is free
    sylvan_protect(&bdd);
    mtbdd_writer_tobinary(f, &bdd, 1);
    test_assert(fwrite(&zero, sizeof(int), 1, f) == 1); // transition groups
    test_assert(fclose(f) == 0);
    sylvan_unprotect(&bdd);

    f = fopen("empty-groups.ldd", "wb");
    test_assert(f != NULL);
    test_assert(fwrite(&variables, sizeof(int), 1, f) == 1);
    test_assert(fwrite(&projection, sizeof(int), 1, f) == 1);
    MDD ldd = lddmc_false;
    lddmc_protect(&ldd);
    ldd = lddmc_makenode(2, lddmc_true, lddmc_false);
    ldd = lddmc_makenode(0, lddmc_true, ldd);
    size_t dd = lddmc_serialize_add(ldd);
    lddmc_serialize_tofile(f);
    test_assert(fwrite(&dd, sizeof(size_t), 1, f) == 1);
    test_assert(fwrite(&zero, sizeof(int), 1, f) == 1);
    test_assert(fclose(f) == 0);
    lddmc_unprotect(&ldd);
    return 0;
}

int main(void)
{
    lace_start(1, 0);
    sylvan_set_sizes(1LL<<12, 1LL<<12, 1LL<<10, 1LL<<10);
    sylvan_init_package();
    sylvan_init_bdd();
    sylvan_init_ldd();
    int result = RUN(write_models);
    sylvan_quit();
    lace_stop();
    return result;
}
