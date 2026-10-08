#include "mem.h"
#include "string.h"
#include "syscall.h"

#include "bsh.h"
#include "bsh_int.h"

/*
 *  The system: sysinfo, bg, ts, kill, meminfo, heap, play, stop.
 */

static void label(BshSession *s, const char *text) { bsh_label(s, BSH_C_LABEL, text); }
static void dim(BshSession *s, const char *text) { bsh_label(s, BSH_C_DIM, text); }
static void error(BshSession *s, const char *text) { bsh_label(s, BSH_C_ERROR, text); }
static void usage(BshSession *s, const char *text) { bsh_label(s, BSH_C_USAGE, text); }

int bsh_cmd_sysinfo(BshSession *s, const uint8_t *arg) {
    (void)arg;
    SysInfo_T si = {0};
    if (!read_sysinfo(&si)) {
        error(s, "sysinfo: failed\n");
        return 0;
    }
    si.system_name[31] = '\0';
    si.system_user[31] = '\0';
    si.system_path[31] = '\0';
    si.system_version[7] = '\0';

    label(s, "System:  ");
    bsh_ustr(s, si.system_name);
    label(s, "\nUser:    ");
    bsh_color(s, BSH_GREEN);
    bsh_ustr(s, si.system_user);
    bsh_color(s, BSH_RESET);
    label(s, "\nPath:    ");
    bsh_color(s, BSH_C_DIR);
    bsh_ustr(s, si.system_path);
    bsh_color(s, BSH_RESET);
    dim(s, " (cluster: ");
    bsh_color(s, BSH_C_DIM);
    bsh_u64(s, si.system_path_cluster, 0);
    bsh_color(s, BSH_RESET);
    dim(s, ")");
    label(s, "\nVersion: ");
    bsh_ustr(s, si.system_version);
    label(s, "\nUptime:  ");
    bsh_u64(s, si.system_uptime / 3600, 0);
    bsh_str(s, "h ");
    bsh_u64(s, (si.system_uptime % 3600) / 60, 0);
    bsh_str(s, "m ");
    bsh_u64(s, si.system_uptime % 60, 0);
    bsh_str(s, "s\n");
    return 0;
}

/*  `bg <name> [args]`, also `run`: the program from the kernel's search
 *  (the working directory, then /mnt/iso/bin and the like), in the
 *  background; its own output goes to the console.  */
int bsh_cmd_bg(BshSession *s, const uint8_t *arg) {
    /* The binary name (up to the first space) for the file lookup. */
    uint8_t name[13];
    uint8_t i = 0;
    while (i < 12 && arg[i] && arg[i] != ' ') {
        name[i] = arg[i];
        i++;
    }
    name[i] = '\0';
    if (!i) {
        usage(s, "bg: usage: bg <name> [args...]\n");
        return 0;
    }

    /* The whole line (name and arguments) is the argv the kernel tokenises:
     * argv[0]=name, argv[1]=first argument, ... */
    uint8_t pid = 0;
    if (!run_elf(name, arg, &pid)) {
        bsh_color(s, BSH_C_ERROR);
        bsh_str(s, "bg: failed to launch '");
        bsh_ustr(s, name);
        bsh_str(s, "'\n");
        bsh_color(s, BSH_RESET);
    } else {
        bsh_color(s, BSH_C_OK);
        bsh_str(s, "bg: launched '");
        bsh_ustr(s, name);
        bsh_str(s, "' pid=");
        bsh_u64(s, pid, 0);
        bsh_color(s, BSH_RESET);
        bsh_str(s, "\n");
    }
    return 0;
}

static const char *const task_status[] = {
    "Ready   ", "Running ", "Idle    ", "Blocked ", "Crashed ", "Dead    ",
};
/*  And how each looks: running is good news, crashed is bad.  */
static const char *const task_status_color[] = {
    BSH_CYAN, BSH_GREEN, BSH_GREY, BSH_YELLOW, BSH_RED, BSH_GREY,
};

/*  The name of a task, trimmed; how many characters it has.  */
static uint8_t task_name_len(const TaskInfo_T *t) {
    uint8_t n = 0;
    while (n < 16 && t->name[n] && t->name[n] != ' ')
        n++;
    return n;
}

