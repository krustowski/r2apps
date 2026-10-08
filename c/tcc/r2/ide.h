/* tcpp -> tcc.elf --ide 0xADDRESS, on r2's shared user heap.
 * The editor owns the allocation until the compiler task has exited.
 * The compiler publishes diagnostics and result before setting DONE. */
#ifndef R2_TCC_IDE_H
#define R2_TCC_IDE_H

#ifdef __cplusplus
#include <r2/types.hpp>
#else
#include <stdint.h>
#endif

#define R2_TCC_IDE_MAGIC 0x49434354u /* "TCCI" */
#define R2_TCC_IDE_VERSION 1u
#define R2_TCC_IDE_PENDING 0u
#define R2_TCC_IDE_RUNNING 1u
#define R2_TCC_IDE_DONE 2u
#define R2_TCC_IDE_PATH_SIZE 64u
#define R2_TCC_IDE_DIAG_SIZE 8192u

typedef struct {
    uint32_t magic;
    uint32_t version;
    volatile uint32_t state;
    int32_t result; /* zero means the executable was written */
    char source[R2_TCC_IDE_PATH_SIZE];
    char output[R2_TCC_IDE_PATH_SIZE];
    uint32_t diagnostic_length;
    uint32_t diagnostic_truncated;
    char diagnostics[R2_TCC_IDE_DIAG_SIZE];
} R2TccIde;

#endif
