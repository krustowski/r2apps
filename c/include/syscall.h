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
 *  type SysInfo_T structure
 *
 *  This structure is used when invoking the syscall with ScSysinfo and value of `0x01` in the 
 *  first argument, whereas the second argument is for a pointer to SysInfo_T instance.
 */
typedef struct {
	uint8_t system_name[32];
	uint8_t system_user[32];
	uint8_t system_path[32];
	uint8_t system_version[8];
	uint32_t  system_uptime;
} __attribute__((packed)) SysInfo_T;

/*
 *  type Entry_T structure
 *
 *  This structure is to contain all attributes and info about a directory entry. Related 
 *  syscalls can be found in the Filesystem section of the ABI specification doc: 0x20--0x2f
 */
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
} __attribute__((packed)) Entry_T;

/*
 *  SyscallNumber enumeration
 *
 *  This enum suits as a helper for a syscall caller not to use hardcoded integers (as those may
 *  change in the future --- a syscall is reassigned to the different value).
 */
enum SyscallNumber: int64_t {
	ScExit,
	// System + Memory Management
	ScSysInfo,
	ScRTC,
	ScFree = 0x0d,
	ScRealloc = 0x0e,
	ScMalloc = 0x0f,
	// Video
	ScPrints = 0x10,
	// Filesystem
	ScReadFile = 0x20,
	ScWriteFile = 0x21,
	ScRenameFile = 0x22,
	ScDeleteFile = 0x23,
	ScWriteSubdir = 0x27,
	ScListDir = 0x28,
	ScRunELF = 0x2a,
	// Port IO + Networking
	ScWritePort = 0x30,
	ScReadPort = 0x31,
	// Audio
	ScPlayFreq = 0x40,
	ScPlayFile = 0x41,
	ScPlayStop = 0x4f
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
 *  int64_t read_sysinfo() prototype
 *
 *  Implementation of syscall 0x01 (arg1 0x01).
 */
int64_t read_sysinfo(SysInfo_T *sysinfo);

/*
 *  int64_t write_sysinfo() prototype
 *
 *  Implementation of syscall 0x01 (arg1 0x02).
 */
int64_t write_sysinfo(const SysInfo_T *sysinfo);

/*
 *  int64_t print() prototype
 *
 *  Implementation of syscall 0x10.
 */
int64_t print(const uint8_t *str);

/*
 *  int64_t read_file() prototype
 *
 *  Implementation of syscall 0x20.
 */
int64_t read_file(const uint8_t *name, uint8_t *buffer);

/*
 *  int64_t write_file() prototype
 *
 *  Implementation of syscall 0x21.
 */
int64_t write_file(const uint8_t *name, const uint8_t *buffer);

/*
 *  int64_t rename_file() prototype
 *
 *  Implementation of syscall 0x22.
 */
int64_t rename_file(const uint8_t *old_name, const uint8_t *new_name);

/*
 *  int64_t delete_file() prototype
 *
 *  Implementation of syscall 0x23.
 */
int64_t delete_file(const uint8_t *name);

/*
 *  int64_t write_subdir() prototype
 *
 *  Implementation of syscall 0x27.
 */
int64_t write_subdir(uint16_t cluster, const uint8_t *name);

/*
 *  int64_t list_dir() prototype
 *
 *  Implementation of syscall 0x28.
 */
int64_t list_dir(int64_t cluster, Entry_T entries[32]);

/*
 *  int64_t run_elf() prototype
 *
 *  Implementation of syscall 0x2A.
 */
int64_t run_elf(const uint8_t *name, uint8_t *pid);

#endif