static void pad(BshSession *s, uint8_t from, uint8_t to) {
    for (; from < to; from++)
        bsh_str(s, " ");
}

int bsh_cmd_ts(BshSession *s, const uint8_t *arg) {
    (void)arg;
    TaskInfo_T tasks[R2_MAX_SLOTS];
    int64_t count = list_tasks(tasks, R2_MAX_SLOTS);
    if (count <= 0) {
        dim(s, "No tasks.\n");
        return 0;
    }

    bsh_label(s, BSH_C_HEAD, "PID  M  STATUS    NAME              RIP\n");
    for (int64_t i = 0; i < count; i++) {
        TaskInfo_T *t = &tasks[i];
        bsh_u64(s, t->id, 3);
        bsh_str(s, "  ");
        if (t->mode == 0)
            dim(s, "K  "); /* the kernel's own */
        else
            bsh_str(s, "U  ");
        uint8_t st = t->status < 6 ? t->status : 0;
        bsh_label(s, task_status_color[st], task_status[st]);
        bsh_str(s, "  ");
        uint8_t nlen = task_name_len(t);
        if (t->mode != 0)
            bsh_color(s, BSH_GREEN);
        bsh_out(s, t->name, nlen);
        bsh_color(s, BSH_RESET);
        pad(s, nlen, 18); /* so the addresses line up */
        bsh_color(s, BSH_C_DIM);
        bsh_hex(s, t->rip, 16);
        bsh_color(s, BSH_RESET);
        bsh_str(s, "\n");
    }
    return 0;
}

/*
 *  End a process by the PID `ts` printed.  A program that stops answering its
 *  own keys cannot be quit from the machine it runs on; this is the way.
 */
int bsh_cmd_kill(BshSession *s, const uint8_t *arg) {
    uint64_t pid = 0;
    uint8_t digits = 0;
    while (arg[digits] >= '0' && arg[digits] <= '9') {
        pid = pid * 10 + (uint64_t)(arg[digits] - '0');
        digits++;
    }
    if (!digits || arg[digits]) {
        usage(s, "kill: usage: kill <pid>\n");
        return 0;
    }
    if (kill_task(pid)) {
        bsh_color(s, BSH_C_OK);
        bsh_str(s, "kill: killed PID ");
        bsh_u64(s, pid, 0);
        bsh_color(s, BSH_RESET);
        bsh_str(s, "\n");
    } else {
        error(s, "kill: no such PID\n");
    }
    return 0;
}

/* Bytes as KiB, one decimal, right-aligned in 9 columns: "   1024.0 KiB". */
static void kib(BshSession *s, uint64_t bytes) {
    uint64_t tenths = (bytes * 10 + 512) / 1024;
    bsh_u64(s, tenths / 10, 7);
    bsh_str(s, ".");
    bsh_u64(s, tenths % 10, 1);
    bsh_str(s, " KiB");
}

static const TaskInfo_T *find_task(const TaskInfo_T *tasks, int64_t count, uint8_t id) {
    for (int64_t i = 0; i < count; i++)
        if (tasks[i].id == id)
            return &tasks[i];
    return 0;
}

/* The name of task <id>, or "?"; how many characters it wrote. */
static uint8_t task_name(BshSession *s, const TaskInfo_T *tasks, int64_t count, uint8_t id) {
    const TaskInfo_T *t = find_task(tasks, count, id);
    if (!t) {
        bsh_str(s, "?");
        return 1;
    }
    uint8_t n = task_name_len(t);
    bsh_out(s, t->name, n);
    return n;
}

/*
 *  The kernel's figures (syscall 0x3c), read once for both commands.  The
 *  kernel answers busy when the heap or the scheduler is locked that instant,
 *  so it is asked a few times before giving up.
 */
static int read_mem(BshSession *s, MemInfo_T *mi) {
    for (int tries = 0; tries < 20; tries++) {
        if (read_meminfo(mi))
            return 1;
        sleep_ms(2);
    }
    error(s, "memory information not available (kernel busy, or older than syscall 0x3c)\n");
    return 0;
}

