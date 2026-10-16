#include "read_ahead.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
int main(void) {
    RntpReadAhead cache = {0};
    uint8_t data[32 * 512], out[512];
    for (unsigned i = 0; i < 32; ++i) memset(data + i * 512, i + 1, 512);
    assert(rntp_read_ahead_count(100, 1000, 39) == 32);
    assert(rntp_read_ahead_count(1, 1000, 39) == 32);
    assert(rntp_read_ahead_count(25, 1000, 39) == 14);
    assert(rntp_read_ahead_count(990, 1000, 39) == 10);
    assert(!rntp_read_ahead_count(1000, 1000, 39));
    assert(!rntp_read_ahead_get(&cache, 100, out));
    assert(rntp_read_ahead_store(&cache, 100, 1000, 32, data, sizeof(data)));
    for (unsigned i = 0; i < 32; ++i) {
        assert(rntp_read_ahead_get(&cache, 100 + i, out));
        for (unsigned j = 0; j < 512; ++j) assert(out[j] == i + 1);
    }
    assert(!rntp_read_ahead_get(&cache, 99, out));
    assert(!rntp_read_ahead_get(&cache, 132, out));
    // LOCK/UNLOCK/ABORT and writes all discard the operation's read buffer.
    rntp_read_ahead_clear(&cache); assert(!rntp_read_ahead_get(&cache, 100, out));
    assert(rntp_read_ahead_store(&cache, 990, 1000, 10, data, 10 * 512));
    assert(rntp_read_ahead_get(&cache, 999, out) && out[0] == 10);
    assert(!rntp_read_ahead_store(&cache, 990, 1000, 11, data, 11 * 512));
    assert(!rntp_read_ahead_get(&cache, 999, out));
    assert(!rntp_read_ahead_store(&cache, 100, 1000, 32, data, 32 * 512 - 1));
    assert(!rntp_read_ahead_store(&cache, 100, 1000, 33, data, 33 * 512));
    assert(!rntp_read_ahead_store(&cache, 1000, 1000, 1, data, 512));
    assert(!rntp_read_ahead_store(&cache, 100, 1000, 0, data, 0));
    puts("operation-scoped read-ahead: PASS"); return 0;
}
