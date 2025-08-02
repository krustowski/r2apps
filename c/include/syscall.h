#ifndef _R2_SYSCALL_INCLUDED_
#define _R2_SYSCALL_INCLUDED_

/*
 *  syscall.h
 *
 *  Custom C header file providing the implementation prototypes, declarations and constants
 *  to wrap the syscall ABI of the rou2exOS kernel project.
 *
 *  krusty@vxn.dev / June 30, 2025
 */

/* Software CPU interrupt number to access the ABI.  */
#define ABI_INTERRUPT 	0x7f

typedef long int64_t;

typedef unsigned char 	uint8_t;
typedef unsigned short 	uint16_t;
typedef unsigned int 	uint32_t;
typedef unsigned long 	uint64_t;

/*
 *  SyscallNumber enumeration
 *
 *  This enum suits as a helper for a syscall caller not to use hardcoded integers (as those may
 *  change in the future --- a syscall is reassigned to the different value).
 */
enum SyscallNumber: int64_t {
	ScExit,
	ScSysinfo,
	// [...]
	ScMalloc = 0x0f,
	// [...]
	ScPrints = 0x10,
	// [...]
	ScReadFile = 0x20,
	ScWriteFile = 0x21,
	ScListDir = 0x28
};

/*
 *  int64_t syscall() prototype
 *
 *  Takes up to three valid arguments plus a <syscall_number> to call. All variables involved
 *  have to be 64bit (8 bytes long).
 *  The implemented function prototype is to wrap a raw interrupt settings provided via 
 *  inline x86 assembly.
 */
int64_t syscall(int64_t number, int64_t arg1, int64_t arg2, int64_t arg3);

/*
 *  void exit() prototype
 *
 *  Implementation of syscall 0x00.
 */
void exit(int64_t pid, int64_t code);

/*
 *  int64_t print() prototype
 *
 *  Implementation of syscall 0x10.
 */
int64_t print(const char *str);

/*
 *  int64_t read_file() prototype
 *
 *  Implementation of syscall 0x20.
 */
int64_t read_file(const char *name, char *buffer);

/*
 *  int64_t write_file() prototype
 *
 *  Implementation of syscall 0x21.
 */
int64_t write_file(const char *name, const char *buffer);

/*
 *  int64_t list_dir() prototype
 *
 *  Implementation of syscall 0x28.
 */
int64_t list_dir(int64_t cluster);

/*
 *  type SysInfo_T structure
 *
 *  This structure is used when invoking the syscall with ScSysinfo and value of `0x01` in the 
 *  first argument, whereas the second argument is for a pointer to SysInfo_T instance.
 */
typedef struct {
	uint8_t system_name[32];
	uint8_t system_user[32];
	uint8_t system_version[9];
	uint32_t  system_uptime;
} __attribute__((packed)) SysInfo_T;

/*
 *  type Entry_T structure
 *
 *  This structure is to contain all attributes and info about a directory entry. Related 
 *  syscalls can be found in the Filesystem section of the ABI specification doc: 0x20--0x2f
 */
#pragma pack(push, 1)
typedef struct {
	uint8_t name[8];
	uint8_t ext[3];
	uint8_t attr;
	uint8_t reserved;
	uint8_t tenths;
	uint16_t create_time;
	uint16_t create_date;
	uint16_t last_access_time;
	uint16_t high_cluster;
	uint16_t write_time;
	uint16_t write_date;
	uint16_t start_cluster;
	uint32_t file_size;
} Entry_T;
#pragma pack(pop)

#endif
