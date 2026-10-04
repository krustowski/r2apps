#include "../../cpp/memento-hello/web/tls.h"
int r2tls_random(unsigned char *out, unsigned long len);
void r2tls_anchors(const unsigned char *data, unsigned long len);

// Caller supplies 49 bytes for the 48-byte CPU brand and terminator.
void r2tls_cpu_name(char *out);

// Memento-compatible timing seed fallback; entropy quality is unverified.
void r2tls_timing_seed(unsigned char *out, unsigned long len);
