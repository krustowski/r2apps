/*
 *  r2ct --- the C library and libcr2 in one program, as tcc builds them on r2
 *  (`tcc -o r2ct.elf r2ct.c -lcr2`).
 *
 *  libcr2's headers come in among the C library's on purpose: they have to
 *  agree on the integer types and leave exit, chdir, read, write, close,
 *  printf, strlen and memcpy to it, whichever is read first.  Results go to
 *  /mnt/fat/R2CT.TXT.
 */
#include <r2/syscall.h>
#include <stdint.h>
#include <stdio.h>
#include <r2/net.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <r2/bytes.h>
#include <r2/printf.h>
#include <r2/string.h>

static char out[2048];
static size_t len;
static int passed, total;

static void check(const char *name, int ok)
{
    total++;
    passed += ok;
    len += (size_t)snprintf(out + len, sizeof(out) - len, "%s %s\n", ok ? "ok  " : "FAIL", name);
}

int main(void)
{
    RTC_T rtc;
    uint64_t t0 = get_ticks(), t1;
    char cwd[64], num[12], name[64];
    uint8_t mac[6] = {1, 2, 3, 4, 5, 6};
    /* taking the addresses links libcr2's TCP/IP stack in next to the C library's fds */
    uint32_t (*rd)(TcpSocket_T *, uint8_t *, uint32_t) = tcp_read;
    void (*cl)(TcpSocket_T *) = tcp_close;
    int (*posix_close)(int) = close;
    FILE *f;

    check("int8_t is signed char", sizeof(int8_t) == 1 && (int8_t)0xff < 0);
    check("ticks", t0 > 0);
    sleep_ms(30);
    t1 = get_ticks();
    snprintf(name, sizeof(name), "sleep_ms(30): %lu ms by get_ticks", (unsigned long)(t1 - t0));
    check(name, t1 >= t0 + 30);
    memset(&rtc, 0, sizeof(rtc));
    read_rtc(&rtc);
    check("read_rtc", rtc.year >= 2024 && rtc.month >= 1 && rtc.month <= 12);
    print((const uint8_t *)"r2ct: libcr2's print\n");
    check("bytes.h", swap16(0x1234) == 0x3412 && swap32(0x11223344) == 0x44332211 &&
                         swap64(0x0102030405060708UL) == 0x0807060504030201UL &&
                         htons(0x0102) == 0x0201);
    u32_to_str(4096, (uint8_t *)num);
    check("u32_to_str", !strcmp(num, "4096"));
    check("strlen is the C library's", strlen("abc") == 3);
    check("net symbols", rd != 0 && cl != 0 && posix_close != 0 && mac[5] == 6);

    check("chdir/getcwd", chdir("/mnt/fat") == 0 && getcwd(cwd, sizeof(cwd)) &&
                              !strncmp(cwd, "/mnt/fat", 8));
    check("chdir missing", chdir("/mnt/nowhere") == -1);

    len += (size_t)snprintf(out + len, sizeof(out) - len, "passed %d/%d\n", passed, total);
    printf("%s", out);
    remove("R2CT.TXT");
    f = fopen("R2CT.TXT", "w");
    if (!f)
        return 2;
    fputs(out, f);
    fclose(f);
    exit(passed == total ? 0 : 1);
}
