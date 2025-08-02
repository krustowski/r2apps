#ifndef _R2_SYSCALL_INCLUDED_
#define _R2_SYSCALL_INCLUDED_

/*
 *  syscall.h
 *
 *  Custom C++ header file providing the implementation prototypes, declarations and constants
 *  to wrap the syscall ABI of the rou2exOS kernel project.
 *
 *  krusty@vxn.dev / June 30, 2025
 */

/* Software CPU interrupt number to access the ABI.  */
#define ABI_INTERRUPT 	0x7f

typedef long int64_t;

static_assert(sizeof(int64_t) == 8, "Type int64_t is required to be 64bit (8 bytes long).");

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
	ScWriteFile = 0x21
};

/*
 *  int64_t syscall() prototype
 *
 *  Takes up to three valid arguments plus a <syscall_number> to call. All variables involved
 *  have to be 64bit (8 bytes long).
 *  The implemented function prototype is to wrap a raw interrupt settings provided via 
 *  inline x86 assembly.
 *  
 */
extern "C" int64_t syscall(int64_t number, int64_t arg1, int64_t arg2, int64_t arg3);

void exit(int64_t pid, int64_t code);
int64_t print(const char *str);
int64_t read_file(const char *name, char *buffer);
int64_t write_file(const char *name, const char *buffer);

/*
 *  SysInfo_T structure
 *
 *  This structure is used when invoking the syscall with ScSysinfo and value of `0x01` in the 
 *  first argument, whereas the second argument is for a pointer to SysInfo_T instance.
 */
struct SysInfo_T {
	char *system_name;
	char *system_user;
	char *system_version;
	int   system_uptime;
};

#endif
