#include "mem.h"
#include "printf.h"

#include "cpu.h"

/*
 *  theM
 *
 *  Simple 16bit CPU eMulator.
 *
 *  krusty@vxn.dev / Nov 3, 2025
 */

int main(int64_t pid, int64_t arg)
{
	print("\ntheM: the 16bit CPU emulator\n");

	uint8_t memory[1 << 20];
	CPU_T cpu;

	/* Reset CPU's program instruction counter */
	cpu.IP = 0;

	/* Load the program */
	read_file((const uint8_t *) "PRG0.BIN", memory);
	/*{
		print("=> Cannot read the binary file... Program exit.\n");
		exit(pid, 161);
	}*/

	/* Print the initial CPU state */
	dump_registers(&cpu);

	/* Switch opcode and emulate the operation */
	switch_opcode(&cpu, memory);

	/* Print the final CPU state */
	dump_registers(&cpu);

	exit(pid, 191);
}

