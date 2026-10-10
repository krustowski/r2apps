#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "entropy.h"
#include "jitterentropy.h"
#include "arch/jitterentropy-arch-timer.h"

/* This test links the real freestanding upstream core, with no OS RNG. */
void crypto_zero(void *out, size_t n) { volatile uint8_t *p=out; while(n--) *p++=0; }
static int stalled;
static void report(const char *text) { puts(text); }
static void timer(void *arg, uint64_t *out) {
    (void)arg;
    unsigned lo,hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    *out=stalled ? 123 : ((uint64_t)hi<<32)|lo;
}
int main(int argc, char **argv) {
    int after=argc>1 && !strcmp(argv[1],"--failed-after-start");
    stalled=argc>1 && !strcmp(argv[1],"--stalled");
    assert(!jent_set_mock_timer(timer,NULL));
    const char *error=NULL;
    bool ok=wgd_entropy_start(false,report,&error);
    if(stalled) {
        assert(!ok && error);
        puts("PASS: stalled timer rejected before entropy is available"); return 0;
    }
    assert(ok && !error);
    uint8_t first[64], second[64];
    assert(wgd_entropy_read(first,sizeof(first)));
    if(after) {
        stalled=1; memset(second,0xa5,sizeof(second));
        assert(!wgd_entropy_read(second,sizeof(second)));
        for(unsigned i=0;i<sizeof(second);++i) assert(second[i]==0);
        puts("PASS: continuous health failure returns no random bytes");
    } else {
        assert(wgd_entropy_read(second,sizeof(second)));
        assert(memcmp(first,second,sizeof(first)));
        wgd_entropy_stop(); memset(second,0xa5,sizeof(second));
        assert(!wgd_entropy_read(second,sizeof(second)));
        for(unsigned i=0;i<sizeof(second);++i) assert(second[i]==0);
        puts("PASS: freestanding Jitterentropy initialization, generation and teardown");
    }
    crypto_zero(first,sizeof(first)); crypto_zero(second,sizeof(second));
    wgd_entropy_stop(); return 0;
}
