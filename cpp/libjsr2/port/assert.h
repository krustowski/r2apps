/* assert.h --- a failed assertion reports through the platform log and stops
 * the process (src/libc.c). */
#ifndef JSR2_PORT_ASSERT_H
#define JSR2_PORT_ASSERT_H
#ifdef __cplusplus
extern "C"
#endif
void jsr2_assert_fail(const char *expr, const char *file, int line) __attribute__((noreturn));
#ifdef NDEBUG
#define assert(e) ((void)0)
#else
#define assert(e) ((e) ? (void)0 : jsr2_assert_fail(#e, __FILE__, __LINE__))
#endif
#endif
