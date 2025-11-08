#ifndef _THEM_CPU_INCLUDED_
#define _THEM_CPU_INCLUDED_

/*
 *  cpu.h
 *
 *  krusty@vxn.dev / Nov 4, 2025
 */

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

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

enum OP_CODES {
    ADD_16 = 0x81, /* SUB_16 */
    ADD_8 = 0x83,  /* SUB_8 */
    HLT = 0xF4,

    INC_AX = 0x40,
    INC_BX = 0x43,
    INC_CX = 0x41,
    INC_DX = 0x42,
    INC_SP = 0x44,
    INC_BP = 0x45,
    INC_SI = 0x46,
    INC_DI = 0x47,

    DEC_AX = 0x48,
    DEC_BX = 0x4B,
    DEC_CX = 0x49,
    DEC_DX = 0x4A,
    DEC_SP = 0x4C,
    DEC_BP = 0x4D,
    DEC_SI = 0x4E,
    DEC_DI = 0x4F,

    INT_8 = 0xCD,
    IRET = 0xCF,

    JMP_REL8 = 0xE8,
    JMP_REL16 = 0xE9,

    MOV_AL = 0xB0,
    MOV_BL = 0xB3,
    MOV_CL = 0xB1,
    MOV_DL = 0xB2,
    MOV_AH = 0xB4,
    MOV_BH = 0xB7,
    MOV_CH = 0xB5,
    MOV_DH = 0xB6,

    MOV_AX = 0xB8,
    MOV_BX = 0xBB,
    MOV_CX = 0xB9,
    MOV_DX = 0xBA,
    MOV_SP = 0xBC,
    MOV_BP = 0xBD,
    MOV_SI = 0xBE,
    MOV_DI = 0xBF,

    MOV_SR = 0x8E,

    NOP = 0x90,

    PUSH_AX = 0x50,
    PUSH_BX = 0x53,
    PUSH_CX = 0x51,
    PUSH_DX = 0x52,
    PUSH_SP = 0x54,
    PUSH_BP = 0x55,
    PUSH_SI = 0x56,
    PUSH_DI = 0x57,
};

/* General-purpose registers */
enum GPR {
    ADD_AX = 0xC0,
    ADD_BX = 0xC3,
    ADD_CX = 0xC1,
    ADD_DX = 0xC2,
    ADD_SP = 0xC4,
    ADD_BP = 0xC5,
    ADD_SI = 0xC6,
    ADD_DI = 0xC7,

    MOV_DS_AX = 0xD8,
    MOV_DS_BX = 0xDB,
    MOV_DS_CX = 0xD9,
    MOV_DS_DX = 0xDA,
    MOV_DS_SP = 0xDC,
    MOV_DS_BP = 0xDD,
    MOV_DS_SI = 0xDE,
    MOV_DS_DI = 0xDF,

    MOV_ES_AX = 0xC0,
    MOV_ES_BX = 0xC3,
    MOV_ES_CX = 0xC1,
    MOV_ES_DX = 0xC2,
    MOV_ES_SP = 0xC4,
    MOV_ES_BP = 0xC5,
    MOV_ES_SI = 0xC6,
    MOV_ES_DI = 0xC7,

    MOV_SS_AX = 0xD0,
    MOV_SS_BX = 0xD3,
    MOV_SS_CX = 0xD1,
    MOV_SS_DX = 0xD2,
    MOV_SS_SP = 0xD4,
    MOV_SS_BP = 0xD5,
    MOV_SS_SI = 0xD6,
    MOV_SS_DI = 0xD7,

    SUB_AX = 0xE8,
    SUB_BX = 0xEB,
    SUB_CX = 0xE9,
    SUB_DX = 0xEA,
    SUB_SP = 0xEC,
    SUB_BP = 0xED,
    SUB_SI = 0xEE,
    SUB_DI = 0xEF
};

/*
 *  dump_registers()
 *
 *  Helper function to dump "all" CPU registers.
 */
void dump_registers(CPU_T *cpu);

/*
 *  switch_opcodes()
 *
 *  Core function to identify an instruction and its operands to emulate a CPU
 * operation.
 */
void switch_opcode(CPU_T *cpu, uint8_t *memory);

#ifdef __cplusplus
}
#endif

#endif
