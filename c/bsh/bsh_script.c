#include "mem.h"
#include "string.h"
#include "syscall.h"

#include "bsh.h"
#include "bsh_int.h"

/*
 *  Scripts: `bsh <file> [args]` runs a .BSH file, a command a line, in the
 *  session it is typed in --- a `cd` in it stays done, as with `source` ---
 *  and `echo` and `sleep` are there for them.
 */

#define SCRIPT_DEPTH 4 /* scripts running scripts, at most */
#define SCRIPT_LINE 160
#define SCRIPT_ARGS 10 /* $0 (the script) to $9 */

static int depth;

int bsh_cmd_echo(BshSession *s, const uint8_t *arg) {
    bsh_ustr(s, arg);
    bsh_str(s, "\n");
    return 0;
}

int bsh_cmd_sleep(BshSession *s, const uint8_t *arg) {
    uint64_t ms = 0;
    uint8_t n = 0;
    while (arg[n] >= '0' && arg[n] <= '9' && n < 9)
        ms = ms * 10 + (uint64_t)(arg[n++] - '0');
    if (!n || arg[n]) {
        bsh_label(s, BSH_C_USAGE, "sleep: usage: sleep <milliseconds>\n");
        return 0;
    }
    if (s->flush)
        s->flush(s); /* what came before is seen before the wait */
    /*  The kernel's sleep may end early (a network frame for the process
     *  wakes it), so it is slept until the clock says the time is up.  */
    uint64_t end = get_ticks() + ms;
    for (uint64_t now = get_ticks(); now < end; now = get_ticks())
        sleep_ms(end - now);
    return 0;
}

/*  <line> with $0..$9 put in from <argv>, and "$$" as a "$", into <out>.  */
static uint32_t expand(const uint8_t *line, uint32_t len, const uint8_t *argv[SCRIPT_ARGS],
                       const uint8_t argl[SCRIPT_ARGS], int argc, uint8_t *out) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < len && n < SCRIPT_LINE - 1; i++) {
        if (line[i] == '$' && i + 1 < len && line[i + 1] >= '0' && line[i + 1] <= '9') {
            int k = line[++i] - '0';
            for (uint8_t j = 0; k < argc && j < argl[k] && n < SCRIPT_LINE - 1; j++)
                out[n++] = argv[k][j];
            continue;
        }
        if (line[i] == '$' && i + 1 < len && line[i + 1] == '$')
            i++;
        out[n++] = line[i];
    }
    out[n] = '\0';
    return n;
}

int bsh_cmd_bsh(BshSession *s, const uint8_t *arg) {
    /*  The script and its arguments, split at the spaces: argv[0] is the
     *  script as it was named.  */
    const uint8_t *argv[SCRIPT_ARGS];
    uint8_t argl[SCRIPT_ARGS];
    int argc = 0;
    for (const uint8_t *p = arg; *p && argc < SCRIPT_ARGS;) {
        while (*p == ' ')
            p++;
        if (!*p)
            break;
        argv[argc] = p;
        uint8_t l = 0;
        while (p[l] && p[l] != ' ' && l < 255)
            l++;
        argl[argc++] = l;
        p += l;
    }
    if (!argc) {
        bsh_label(s, BSH_C_USAGE, "bsh: usage: bsh <file.bsh> [args...]\n");
        return 0;
    }
    if (depth >= SCRIPT_DEPTH) {
        bsh_label(s, BSH_C_ERROR, "bsh: scripts are nested too deep\n");
        return 0;
    }

    uint8_t name[BSH_PATH], abs[BSH_PATH];
    uint8_t nl = argl[0] < BSH_PATH - 1 ? argl[0] : BSH_PATH - 1;
    memcpy(name, argv[0], nl);
    name[nl] = '\0';
    bsh_abs_path(s, abs, name);

    uint8_t *buf;
    uint32_t len;
    if (!bsh_load_file(s, "bsh", abs, &buf, &len))
        return 0;

    /*  A line at a time: blank ones and comments ("#", a "#!" first line
     *  too) skipped, "\r\n" taken as well as "\n".  `exit` ends the script,
     *  not the session.  */
    depth++;
    uint8_t line[SCRIPT_LINE];
    for (uint32_t at = 0; at < len;) {
        uint32_t end = at;
        while (end < len && buf[end] != '\n')
            end++;
        uint32_t from = at, to = end;
        at = end + 1;
        while (from < to && (buf[from] == ' ' || buf[from] == '\t'))
            from++;
        while (to > from && (buf[to - 1] == '\r' || buf[to - 1] == ' ' || buf[to - 1] == '\t'))
            to--;
        if (from == to || buf[from] == '#')
            continue;
        uint32_t n = expand(buf + from, to - from, argv, argl, argc, line);
        int done = bsh_dispatch(s, line, n) == BSH_EXIT;
        if (s->flush)
            s->flush(s); /* each line's output as it comes */
        if (done)
            break;
    }
    depth--;
    free(buf);
    return 0;
}
