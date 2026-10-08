#ifndef _BSH_H_
#define _BSH_H_

#include "types.h"

/*
 *  bsh --- the base shell.
 *
 *  The commands r2's shells have in common, written once: the console shell
 *  (r2sh, also the shell in Memento's Shell window) and the telnet one (tnt)
 *  both run on it, so a command added or fixed here is there in both, and
 *  `help` is made from the same table it dispatches by.
 *
 *  A host --- the program that has the keyboard or the connection --- gives
 *  each session a way to write, and may add commands of its own: tnt has
 *  `get` and `net`, and a `read` that streams over TCP.  A host command
 *  with the name of a base one is used instead of it.  Reading the input,
 *  echoing it and editing the line stay the host's.
 *
 *  Text goes out with '\n' line ends; a host whose terminal wants "\r\n"
 *  makes them in its write function.
 *
 *  Colour is off unless the host turns it on: a session with `color` set
 *  colours the prompt and some output with ANSI sequences (ESC [ ... m),
 *  which the kernel's console and Memento's Shell window understand.  One
 *  whose host leaves `color_ok` clear (tnt) never has any, `color on` or not.
 */

#define BSH_PATH 64       /* an absolute VFS path, NUL included */
#define BSH_EXIT 1        /* from a command or bsh_dispatch(): the session ends */
#define BSH_SIZE_UNKNOWN (-2) /* bsh_file_size(): readable, size not listed */
#define BSH_READ_MAX (8u * 1024 * 1024) /* the most bsh_load_file() reads */

typedef struct BshSession BshSession;

typedef struct {
    const char *name;  /* what is typed: "ls" */
    const char *usage; /* its arguments for `help`: "[path]", or "" */
    const char *help;  /* one line for `help`; 0 keeps it out (an alias) */
    /* <arg> is what follows the name and its spaces, "" when nothing does.
     * Returns BSH_EXIT to end the session, else 0. */
    int (*run)(BshSession *s, const uint8_t *arg);
} BshCommand;

struct BshSession {
    uint8_t cwd[BSH_PATH];   /* the working directory */
    uint8_t start[BSH_PATH]; /* where the session started: `cd` alone */
    /* Where the session's text goes. */
    void (*write)(BshSession *s, const uint8_t *text, uint32_t len);
    /* Clears the screen; 0 when the host cannot. */
    void (*clear)(BshSession *s);
    uint8_t color;    /* colour the output: off unless the host turns it on */
    uint8_t color_ok; /* the host can show colour: `color on` may turn it on */
    /* The host's own commands, ended by one with no name; 0 for none. */
    const BshCommand *commands;
    void *host; /* the host's, for its commands */
};

/*
 *  A new session, in /mnt/fat when there is a floppy to read and in / when
 *  there is not (the kernel keeps /mnt/fat in its mount table with no disk in
 *  the drive).
 */
void bsh_init(BshSession *s, void (*write)(BshSession *, const uint8_t *, uint32_t), const BshCommand *commands,
              void *host);

/*  Changes the session's directory without a word; 0 when there is no such
 *  directory.  The host's way to start somewhere else.  */
int bsh_cd(BshSession *s, const uint8_t *path);

/*  "user@host:/mnt/fat> ": the system user (Memento's login sets it), the
 *  host name and the working directory.  */
void bsh_prompt(BshSession *s);

/*  Runs one typed line (which it may change: it is trimmed in place).
 *  Returns BSH_EXIT when the session is to end.  */
int bsh_dispatch(BshSession *s, uint8_t *line, uint32_t len);

/*
 *  Output, for the host's commands too.
 */
void bsh_out(BshSession *s, const uint8_t *text, uint32_t len);
void bsh_str(BshSession *s, const char *text);
void bsh_ustr(BshSession *s, const uint8_t *text);
/*  A number, right-aligned in <width> columns (0: as wide as it is).  */
void bsh_u64(BshSession *s, uint64_t v, uint8_t width);
/*  "0xC00000": hex in as few digits as it takes, or <digits> of them.  */
void bsh_hex(BshSession *s, uint64_t v, uint8_t digits);
void bsh_ip(BshSession *s, const uint8_t ip[4]);
void bsh_mac(BshSession *s, const uint8_t mac[6]);

/*  Colour from here on, as an SGR parameter list (BSH_* below, or any such
 *  as "1;33"), when the session has colour; nothing when it has not.  */
void bsh_color(BshSession *s, const char *sgr);
/*  <text> in a colour, and the colour back to plain after it.  */
void bsh_label(BshSession *s, const char *sgr, const char *text);

#define BSH_RESET "0"
#define BSH_RED "1;31"
#define BSH_GREEN "1;32"
#define BSH_YELLOW "1;33"
#define BSH_BLUE "1;34"
#define BSH_MAGENTA "1;35"
#define BSH_CYAN "1;36"
#define BSH_GREY "0;37"

/*  What the colours are for, so that commands agree on them.  */
#define BSH_C_ERROR BSH_RED     /* something went wrong */
#define BSH_C_USAGE BSH_YELLOW  /* how a command is used */
#define BSH_C_HEAD BSH_YELLOW   /* a table's headings */
#define BSH_C_LABEL BSH_CYAN    /* "System:", "RAM" */
#define BSH_C_OK BSH_GREEN      /* done: launched, killed */
#define BSH_C_DIM BSH_GREY      /* by the way: "(empty)", "free" */
#define BSH_C_DIR BSH_BLUE      /* a directory */

/*
 *  Files.
 */

/*  <arg> as an absolute path: from the working directory when it is
 *  relative, with "." and ".." worked out and doubled or trailing slashes
 *  dropped.  "/mnt/fat/.." is "/mnt"; ".." at the root stays there.  */
void bsh_abs_path(BshSession *s, uint8_t out[BSH_PATH], const uint8_t *arg);

/*  The size of the file at absolute <path>: -1 when it cannot be read as a
 *  file, BSH_SIZE_UNKNOWN when it can but its directory does not say.  */
int64_t bsh_file_size(const uint8_t *path);

/*
 *  The whole file at absolute <path> in a block of the user heap (syscall
 *  0x0a, which grows past its first 4 MiB when it has to): *buf to free()
 *  and *len.  Up to BSH_READ_MAX.  Returns 1, or 0 when it says why not to
 *  the session, as "<cmd>: ...".
 */
int bsh_load_file(BshSession *s, const char *cmd, const uint8_t *path, uint8_t **buf, uint32_t *len);

#endif
