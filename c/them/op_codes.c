#include "cpu.h"
#include "gfx.h"
#include "int.h"
#include "io.h"
#include "log.h"
#include "mmu.h"
#include "printf.h"
#include "syscall.h"
#include "vga.h"

/*
 *  op_codes.c
 *
 *  The instruction interpreter.
 *
 *  Decoding follows the shape of the real 8086 encoding: an optional run of
 *  prefixes, one opcode byte, an optional ModR/M byte with its displacement
 *  and finally the immediate. Everything that touches memory goes through
 *  one effective-address routine, so segment overrides, the SS default of
 *  the BP-based modes and the 64 KiB offset wrap are handled in a single
 *  place instead of per instruction.
 *
 *  krusty@vxn.dev / Nov 3, 2025
 */

extern uint8_t debug;

#define SEG_ES 0
#define SEG_CS 1
#define SEG_SS 2
#define SEG_DS 3
#define SEG_NONE 0xFF

typedef struct {
    uint8_t mod;
    uint8_t reg;
    uint8_t rm;
    uint16_t seg;
    uint16_t off;
} RM_T;

/*
 *  The flags a program can see and set: those of a 286 in real mode.
 *
 *  What sets an 8086 apart from a 286 is the top nibble: the 8086 (and the
 *  186) always reads it back as ones, the 286 in real mode as zeros, and that
 *  is the test every CPU check starts with.  Reporting an 8086 turned away
 *  every program that asks for a 286 -- Ugh! among them, which quits on the
 *  spot -- while nothing a 286 adds in real mode is missing here: the
 *  instructions are the 186's, and the rest is protected mode.  A program
 *  that goes on to look for a 386 sets the IOPL and NT bits and finds them
 *  zero, which is the right answer too.
 */
#define FLAGS_WRITABLE 0x0FD5 /* CF PF AF ZF SF TF IF DF OF */
#define FLAGS_FIXED 0x0002    /* bit 1 always reads as one */

static uint8_t cpu_halted = 0;
static uint8_t int_nesting = 0;

/* Where each hardware interrupt's frame went on the stack, as a linear
 * address, so IRET or RETF 2 can recognise it. The handler may have moved
 * SS:SP anywhere in between; it has to put it back to return. */
static uint32_t irq_frame[2];

void cpu_irq_save(CpuIrqState_T *state) {
    for (uint8_t i = 0; i < 2; i++)
        state->frame[i] = irq_frame[i];
    state->busy = hw_irq_busy();
}

void cpu_irq_restore(const CpuIrqState_T *state) {
    for (uint8_t i = 0; i < 2; i++)
        irq_frame[i] = state->frame[i];
    hw_irq_restore(state->busy);
}

static uint32_t stack_linear(CPU_T *cpu) { return (((uint32_t)cpu->SS << 4) + cpu->SP) & 0xFFFFF; }

/* A chained handler can use RETF 2 instead of IRET to retain its own flags.
 * Both consume the interrupt frame; returns from calls inside the handler
 * leave the hold intact, including calls made on a private stack. */
static void irq_frame_return(CPU_T *cpu) {
    uint32_t at = stack_linear(cpu);
    for (uint8_t i = 0; i < 2; i++)
        if ((hw_irq_busy() >> i) & 1 && irq_frame[i] == at)
            hw_irq_return(i);
}

/* --- fetch ----------------------------------------------------------- */

static uint8_t fetch8(CPU_T *cpu, Memory_T *mem) {
    uint8_t b = mem_r8(mem, cpu->CS, cpu->IP);
    cpu->IP++;
    return b;
}

static uint16_t fetch16(CPU_T *cpu, Memory_T *mem) {
    uint16_t lo = fetch8(cpu, mem);
    uint16_t hi = fetch8(cpu, mem);
    return (uint16_t)((hi << 8) | lo);
}

/* --- flags ----------------------------------------------------------- */

static uint8_t parity8(uint8_t v) {
    v ^= (uint8_t)(v >> 4);
    v ^= (uint8_t)(v >> 2);
    v ^= (uint8_t)(v >> 1);
    return (uint8_t)(!(v & 1));
}

static void set_szp8(CPU_T *cpu, uint8_t r) {
    set_flag(cpu, ZF, r == 0);
    set_flag(cpu, SF, (r & 0x80) != 0);
    set_flag(cpu, PF, parity8(r));
}

static void set_szp16(CPU_T *cpu, uint16_t r) {
    set_flag(cpu, ZF, r == 0);
    set_flag(cpu, SF, (r & 0x8000) != 0);
    set_flag(cpu, PF, parity8((uint8_t)(r & 0xFF)));
}

static uint8_t add8(CPU_T *cpu, uint8_t a, uint8_t b, uint8_t c) {
    uint16_t full = (uint16_t)a + b + c;
    uint8_t r = (uint8_t)full;

    set_szp8(cpu, r);
    set_flag(cpu, CF, (full & 0x100) != 0);
    set_flag(cpu, AF, (((a ^ b ^ r) & 0x10) != 0));
    set_flag(cpu, OF, ((~(a ^ b) & (a ^ r) & 0x80) != 0));

    return r;
}

static uint16_t add16(CPU_T *cpu, uint16_t a, uint16_t b, uint8_t c) {
    uint32_t full = (uint32_t)a + b + c;
    uint16_t r = (uint16_t)full;

    set_szp16(cpu, r);
    set_flag(cpu, CF, (full & 0x10000) != 0);
    set_flag(cpu, AF, (((a ^ b ^ r) & 0x10) != 0));
    set_flag(cpu, OF, ((~(a ^ b) & (a ^ r) & 0x8000) != 0));

    return r;
}

static uint8_t sub8(CPU_T *cpu, uint8_t a, uint8_t b, uint8_t c) {
    uint16_t full = (uint16_t)a - b - c;
    uint8_t r = (uint8_t)full;

    set_szp8(cpu, r);
    set_flag(cpu, CF, (full & 0x100) != 0);
    set_flag(cpu, AF, (((a ^ b ^ r) & 0x10) != 0));
    set_flag(cpu, OF, (((a ^ b) & (a ^ r) & 0x80) != 0));

    return r;
}

static uint16_t sub16(CPU_T *cpu, uint16_t a, uint16_t b, uint8_t c) {
    uint32_t full = (uint32_t)a - b - c;
    uint16_t r = (uint16_t)full;

    set_szp16(cpu, r);
    set_flag(cpu, CF, (full & 0x10000) != 0);
    set_flag(cpu, AF, (((a ^ b ^ r) & 0x10) != 0));
    set_flag(cpu, OF, (((a ^ b) & (a ^ r) & 0x8000) != 0));

    return r;
}

static void logic8(CPU_T *cpu, uint8_t r) {
    set_szp8(cpu, r);
    set_flag(cpu, CF, 0);
    set_flag(cpu, OF, 0);
    set_flag(cpu, AF, 0);
}

static void logic16(CPU_T *cpu, uint16_t r) {
    set_szp16(cpu, r);
    set_flag(cpu, CF, 0);
    set_flag(cpu, OF, 0);
    set_flag(cpu, AF, 0);
}

/* op: 0 ADD, 1 OR, 2 ADC, 3 SBB, 4 AND, 5 SUB, 6 XOR, 7 CMP */
static uint8_t alu8(CPU_T *cpu, uint8_t op, uint8_t a, uint8_t b) {
    uint8_t r;

    switch (op) {
    case 0:
        return add8(cpu, a, b, 0);
    case 1:
        r = (uint8_t)(a | b);
        logic8(cpu, r);
        return r;
    case 2:
        return add8(cpu, a, b, get_flag(cpu, CF));
    case 3:
        return sub8(cpu, a, b, get_flag(cpu, CF));
    case 4:
        r = (uint8_t)(a & b);
        logic8(cpu, r);
        return r;
    case 5:
        return sub8(cpu, a, b, 0);
    case 6:
        r = (uint8_t)(a ^ b);
        logic8(cpu, r);
        return r;
    default:
        sub8(cpu, a, b, 0);
        return a;
    }
}

static uint16_t alu16(CPU_T *cpu, uint8_t op, uint16_t a, uint16_t b) {
    uint16_t r;

    switch (op) {
    case 0:
        return add16(cpu, a, b, 0);
    case 1:
        r = (uint16_t)(a | b);
        logic16(cpu, r);
        return r;
    case 2:
        return add16(cpu, a, b, get_flag(cpu, CF));
    case 3:
        return sub16(cpu, a, b, get_flag(cpu, CF));
    case 4:
        r = (uint16_t)(a & b);
        logic16(cpu, r);
        return r;
    case 5:
        return sub16(cpu, a, b, 0);
    case 6:
        r = (uint16_t)(a ^ b);
        logic16(cpu, r);
        return r;
    default:
        sub16(cpu, a, b, 0);
        return a;
    }
}

/* --- stack ----------------------------------------------------------- */

void push16(CPU_T *cpu, Memory_T *mem, uint16_t value) {
    cpu->SP -= 2;
    mem_w16(mem, cpu->SS, cpu->SP, value);
}

uint16_t pop16(CPU_T *cpu, Memory_T *mem) {
    uint16_t v = mem_r16(mem, cpu->SS, cpu->SP);
    cpu->SP += 2;
    return v;
}

/* --- interrupts ------------------------------------------------------ */

/* Software interrupts and faults consume the instruction's trap. */
static uint8_t instruction_interrupted;

