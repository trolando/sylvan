/**
 * Just a small test file to ensure that Sylvan can compile in C++
 * Suggested by Shota Soga <shota.soga@gmail.com> for testing C++ compatibility
 */

#include <sylvan.h>

int main()
{
    // Standard Lace initialization
	lace_init(0, 1000000);
	lace_startup(0, NULL, NULL);

    // Simple Sylvan initialization, also initialize BDD and LDD support
	sylvan_init_package(1LL<<16, 1LL<<16, 1LL<<16, 1LL<<16);
	sylvan_init_bdd(1);
    sylvan_init_ldd();

	BDD one = sylvan_true;
	BDD zero = sylvan_false;

	BDD v1 = sylvan_ithvar(1);
	BDD v2 = sylvan_ithvar(2);

	sylvan_quit();
    lace_exit();
}
