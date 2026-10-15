#ifndef _THEM_CPU_INCLUDED_
#define _THEM_CPU_INCLUDED_

/*
 *  cpu.h
 *
 *  krusty@vxn.dev / Nov 4, 2025
 */

#include "mmu.h"
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

typedef enum {
    AAA = 0x37,

    AAD_IMM8 = 0xD5,
    AAM_IMM8 = 0xD4,
    AAS = 0x3F,

    ADC_AL_IMM8 = 0x14,
    ADC_AX_IMM16 = 0x15,

    ADD_16 = 0x81, /* SUB_16 */
    ADD_8 = 0x83,  /* SUB_8 */

    AND_AL_IMM8 = 0x24,
    AND_AX_IMM16 = 0x25,

    BOUND_R16 = 0x62,

    CALL_REL16 = 0xE8,
    CALL_PTR16 = 0x9A,

    CMP_AL_IMM8 = 0x3C,
    CMP_AX_IMM16 = 0x3D,

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

    HLT = 0xF4,

    INT_8 = 0xCD,
    IRET = 0xCF,

    JMP_REL8 = 0xEB,
    JMP_REL16 = 0xE9,
    JMP_RM16 = 0xFF,
    JMP_PTR16 = 0xEA,

    JA_REL8 = 0x77,
    JAE_REL8 = 0x73,
    JB_REL8 = 0x72,
    JBE_REL8 = 0x76,
    JC_REL8 = 0x72,
    JCXZ_REL8 = 0xE3,
    JE_REL8 = 0x74,
    JG_REL8 = 0x7F,
    JGE_REL8 = 0x7D,
    JL_REL8 = 0x7C,
    JLE_REL8 = 0x7E,
    JNA_REL8 = 0x76,
    JNAE_REL8 = 0x72,
    JNB_REL8 = 0x73,
    JNBE_REL8 = 0x77,
    JNC_REL8 = 0x73,
    JNE_REL8 = 0x75,
    JNG_REL8 = 0x7E,
    JNGE_REL8 = 0x7C,
    JNL_REL8 = 0x7D,
    JNLE_REL8 = 0x7F,
    JNO_REL8 = 0x71,
    JNP_REL8 = 0x7B,
    JNS_REL8 = 0x79,
    JNZ_REL8 = 0x75,
    JO_REL8 = 0x70,
    JP_REL8 = 0x7A,
    JPE_REL8 = 0x7A,
    JPO_REL8 = 0x7B,
    JS_REL8 = 0x78,
    JZ_REL8 = 0x74,

    LDS_RM16 = 0xC5,
    LES_RM16 = 0xC4,
    LEA_RM16 = 0x8D,
    LEAVE = 0xC9,
    LODS_M8 = 0xAC,
    LODS_M16 = 0xAD,
    LOOP_REL8 = 0xE2,
    LOOPE_REL8 = 0xE1,
    LOOPNE_REL8 = 0xE0,

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
    MOV_PTR = 0x26,

    NOP = 0x90,

    POPA = 0x61,
    PUSHA = 0x60,

    POPF = 0x9D,
    PUSHF = 0x9C,

    POP_RM16 = 0x8F,
    POP_R16 = 0x58,

    POP_DS = 0x1F,
    POP_ES = 0x07,
    POP_SS = 0x17,

    POP_AX = 0x58,
    POP_BX = 0x5B,
    POP_CX = 0x59,
    POP_DX = 0x5A,
    POP_SP = 0x5C,
    POP_BP = 0x5D,
    POP_SI = 0x5E,
    POP_DI = 0x5F,

    PREFIX_0F = 0x0F,
    PREFIX_80 = 0x80,
    PREFIX_8F = 0x8F,
    PREFIX_D4 = 0xD4,
    PREFIX_D5 = 0xD5,
    PREFIX_FE = 0xFE,

    PUSH_RM16 = 0xFF,
    PUSH_R16 = 0x50,
    PUSH_IMM8 = 0x6A,
    PUSH_IMM16 = 0x68,

    PUSH_CS = 0x0E,
    PUSH_DS = 0x1E,
    PUSH_ES = 0x06,
    PUSH_SS = 0x16,

    PUSH_AX = 0x50,
    PUSH_BX = 0x53,
    PUSH_CX = 0x51,
    PUSH_DX = 0x52,
    PUSH_SP = 0x54,
    PUSH_BP = 0x55,
    PUSH_SI = 0x56,
    PUSH_DI = 0x57,

    RET_IMM16_NEAR = 0xC2,
    RET_NEAR = 0xC3,
    RET_IMM16_FAR = 0xCA,

    XOR_AL_IMM8 = 0x34,
    XOR_AX_IMM16 = 0x35,
} OP_CODES;