void cpu_int(CPU_T *cpu, Memory_T *mem, uint8_t n) {
    instruction_interrupted = 1;
    uint16_t off = mem_r16(mem, 0, (uint16_t)(n * 4));
    uint16_t seg = mem_r16(mem, 0, (uint16_t)(n * 4 + 2));

    push16(cpu, mem, cpu->FLAGS);
    push16(cpu, mem, cpu->CS);
    push16(cpu, mem, cpu->IP);

    set_flag(cpu, IF, 0);
    set_flag(cpu, TF, 0);

    cpu->CS = seg;
    cpu->IP = off;

    if (int_nesting < 255)
        int_nesting++;
}

void cpu_halt(void) { cpu_halted = 1; }

/* --- effective addresses --------------------------------------------- */

static uint16_t seg_reg_value(CPU_T *cpu, uint8_t which) {
    switch (which) {
    case SEG_ES:
        return cpu->ES;
    case SEG_CS:
        return cpu->CS;
    case SEG_SS:
        return cpu->SS;
    default:
        return cpu->DS;
    }
}

static RM_T fetch_rm(CPU_T *cpu, Memory_T *mem, uint8_t ovr) {
    uint8_t byte = fetch8(cpu, mem);
    RM_T m;
    uint16_t base = 0;
    uint8_t def_seg = SEG_DS;

    m.mod = (uint8_t)((byte >> 6) & 3);
    m.reg = (uint8_t)((byte >> 3) & 7);
    m.rm = (uint8_t)(byte & 7);
    m.seg = 0;
    m.off = 0;

    if (m.mod == 3)
        return m;

    switch (m.rm) {
    case 0:
        base = (uint16_t)(cpu->BX + cpu->SI);
        break;
    case 1:
        base = (uint16_t)(cpu->BX + cpu->DI);
        break;
    case 2:
        base = (uint16_t)(cpu->BP + cpu->SI);
        def_seg = SEG_SS;
        break;
    case 3:
        base = (uint16_t)(cpu->BP + cpu->DI);
        def_seg = SEG_SS;
        break;
    case 4:
        base = cpu->SI;
        break;
    case 5:
        base = cpu->DI;
        break;
    case 6:
        if (m.mod == 0) {
            base = fetch16(cpu, mem); /* direct address */
        } else {
            base = cpu->BP;
            def_seg = SEG_SS;
        }
        break;
    default:
        base = cpu->BX;
        break;
    }

    if (m.mod == 1)
        base = (uint16_t)(base + (int8_t)fetch8(cpu, mem));
    else if (m.mod == 2)
        base = (uint16_t)(base + (int16_t)fetch16(cpu, mem));

    m.off = base;
    m.seg = seg_reg_value(cpu, (ovr == SEG_NONE) ? def_seg : ovr);

    return m;
}

static uint8_t rm_get8(CPU_T *cpu, Memory_T *mem, RM_T *m) {
    if (m->mod == 3)
        return get_reg8(cpu, m->rm);
    return mem_r8(mem, m->seg, m->off);
}

static void rm_set8(CPU_T *cpu, Memory_T *mem, RM_T *m, uint8_t v) {
    if (m->mod == 3)
        set_reg8(cpu, m->rm, v);
    else
        mem_w8(mem, m->seg, m->off, v);
}

static uint16_t rm_get16(CPU_T *cpu, Memory_T *mem, RM_T *m) {
    if (m->mod == 3)
        return get_reg16(cpu, m->rm);
    return mem_r16(mem, m->seg, m->off);
}

static void rm_set16(CPU_T *cpu, Memory_T *mem, RM_T *m, uint16_t v) {
    if (m->mod == 3)
        set_reg16(cpu, m->rm, v);
    else
        mem_w16(mem, m->seg, m->off, v);
}

static uint16_t sreg_get(CPU_T *cpu, uint8_t idx) {
    switch (idx & 3) {
    case 0:
        return cpu->ES;
    case 1:
        return cpu->CS;
    case 2:
        return cpu->SS;
    default:
        return cpu->DS;
    }
}

static void sreg_set(CPU_T *cpu, uint8_t idx, uint16_t v) {
    switch (idx & 3) {
    case 0:
        cpu->ES = v;
        break;
    case 1:
        cpu->CS = v;
        break;
    case 2:
        cpu->SS = v;
        break;
    default:
        cpu->DS = v;
        break;
    }
}

/* --- shifts and rotates ---------------------------------------------- */

static uint8_t shift8(CPU_T *cpu, uint8_t op, uint8_t v, uint8_t cnt) {
    uint8_t cf;

    cnt &= 0x1F;
    if (!cnt)
        return v;

    switch (op) {
    case 0: /* ROL */
        for (uint8_t i = 0; i < cnt; i++) {
            cf = (uint8_t)((v >> 7) & 1);
            v = (uint8_t)((v << 1) | cf);
        }
        set_flag(cpu, CF, v & 1);
        set_flag(cpu, OF, (((v >> 7) & 1) ^ (v & 1)) != 0);
        break;
    case 1: /* ROR */
        for (uint8_t i = 0; i < cnt; i++) {
            cf = (uint8_t)(v & 1);
            v = (uint8_t)((v >> 1) | (cf << 7));
        }
        set_flag(cpu, CF, (v >> 7) & 1);
        set_flag(cpu, OF, ((((v >> 7) & 1) ^ ((v >> 6) & 1)) != 0));
        break;
    case 2: /* RCL */
        for (uint8_t i = 0; i < cnt; i++) {
            cf = get_flag(cpu, CF);
            uint8_t msb = (uint8_t)((v >> 7) & 1);
            v = (uint8_t)((v << 1) | cf);
            set_flag(cpu, CF, msb);
        }
        set_flag(cpu, OF, ((((v >> 7) & 1) ^ get_flag(cpu, CF)) != 0));
        break;
    case 3: /* RCR */
        for (uint8_t i = 0; i < cnt; i++) {
            cf = get_flag(cpu, CF);
            uint8_t lsb = (uint8_t)(v & 1);
            v = (uint8_t)((v >> 1) | (cf << 7));
            set_flag(cpu, CF, lsb);
        }
        set_flag(cpu, OF, ((((v >> 7) & 1) ^ ((v >> 6) & 1)) != 0));
        break;
    case 4: /* SHL/SAL */
    case 6:
        cf = (cnt <= 8) ? (uint8_t)((v >> (8 - cnt)) & 1) : 0;
        v = (uint8_t)(cnt >= 8 ? 0 : (v << cnt));
        set_flag(cpu, CF, cf);
        set_flag(cpu, OF, ((((v >> 7) & 1) ^ cf) != 0));
        set_szp8(cpu, v);
        set_flag(cpu, AF, 0);
        return v;
    case 5: /* SHR */
        cf = (cnt <= 8) ? (uint8_t)((v >> (cnt - 1)) & 1) : 0;
        {
            uint8_t before = v;
            v = (uint8_t)(cnt >= 8 ? 0 : (v >> cnt));
            set_flag(cpu, CF, cf);
            set_flag(cpu, OF, ((before >> 7) & 1) != 0);
        }
        set_szp8(cpu, v);
        set_flag(cpu, AF, 0);
        return v;
    default: /* SAR */
        cf = 0;
        for (uint8_t i = 0; i < cnt; i++) {
            cf = (uint8_t)(v & 1);
            v = (uint8_t)((v >> 1) | (v & 0x80));
        }
        set_flag(cpu, CF, cf);
        set_flag(cpu, OF, 0);
        set_szp8(cpu, v);
        set_flag(cpu, AF, 0);
        return v;
    }

    return v;
}

static uint16_t shift16(CPU_T *cpu, uint8_t op, uint16_t v, uint8_t cnt) {
    uint8_t cf;

    cnt &= 0x1F;
    if (!cnt)
        return v;

    switch (op) {
    case 0: /* ROL */
        for (uint8_t i = 0; i < cnt; i++) {
            cf = (uint8_t)((v >> 15) & 1);
            v = (uint16_t)((v << 1) | cf);
        }
        set_flag(cpu, CF, v & 1);
        set_flag(cpu, OF, ((((v >> 15) & 1) ^ (v & 1)) != 0));
        break;
    case 1: /* ROR */
        for (uint8_t i = 0; i < cnt; i++) {
            cf = (uint8_t)(v & 1);
            v = (uint16_t)((v >> 1) | ((uint16_t)cf << 15));
        }
        set_flag(cpu, CF, (v >> 15) & 1);
        set_flag(cpu, OF, ((((v >> 15) & 1) ^ ((v >> 14) & 1)) != 0));
        break;
    case 2: /* RCL */
        for (uint8_t i = 0; i < cnt; i++) {
            cf = get_flag(cpu, CF);
            uint8_t msb = (uint8_t)((v >> 15) & 1);
            v = (uint16_t)((v << 1) | cf);
            set_flag(cpu, CF, msb);
        }
        set_flag(cpu, OF, ((((v >> 15) & 1) ^ get_flag(cpu, CF)) != 0));
        break;
    case 3: /* RCR */
        for (uint8_t i = 0; i < cnt; i++) {
            cf = get_flag(cpu, CF);
            uint8_t lsb = (uint8_t)(v & 1);
            v = (uint16_t)((v >> 1) | ((uint16_t)cf << 15));
            set_flag(cpu, CF, lsb);
        }
        set_flag(cpu, OF, ((((v >> 15) & 1) ^ ((v >> 14) & 1)) != 0));
        break;
    case 4:
    case 6: /* SHL/SAL */
        cf = (cnt <= 16) ? (uint8_t)((v >> (16 - cnt)) & 1) : 0;
        v = (uint16_t)(cnt >= 16 ? 0 : (v << cnt));
        set_flag(cpu, CF, cf);
        set_flag(cpu, OF, ((((v >> 15) & 1) ^ cf) != 0));
        set_szp16(cpu, v);
        set_flag(cpu, AF, 0);
        return v;
    case 5: /* SHR */
        cf = (cnt <= 16) ? (uint8_t)((v >> (cnt - 1)) & 1) : 0;
        {
            uint16_t before = v;
            v = (uint16_t)(cnt >= 16 ? 0 : (v >> cnt));
            set_flag(cpu, CF, cf);
            set_flag(cpu, OF, ((before >> 15) & 1) != 0);
        }
        set_szp16(cpu, v);
        set_flag(cpu, AF, 0);
        return v;
    default: /* SAR */
        cf = 0;
        for (uint8_t i = 0; i < cnt; i++) {
            cf = (uint8_t)(v & 1);
            v = (uint16_t)((v >> 1) | (v & 0x8000));
        }
        set_flag(cpu, CF, cf);
        set_flag(cpu, OF, 0);
        set_szp16(cpu, v);
        set_flag(cpu, AF, 0);
        return v;
    }

    return v;
}

