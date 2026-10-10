/* CPU entropy selection. Crypto and health tests live in the upstream library. */
#include "entropy.h"
#include "crypto.h"
#include "jitterentropy.h"

#if defined(JENT_CONF_ENABLE_MOCK_TIMER) && !defined(WGD_ENTROPY_TEST)
#error "Mock entropy timers must never enter a production daemon"
#endif

static unsigned hardware;
static struct rand_data *collector;
static void (*report_event)(const char *);
static const unsigned jitter_flags = JENT_FORCE_FIPS | JENT_DISABLE_INTERNAL_TIMER | JENT_MAX_MEMSIZE_2MB;

static void cpuid(unsigned leaf, unsigned *a, unsigned *b, unsigned *c, unsigned *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}
static void identify(void) {
    unsigned a,b,c,d;
    cpuid(0,&a,&b,&c,&d); unsigned max=a;
    if(max>=1) { cpuid(1,&a,&b,&c,&d); if(c&(1u<<30)) hardware|=1; }
    if(max>=7) { cpuid(7,&a,&b,&c,&d); if(b&(1u<<18)) hardware|=2; }
    cpuid(0x80000000u,&a,&b,&c,&d);
    if(a>=0x80000004u) {
        char name[54]="CPU: ";
        for(unsigned leaf=0;leaf<3;++leaf) {
            cpuid(0x80000002u+leaf,&a,&b,&c,&d); unsigned words[4]={a,b,c,d};
            for(unsigned w=0;w<4;++w) for(unsigned byte=0;byte<4;++byte)
                name[5+leaf*16+w*4+byte]=(char)(words[w]>>(byte*8));
        }
        name[53]=0; report_event(name);
    }
    report_event(hardware==3 ? "CPU entropy: RDSEED and RDRAND" : hardware==2 ? "CPU entropy: RDSEED" :
                 hardware==1 ? "CPU entropy: RDRAND" : "CPU has no RDSEED/RDRAND");
}
static bool hardware_read(void *out, size_t n) {
    uint8_t *bytes=out;
    while(n) {
        uint64_t value=0; uint8_t ok=0;
        if(hardware&2) for(unsigned i=0;i<128 && !ok;++i) {
            __asm__ volatile("rdseed %0; setc %1" : "=r"(value), "=qm"(ok) : : "cc");
            if(!ok) __asm__ volatile("pause");
        }
        if(hardware&1) for(unsigned i=0;i<16 && !ok;++i) {
            __asm__ volatile("rdrand %0; setc %1" : "=r"(value), "=qm"(ok) : : "cc");
            if(!ok) __asm__ volatile("pause");
        }
        if(!ok) return false;
        for(unsigned i=0;i<8 && n;++i,--n) { *bytes++=(uint8_t)value; value>>=8; }
    }
    return true;
}
static void failure_code(const char *prefix, int code) {
    char text[100]; size_t at=0;
    while(*prefix && at<80) text[at++]=*prefix++;
    if(code<0) { text[at++]='-'; code=-code; }
    char digits[10]; unsigned n=0;
    do { digits[n++]=(char)('0'+code%10); code/=10; } while(code);
    while(n) text[at++]=digits[--n];
    text[at]=0; report_event(text);
}
static bool jitter_start(void) {
    report_event("testing Jitterentropy CPU noise source; please wait");
    int status=jent_entropy_init_ex(0,jitter_flags);
    if(status) { failure_code("Jitterentropy startup test failed, code=",status); return false; }
    collector=jent_entropy_collector_alloc(0,jitter_flags);
    if(!collector) { report_event("Jitterentropy collector allocation or health test failed"); return false; }
    uint8_t sample[32]; ssize_t n=jent_read_entropy(collector,(char *)sample,sizeof(sample));
    crypto_zero(sample,sizeof(sample));
    if(n!=(ssize_t)sizeof(sample)) {
        failure_code("Jitterentropy initial read failed, code=",(int)n);
        jent_entropy_collector_free(collector); collector=NULL; return false;
    }
    report_event("entropy: Jitterentropy (startup and continuous health tests enabled)");
    return true;
}
bool wgd_entropy_start(bool use_hardware, void (*report)(const char *), const char **error) {
    report_event=report; hardware=0;
    identify();
    if(!use_hardware) hardware=0;
    uint8_t sample[32]; bool ok=hardware && hardware_read(sample,sizeof(sample));
    crypto_zero(sample,sizeof(sample));
    if(ok) { report_event("entropy: hardware random generator"); *error=NULL; return true; }
    if(hardware) report_event("CPU random instructions exhausted retries; trying Jitterentropy");
    hardware=0;
    if(jitter_start()) { *error=NULL; return true; }
    *error="no usable entropy source; Jitterentropy failed its startup/health tests";
    return false;
}
bool wgd_entropy_read(void *out, size_t n) {
    if(!n) return true;
    if(hardware && hardware_read(out,n)) return true;
    if(hardware) {
        hardware=0; crypto_zero(out,n);
        report_event("CPU random generator failed; trying Jitterentropy");
        if(!jitter_start()) return false;
    }
    ssize_t got=collector ? jent_read_entropy(collector,out,n) : -1;
    if(got==(ssize_t)n) return true;
    crypto_zero(out,n);
    failure_code("entropy health failure, code=",(int)got);
    return false;
}
void wgd_entropy_stop(void) {
    if(collector) jent_entropy_collector_free(collector);
    collector=NULL; hardware=0;
}
