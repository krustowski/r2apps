/*
 *  tcc on r2: tinycc's own driver, given the options every r2 program needs.
 *
 *  The kernel has no dynamic loader and puts a program's segments at 0x600000
 *  and up, so each link is static with .text at 0x600000.  tcc has no config
 *  file to keep that in, and its sources stay as upstream has them, so the
 *  options are added here --- after the user's own, where they cannot be taken
 *  for the operands of `tcc -ar`, which gets none.
 */
#include "../libc/r2sys.h"

#define main tcc_main
#include "tcc.c"
#undef main

#include "ide.h"

static char *r2_options[] = {"-static", "-Wl,-Ttext=0x600000", "-D__r2__=1"};

static void ide_error(void *opaque, const char *message)
{
    R2TccIde *request = opaque;
    size_t n = strlen(message), room = sizeof(request->diagnostics) - 1 - request->diagnostic_length;
    if (n + 1 > room) {
        request->diagnostic_truncated = 1;
        n = room;
    }
    memcpy(request->diagnostics + request->diagnostic_length, message, n);
    request->diagnostic_length += n;
    if (n < room)
        request->diagnostics[request->diagnostic_length++] = '\n';
    request->diagnostics[request->diagnostic_length] = 0;
}

static int ide_build(const char *address)
{
    char *end;
    unsigned long addr = strtoul(address, &end, 16);
    R2TccIde *request;
    TCCState *s;
    int result = -1;

    /* A whole aligned block must fit in the shared heap.  Never use a
     * pointer into the private image or accept a partially parsed address. */
    if (!*address || *end || (addr & 7) || !shared_heap_contains((void *)addr, sizeof(R2TccIde)))
        return 1;
    request = (R2TccIde *)addr;
    if (request->magic != R2_TCC_IDE_MAGIC || request->version != R2_TCC_IDE_VERSION ||
        request->state != R2_TCC_IDE_PENDING)
        return 1;
    request->diagnostic_length = request->diagnostic_truncated = 0;
    request->diagnostics[0] = 0;
    request->state = R2_TCC_IDE_RUNNING;
    if (!memchr(request->source, 0, sizeof(request->source)) ||
        !memchr(request->output, 0, sizeof(request->output)) ||
        request->source[0] != '/' || request->output[0] != '/' ||
        !strcmp(request->source, request->output)) {
        ide_error(request, "Invalid source or output path");
    } else {
        s = tcc_new();
        tcc_set_error_func(s, request, ide_error);
        /* FAT listings use .C, while upstream guesses extensions with
         * case-sensitive comparisons.  The IDE explicitly builds C. */
        result = tcc_set_options(s, "-static -Wl,-Ttext=0x600000 -D__r2__=1 -x c");
        if (!result)
            result = tcc_set_output_type(s, TCC_OUTPUT_EXE);
        if (!result)
            result = tcc_add_file(s, request->source);
        if (!result)
            result = tcc_add_library(s, "cr2");
        if (!result)
            result = tcc_output_file(s, request->output);
        tcc_delete(s);
    }
    request->result = result;
    __asm__ volatile("" ::: "memory");
    request->state = R2_TCC_IDE_DONE;
    return result != 0;
}

int main(int argc, char **argv)
{
    char **args;
    int i, n = 0;

    if (argc > 1 && !strcmp(argv[1], "--ide"))
        return argc == 3 ? ide_build(argv[2]) : 1;

    if (argc > 1 && (!strcmp(argv[1], "-ar") || !strcmp(argv[1], "-impdef")))
        return tcc_main(argc, argv);

    args = tcc_malloc((argc + countof(r2_options) + 1) * sizeof(*args));
    for (i = 0; i < argc; i++)
        args[n++] = argv[i];
    for (i = 0; i < (int)countof(r2_options); i++)
        args[n++] = r2_options[i];
    args[n] = 0;
    return tcc_main(n, args);
}
