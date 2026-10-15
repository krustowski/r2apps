#ifndef _THEM_INT_INCLUDED_
#define _THEM_INT_INCLUDED_

/*
 *  int.h
 *
 *  krusty@vxn.dev / Nov 4, 2025
 */

#include "cpu.h"
#include "mmu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* BIOS interrupts */
typedef enum {
    BOUND_FAIL = 0x05,
    REAL_TIME_CLOCK_TICKS = 0x08,
    KEYBOARD_INT = 0x09,
    VIDEO = 0x10,
    EQUIPMENT_LIST,
    MEMORY_CONVENTIONAL_SIZE,
    DISK_SERVICES,
    SERIAL_PORT_SERVICES,
    MISC_SYSTEM_SERVICES,
    KEYBOARD_SERVICES,
    PRINTER_SERVICES,
    EXEC_CASSETTE_BASIC,
    LOAD_OPERATING_SYSTEM,
    RTC_AND_PCI_SERVICES,
    CTRL_BREAK_HANDLER,
    TIMER_TICK_HANDLER,
    _POINTER_TO_VPT,
    _POINTER_TO_DPT,
    _POINTER_TO_VGCT,
    DOS_RESERVED = 0x20,
    DOS_SERVICES,
    ADDRESS_POINTER_FDPT_DRV1 = 0x41,
    ADDRESS_POINTER_FDPT_DRV2 = 0x46,
    RTC_ALARM = 0x4A
} INT_BIOS;

typedef enum {
    SET_VIDEO_MODE = 0x00,
    SET_CURSOR_SHAPE,
    SET_CURSOR_POSITION,
    GET_CURSOR_POSITION_AND_SHAPE,
    GET_LIGHT_PEN_POSITION,
    GET_DISPLAY_PAGE,
    CLEAR_SCROLL_SCREEN_UP,
    CLEAR_SCROLL_SCREEN_DOWN,
    READ_CHAR_WITH_ATTR_AT_CURSOR,
    WRITE_CHAR_WITH_ATTR_AT_CURSOR,
    WRITE_CHAR_AT_CURSOR,
    SET_BORDER_COLOR,
    WRITE_GRAPHICS_PIXEL,
    READ_GRAPHICS_PIXEL,
    WRITE_CHAR_IN_TTY_MODE,
    GET_VIDEO_MODE,
    SET_PALETTE_REGISTERS = 0x10,
    CHAR_GENERATOR,
    VIDEO_SUBSYSTEM_CONFIGURATION,
    WRITE_STRING,
    GET_SET_DISPLAY_COMBINATION_CODE = 0x1A,
    GET_FUNC_INFO,
    SAVE_RESTORE_VIDEO_STATE,
    VESA_BIOS_EXT_FUNC = 0x4F
} SRV_10H;

typedef enum {
    RESET_DRIVES = 0x00,
    CHECK_DRIVE_STATUS,
    READ_SECTORS,
    WRITE_SECTORS,
    VERIFY_SECTORS,
    FORMAT_TRACK,
    GET_DRIVE_PARAMS = 0x08,
    INIT_FDC,
    SEEK_TO_SPEC_TRACK = 0x0C,
    RESET_FDC,
    GET_DRIVE_TYPE = 0x15,
    GET_FLOPPY_DRV_MEDIA_CHANGE_STATUS,
    SET_DISK_TYPE,
    EXT_DISK_DRIVE_INST_CHECK = 0x41,
    EXT_READ_SECTORS,
    EXT_WRITE_SECTORS,
    EXT_VERIFY_SECTORS,
    LOCK_UNLOCK_DRIVE,
    EJECT_MEDIA,
    EXT_SEEK,
    EXT_GET_DRIVE_PARAMS,
    EXT_GET_MEDIA_CHANGE_STATUS,
    EXT_SET_HW_CONFIG = 0x4E
} SRV_13H;

typedef enum { SERIAL_PORT_INIT = 0x00, TRANSMIT_CHAR, RECEIVE_CHAR, STATUS } SRV_14H;

typedef enum {
    TURN_ON_CASSETTE_DRV_MOTOR = 0x00,
    TURN_OFF_CASSETTE_DRV_MOTOR,
    READ_DATA_BLOCKS_FROM_CASSETTE,
    WRITE_DATA_BLOCKS_TO_CASSETTE,
    KEYBOARD_INTERCEPT = 0x4F,
    EVENT_WAIT = 0x83,
    READ_JOYSTICK,
    SYSREQ_KEY_CALLOUT,
    WAIT,
    MOVE_BLOCK,
    GET_EXTENDED_MEM_SIZE,
    SWITCH_TO_PROTECTED_MODE,
    GET_SYSTEM_PARAMS = 0xC0,
    GET_EXTENDED_BIOS_DATA_AREA_SEGMENT,
    POINTING_DEVICE_SERVICES,
    WATCHDOG_TIMER_FUNCTIONS,
    PROGRAMMABLE_OPTION_SELECT,
    EISA_SYS_FUNCTIONS = 0xD8,
    GET_EXTENDED_MEM_SIZE_NEW = 0xE8,
    QUERY_SYS_ADDR_MAP = 0xE8
} SRV_15H;