/*
 *  meminfo: the machine's memory at a glance --- the RAM, each process's
 *  private 2 MiB frame and who is in it, and the user heap in one line.
 */
int bsh_cmd_meminfo(BshSession *s, const uint8_t *arg) {
    (void)arg;
    MemInfo_T mi;
    if (!read_mem(s, &mi))
        return 0;

    TaskInfo_T tasks[R2_MAX_SLOTS];
    int64_t count = list_tasks(tasks, R2_MAX_SLOTS);
    if (count < 0)
        count = 0;

    label(s, "RAM            ");
    bsh_u64(s, mi.total_ram / (1024 * 1024), 0);
    bsh_str(s, " MiB (");
    bsh_u64(s, mi.total_ram, 0);
    bsh_str(s, " bytes)\n");

    /* Kernel tasks take a slot too, but run on the kernel's own mappings:
     * only a user program's frame holds anything. */
    uint64_t slots = mi.slots < R2_MAX_SLOTS ? mi.slots : R2_MAX_SLOTS;
    uint64_t in_use = 0;
    for (uint64_t k = 0; k < slots; k++) {
        const TaskInfo_T *t = mi.slot_task[k] == 0xFF ? 0 : find_task(tasks, count, mi.slot_task[k]);
        if (t && t->mode != 0)
            in_use++;
    }

    label(s, "Process frames ");
    bsh_u64(s, slots, 0);
    bsh_str(s, " x ");
    bsh_u64(s, mi.frame_size / 1024, 0);
    bsh_str(s, " KiB, each seen at ");
    bsh_hex(s, mi.frame_virt, 0);
    bsh_str(s, "; ");
    bsh_u64(s, in_use, 0);
    bsh_str(s, " held by programs\n");

    /* The free ones are counted rather than listed: most of 32 are. */
    bsh_label(s, BSH_C_HEAD, "  SLOT  PHYSICAL    PID  NAME\n");
    uint64_t free_slots = 0;
    for (uint64_t k = 0; k < slots; k++) {
        if (mi.slot_task[k] == 0xFF) {
            free_slots++;
            continue;
        }
        bsh_u64(s, k, 6);
        bsh_str(s, "  ");
        bsh_hex(s, mi.frame_base + k * mi.frame_size, 0);
        bsh_str(s, "  ");
        bsh_u64(s, mi.slot_task[k], 4);
        bsh_str(s, "  ");
        uint8_t nlen = task_name(s, tasks, count, mi.slot_task[k]);
        const TaskInfo_T *t = find_task(tasks, count, mi.slot_task[k]);
        if (t && t->mode == 0) {
            pad(s, nlen, 10);
            dim(s, "(kernel task: frame unused)");
        }
        bsh_str(s, "\n");
    }
    if (free_slots) {
        bsh_color(s, BSH_C_DIM);
        bsh_str(s, "  ");
        bsh_u64(s, free_slots, 0);
        bsh_str(s, free_slots == 1 ? " slot free\n" : " slots free\n");
        bsh_color(s, BSH_RESET);
    }

    label(s, "User heap      ");
    bsh_u64(s, mi.heap_size / 1024, 0);
    bsh_str(s, " KiB at ");
    bsh_hex(s, mi.heap_start, 0);
    bsh_str(s, ": ");
    bsh_u64(s, (mi.heap_used + 512) / 1024, 0);
    bsh_str(s, " KiB used, ");
    bsh_u64(s, (mi.heap_free + 512) / 1024, 0);
    bsh_str(s, " KiB free, largest ");
    bsh_u64(s, (mi.heap_largest_free + 512) / 1024, 0);
    bsh_str(s, " KiB ");
    dim(s, "('heap' for more)");
    bsh_str(s, "\n");
    return 0;
}

/*
 *  heap: the user heap (malloc, syscall 0x0a) in detail --- what is used, what
 *  is free and how broken up it is, and which task holds how much.
 */
