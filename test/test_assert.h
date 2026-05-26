#ifndef TEST_ASSERT_H
#define TEST_ASSERT_H

#ifndef TEST_FUNCTION
# if defined(_MSC_VER)
#  define TEST_FUNCTION __FUNCSIG__
# elif defined(__GNUC__) || defined(__clang__)
#  define TEST_FUNCTION __PRETTY_FUNCTION__
# else
#  define TEST_FUNCTION __func__
# endif
#endif

#ifndef test_assert
#define test_assert(expr)         do {                                  \
 if (!(expr))                                                           \
 {                                                                      \
         fprintf(stderr,                                                \
                "file %s: line %d (%s): precondition `%s' failed.\n",   \
                __FILE__,                                               \
                __LINE__,                                               \
                TEST_FUNCTION,                                          \
                #expr);                                                 \
         return 1;                                                      \
 } } while(0)
#endif

#endif
