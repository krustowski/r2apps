#ifndef WGD_ENTROPY_H
#define WGD_ENTROPY_H
#include <stdbool.h>
#include <stddef.h>

bool wgd_entropy_start(bool use_hardware, void (*report)(const char *), const char **error);
bool wgd_entropy_read(void *out, size_t n);
void wgd_entropy_stop(void);
#endif
