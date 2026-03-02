#ifndef TEST_FUNCNAME
    #if defined(_MSC_VER)
        #ifdef __cplusplus
            #define TEST_FUNCNAME __FUNCSIG__
        #else
            #define TEST_FUNCNAME __FUNCTION__
        #endif
    #elif defined(__GNUC__) || defined(__clang__)
        #define TEST_FUNCNAME __PRETTY_FUNCTION__
    #else
        #define TEST_FUNCNAME __func__
    #endif
#endif

#ifndef test_assert
#define test_assert(expr)         do {                                  \
 if (!(expr))                                                           \
 {                                                                      \
         fprintf(stderr,                                                \
                "file %s: line %d (%s): precondition `%s' failed.\n",   \
                __FILE__,                                               \
                __LINE__,                                               \
                TEST_FUNCNAME,                                          \
                #expr);                                                 \
         return 1;                                                      \
 } } while(0)
#endif
