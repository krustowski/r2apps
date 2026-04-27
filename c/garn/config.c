#include "config.h"
#include "mem.h"
#include "string.h"
#include "syscall.h"

/*
 *  config.c
 *
 *  Loads a key=value config file from the filesystem into a GarnConfig_T.
 *  Lines starting with '#' and blank lines are ignored.
 *  Whitespace around keys and values is trimmed.
 *
 *  krusty@vxn.dev / Apr 26, 2026
 */

static uint8_t is_ws(uint8_t c) { return c == ' ' || c == '\t'; }

static uint16_t parse_u16(const uint8_t *s, uint32_t len) {
    uint16_t v = 0;
    for (uint32_t i = 0; i < len && s[i] >= '0' && s[i] <= '9'; i++)
        v = (uint16_t)(v * 10 + s[i] - '0');
    return v;
}

void config_defaults(GarnConfig_T *cfg) {
    cfg->port   = 80;
    cfg->debug  = 0;
    cfg->net[0] = 's'; cfg->net[1] = 'l'; cfg->net[2] = 'i';
    cfg->net[3] = 'p'; cfg->net[4] = '\0';
    cfg->path[0] = '\0';
}

void config_load(const uint8_t *path, GarnConfig_T *cfg) {
    static uint8_t buf[512];

    /* Capture current system_path before any chdir so we can restore it. */
    SysInfo_T si;
    si.system_path[0]  = '\0';
    si.system_path[31] = '\0';
    read_sysinfo(&si);
    si.system_path[31] = '\0'; /* clamp in case kernel didn't null-terminate */

    /* Build an absolute path.  Bare/relative names are prepended with
     * system_path so they resolve correctly on FAT12. */
    uint8_t abs[64];
    if (path[0] == '/') {
        uint32_t n = strlen(path);
        if (n > 63) n = 63;
        memcpy(abs, path, n);
        abs[n] = '\0';
    } else {
        uint32_t n = strlen(si.system_path);
        memcpy(abs, si.system_path, n);
        if (n > 0 && abs[n - 1] != '/') abs[n++] = '/';
        uint32_t flen = strlen(path);
        if (n + flen > 63) flen = 63 - n;
        memcpy(abs + n, path, flen);
        abs[n + flen] = '\0';
    }

    /* Try the full path first — works for ISO9660 and FAT12 root files. */
    int64_t flen = read_file(abs, buf);

    if (!flen) {
        /* ScReadFile can't walk FAT12 subdirectories via a path string.
         * Split abs into parent dir + basename, chdir, read, then restore.
         * Same pattern used in tnt/shell.c and r2sh/main.c. */
        uint32_t last = 0;
        for (uint32_t k = 0; abs[k]; k++)
            if (abs[k] == '/') last = k;

        uint8_t parent[64];
        uint32_t plen = (last == 0) ? 1 : last;
        memcpy(parent, abs, plen);
        parent[plen] = '\0';

        chdir(parent);
        flen = read_file(abs + last + 1, buf);
        if (si.system_path[0])
            chdir(si.system_path);
    }

    if (flen <= 0)
        return;

    /* read_file returns a success indicator, not a byte count — scan for the
     * null terminator to find the actual content length (same as the shells). */
    uint32_t len = 0;
    while (len < (uint32_t)(sizeof(buf) - 1) && buf[len])
        len++;
    if (len == 0)
        return;

    uint32_t i = 0;

    while (i < len) {
        while (i < len && is_ws(buf[i])) i++;

        /* skip blank lines and comments */
        if (i >= len || buf[i] == '#' || buf[i] == '\n' || buf[i] == '\r') {
            while (i < len && buf[i] != '\n') i++;
            if (i < len) i++;
            continue;
        }

        /* key: up to '=' */
        uint32_t ks = i;
        while (i < len && buf[i] != '=' && buf[i] != '\n') i++;
        if (i >= len || buf[i] != '=') {
            while (i < len && buf[i] != '\n') i++;
            if (i < len) i++;
            continue;
        }
        uint32_t ke = i;
        while (ke > ks && is_ws(buf[ke - 1])) ke--;

        i++; /* skip '=' */
        while (i < len && is_ws(buf[i])) i++;

        /* value: up to newline */
        uint32_t vs = i;
        while (i < len && buf[i] != '\n' && buf[i] != '\r') i++;
        uint32_t ve = i;
        while (ve > vs && is_ws(buf[ve - 1])) ve--;

        uint32_t klen = ke - ks;
        uint32_t vlen = ve - vs;

        if (klen == 4 && memcmp(buf + ks, (const uint8_t *)"port", 4) == 0) {
            uint16_t p = parse_u16(buf + vs, vlen);
            if (p) cfg->port = p;
        } else if (klen == 3 && memcmp(buf + ks, (const uint8_t *)"net", 3) == 0) {
            uint32_t n = vlen < 7 ? vlen : 7;
            memcpy(cfg->net, buf + vs, n);
            cfg->net[n] = '\0';
        } else if (klen == 5 && memcmp(buf + ks, (const uint8_t *)"debug", 5) == 0) {
            cfg->debug = (vlen > 0 && buf[vs] == '1') ? 1 : 0;
        } else if (klen == 4 && memcmp(buf + ks, (const uint8_t *)"path", 4) == 0) {
            uint32_t n = vlen < 63 ? vlen : 63;
            memcpy(cfg->path, buf + vs, n);
            cfg->path[n] = '\0';
        }

        if (i < len && buf[i] == '\r') i++;
        if (i < len && buf[i] == '\n') i++;
    }
}
