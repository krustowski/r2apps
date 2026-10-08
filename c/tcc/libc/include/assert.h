/* No include guard: assert.h is re-read whenever NDEBUG changes. */
#undef assert

#ifdef NDEBUG
#define assert(e) ((void)0)
#else
void __assert_fail(const char *expr, const char *file, unsigned int line, const char *func)
    __attribute__((noreturn));
#define assert(e) ((e) ? (void)0 : __assert_fail(#e, __FILE__, __LINE__, __func__))
#endif
