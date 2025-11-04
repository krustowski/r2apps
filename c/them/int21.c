#include "printf.h"

#include "cpu.h"
#include "int.h"

/*  
 *  int21h()
 *
 *  Interrupt 21h DOS service dispathcer.
 *
 *  https://www.stanislavs.org/helppc/int_21.html
 */
void int21h(CPU_T *cpu, uint8_t *memory)
{
	enum SRV_21H service = cpu->AX >> 8;

	printf("=> Interrupt 21h service %x\n", service);

	switch (service)
	{
		case PROGRAM_TERMINATE:
			/*
			 *  AH = 00
			 *  CS = PSP segmenta addr
			 *
			 *  returns nothing
			 */
			{
				exit(123, 123);
				break;
			}
		case CHAR_INPUT_W_ECHO:
			/*
			 *  AH = 01
			 *
			 *  on return:
			 *  AL = character from STDIN
			 */
			{
				break;
			}
		case CHAR_OUTPUT:
			/*
			 *  AH = 02
			 *  DL = char to output 
			 *
			 *  returns nothing
			 */
			{
				uint8_t dl = cpu->DX & 0xff;
				uint8_t pair[2] = {dl, 0};

				printf("%s\n", pair);
				break;
			}
		case AUX_INPUT:
			{
				break;
			}
		case AUX_OUTPUT:
			{
				break;
			}
		case PRINTER_OUTPUT:
			{
				break;
			}
		case DIRECT_CONS_IO:
			/*
			 *  AH = 06
			 *  DL = (0-FE) char to output 
			 *     = FF for input request
			 *
			 *  on return:
			 *  AL = input character (if DL = FF)
			 *  ZF = 0 if input char available in AL 
			 *     = 1 if no char ready when the input was requested
			 */
			{
				uint8_t dl = cpu->DX & 0xff;
				
				if (dl == 0xff)
				{
					// TODO: Implement char input
					break;
				}

				uint8_t pair[2] = {dl, 0};
				
				printf("%s\n", pair);
				break;
			}
		case UNFILTERED_CHAR_INPUT:
			{
				break;
			}
		case CHAR_INPUT:
			{
				break;
			}
		case CHAR_STRING_OUTPUT:
			/*
			 *  AH = 09
			 *  DS:DX = pointer to string ending with '$'
			 *
			 *  returns nothing
			 */
			{
				uint8_t offset = cpu->DX & 0xff;
				uint8_t segment = cpu->DS & 0xff;

				/* Linear addr in Real mode */
				uint16_t addr = ((uint16_t)segment << 4) + offset;

				uint8_t *p = &memory[addr];

				while (*p != '$')
				{
					uint8_t pair[2] = {*p, 0};
					print(pair);
					p++;
				}

				printf("\n");
				break;
			}
		case BUFFERED_INPUT:
			{
				break;
			}
		case GET_INPUT_STATUS:
			{
				break;
			}
		case RESET_INPUT_BUFFER_AND_INPUT:
			{
				break;
			}
		case DISK_RESET:
			{
				break;
			}
		case SET_DEF_DISK_DRIVE:
			{
				break;
			}
		case OPEN_FILE:
			{
				break;
			}
		case CLOSE_FILE:
			{
				break;
			}
	}
}
