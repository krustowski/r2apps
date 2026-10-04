//go:build r2

#include "bridge.h"

extern unsigned long r2_syscall(unsigned long, unsigned long, unsigned long);
void *web_tls_alloc(unsigned long n) { return (void *)r2_syscall(0x0a,n,0); }
void web_tls_release(void *p) { r2_syscall(0x0f,(unsigned long)p,0); }
static const unsigned char *anchor_data;
static unsigned long anchor_len;
void r2tls_anchors(const unsigned char *data, unsigned long len) {anchor_data=data;anchor_len=len;}
int web_tls_platform_anchors(const unsigned char **data,unsigned long *len) {
 *data=anchor_data;*len=anchor_len;return anchor_data?0:-1;
}
const char *web_tls_platform_anchor_path(void) {return "/mnt/tar/opt/memento/cacerts.bin";}

// Only execute entropy instructions after checking their CPUID feature bits.
// RDSEED is an alternative on machines where RDRAND is unavailable or fails.
// Return -1 when neither exists, 0 for exhausted retries, and 1 on success.
int r2tls_random(unsigned char *out, unsigned long len) {
 unsigned a, b, c, d;
 __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0), "c"(0));
 unsigned max_leaf = a;
 int has_rdrand = 0, has_rdseed = 0;
 if (max_leaf >= 1) {
  __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
  has_rdrand = (c & (1u << 30)) != 0;
 }
 if (max_leaf >= 7) {
  __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0));
  has_rdseed = (b & (1u << 18)) != 0;
 }
 if (!has_rdrand && !has_rdseed) return -1;
 while (len) {
  unsigned long value = 0;
  unsigned char ok = 0;
  if (has_rdseed) {
   for (int tries = 0; tries < 128 && !ok; tries++) {
    __asm__ volatile("rdseed %0; setc %1" : "=r"(value), "=qm"(ok) : : "cc");
    if (!ok) __asm__ volatile("pause");
   }
  }
  if (has_rdrand && !ok) {
   for (int tries = 0; tries < 16 && !ok; tries++) {
    __asm__ volatile("rdrand %0; setc %1" : "=r"(value), "=qm"(ok) : : "cc");
    if (!ok) __asm__ volatile("pause");
   }
  }
  if (!ok) return 0;
  for (int i = 0; i < 8 && len; i++, len--) {
   *out++ = (unsigned char)value;
   value >>= 8;
  }
 }
 return 1;
}

// CPUID's extended brand string identifies bare-metal machines in errors.
void r2tls_cpu_name(char *out) {
 unsigned a, b, c, d;
 for (int i = 0; i < 49; i++) out[i] = 0;
 __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000000u), "c"(0));
 if (a < 0x80000004u) return;
 for (unsigned leaf = 0; leaf < 3; leaf++) {
  __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000002u + leaf), "c"(0));
  unsigned words[4] = {a, b, c, d};
  for (unsigned word = 0; word < 4; word++)
   for (unsigned byte = 0; byte < 4; byte++)
    out[leaf * 16 + word * 4 + byte] = (char)(words[word] >> (byte * 8));
 }
}

static unsigned long long read_tsc(void) {
 unsigned lo, hi;
 __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
 return ((unsigned long long)hi << 32) | lo;
}

static void seed_word(unsigned char *out, unsigned long len,
                      unsigned long *at, unsigned long long value) {
 for (int byte = 0; byte < 8 && *at < len; byte++)
  out[(*at)++] = (unsigned char)(value >> (byte * 8));
}

// Compatibility fallback matching Memento web::gatherEntropy. These timing
// samples have no established entropy estimate; BearSSL's HMAC-DRBG mixes
// them but cannot manufacture unpredictability. Do not call this a CSPRNG.
void r2tls_timing_seed(unsigned char *out, unsigned long len) {
 struct __attribute__((packed)) {
  unsigned char seconds, minutes, hours, day, month;
  unsigned short year;
 } rtc = {0};
 unsigned long at = 0;
 r2_syscall(0x02, 1, (unsigned long)&rtc);
 seed_word(out, len, &at, ((unsigned long long)rtc.year << 40) |
  ((unsigned long long)rtc.month << 32) | ((unsigned long long)rtc.day << 24) |
  ((unsigned long long)rtc.hours << 16) | ((unsigned long long)rtc.minutes << 8) | rtc.seconds);
 seed_word(out, len, &at, r2_syscall(0x04, 0, 0));
 seed_word(out, len, &at, (unsigned long)&at);
 unsigned long long previous = read_tsc();
 for (unsigned long sample = 0; at < len; sample++) {
  if ((sample & 63) == 63) r2_syscall(0x05, 1, 0);
  else r2_syscall(0x04, 0, 0);
  unsigned long long now = read_tsc();
  unsigned long long delta = now - previous;
  previous = now;
  out[at++] = (unsigned char)delta;
  if (at < len) out[at++] = (unsigned char)(delta >> 8) ^ (unsigned char)(now >> 3);
 }
}
