#include "int.h"
#include "syscall.h"

/*
 *  theM
 *
 *  Simple 16-bit CPU eMulator (x86-16). The emulator also aims to run MS-DOS native
 *  programs and games.
 *
 *  krusty@vxn.dev / Nov 3, 2025
 */

int main(void) {
    print((const uint8_t *)"\ntheM: the 16bit CPU emulator\n");

    CPU_T cpu;
    Memory_T ram;

    /* Reset CPU's program instruction counter */
    cpu.CS = 0x1000;
    cpu.DS = 0x1000;
    cpu.IP = 0x0000;

    ram.start = 0x0000;

    /* Load the program */
    if (!read_file((const uint8_t *)"PRG0.BIN", &ram.bytes[(cpu.CS << 4) + cpu.IP])) {
        print((const uint8_t *)"=> Cannot read the binary file... Program exit.\n");
        return 1;
    }

    /* Print the initial CPU state */
    dump_registers(&cpu);

    /* Switch opcode and emulate the operation */
    switch_opcode(&cpu, &ram);

    /* Print the final CPU state */
    dump_registers(&cpu);

    return 0;
}