typedef enum {
    JA_REL16 = 0x87,
    JAE_REL16 = 0x83,
    JB_REL16 = 0x82,
    JBE_REL16 = 0x86,
    JC_REL16 = 0x82,
    JE_REL16 = 0x84,
    JZ_REL16 = 0x84,
    JG_REL16 = 0x8F,
    JGE_REL16 = 0x8D,
    JL_REL16 = 0x8C,
    JLE_REL16 = 0x8E,
    JNA_REL16 = 0x86,
    JNAE_REL16 = 0x82,
    JNB_REL16 = 0x83,
    JNBE_REL16 = 0x87,
    JNC_REL16 = 0x83,
    JNE_REL16 = 0x85,
    JNG_REL16 = 0x8E,
    JNGE_REL16 = 0x8C,
    JNL_REL16 = 0x8D,
    JNLE_REL16 = 0x8F,
    JNO_REL16 = 0x81,
    JNP_REL16 = 0x8B,
    JNS_REL16 = 0x89,
    JNZ_REL16 = 0x85,
    JO_REL16 = 0x80,
    JP_REL16 = 0x8A,
    JPE_REL16 = 0x8A,
    JPO_REL16 = 0x8A,
    JS_REL16 = 0x88,

    LSS_RM16 = 0xB2,
    LFS_RM16 = 0xB4,
    LGS_RM16 = 0xB5,
    LSL_RM16 = 0x03,
    LTR_RM16 = 0x00,

    POP_FS = 0xA1,
    POP_GS = 0xA9,
} PREFIX_0F_SUBTYPE;

typedef enum {
    AAM = 0x0A,
} PREFIX_D4_SUBTYPE;

typedef enum {
    AAD = 0x0A,
} PREFIX_D5_SUBTYPE;

typedef enum {
    CF = 0x0001,
    PF = 0x0004,
    AF = 0x0010,
    ZF = 0x0040,
    SF = 0x0080,
    TF = 0x0100,
    IF = 0x0200,
    DF = 0x0400,
    OF = 0x0800,
    IOPL = 0x3000,
} FLAGS_MASK;

/*
 *  dump_registers()
 *
 *  Helper function to dump "all" CPU registers.
 */
void dump_registers(CPU_T *);

uint8_t get_flag(CPU_T *, FLAGS_MASK);
uint8_t set_flag(CPU_T *, FLAGS_MASK, uint8_t);

uint8_t get_reg8(CPU_T *, uint8_t idx);
uint16_t get_reg16(CPU_T *, uint8_t idx);

void set_reg8(CPU_T *, uint8_t idx, uint8_t val);
void set_reg16(CPU_T *, uint8_t idx, uint16_t val);

/* Linear memory access with the 8086 segment:offset rules; the offset wraps
 * inside its 64 KiB segment, so a word at offset 0xFFFF reads offset 0. */
uint8_t mem_r8(Memory_T *, uint16_t seg, uint16_t off);
void mem_w8(Memory_T *, uint16_t seg, uint16_t off, uint8_t value);
uint16_t mem_r16(Memory_T *, uint16_t seg, uint16_t off);
void mem_w16(Memory_T *, uint16_t seg, uint16_t off, uint16_t value);

void push16(CPU_T *, Memory_T *, uint16_t value);
uint16_t pop16(CPU_T *, Memory_T *);

/* Raise interrupt <n> through the interrupt vector table. */
void cpu_int(CPU_T *, Memory_T *, uint8_t n);

/* EXEC can terminate a child from inside its IRQ handler. Restoring the
 * parent's CPU also restores ownership of its interrupt frames, discarding
 * the child's abandoned frames without expiring any active parent handler. */
typedef struct {
    uint32_t frame[2];
    uint8_t busy;
} CpuIrqState_T;
void cpu_irq_save(CpuIrqState_T *state);
void cpu_irq_restore(const CpuIrqState_T *state);

/* Stop the run loop (used by the DOS terminate services). */
void cpu_halt(void);

/* The interpreter entry point: runs until the guest terminates. */
/* How many times a prefix run was cut short as too long to be an instruction,
 * and the CS:IP of the first one.  A non-zero count means the guest ran off
 * into something that is not code. */
void cpu_overlong(uint32_t *count, uint16_t *cs, uint16_t *ip);

/* How many times a REP ran past 0x10000 passes, and the CS:IP of the first.
 * Non-zero means CX stopped being the counter it is supposed to be. */
void cpu_runaway(uint32_t *count, uint16_t *cs, uint16_t *ip);

/* Total repeat passes executed, how many slices ended unfinished, and the
 * opcode and remaining CX of the last one. */
void cpu_strings(uint64_t *iters, uint32_t *slices, uint8_t *op, uint16_t *cx);

void switch_opcode(CPU_T *, Memory_T *);

#ifdef __cplusplus
}
#endif

#endif