/* --- string operations ----------------------------------------------- */

static void string_step(CPU_T *cpu, Memory_T *mem, uint8_t op, uint8_t ovr) {
    int16_t delta = get_flag(cpu, DF) ? -1 : 1;
    uint16_t src_seg = (ovr == SEG_NONE) ? cpu->DS : seg_reg_value(cpu, ovr);

    switch (op) {
    case 0xA4: { /* MOVSB */
        mem_w8(mem, cpu->ES, cpu->DI, mem_r8(mem, src_seg, cpu->SI));
        cpu->SI = (uint16_t)(cpu->SI + delta);
        cpu->DI = (uint16_t)(cpu->DI + delta);
        break;
    }
    case 0xA5: { /* MOVSW */
        mem_w16(mem, cpu->ES, cpu->DI, mem_r16(mem, src_seg, cpu->SI));
        cpu->SI = (uint16_t)(cpu->SI + 2 * delta);
        cpu->DI = (uint16_t)(cpu->DI + 2 * delta);
        break;
    }
    case 0xA6: { /* CMPSB */
        uint8_t a = mem_r8(mem, src_seg, cpu->SI);
        uint8_t b = mem_r8(mem, cpu->ES, cpu->DI);
        sub8(cpu, a, b, 0);
        cpu->SI = (uint16_t)(cpu->SI + delta);
        cpu->DI = (uint16_t)(cpu->DI + delta);
        break;
    }
    case 0xA7: { /* CMPSW */
        uint16_t a = mem_r16(mem, src_seg, cpu->SI);
        uint16_t b = mem_r16(mem, cpu->ES, cpu->DI);
        sub16(cpu, a, b, 0);
        cpu->SI = (uint16_t)(cpu->SI + 2 * delta);
        cpu->DI = (uint16_t)(cpu->DI + 2 * delta);
        break;
    }
    case 0xAA: { /* STOSB */
        mem_w8(mem, cpu->ES, cpu->DI, (uint8_t)(cpu->AX & 0xFF));
        cpu->DI = (uint16_t)(cpu->DI + delta);
        break;
    }
    case 0xAB: { /* STOSW */
        mem_w16(mem, cpu->ES, cpu->DI, cpu->AX);
        cpu->DI = (uint16_t)(cpu->DI + 2 * delta);
        break;
    }
    case 0xAC: { /* LODSB */
        cpu->AX = (uint16_t)((cpu->AX & 0xFF00) | mem_r8(mem, src_seg, cpu->SI));
        cpu->SI = (uint16_t)(cpu->SI + delta);
        break;
    }
    case 0xAD: { /* LODSW */
        cpu->AX = mem_r16(mem, src_seg, cpu->SI);
        cpu->SI = (uint16_t)(cpu->SI + 2 * delta);
        break;
    }
    case 0xAE: { /* SCASB */
        uint8_t b = mem_r8(mem, cpu->ES, cpu->DI);
        sub8(cpu, (uint8_t)(cpu->AX & 0xFF), b, 0);
        cpu->DI = (uint16_t)(cpu->DI + delta);
        break;
    }
    case 0xAF: { /* SCASW */
        uint16_t b = mem_r16(mem, cpu->ES, cpu->DI);
        sub16(cpu, cpu->AX, b, 0);
        cpu->DI = (uint16_t)(cpu->DI + 2 * delta);
        break;
    }
    case 0x6C: { /* INSB */
        mem_w8(mem, cpu->ES, cpu->DI, io_in8(cpu->DX));
        cpu->DI = (uint16_t)(cpu->DI + delta);
        break;
    }
    case 0x6D: { /* INSW */
        mem_w16(mem, cpu->ES, cpu->DI, io_in16(cpu->DX));
        cpu->DI = (uint16_t)(cpu->DI + 2 * delta);
        break;
    }
    case 0x6E: { /* OUTSB */
        io_out8(cpu->DX, mem_r8(mem, src_seg, cpu->SI));
        cpu->SI = (uint16_t)(cpu->SI + delta);
        break;
    }
    case 0x6F: { /* OUTSW */
        io_out16(cpu->DX, mem_r16(mem, src_seg, cpu->SI));
        cpu->SI = (uint16_t)(cpu->SI + 2 * delta);
        break;
    }
    default:
        break;
    }
}

/* Times a repeat ran past what CX could possibly ask for, and where. */
static uint32_t runaway = 0;
static uint16_t runaway_cs = 0;
static uint16_t runaway_ip = 0;

void cpu_runaway(uint32_t *count, uint16_t *cs, uint16_t *ip) {
    if (count)
        *count = runaway;
    if (cs)
        *cs = runaway_cs;
    if (ip)
        *ip = runaway_ip;
}

/*
 *  How many passes of a repeat to run before handing the machine back.
 *
 *  A repeat used to run to the end inside one step(), and a full screen of
 *  Mode X is tens of thousands of passes through the planar write path: for
 *  the whole of it nothing above step() runs, so no frame is presented, the
 *  keyboard is never drained and F12 is never seen. Long enough and that is
 *  indistinguishable from a hang -- the process is busy, and there is no way
 *  to ask it to stop.
 *
 *  Real hardware takes interrupts between passes of a repeat and carries on
 *  afterwards with CX, SI and DI where it left them, so stopping partway is
 *  what the instruction is supposed to do.
 */
#define STRING_SLICE 4096

static void string_run(CPU_T *cpu, Memory_T *mem, uint8_t op, uint8_t ovr, uint8_t rep);

/* 1 when the repeat still has passes left and the instruction must be re-run. */
static uint8_t string_more = 0;

/* What the repeats cost, and what the last unfinished one was. A CX that does
 * not fall between one report and the next is a repeat going nowhere; one that
 * does is simply a guest asking for a lot of work. */
static uint64_t string_iters = 0;
static uint32_t string_slices = 0;
static uint8_t string_last_op = 0;
static uint16_t string_last_cx = 0;

void cpu_strings(uint64_t *iters, uint32_t *slices, uint8_t *op, uint16_t *cx) {
    if (iters)
        *iters = string_iters;
    if (slices)
        *slices = string_slices;
    if (op)
        *op = string_last_op;
    if (cx)
        *cx = string_last_cx;
}

static void string_run(CPU_T *cpu, Memory_T *mem, uint8_t op, uint8_t ovr, uint8_t rep) {
    uint8_t compares = (op == 0xA6 || op == 0xA7 || op == 0xAE || op == 0xAF);
    uint32_t guard = 0;
    uint32_t slice = 0;

    string_more = 0;

    if (!rep) {
        string_step(cpu, mem, op, ovr);
        return;
    }

    while (cpu->CX) {
        string_step(cpu, mem, op, ovr);
        cpu->CX--;
        string_iters++;

        /*
         *  CX counts down to zero, so 0x10000 passes is already one more than
         *  a repeat can ask for. Going past it means CX is not the counter it
         *  is supposed to be -- and the whole repeat happens inside one step(),
         *  so a repeat that does not end is an emulator that does not breathe.
         */
        /* Out of slice with work left: stop here and let the caller re-run
         * the instruction, the way an interrupted repeat resumes. */
        if (++slice >= STRING_SLICE && cpu->CX) {
            string_more = 1;
            string_slices++;
            string_last_op = op;
            string_last_cx = cpu->CX;
            break;
        }

        if (++guard > 0x10000) {
            if (!runaway) {
                runaway_cs = cpu->CS;
                runaway_ip = cpu->IP;
            }
            runaway++;
            break;
        }

        if (compares) {
            uint8_t zf = get_flag(cpu, ZF);
            if (rep == 0xF3 && !zf)
                break;
            if (rep == 0xF2 && zf)
                break;
        }
    }
}

/* --- the run loop ---------------------------------------------------- */

/* Times the prefix run was cut short, and where it first happened. */
static uint32_t overlong = 0;
static uint16_t overlong_cs = 0;
static uint16_t overlong_ip = 0;