int bsh_cmd_heap(BshSession *s, const uint8_t *arg) {
    (void)arg;
    MemInfo_T mi;
    if (!read_mem(s, &mi))
        return 0;

    TaskInfo_T tasks[R2_MAX_SLOTS];
    int64_t count = list_tasks(tasks, R2_MAX_SLOTS);
    if (count < 0)
        count = 0;

    uint64_t headers = mi.heap_size - mi.heap_used - mi.heap_free;

    label(s, "User heap ");
    bsh_hex(s, mi.heap_start, 0);
    bsh_str(s, "-");
    bsh_hex(s, mi.heap_start + mi.heap_size - 1, 0);
    dim(s, ", shared by every process");
    bsh_str(s, "\n");

    label(s, "  size         ");
    kib(s, mi.heap_size);
    label(s, "\n  used         ");
    kib(s, mi.heap_used);
    bsh_str(s, "  ");
    bsh_u64(s, mi.heap_size ? mi.heap_used * 100 / mi.heap_size : 0, 3);
    bsh_str(s, "%");
    label(s, "\n  free         ");
    kib(s, mi.heap_free);
    bsh_str(s, "  ");
    bsh_u64(s, mi.heap_size ? mi.heap_free * 100 / mi.heap_size : 0, 3);
    bsh_str(s, "%");
    label(s, "\n  largest free ");
    kib(s, mi.heap_largest_free);
    dim(s, "  (the biggest one malloc can get)");
    label(s, "\n  headers      ");
    kib(s, headers);
    label(s, "\n  blocks       ");
    bsh_u64(s, mi.heap_blocks, 9);
    bsh_str(s, "      (");
    bsh_u64(s, mi.heap_free_blocks, 0);
    bsh_str(s, " free)\n");

    /* Free space in many pieces is space a big request cannot have. */
    if (mi.heap_free) {
        label(s, "  fragmented   ");
        bsh_u64(s, 100 - mi.heap_largest_free * 100 / mi.heap_free, 9);
        bsh_str(s, "%     of the free space is outside the largest block\n");
    }

    bsh_str(s, "\n");
    bsh_label(s, BSH_C_HEAD, "Held by\n  SLOT   PID  NAME                 USED\n");
    int any = 0;
    for (int k = 0; k <= R2_MAX_SLOTS; k++) {
        uint64_t bytes = mi.heap_by_slot[k];
        if (!bytes)
            continue;
        any = 1;
        if (k == R2_MAX_SLOTS) {
            dim(s, "     -     -  (no owner)      ");
        } else {
            bsh_u64(s, (uint64_t)k, 6);
            uint8_t id = mi.slot_task[k];
            if (id == 0xFF) {
                /* Bytes tagged to a slot with nobody in it: a leak, or a sweep
                 * that has not run yet. */
                error(s, "     -  (exited, not freed) ");
            } else {
                bsh_u64(s, id, 6);
                bsh_str(s, "  ");
                pad(s, task_name(s, tasks, count, id), 14);
            }
        }
        kib(s, bytes);
        bsh_str(s, "\n");
    }
    if (!any)
        dim(s, "  nothing is allocated\n");
    return 0;
}

int bsh_cmd_play(BshSession *s, const uint8_t *arg) {
    if (!arg[0]) {
        usage(s, "play: usage: play <name>\n");
        return 0;
    }
    /* Blocks until the song is over; 0 when the file could not be read or
     * parsed.  A relative name is found in the working directory, on the
     * floppy or on the tar archive alike. */
    if (!play_midi_file(arg)) {
        bsh_color(s, BSH_C_ERROR);
        bsh_str(s, "play: failed to open '");
        bsh_ustr(s, arg);
        bsh_str(s, "'\n");
        bsh_color(s, BSH_RESET);
    } else {
        bsh_color(s, BSH_C_OK);
        bsh_str(s, "play: finished '");
        bsh_ustr(s, arg);
        bsh_str(s, "'\n");
        bsh_color(s, BSH_RESET);
    }
    return 0;
}

int bsh_cmd_stop(BshSession *s, const uint8_t *arg) {
    (void)arg;
    stop_speaker();
    bsh_label(s, BSH_C_OK, "play: stopped\n");
    return 0;
}
