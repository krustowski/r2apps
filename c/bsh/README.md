# bsh

The base shell: the commands r2's shells have in common, written once.  `r2sh` (the console
shell, and the shell in Memento's Shell window) and `tnt` (the telnet one) both run on it, so a
command added or fixed here is in both, and `help` is made from the same table the commands are
dispatched by.

bsh is not a program.  Its sources are compiled into each shell that uses it:

```make
SOURCE_FILES := ../*.c ../../bsh/*.c      # tnt (Makefile.tmpl builds in ./build)
EXTRA_CFLAGS := -I ../../bsh
```

## Commands

| Command | What it does |
|---------|--------------|
| `help` | the commands, the host's included |
| `ls [path]` | list a directory; `/` and `/mnt` come from the mount table, with each mount's filesystem |
| `cd [path]` | change directory; alone, back to where the session started |
| `mkdir <path>` | make a directory: up to 8 characters, no dot, under `/mnt/fat` or `/mnt/tmp` |
| `rmdir <path>` | remove an empty directory |
| `rm <path>` | delete a file |
| `read <path>` | print a file, read whole into the user heap first (up to 8 MiB) |
| `mount` | the mounted filesystems |
| `sysinfo` | the kernel's system information |
| `bg <name> [args]` (`run`) | start a program in the background |
| `ts` | the tasks |
| `kill <pid>` | end a task |
| `meminfo` (`mem`) | RAM, process frames, the user heap in a line |
| `heap` | the user heap in detail, and who holds it |
| `play <name>`, `stop` | a MIDI file |
| `clear` | clear the screen, when the host can |
| `exit` (`quit`) | end the session |

Paths may be relative and use `.` and `..`.  A session starts in `/mnt/fat` when there is a floppy
to read and in `/` when there is not.  The prompt is `user@host:cwd> `, the user being the system
user (Memento's login sets it).  Changing the disk (`mkdir`, `rmdir`, `rm`) is only done on the FAT12
mounts, the floppy and `/mnt/tmp`; the kernel would take `/` for the floppy's root.

## Hosts

A host has the keyboard or the connection.  It reads and edits the line, and hands bsh each one:

```c
#include "bsh.h"

static void my_write(BshSession *s, const uint8_t *text, uint32_t len);  /* text with '\n' */

static int cmd_net(BshSession *s, const uint8_t *arg) { bsh_str(s, "...\n"); return 0; }

static const BshCommand mine[] = {
    {"net", "", "show the connections", cmd_net},
    {0, 0, 0, 0},
};

BshSession s;
bsh_init(&s, my_write, mine, my_data);      /* s.clear = ... when the screen can be cleared */
bsh_prompt(&s);
/* for each line typed: */
if (bsh_dispatch(&s, line, len) == BSH_EXIT) ...;
bsh_prompt(&s);
```

A host command with the name of a base one is used instead of it (tnt's `read` streams the file over
TCP with resends); `help` then shows the host's line for it.  bsh writes `\n` line ends; a host whose
terminal wants `\r\n` makes them in its write function, as tnt does.  `bsh_abs_path`,
`bsh_file_size` and `bsh_load_file` are there for the host's own commands, with the output helpers
(`bsh_str`, `bsh_u64`, `bsh_hex`, `bsh_ip`, `bsh_mac`).

Each session has its own working directory; the kernel's (one per process) is set to the session's
before each command, for `play` and `bg`, which take names relative to it.
