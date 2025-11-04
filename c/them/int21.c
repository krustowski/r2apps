#include "printf.h"

#include "cpu.h"

void int21h(CPU_T* cpu, uint8_t* memory)
{
	if (cpu->AX >> 8 == 0x09)
	{
		uint8_t offset = cpu->DX & 0xff;
		uint8_t segment = cpu->DS & 0xff;

		// Linear addr in Real mode
		uint16_t addr = ((uint16_t)segment << 4) + offset;

		uint8_t *p = &memory[addr];

		printf("=> INT 21H: printing string from addr: %x\n", addr);

		/*while (*p != '$')
		{
			print(p);
			p++;
		}*/

		print(p);
		print("\n");
	}
}