void cpu_overlong(uint32_t *count, uint16_t *cs, uint16_t *ip) {
    if (count)
        *count = overlong;
    if (cs)
        *cs = overlong_cs;
    if (ip)
        *ip = overlong_ip;
}

static void step(CPU_T *cpu, Memory_T *mem) {
    uint8_t ovr = SEG_NONE;
    uint8_t rep = 0;
    uint8_t op;
    uint8_t len = 0;
    /* Where this instruction starts, prefixes included, so an unfinished
     * repeat can be re-entered exactly as it was first decoded. */
    uint16_t entry_ip = cpu->IP;

    for (;;) {
        op = fetch8(cpu, mem);

        /*
         *  Fifteen bytes is as long as an x86 instruction gets, so a run of
         *  prefixes longer than that is not an instruction at all -- a jump
         *  landing in data, say, on a stretch of 0x26 or 0xF3.
         *
         *  Without this the loop eats the whole segment and starts again: IP
         *  is sixteen bits and wraps, so there is no end to it, and step()
         *  simply never returns.  Nothing above it runs again -- no frame is
         *  presented, the keyboard is never drained, F12 is never seen -- and
         *  the process sits there runnable with no way left to stop it.
         *
         *  Stopping here hands the byte to the dispatch below instead. The
         *  guest is already lost by this point; what matters is that the
         *  emulator keeps breathing and can still be quit.
         */
        if (len++ >= 15) {
            if (!overlong) {
                overlong_cs = cpu->CS;
                overlong_ip = cpu->IP;
            }
            overlong++;
            break;
        }

        switch (op) {
        case 0x26:
            ovr = SEG_ES;
            continue;
        case 0x2E:
            ovr = SEG_CS;
            continue;
        case 0x36:
            ovr = SEG_SS;
            continue;
        case 0x3E:
            ovr = SEG_DS;
            continue;
        case 0xF0: /* LOCK */
            continue;
        case 0xF2:
        case 0xF3:
            rep = op;
            continue;
        default:
            break;
        }

        break;
    }

    /* ALU group: 00-3F, six encodings per operation. */
    if (op < 0x40 && (op & 7) < 6) {
        uint8_t aluop = (uint8_t)(op >> 3);
        uint8_t form = (uint8_t)(op & 7);

        switch (form) {
        case 0: { /* r/m8, r8 */
            RM_T m = fetch_rm(cpu, mem, ovr);
            uint8_t r = alu8(cpu, aluop, rm_get8(cpu, mem, &m), get_reg8(cpu, m.reg));
            if (aluop != 7)
                rm_set8(cpu, mem, &m, r);
            return;
        }
        case 1: { /* r/m16, r16 */
            RM_T m = fetch_rm(cpu, mem, ovr);
            uint16_t r = alu16(cpu, aluop, rm_get16(cpu, mem, &m), get_reg16(cpu, m.reg));
            if (aluop != 7)
                rm_set16(cpu, mem, &m, r);
            return;
        }
        case 2: { /* r8, r/m8 */
            RM_T m = fetch_rm(cpu, mem, ovr);
            uint8_t r = alu8(cpu, aluop, get_reg8(cpu, m.reg), rm_get8(cpu, mem, &m));
            if (aluop != 7)
                set_reg8(cpu, m.reg, r);
            return;
        }
        case 3: { /* r16, r/m16 */
            RM_T m = fetch_rm(cpu, mem, ovr);
            uint16_t r = alu16(cpu, aluop, get_reg16(cpu, m.reg), rm_get16(cpu, mem, &m));
            if (aluop != 7)
                set_reg16(cpu, m.reg, r);
            return;
        }
        case 4: { /* AL, imm8 */
            uint8_t r = alu8(cpu, aluop, (uint8_t)(cpu->AX & 0xFF), fetch8(cpu, mem));
            if (aluop != 7)
                cpu->AX = (uint16_t)((cpu->AX & 0xFF00) | r);
            return;
        }
        default: { /* AX, imm16 */
            uint16_t r = alu16(cpu, aluop, cpu->AX, fetch16(cpu, mem));
            if (aluop != 7)
                cpu->AX = r;
            return;
        }
        }
    }

    switch (op) {
    /* --- segment register push/pop --- */
    case 0x06:
        push16(cpu, mem, cpu->ES);
        return;
    case 0x07:
        cpu->ES = pop16(cpu, mem);
        return;
    case 0x0E:
        push16(cpu, mem, cpu->CS);
        return;
    case 0x0F: { /* the 286's two byte opcodes; the 8086 read this as POP CS */
        uint8_t op2 = fetch8(cpu, mem);

        if (op2 == 0x01) {
            RM_T m = fetch_rm(cpu, mem, ovr);

            /* SMSW: the machine status word of a 286 in real mode, the
             * one way a program asks whether it is already in protected
             * mode. LMSW, the way into it, is not followed -- there is no
             * protected mode here -- and neither are the descriptor table
             * loads and stores beside them. */
            if (m.reg == 4)
                rm_set16(cpu, mem, &m, 0xFFF0);
            else if (debug)
                log_line((const uint8_t *)"UNHANDLED 0F 01 /%u at %x:%x", (unsigned int)m.reg, (unsigned int)cpu->CS, (unsigned int)cpu->IP);
            return;
        }

        if (debug)
            log_line((const uint8_t *)"UNHANDLED OPCODE 0F %x at %x:%x", (unsigned int)op2, (unsigned int)cpu->CS, (unsigned int)(uint16_t)(cpu->IP - 2));
        return;
    }
    case 0x16:
        push16(cpu, mem, cpu->SS);
        return;
    case 0x17:
        cpu->SS = pop16(cpu, mem);
        return;
    case 0x1E:
        push16(cpu, mem, cpu->DS);
        return;
    case 0x1F:
        cpu->DS = pop16(cpu, mem);
        return;

    /* --- BCD helpers --- */
    case 0x27: { /* DAA */
        uint8_t al = (uint8_t)(cpu->AX & 0xFF);
        uint8_t old_cf = get_flag(cpu, CF);

        set_flag(cpu, CF, 0);
        if ((al & 0x0F) > 9 || get_flag(cpu, AF)) {
            al = (uint8_t)(al + 6);
            set_flag(cpu, CF, old_cf || (al < 6));
            set_flag(cpu, AF, 1);
        } else {
            set_flag(cpu, AF, 0);
        }
        if ((cpu->AX & 0xFF) > 0x99 || old_cf) {
            al = (uint8_t)(al + 0x60);
            set_flag(cpu, CF, 1);
        }
        cpu->AX = (uint16_t)((cpu->AX & 0xFF00) | al);
        set_szp8(cpu, al);
        return;
    }
    case 0x2F: { /* DAS */
        uint8_t al = (uint8_t)(cpu->AX & 0xFF);
        uint8_t old_al = al;
        uint8_t old_cf = get_flag(cpu, CF);

        set_flag(cpu, CF, 0);
        if ((al & 0x0F) > 9 || get_flag(cpu, AF)) {
            al = (uint8_t)(al - 6);
            set_flag(cpu, CF, old_cf || (old_al < 6));
            set_flag(cpu, AF, 1);
        } else {
            set_flag(cpu, AF, 0);
        }
        if (old_al > 0x99 || old_cf) {
            al = (uint8_t)(al - 0x60);
            set_flag(cpu, CF, 1);
        }
        cpu->AX = (uint16_t)((cpu->AX & 0xFF00) | al);
        set_szp8(cpu, al);
        return;
    }
    case 0x37: { /* AAA */
        if ((cpu->AX & 0x0F) > 9 || get_flag(cpu, AF)) {
            cpu->AX = (uint16_t)(cpu->AX + 0x106);
            set_flag(cpu, AF, 1);
            set_flag(cpu, CF, 1);
        } else {
            set_flag(cpu, AF, 0);
            set_flag(cpu, CF, 0);
        }
        cpu->AX &= 0xFF0F;
        return;
    }
    case 0x3F: { /* AAS */
        if ((cpu->AX & 0x0F) > 9 || get_flag(cpu, AF)) {
            cpu->AX = (uint16_t)(cpu->AX - 6);
            cpu->AX = (uint16_t)((cpu->AX & 0x00FF) | ((cpu->AX - 0x100) & 0xFF00));
            set_flag(cpu, AF, 1);
            set_flag(cpu, CF, 1);
        } else {
            set_flag(cpu, AF, 0);
            set_flag(cpu, CF, 0);
        }
        cpu->AX &= 0xFF0F;
        return;
    }

    /* --- INC/DEC r16 --- */
    case 0x40:
    case 0x41:
    case 0x42:
    case 0x43:
    case 0x44:
    case 0x45:
    case 0x46:
    case 0x47: {
        uint8_t idx = (uint8_t)(op & 7);
        uint16_t v = get_reg16(cpu, idx);
        uint8_t cf = get_flag(cpu, CF);
        set_reg16(cpu, idx, add16(cpu, v, 1, 0));
        set_flag(cpu, CF, cf); /* INC leaves CF alone */
        return;
    }
    case 0x48:
    case 0x49:
    case 0x4A:
    case 0x4B:
    case 0x4C:
    case 0x4D:
    case 0x4E:
    case 0x4F: {
        uint8_t idx = (uint8_t)(op & 7);
        uint16_t v = get_reg16(cpu, idx);
        uint8_t cf = get_flag(cpu, CF);
        set_reg16(cpu, idx, sub16(cpu, v, 1, 0));
        set_flag(cpu, CF, cf);
        return;
    }

    /* --- PUSH/POP r16 --- */
    case 0x50:
    case 0x51:
    case 0x52:
    case 0x53:
    case 0x54:
    case 0x55:
    case 0x56:
    case 0x57:
        push16(cpu, mem, get_reg16(cpu, (uint8_t)(op & 7)));
        return;
    case 0x58:
    case 0x59:
    case 0x5A:
    case 0x5B:
    case 0x5C:
    case 0x5D:
    case 0x5E:
    case 0x5F:
        set_reg16(cpu, (uint8_t)(op & 7), pop16(cpu, mem));
        return;

    case 0x60: { /* PUSHA */
        uint16_t sp = cpu->SP;
        push16(cpu, mem, cpu->AX);
        push16(cpu, mem, cpu->CX);
        push16(cpu, mem, cpu->DX);
        push16(cpu, mem, cpu->BX);
        push16(cpu, mem, sp);
        push16(cpu, mem, cpu->BP);
        push16(cpu, mem, cpu->SI);
        push16(cpu, mem, cpu->DI);
        return;
    }
    case 0x61: { /* POPA */
        cpu->DI = pop16(cpu, mem);
        cpu->SI = pop16(cpu, mem);
        cpu->BP = pop16(cpu, mem);
        (void)pop16(cpu, mem); /* discarded SP */
        cpu->BX = pop16(cpu, mem);
        cpu->DX = pop16(cpu, mem);
        cpu->CX = pop16(cpu, mem);
        cpu->AX = pop16(cpu, mem);
        return;
    }
    case 0x62: { /* BOUND r16, m16&16 */
        RM_T m = fetch_rm(cpu, mem, ovr);
        int16_t idx = (int16_t)get_reg16(cpu, m.reg);
        int16_t lo = (int16_t)mem_r16(mem, m.seg, m.off);
        int16_t hi = (int16_t)mem_r16(mem, m.seg, (uint16_t)(m.off + 2));

        if (idx < lo || idx > hi)
            cpu_int(cpu, mem, 5);
        return;
    }
    case 0x68:
        push16(cpu, mem, fetch16(cpu, mem));
        return;
    case 0x6A:
        push16(cpu, mem, (uint16_t)(int16_t)(int8_t)fetch8(cpu, mem));
        return;
    case 0x69: { /* IMUL r16, r/m16, imm16 */
        RM_T m = fetch_rm(cpu, mem, ovr);
        int32_t a = (int16_t)rm_get16(cpu, mem, &m);
        int32_t b = (int16_t)fetch16(cpu, mem);
        int32_t r = a * b;

        set_reg16(cpu, m.reg, (uint16_t)r);
        set_flag(cpu, CF, r != (int16_t)r);
        set_flag(cpu, OF, r != (int16_t)r);
        return;
    }
    case 0x6B: { /* IMUL r16, r/m16, imm8 */
        RM_T m = fetch_rm(cpu, mem, ovr);
        int32_t a = (int16_t)rm_get16(cpu, mem, &m);
        int32_t b = (int8_t)fetch8(cpu, mem);
        int32_t r = a * b;

        set_reg16(cpu, m.reg, (uint16_t)r);
        set_flag(cpu, CF, r != (int16_t)r);
        set_flag(cpu, OF, r != (int16_t)r);
        return;
    }
    case 0x6C:
    case 0x6D:
    case 0x6E:
    case 0x6F:
        string_run(cpu, mem, op, ovr, rep);
        if (string_more)
            cpu->IP = entry_ip;
        return;

    /* --- conditional jumps --- */
    case 0x70:
    case 0x71:
    case 0x72:
    case 0x73:
    case 0x74:
    case 0x75:
    case 0x76:
    case 0x77:
    case 0x78:
    case 0x79:
    case 0x7A:
    case 0x7B:
    case 0x7C:
    case 0x7D:
    case 0x7E:
    case 0x7F: {
        int8_t disp = (int8_t)fetch8(cpu, mem);
        uint8_t take = 0;

        switch (op & 0x0E) {
        case 0x00:
            take = get_flag(cpu, OF);
            break;
        case 0x02:
            take = get_flag(cpu, CF);
            break;
        case 0x04:
            take = get_flag(cpu, ZF);
            break;
        case 0x06:
            take = (uint8_t)(get_flag(cpu, CF) | get_flag(cpu, ZF));
            break;
        case 0x08:
            take = get_flag(cpu, SF);
            break;
        case 0x0A:
            take = get_flag(cpu, PF);
            break;
        case 0x0C:
            take = (uint8_t)(get_flag(cpu, SF) != get_flag(cpu, OF));
            break;
        default:
            take = (uint8_t)(get_flag(cpu, ZF) || (get_flag(cpu, SF) != get_flag(cpu, OF)));
            break;
        }

        if (op & 1)
            take = (uint8_t)!take;

        if (take)
            cpu->IP = (uint16_t)(cpu->IP + disp);
        return;
    }

    /* --- immediate group --- */
    case 0x80:
    case 0x82: {
        RM_T m = fetch_rm(cpu, mem, ovr);
        uint8_t imm = fetch8(cpu, mem);
        uint8_t r = alu8(cpu, m.reg, rm_get8(cpu, mem, &m), imm);
        if (m.reg != 7)
            rm_set8(cpu, mem, &m, r);
        return;
    }
    case 0x81: {
        RM_T m = fetch_rm(cpu, mem, ovr);
        uint16_t imm = fetch16(cpu, mem);
        uint16_t r = alu16(cpu, m.reg, rm_get16(cpu, mem, &m), imm);
        if (m.reg != 7)
            rm_set16(cpu, mem, &m, r);
        return;
    }
    case 0x83: {
        RM_T m = fetch_rm(cpu, mem, ovr);
        uint16_t imm = (uint16_t)(int16_t)(int8_t)fetch8(cpu, mem);
        uint16_t r = alu16(cpu, m.reg, rm_get16(cpu, mem, &m), imm);
        if (m.reg != 7)
            rm_set16(cpu, mem, &m, r);
        return;
    }

    case 0x84: { /* TEST r/m8, r8 */
        RM_T m = fetch_rm(cpu, mem, ovr);
        logic8(cpu, (uint8_t)(rm_get8(cpu, mem, &m) & get_reg8(cpu, m.reg)));
        return;
    }
    case 0x85: { /* TEST r/m16, r16 */
        RM_T m = fetch_rm(cpu, mem, ovr);
        logic16(cpu, (uint16_t)(rm_get16(cpu, mem, &m) & get_reg16(cpu, m.reg)));
        return;
    }
    case 0x86: { /* XCHG r8, r/m8 */
        RM_T m = fetch_rm(cpu, mem, ovr);
        uint8_t a = rm_get8(cpu, mem, &m);
        uint8_t b = get_reg8(cpu, m.reg);
        rm_set8(cpu, mem, &m, b);
        set_reg8(cpu, m.reg, a);
        return;
    }
    case 0x87: { /* XCHG r16, r/m16 */
        RM_T m = fetch_rm(cpu, mem, ovr);
        uint16_t a = rm_get16(cpu, mem, &m);
        uint16_t b = get_reg16(cpu, m.reg);
        rm_set16(cpu, mem, &m, b);
        set_reg16(cpu, m.reg, a);
        return;
    }
    case 0x88: { /* MOV r/m8, r8 */
        RM_T m = fetch_rm(cpu, mem, ovr);
        rm_set8(cpu, mem, &m, get_reg8(cpu, m.reg));
        return;
    }
    case 0x89: { /* MOV r/m16, r16 */
        RM_T m = fetch_rm(cpu, mem, ovr);
        rm_set16(cpu, mem, &m, get_reg16(cpu, m.reg));
        return;
    }
    case 0x8A: { /* MOV r8, r/m8 */
        RM_T m = fetch_rm(cpu, mem, ovr);
        set_reg8(cpu, m.reg, rm_get8(cpu, mem, &m));
        return;
    }
    case 0x8B: { /* MOV r16, r/m16 */
        RM_T m = fetch_rm(cpu, mem, ovr);
        set_reg16(cpu, m.reg, rm_get16(cpu, mem, &m));
        return;
    }
    case 0x8C: { /* MOV r/m16, Sreg */
        RM_T m = fetch_rm(cpu, mem, ovr);
        rm_set16(cpu, mem, &m, sreg_get(cpu, m.reg));
        return;
    }
    case 0x8D: { /* LEA r16, m */
        RM_T m = fetch_rm(cpu, mem, ovr);
        set_reg16(cpu, m.reg, m.off);
        return;
    }
    case 0x8E: { /* MOV Sreg, r/m16 */
        RM_T m = fetch_rm(cpu, mem, ovr);
        sreg_set(cpu, m.reg, rm_get16(cpu, mem, &m));
        return;
    }
    case 0x8F: { /* POP r/m16 */
        RM_T m = fetch_rm(cpu, mem, ovr);
        rm_set16(cpu, mem, &m, pop16(cpu, mem));
        return;
    }

    case 0x90:
        return; /* NOP */
    case 0x91:
    case 0x92:
    case 0x93:
    case 0x94:
    case 0x95:
    case 0x96:
    case 0x97: {
        uint8_t idx = (uint8_t)(op & 7);
        uint16_t tmp = cpu->AX;
        cpu->AX = get_reg16(cpu, idx);
        set_reg16(cpu, idx, tmp);
        return;
    }
    case 0x98: /* CBW */
        cpu->AX = (uint16_t)(int16_t)(int8_t)(cpu->AX & 0xFF);
        return;
    case 0x99: /* CWD */
        cpu->DX = (cpu->AX & 0x8000) ? 0xFFFF : 0x0000;
        return;
    case 0x9A: { /* CALL far */
        uint16_t off = fetch16(cpu, mem);
        uint16_t seg = fetch16(cpu, mem);
        push16(cpu, mem, cpu->CS);
        push16(cpu, mem, cpu->IP);
        cpu->CS = seg;
        cpu->IP = off;
        return;
    }
    case 0x9B: /* WAIT */
        return;
    case 0x9C: /* PUSHF */
        push16(cpu, mem, (uint16_t)((cpu->FLAGS & FLAGS_WRITABLE) | FLAGS_FIXED));
        return;
    case 0x9D: /* POPF */
        cpu->FLAGS = (uint16_t)((pop16(cpu, mem) & FLAGS_WRITABLE) | FLAGS_FIXED);
        return;
    case 0x9E: /* SAHF */
        cpu->FLAGS = (uint16_t)((cpu->FLAGS & 0xFF00) | ((cpu->AX >> 8) & 0xD5) | 0x02);
        return;
    case 0x9F: /* LAHF */
        cpu->AX = (uint16_t)((cpu->AX & 0x00FF) | ((cpu->FLAGS & 0xD5) | 0x02) << 8);
        return;

    case 0xA0: { /* MOV AL, moffs8 */
        uint16_t off = fetch16(cpu, mem);
        uint16_t seg = (ovr == SEG_NONE) ? cpu->DS : seg_reg_value(cpu, ovr);
        cpu->AX = (uint16_t)((cpu->AX & 0xFF00) | mem_r8(mem, seg, off));
        return;
    }
    case 0xA1: { /* MOV AX, moffs16 */
        uint16_t off = fetch16(cpu, mem);
        uint16_t seg = (ovr == SEG_NONE) ? cpu->DS : seg_reg_value(cpu, ovr);
        cpu->AX = mem_r16(mem, seg, off);
        return;
    }
    case 0xA2: { /* MOV moffs8, AL */
        uint16_t off = fetch16(cpu, mem);
        uint16_t seg = (ovr == SEG_NONE) ? cpu->DS : seg_reg_value(cpu, ovr);
        mem_w8(mem, seg, off, (uint8_t)(cpu->AX & 0xFF));
        return;
    }
    case 0xA3: { /* MOV moffs16, AX */
        uint16_t off = fetch16(cpu, mem);
        uint16_t seg = (ovr == SEG_NONE) ? cpu->DS : seg_reg_value(cpu, ovr);
        mem_w16(mem, seg, off, cpu->AX);
        return;
    }
    case 0xA4:
    case 0xA5:
    case 0xA6:
    case 0xA7:
    case 0xAA:
    case 0xAB:
    case 0xAC:
    case 0xAD:
    case 0xAE:
    case 0xAF:
        string_run(cpu, mem, op, ovr, rep);
        if (string_more)
            cpu->IP = entry_ip;
        return;
    case 0xA8: /* TEST AL, imm8 */
        logic8(cpu, (uint8_t)((cpu->AX & 0xFF) & fetch8(cpu, mem)));
        return;
    case 0xA9: /* TEST AX, imm16 */
        logic16(cpu, (uint16_t)(cpu->AX & fetch16(cpu, mem)));
        return;

    case 0xB0:
    case 0xB1:
    case 0xB2:
    case 0xB3:
    case 0xB4:
    case 0xB5:
    case 0xB6:
    case 0xB7:
        set_reg8(cpu, (uint8_t)(op & 7), fetch8(cpu, mem));
        return;
    case 0xB8:
    case 0xB9:
    case 0xBA:
    case 0xBB:
    case 0xBC:
    case 0xBD:
    case 0xBE:
    case 0xBF:
        set_reg16(cpu, (uint8_t)(op & 7), fetch16(cpu, mem));
        return;

    case 0xC0: { /* shift r/m8, imm8 */
        RM_T m = fetch_rm(cpu, mem, ovr);
        uint8_t cnt = fetch8(cpu, mem);
        rm_set8(cpu, mem, &m, shift8(cpu, m.reg, rm_get8(cpu, mem, &m), cnt));
        return;
    }
    case 0xC1: { /* shift r/m16, imm8 */
        RM_T m = fetch_rm(cpu, mem, ovr);
        uint8_t cnt = fetch8(cpu, mem);
        rm_set16(cpu, mem, &m, shift16(cpu, m.reg, rm_get16(cpu, mem, &m), cnt));
        return;
    }
    case 0xC2: { /* RET imm16 */
        uint16_t n = fetch16(cpu, mem);
        cpu->IP = pop16(cpu, mem);
        cpu->SP = (uint16_t)(cpu->SP + n);
        return;
    }
    case 0xC3:
        cpu->IP = pop16(cpu, mem);
        return;
    case 0xC4: { /* LES r16, m16:16 */
        RM_T m = fetch_rm(cpu, mem, ovr);
        set_reg16(cpu, m.reg, mem_r16(mem, m.seg, m.off));
        cpu->ES = mem_r16(mem, m.seg, (uint16_t)(m.off + 2));
        return;
    }
    case 0xC5: { /* LDS r16, m16:16 */
        RM_T m = fetch_rm(cpu, mem, ovr);
        set_reg16(cpu, m.reg, mem_r16(mem, m.seg, m.off));
        cpu->DS = mem_r16(mem, m.seg, (uint16_t)(m.off + 2));
        return;
    }
    case 0xC6: { /* MOV r/m8, imm8 */
        RM_T m = fetch_rm(cpu, mem, ovr);
        rm_set8(cpu, mem, &m, fetch8(cpu, mem));
        return;
    }
    case 0xC7: { /* MOV r/m16, imm16 */
        RM_T m = fetch_rm(cpu, mem, ovr);
        rm_set16(cpu, mem, &m, fetch16(cpu, mem));
        return;
    }
    case 0xC8: { /* ENTER imm16, imm8 */
        uint16_t size = fetch16(cpu, mem);
        uint8_t level = (uint8_t)(fetch8(cpu, mem) & 0x1F);
        uint16_t frame;

        push16(cpu, mem, cpu->BP);
        frame = cpu->SP;

        for (uint8_t i = 1; i < level; i++) {
            cpu->BP = (uint16_t)(cpu->BP - 2);
            push16(cpu, mem, mem_r16(mem, cpu->SS, cpu->BP));
        }
        if (level)
            push16(cpu, mem, frame);

        cpu->BP = frame;
        cpu->SP = (uint16_t)(cpu->SP - size);
        return;
    }
    case 0xC9: /* LEAVE */
        cpu->SP = cpu->BP;
        cpu->BP = pop16(cpu, mem);
        return;
    case 0xCA: { /* RETF imm16 */
        uint16_t n = fetch16(cpu, mem);
        if (n >= 2)
            irq_frame_return(cpu);
        cpu->IP = pop16(cpu, mem);
        cpu->CS = pop16(cpu, mem);
        cpu->SP = (uint16_t)(cpu->SP + n);
        return;
    }
    case 0xCB: /* RETF */
        cpu->IP = pop16(cpu, mem);
        cpu->CS = pop16(cpu, mem);
        return;
    case 0xCC:
        cpu_int(cpu, mem, 3);
        return;
    case 0xCD:
        cpu_int(cpu, mem, fetch8(cpu, mem));
        return;
    case 0xCE:
        if (get_flag(cpu, OF))
            cpu_int(cpu, mem, 4);
        return;
    case 0xCF: /* IRET */
        if (hw_irq_busy())
            irq_frame_return(cpu);
        cpu->IP = pop16(cpu, mem);
        cpu->CS = pop16(cpu, mem);
        cpu->FLAGS = (uint16_t)((pop16(cpu, mem) & FLAGS_WRITABLE) | FLAGS_FIXED);
        if (int_nesting)
            int_nesting--;
        return;

    case 0xD0:
    case 0xD1:
    case 0xD2:
    case 0xD3: {
        RM_T m = fetch_rm(cpu, mem, ovr);
        uint8_t cnt = (op & 2) ? (uint8_t)(cpu->CX & 0xFF) : 1;

        if (op & 1)
            rm_set16(cpu, mem, &m, shift16(cpu, m.reg, rm_get16(cpu, mem, &m), cnt));
        else
            rm_set8(cpu, mem, &m, shift8(cpu, m.reg, rm_get8(cpu, mem, &m), cnt));
        return;
    }
    case 0xD4: { /* AAM */
        uint8_t base = fetch8(cpu, mem);
        uint8_t al = (uint8_t)(cpu->AX & 0xFF);

        if (!base) {
            cpu_int(cpu, mem, 0);
            return;
        }
        cpu->AX = (uint16_t)(((al / base) << 8) | (al % base));
        set_szp8(cpu, (uint8_t)(cpu->AX & 0xFF));
        return;
    }
    case 0xD5: { /* AAD */
        uint8_t base = fetch8(cpu, mem);
        uint8_t al = (uint8_t)(cpu->AX & 0xFF);
        uint8_t ah = (uint8_t)(cpu->AX >> 8);

        cpu->AX = (uint16_t)((uint8_t)(al + ah * base));
        set_szp8(cpu, (uint8_t)(cpu->AX & 0xFF));
        return;
    }
    case 0xD6: /* SALC */
        cpu->AX = (uint16_t)((cpu->AX & 0xFF00) | (get_flag(cpu, CF) ? 0xFF : 0x00));
        return;
    case 0xD7: { /* XLAT */
        uint16_t seg = (ovr == SEG_NONE) ? cpu->DS : seg_reg_value(cpu, ovr);
        cpu->AX = (uint16_t)((cpu->AX & 0xFF00) | mem_r8(mem, seg, (uint16_t)(cpu->BX + (cpu->AX & 0xFF))));
        return;
    }
    case 0xD8:
    case 0xD9:
    case 0xDA:
    case 0xDB:
    case 0xDC:
    case 0xDD:
    case 0xDE:
    case 0xDF: /* no FPU: consume the operand and carry on */
        fetch_rm(cpu, mem, ovr);
        return;

    case 0xE0:
    case 0xE1:
    case 0xE2: { /* LOOPNZ / LOOPZ / LOOP */
        int8_t disp = (int8_t)fetch8(cpu, mem);
        uint8_t take;

        cpu->CX--;
        if (op == 0xE0)
            take = (uint8_t)(cpu->CX != 0 && !get_flag(cpu, ZF));
        else if (op == 0xE1)
            take = (uint8_t)(cpu->CX != 0 && get_flag(cpu, ZF));
        else
            take = (uint8_t)(cpu->CX != 0);

        if (take)
            cpu->IP = (uint16_t)(cpu->IP + disp);
        return;
    }
    case 0xE3: { /* JCXZ */
        int8_t disp = (int8_t)fetch8(cpu, mem);
        if (!cpu->CX)
            cpu->IP = (uint16_t)(cpu->IP + disp);
        return;
    }
    case 0xE4:
        cpu->AX = (uint16_t)((cpu->AX & 0xFF00) | io_in8(fetch8(cpu, mem)));
        return;
    case 0xE5:
        cpu->AX = io_in16(fetch8(cpu, mem));
        return;
    case 0xE6:
        io_out8(fetch8(cpu, mem), (uint8_t)(cpu->AX & 0xFF));
        return;
    case 0xE7:
        io_out16(fetch8(cpu, mem), cpu->AX);
        return;
    case 0xE8: { /* CALL rel16 */
        int16_t disp = (int16_t)fetch16(cpu, mem);
        push16(cpu, mem, cpu->IP);
        cpu->IP = (uint16_t)(cpu->IP + disp);
        return;
    }
    case 0xE9: { /* JMP rel16 */
        int16_t disp = (int16_t)fetch16(cpu, mem);
        cpu->IP = (uint16_t)(cpu->IP + disp);
        return;
    }
    case 0xEA: { /* JMP far */
        uint16_t off = fetch16(cpu, mem);
        uint16_t seg = fetch16(cpu, mem);
        cpu->CS = seg;
        cpu->IP = off;
        return;
    }
    case 0xEB: { /* JMP rel8 */
        int8_t disp = (int8_t)fetch8(cpu, mem);
        cpu->IP = (uint16_t)(cpu->IP + disp);
        return;
    }
    case 0xEC:
        cpu->AX = (uint16_t)((cpu->AX & 0xFF00) | io_in8(cpu->DX));
        return;
    case 0xED:
        cpu->AX = io_in16(cpu->DX);
        return;
    case 0xEE:
        io_out8(cpu->DX, (uint8_t)(cpu->AX & 0xFF));
        return;
    case 0xEF:
        io_out16(cpu->DX, cpu->AX);
        return;

    case 0xF1: { /* emulator hook: run a native BIOS/DOS service */
        /* Remember where this interrupt's frame sits before running the
         * service: a handler may raise a further interrupt of its own (the
         * timer chains to INT 1Ch), which moves SP somewhere else. */
        uint16_t frame_ss = cpu->SS;
        uint16_t frame_sp = cpu->SP;

        int_bios(cpu, mem);

        /* The service reports its status in the flags, but IRET is about to
         * restore the flags pushed by INT. Merge the status bits into that
         * saved copy the way a real handler does -- and only those: the
         * control bits, the interrupt enable above all, belong to the code
         * that was interrupted. Handing back the handler's own IF=0 would
         * leave the guest with interrupts disabled for good. */
        {
            const uint16_t status = CF | PF | AF | ZF | SF | OF;
            uint16_t saved = mem_r16(mem, frame_ss, (uint16_t)(frame_sp + 4));

            mem_w16(mem, frame_ss, (uint16_t)(frame_sp + 4), (uint16_t)((saved & ~status) | (cpu->FLAGS & status)));
        }
        return;
    }
    case 0xF4: /* HLT */
        cpu_halted = 2;
        return;
    case 0xF5: /* CMC */
        set_flag(cpu, CF, (uint8_t)!get_flag(cpu, CF));
        return;

    case 0xF6: { /* unary group, byte */
        RM_T m = fetch_rm(cpu, mem, ovr);
        uint8_t v = rm_get8(cpu, mem, &m);

        switch (m.reg) {
        case 0:
        case 1:
            logic8(cpu, (uint8_t)(v & fetch8(cpu, mem)));
            return;
        case 2:
            rm_set8(cpu, mem, &m, (uint8_t)~v);
            return;
        case 3:
            rm_set8(cpu, mem, &m, sub8(cpu, 0, v, 0));
            set_flag(cpu, CF, v != 0);
            return;
        case 4: { /* MUL */
            uint16_t r = (uint16_t)((cpu->AX & 0xFF) * v);
            cpu->AX = r;
            set_flag(cpu, CF, (r >> 8) != 0);
            set_flag(cpu, OF, (r >> 8) != 0);
            set_szp8(cpu, (uint8_t)(r & 0xFF));
            set_flag(cpu, ZF, r == 0);
            return;
        }
        case 5: { /* IMUL */
            int16_t r = (int16_t)((int8_t)(cpu->AX & 0xFF) * (int8_t)v);
            cpu->AX = (uint16_t)r;
            set_flag(cpu, CF, r != (int8_t)r);
            set_flag(cpu, OF, r != (int8_t)r);
            return;
        }
        case 6: { /* DIV */
            uint16_t num = cpu->AX;
            if (!v || (num / v) > 0xFF) {
                cpu_int(cpu, mem, 0);
                return;
            }
            cpu->AX = (uint16_t)(((num % v) << 8) | (num / v));
            return;
        }
        default: { /* IDIV */
            int16_t num = (int16_t)cpu->AX;
            int8_t d = (int8_t)v;
            if (!d) {
                cpu_int(cpu, mem, 0);
                return;
            }
            {
                int16_t q = (int16_t)(num / d);
                int16_t r = (int16_t)(num % d);
                if (q > 127 || q < -128) {
                    cpu_int(cpu, mem, 0);
                    return;
                }
                cpu->AX = (uint16_t)(((uint8_t)r << 8) | (uint8_t)q);
            }
            return;
        }
        }
    }
    case 0xF7: { /* unary group, word */
        RM_T m = fetch_rm(cpu, mem, ovr);
        uint16_t v = rm_get16(cpu, mem, &m);

        switch (m.reg) {
        case 0:
        case 1:
            logic16(cpu, (uint16_t)(v & fetch16(cpu, mem)));
            return;
        case 2:
            rm_set16(cpu, mem, &m, (uint16_t)~v);
            return;
        case 3:
            rm_set16(cpu, mem, &m, sub16(cpu, 0, v, 0));
            set_flag(cpu, CF, v != 0);
            return;
        case 4: { /* MUL */
            uint32_t r = (uint32_t)cpu->AX * v;
            cpu->AX = (uint16_t)(r & 0xFFFF);
            cpu->DX = (uint16_t)(r >> 16);
            set_flag(cpu, CF, cpu->DX != 0);
            set_flag(cpu, OF, cpu->DX != 0);
            set_szp16(cpu, cpu->AX);
            set_flag(cpu, ZF, r == 0);
            return;
        }
        case 5: { /* IMUL */
            int32_t r = (int32_t)(int16_t)cpu->AX * (int16_t)v;
            cpu->AX = (uint16_t)(r & 0xFFFF);
            cpu->DX = (uint16_t)((uint32_t)r >> 16);
            set_flag(cpu, CF, r != (int16_t)r);
            set_flag(cpu, OF, r != (int16_t)r);
            return;
        }
        case 6: { /* DIV */
            uint32_t num = ((uint32_t)cpu->DX << 16) | cpu->AX;
            if (!v || (num / v) > 0xFFFF) {
                cpu_int(cpu, mem, 0);
                return;
            }
            cpu->AX = (uint16_t)(num / v);
            cpu->DX = (uint16_t)(num % v);
            return;
        }
        default: { /* IDIV */
            int32_t num = (int32_t)(((uint32_t)cpu->DX << 16) | cpu->AX);
            int16_t d = (int16_t)v;
            if (!d) {
                cpu_int(cpu, mem, 0);
                return;
            }
            {
                int32_t q = num / d;
                int32_t r = num % d;
                if (q > 32767 || q < -32768) {
                    cpu_int(cpu, mem, 0);
                    return;
                }
                cpu->AX = (uint16_t)q;
                cpu->DX = (uint16_t)r;
            }
            return;
        }
        }
    }

    case 0xF8:
        set_flag(cpu, CF, 0);
        return;
    case 0xF9:
        set_flag(cpu, CF, 1);
        return;
    case 0xFA:
        set_flag(cpu, IF, 0);
        return;
    case 0xFB:
        set_flag(cpu, IF, 1);
        return;
    case 0xFC:
        set_flag(cpu, DF, 0);
        return;
    case 0xFD:
        set_flag(cpu, DF, 1);
        return;

    case 0xFE: { /* INC/DEC r/m8 */
        RM_T m = fetch_rm(cpu, mem, ovr);
        uint8_t v = rm_get8(cpu, mem, &m);
        uint8_t cf = get_flag(cpu, CF);

        rm_set8(cpu, mem, &m, (m.reg == 0) ? add8(cpu, v, 1, 0) : sub8(cpu, v, 1, 0));
        set_flag(cpu, CF, cf);
        return;
    }
    case 0xFF: { /* INC/DEC/CALL/JMP/PUSH r/m16 */
        RM_T m = fetch_rm(cpu, mem, ovr);

        switch (m.reg) {
        case 0:
        case 1: {
            uint16_t v = rm_get16(cpu, mem, &m);
            uint8_t cf = get_flag(cpu, CF);
            rm_set16(cpu, mem, &m, (m.reg == 0) ? add16(cpu, v, 1, 0) : sub16(cpu, v, 1, 0));
            set_flag(cpu, CF, cf);
            return;
        }
        case 2: { /* CALL near indirect */
            uint16_t target = rm_get16(cpu, mem, &m);
            push16(cpu, mem, cpu->IP);
            cpu->IP = target;
            return;
        }
        case 3: { /* CALL far indirect */
            uint16_t off = mem_r16(mem, m.seg, m.off);
            uint16_t seg = mem_r16(mem, m.seg, (uint16_t)(m.off + 2));
            push16(cpu, mem, cpu->CS);
            push16(cpu, mem, cpu->IP);
            cpu->CS = seg;
            cpu->IP = off;
            return;
        }
        case 4:
            cpu->IP = rm_get16(cpu, mem, &m);
            return;
        case 5: { /* JMP far indirect */
            uint16_t off = mem_r16(mem, m.seg, m.off);
            uint16_t seg = mem_r16(mem, m.seg, (uint16_t)(m.off + 2));
            cpu->CS = seg;
            cpu->IP = off;
            return;
        }
        default:
            push16(cpu, mem, rm_get16(cpu, mem, &m));
            return;
        }
    }

    default:
        if (debug)
            log_line((const uint8_t *)"UNHANDLED OPCODE %x at %x:%x", (unsigned int)op, (unsigned int)cpu->CS, (unsigned int)(uint16_t)(cpu->IP - 1));
        return;
    }
}

