/* malloc.h --- QuickJS's default allocator wants malloc_usable_size; jsr2
 * never installs that allocator, and this answers 0 (unknown). */
#ifndef JSR2_PORT_MALLOC_H
#define JSR2_PORT_MALLOC_H
#include <stddef.h>
#define malloc_usable_size jsr2_malloc_usable_size
size_t malloc_usable_size(void *);
#endif
