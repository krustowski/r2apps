#include "types.h"

typedef struct {
    /* 16-bit general-purpose (GP) registers */
    uint16_t AX;
    uint16_t BX;
    uint16_t CX;
    uint16_t DX;

    /* The stack pointer (top of the stack) */
    uint16_t SP;
    /* Base pointer */
    uint16_t BP;

    /* Address registers */
    uint16_t SI;
    uint16_t DI;

    /* Segment registers (to form a memory address) */
    uint16_t CS;
    uint16_t DS;
    uint16_t ES;
    uint16_t SS;

    uint16_t FLAGS;
    uint16_t IP;
} __attribute__((packed)) CPU_T;

typedef struct {
    uint8_t bytes[1 << 20];
    uint16_t start;
} __attribute__((packed)) Memory_T;
