#include "printf.h"

#include "cpu.h"
#include "int.h"

void int21h(CPU_T* cpu, uint8_t* memory)
{
	enum SRV_21H service = cpu->AX >> 8;

	switch (service)
	{
		case CHAR_STRING_OUTPUT:
			{
				uint8_t offset = cpu->DX & 0xff;
				uint8_t segment = cpu->DS & 0xff;

				/* Linear addr in Real mode */
				uint16_t addr = ((uint16_t)segment << 4) + offset;

				uint8_t *p = &memory[addr];

				/*printf("=> INT 21H: printing string from addr: %x\n", addr);*/

				print(p);
				print("\n");
			}
	}
}
