#include "cpu.h"
#include "int.h"
#include "mmu.h"
#include "printf.h"

/*
 *  handle_21h()
 *
 *  Interrupt 21h DOS service dispathcer.
 *
 *  krusty@vxn.dev / Nov 5, 2025
 *
 *  https://www.stanislavs.org/helppc/int_21.html
 */
void handle_21h(CPU_T *cpu, Memory_T *memory) {
    SRV_21H service = cpu->AX >> 8;

    printf((const uint8_t *)"=> Interrupt 21h service 0x%x\n", service);

    switch (service) {
    case PROGRAM_TERMINATE: {
        /*
         *  AH = 00
         *  CS = PSP segment addr
         *
         *  returns nothing
         */
        exit(123, 123);
        break;
    }
    case CHAR_INPUT_W_ECHO: {
        /*
         *  AH = 01
         *
         *  on return:
         *  AL = character from STDIN
         */
        break;
    }
    case CHAR_OUTPUT: {
        /*
         *  AH = 02
         *  DL = char to output
         *
         *  returns nothing
         */
        uint8_t dl = cpu->DX & 0xff;
        uint8_t pair[2] = {dl, 0};

        printf((const uint8_t *)"%s\n", pair);
        break;
    }
    case AUX_INPUT: {
        break;
    }
    case AUX_OUTPUT: {
        break;
    }
    case PRINTER_OUTPUT: {
        break;
    }
    case DIRECT_CONS_IO: {
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
        uint8_t dl = cpu->DX & 0xff;

        if (dl == 0xff) {
            // TODO: Implement char input
            break;
        }

        uint8_t pair[2] = {dl, 0};

        printf((const uint8_t *)"%s\n", pair);
        break;
    }
    case UNFILTERED_CHAR_INPUT: {
        break;
    }
    case CHAR_INPUT: {
        break;
    }
    case CHAR_STRING_OUTPUT: {
        /*
         *  AH = 09
         *  DS:DX = pointer to string ending with '$'
         *
         *  returns nothing
         */
        /* Linear addr in Real mode */
        uint32_t addr = (cpu->DS << 4) + cpu->DX;

        uint8_t *p = &memory->bytes[addr];

        while (*p != '$') {
            uint8_t pair[2] = {*p, 0};
            printf((const uint8_t *)"%s", pair);
            p++;
        }

        printf((const uint8_t *)"\n");

        break;
    }
    case BUFFERED_INPUT: {
        break;
    }
    case GET_INPUT_STATUS: {
        break;
    }
    case RESET_INPUT_BUFFER_AND_INPUT: {
        break;
    }
    case DISK_RESET: {
        break;
    }
    case SET_DEF_DISK_DRIVE: {
        break;
    }
    case OPEN_FILE: {
        break;
    }
    case CLOSE_FILE: {
        break;
    }
    case FIND_FIRST_FILE:
    case FIND_NEXT_FILE:
    case DELETE_FILE:
    case SEQUENTIAL_READ:
    case SEQUENTIAL_WRITE:
    case CREATE_OR_TRUNCATE_FILE:
    case RENAME_FILE:
    case RESERVED_18H:
    case GET_DEF_DISK_DRIVE:
    case SET_DISK_TRANSFER_AREA_ADDR:
    case GET_CURRENT_DRIVE_ALLOC_DATA:
    case GET_ALLOC_DATA_FOR_SPEC_DRIVE:
    case RESERVED_1DH:
    case RESERVED_1EH:
    case RESERVED_1FH:
    case RESERVED_20H:
    case RANDOM_READ:
    case RANDOM_WRITE:
    case GET_FILE_SIZE_IN_RECORDS:
    case SET_RANDOM_RECORD_NUM:
    case SET_INTERRUPT_VECTOR:
    case CREATE_PROGRAM_SEGMENT_PREFIX:
    case RANDOM_BLOCK_READ:
    case RANDOM_BLOCK_WRITE:
    case PARSE_FILENAME:
    case GET_SYSTEM_DATE:
    case SET_SYSTEM_DATE:
    case GET_SYSTEM_TIME:
    case SET_SYSTEM_TIME:
    case SET_VERIFY_FLAG:
    case GET_DTA_ADDRESS:
    case GET_DOS_VERSION:
    case TERMINATE_AND_STAY_RESIDENT:
    case GET_DISK_INFO:
    case GET_SET_CTRL_BREAK_FLAG:
    case FIND_ACTIVE_BYTE:
    case GET_INTERRUPT_VECTOR:
    case GET_FREE_DISK_SPACE:
    case RESERVED_37H:
    case GET_COUNTRY_INFORMATION:
    case CREATE_SUBDIRECTORY:
    case DELETE_SUBDIRECTORY:
    case SET_CURRENT_DIRECTORY:
    case CREATE_OR_TRUNCATE_FILE_3CH:
    case OPEN_FILE_3DH:
    case CLOSE_FILE_3EH:
    case READ_FILE_OR_DEVICE:
    case WRITE_FILE_OR_DEVICE:
    case DELETE_FILE_41H:
    case MOVE_FILE_POINTER:
    case GET_OR_SET_FILE_ATTRS:
    case DEVICE_DRIVER_CONTROL:
    case DUPLICATE_HANDLE:
    case FORCE_DUPLICATE_HANDLE:
    case GET_CURRENT_DIRECTORY:
    case ALLOC_MEMORY:
    case RELEASE_MEMORY:
    case RESERVED_4AH:
    case EXECUTE_PROGRAM:
    case TERMINATE_PROGRAM_WITH_RET_CODE:
    case GET_RET_CODE:
    case SEARCH_FOR_FIRST_MATCH:
    case SEARCH_FOR_NEXT_MATCH:
    case GET_DISK_INFO_50H:
    case RESERVED_51H:
    case RESERVED_52H:
    case RESERVED_53H:
    case GET_VERIFY_FLAG:
    case RESERVED_55H:
    case RENAME_FILE_56H:
    case GET_SET_FILE_DATE_TIME:
    case GET_SET_MEM_ALLOC_STRATEGY:
    case GET_EXTENDED_ERROR_INFORMATION:
    case CREATE_TEMP_FILE:
    case CREATE_NEW_FILE:
    case LOCK_UNLOCK_FILE_ACCESS:
    case CRITICAL_ERROR_INFORMATION:
    case NETWORK_SERVICES:
    case NETOWRK_REDIRECTION:
    case GET_FULLY_QUAL_FILE_NAME:
    case RESERVED_61H:
    case GET_ADDR_OF_PROG_SEGMENT_PREFIX:
    case GET_SYS_LEAD_BYTE_TABLE:
    case GET_DEVICE_DRIVER_LOOK_AHEAD:
    case GET_EXTENDED_COUNTRY_INFO:
    case GET_SET_GLOBAL_CODE_PAGE:
    case SET_HANDLE_COUNT:
    case FLUSH_BUFFER:
    case GET_SET_DISK_SERIAL_NUMBER:
    case RESERVED_6AH:
    case RESERVED_6BH:
    case EXTENDED_OPEN_CREATE:
        break;
    }
}