void switch_opcode(CPU_T *cpu, Memory_T *memory) {
    /* Start due, so the first pass happens before any guest code runs. */
    uint32_t budget = 4096;
    uint64_t prev_iters = 0;
    uint64_t steps = 0;
    uint64_t frames = 0;
    uint64_t hb_next = 0;
    uint64_t ms_poll = 0;
    uint64_t ms_gfx = 0;
    uint64_t ms_log = 0;
    uint32_t passes = 0;
    uint8_t logging = log_on();

    cpu_halted = 0;

    for (;;) {
        /*
         *  Every 4096 units of work, where a repeat's passes count as the work
         *  they are. Counting instructions instead meant that during a blit —
         *  where one instruction is a 4096-pass slice — 4096 steps was sixteen
         *  million byte copies, some twenty seconds: the screen was repainted
         *  once in that time and F12 was looked at once in that time. The
         *  machine was not stuck, it was being asked for the time of day once
         *  every twenty seconds.
         */
        if (budget >= 4096) {
            budget = 0;
            /* Timing the heartbeat costs syscalls itself. Memento launches
             * without logging, so leave those calls out of normal gameplay. */
            uint64_t t0 = logging ? get_ticks() : 0;

            hw_poll();

            {
                uint64_t t1 = logging ? get_ticks() : 0;

                gfx_frame(memory);
                frames++;

                if (logging) {
                    ms_poll += t1 - t0;
                    ms_gfx += get_ticks() - t1;
                }
            }

            /*
             *  A heartbeat, because the two ways this goes wrong look the same
             *  from outside: a guest that has stopped executing and a frame
             *  that has stopped being presented. One line a second tells them
             *  apart afterwards -- steps stuck means the guest, frames stuck
             *  means the display, neither moving means the emulator never got
             *  back from a syscall.
             */
            if (logging) {
                uint64_t now = get_ticks();

                if (now >= hb_next) {
                    uint16_t pitch = 0, gw = 0, gh = 0;
                    uint16_t sh256 = 0, ch4 = 0;
                    uint32_t pfx_count = 0;
                    uint16_t pfx_cs = 0, pfx_ip = 0;
                    uint32_t rep_count = 0;
                    uint16_t rep_cs = 0, rep_ip = 0;
                    uint64_t str_iters = 0;
                    uint32_t str_slices = 0;
                    uint8_t str_op = 0;
                    uint16_t str_cx = 0;

                    hb_next = now + 1000;
                    vga_geometry(&pitch, &gw, &gh);
                    vga_flip_counts(&sh256, &ch4);
                    cpu_overlong(&pfx_count, &pfx_cs, &pfx_ip);
                    cpu_runaway(&rep_count, &rep_cs, &rep_ip);
                    cpu_strings(&str_iters, &str_slices, &str_op, &str_cx);

                    /*
                     *  Everything needed to tell the three stalls apart without
                     *  guessing: where the wall time went (poll, frame, or the
                     *  interpreter, which is whatever is left), how much of the
                     *  run is going through real port syscalls, and the geometry
                     *  the picture is being decoded with.
                     */
                    log_line((const uint8_t *)"hb steps=%l frames=%l cs=%x ip=%x mode=%x hlt=%u "
                                              "poll=%l gfx=%l/%l log=%l dac=%u/%l ports=%u pit=%u/%u "
                                              "unch=%u plan=%u pitch=%u gw=%u gh=%u flips=%u/%u drop=%u pfx=%u@%x:%x rep=%u@%x:%x "
                                              "str=%l/%u last=%x:cx=%u",
                             steps, frames, cpu->CS, cpu->IP, (unsigned int)vga_mode(), (unsigned int)cpu_halted, ms_poll, ms_gfx, gfx_frame_cost(), ms_log, gfx_dac_loads(), gfx_dac_ms(),
                             hw_port_calls(), (unsigned int)hw_pit_pending(), (unsigned int)hw_pit_rate(), (unsigned int)vga_unchained(), (unsigned int)vga_planar(), (unsigned int)pitch,
                             (unsigned int)gw, (unsigned int)gh, (unsigned int)sh256, (unsigned int)ch4, (unsigned int)log_dropped(), (unsigned int)pfx_count, (unsigned int)pfx_cs,
                             (unsigned int)pfx_ip, (unsigned int)rep_count, (unsigned int)rep_cs, (unsigned int)rep_ip, str_iters, (unsigned int)str_slices, (unsigned int)str_op,
                             (unsigned int)str_cx);
                }
            }

            /*
             *  F12 before the flush, never after.
             *
             *  A flush is a floppy write and the drive can take seconds over
             *  one, so a quit checked afterwards is a quit the player cannot
             *  make: the emulator is inside a syscall for the whole of it and
             *  there is no other way to end the process.
             */
            /*
             *  With the trace on, where the guest is every pass. The heartbeat
             *  only says that once a second, which is hundreds of thousands of
             *  instructions -- far too coarse to point at an instruction that
             *  never finishes. This is within one pass of it.
             */
            if (debug && (passes++ & 3) == 0)
                log_line((const uint8_t *)"at %x:%x steps=%l", (unsigned int)cpu->CS, (unsigned int)cpu->IP, steps);

            if (hw_quit_requested())
                return;

            /* Measured like the rest: an unmeasured call is exactly where the
             * time hid the last two times this looked like a freeze. */
            if (logging) {
                uint64_t t = get_ticks();

                log_tick();
                ms_log += get_ticks() - t;
            }
        }

        if (cpu_halted == 1)
            return;

        /* A pending hardware interrupt takes effect between instructions. */
        if (get_flag(cpu, IF)) {
            uint8_t irq = hw_pending_irq();

            if (irq != 0xFF) {
                hw_take_irq(irq);
                cpu_halted = (cpu_halted == 2) ? 0 : cpu_halted;
                cpu_int(cpu, memory, (uint8_t)(0x08 + irq));
                if (irq < 2)
                    irq_frame[irq] = stack_linear(cpu);
            }
        }

        if (cpu_halted == 2) {
            /* HLT: nothing to do until the next interrupt arrives. */
            hw_poll();
            gfx_frame(memory);
            if (hw_quit_requested())
                return;
            continue;
        }

        {
            /* TF is sampled before the instruction: POPF/IRET enabling it
             * trap after the following instruction, not after themselves. */
            uint8_t trap = get_flag(cpu, TF);
            instruction_interrupted = 0;
            step(cpu, memory);
            if (trap && !instruction_interrupted)
                cpu_int(cpu, memory, 1);
        }
        steps++;

        {
            uint64_t done = 0;

            cpu_strings(&done, (uint32_t *)0, (uint8_t *)0, (uint16_t *)0);
            budget += (uint32_t)(1 + (done - prev_iters));
            prev_iters = done;
        }
    }
}