typedef enum {
    READ_CHAR = 0x00,
    READ_INPUT_STATUS,
    READ_KEYBOARD_SHIFT_STATUS,
    STORE_KEYSTROKE_IN_KEYBOARD_BUF = 0x05,
    READ_CHAR_EXTENDED = 0x10,
    READ_INPUT_STATUS_EXTENDED,
    READ_KEYBOARD_SHIFT_STATUS_EXTENDED
} SRV_16H;

typedef enum { PRINT_CHAR_TO_PRINTER = 0x00, INIT_PRINTER, CHECK_PRINTER_STATUS } SRV_17H;

/* 0x21 services */
typedef enum {
    PROGRAM_TERMINATE = 0x00,
    CHAR_INPUT_W_ECHO,
    CHAR_OUTPUT,
    AUX_INPUT,
    AUX_OUTPUT,
    PRINTER_OUTPUT,
    DIRECT_CONS_IO,
    UNFILTERED_CHAR_INPUT,
    CHAR_INPUT,
    CHAR_STRING_OUTPUT,
    BUFFERED_INPUT,
    GET_INPUT_STATUS,
    RESET_INPUT_BUFFER_AND_INPUT,
    DISK_RESET,
    SET_DEF_DISK_DRIVE,
    OPEN_FILE,
    CLOSE_FILE = 0x10,
    FIND_FIRST_FILE,
    FIND_NEXT_FILE,
    DELETE_FILE,
    SEQUENTIAL_READ,
    SEQUENTIAL_WRITE,
    CREATE_OR_TRUNCATE_FILE,
    RENAME_FILE,
    RESERVED_18H,
    GET_DEF_DISK_DRIVE,
    SET_DISK_TRANSFER_AREA_ADDR,
    GET_CURRENT_DRIVE_ALLOC_DATA,
    GET_ALLOC_DATA_FOR_SPEC_DRIVE,
    RESERVED_1DH,
    RESERVED_1EH,
    RESERVED_1FH,
    RESERVED_20H = 0x20,
    RANDOM_READ,
    RANDOM_WRITE,
    GET_FILE_SIZE_IN_RECORDS,
    SET_RANDOM_RECORD_NUM,
    SET_INTERRUPT_VECTOR,
    CREATE_PROGRAM_SEGMENT_PREFIX,
    RANDOM_BLOCK_READ,
    RANDOM_BLOCK_WRITE,
    PARSE_FILENAME,
    GET_SYSTEM_DATE,
    SET_SYSTEM_DATE,
    GET_SYSTEM_TIME,
    SET_SYSTEM_TIME,
    SET_VERIFY_FLAG,
    GET_DTA_ADDRESS,
    GET_DOS_VERSION = 0x30,
    TERMINATE_AND_STAY_RESIDENT,
    GET_DISK_INFO,
    GET_SET_CTRL_BREAK_FLAG,
    FIND_ACTIVE_BYTE,
    GET_INTERRUPT_VECTOR,
    GET_FREE_DISK_SPACE,
    RESERVED_37H,
    GET_COUNTRY_INFORMATION,
    CREATE_SUBDIRECTORY,
    DELETE_SUBDIRECTORY,
    SET_CURRENT_DIRECTORY,
    CREATE_OR_TRUNCATE_FILE_3CH,
    OPEN_FILE_3DH,
    CLOSE_FILE_3EH,
    READ_FILE_OR_DEVICE,
    WRITE_FILE_OR_DEVICE = 0x40,
    DELETE_FILE_41H,
    MOVE_FILE_POINTER,
    GET_OR_SET_FILE_ATTRS,
    DEVICE_DRIVER_CONTROL, /* IOCTL */
    DUPLICATE_HANDLE,
    FORCE_DUPLICATE_HANDLE,
    GET_CURRENT_DIRECTORY,
    ALLOC_MEMORY,
    RELEASE_MEMORY,
    RESERVED_4AH,
    EXECUTE_PROGRAM,
    TERMINATE_PROGRAM_WITH_RET_CODE,
    GET_RET_CODE,
    SEARCH_FOR_FIRST_MATCH,
    SEARCH_FOR_NEXT_MATCH,
    GET_DISK_INFO_50H = 0x50,
    RESERVED_51H,
    RESERVED_52H,
    RESERVED_53H,
    GET_VERIFY_FLAG,
    RESERVED_55H,
    RENAME_FILE_56H,
    GET_SET_FILE_DATE_TIME,
    GET_SET_MEM_ALLOC_STRATEGY,
    GET_EXTENDED_ERROR_INFORMATION,
    CREATE_TEMP_FILE,
    CREATE_NEW_FILE,
    LOCK_UNLOCK_FILE_ACCESS,
    CRITICAL_ERROR_INFORMATION,
    NETWORK_SERVICES,
    NETOWRK_REDIRECTION,
    GET_FULLY_QUAL_FILE_NAME = 0x60,
    RESERVED_61H,
    GET_ADDR_OF_PROG_SEGMENT_PREFIX,
    GET_SYS_LEAD_BYTE_TABLE,
    GET_DEVICE_DRIVER_LOOK_AHEAD,
    GET_EXTENDED_COUNTRY_INFO,
    GET_SET_GLOBAL_CODE_PAGE,
    SET_HANDLE_COUNT,
    FLUSH_BUFFER,
    GET_SET_DISK_SERIAL_NUMBER,
    RESERVED_6AH,
    RESERVED_6BH,
    EXTENDED_OPEN_CREATE,
} SRV_21H;

extern uint8_t debug;

/*
 *  switch_opcodes()
 *
 *  Core function to identify an instruction and its operands to emulate a CPU
 *  operation.
 */
void switch_opcode(CPU_T *cpu, Memory_T *memory);

void int_bios(CPU_T *cpu, Memory_T *memory);

/* Initialise BIOS drive A: and its diskette parameter table in guest RAM. */
void bios_disk_init(Memory_T *);

/* Keyboard helpers shared between the BIOS and the DOS console services. */
uint8_t bios_key_ready(Memory_T *);
uint8_t bios_key_read(Memory_T *, uint16_t *entry);
void bios_retry_instruction(CPU_T *, Memory_T *);

/* Set up the DOS state for a program loaded with the given PSP segment. */
void dos_init(uint16_t psp_segment);
void dos_shutdown(void);
uint8_t dos_exit_code(void);

/* Size of a file in the working directory, or 0 when it is not there. */
uint32_t dos_file_size(const uint8_t *name);

/* Read a file into <dest>; returns the byte count, or 0 on failure.  When
 * the directory listing cannot supply a size, <fallback> is used instead
 * (0 makes an unknown size a failure). */
uint32_t dos_read_file(const uint8_t *name, uint8_t *dest, uint32_t cap, uint32_t fallback);

/* The working directory in the kernel's spelling, asked for until the kernel
 * actually answers.  Returns 0, and "/", when it never did. */
uint8_t dos_cwd(uint8_t *out, uint16_t cap);
void dos_set_cwd(const uint8_t *dir);

/* Put the working directory in front of a relative <name>, in place. */
void dos_absolute(uint8_t *name, uint16_t cap);

/* Load a .COM or .EXE at <psp_segment> and set the CPU up to enter it.
 * A flat .COM is loaded and entered at <com_org> within that segment;
 * .EXE entry points come from the header. DOS EXEC uses com_org = 0x100.
 * <name> is resolved the way DOS resolves one, so it may be extended in
 * place with ".COM" or ".EXE". */
uint8_t dos_load_program(Memory_T *, CPU_T *, uint16_t psp_segment, uint16_t com_org, uint8_t *name);

void handle_05h(CPU_T *, Memory_T *);
void handle_08h(CPU_T *, Memory_T *);
void handle_09h(CPU_T *, Memory_T *);
void handle_10h(CPU_T *, Memory_T *);
void handle_11h(CPU_T *, Memory_T *);
void handle_12h(CPU_T *, Memory_T *);
void handle_13h(CPU_T *, Memory_T *);
void handle_14h(CPU_T *, Memory_T *);
void handle_15h(CPU_T *, Memory_T *);
void handle_16h(CPU_T *, Memory_T *);
void handle_17h(CPU_T *, Memory_T *);
void handle_18h(CPU_T *, Memory_T *);
void handle_19h(CPU_T *, Memory_T *);
void handle_1Ah(CPU_T *, Memory_T *);
void handle_1Bh(CPU_T *, Memory_T *);
void handle_1Ch(CPU_T *, Memory_T *);
void handle_1Dh(CPU_T *, Memory_T *);
void handle_1Eh(CPU_T *, Memory_T *);
void handle_1Fh(CPU_T *, Memory_T *);
void handle_20h(CPU_T *, Memory_T *);
void handle_21h(CPU_T *, Memory_T *);
void handle_41h(CPU_T *, Memory_T *);
void handle_46h(CPU_T *, Memory_T *);
void handle_4Ah(CPU_T *, Memory_T *);

#ifdef __cplusplus
}
#endif

#endif
