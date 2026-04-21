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

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"

/* Software CPU interrupt number to access the ABI.  */
#define ABI_INTERRUPT 0x7f

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
    uint32_t system_uptime;
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
 *  type FsckReport_T structure
 *
 *  This structure hold the filesystem check report variables.
 */
typedef struct {
    uint64_t errors;
    uint64_t orphan_clusters;
    uint64_t cross_linked;
    uint64_t invalid_entries;
} __attribute__((packed)) FsckReport_T;

/*
 *  type RTC_T structure
 *
 *  This structure is to hold all important fields needed to read time from the RTC hardware chip.
 */
typedef struct {
    uint8_t seconds;
    uint8_t minutes;
    uint8_t hours;
    uint8_t day;
    uint8_t month;
    uint16_t year;
} __attribute__((packed)) RTC_T;

/*
 *  SyscallNumber enumeration
 *
 *  This enum suits as a helper for a syscall caller not to use hardcoded integers (as those may
 *  change in the future --- a syscall is reassigned to the different value).
 */
typedef enum SyscallNumber : int64_t {
    ScExit = 0x00,
    // System + Memory Management
    ScSysInfo = 0x01,
    ScRTC = 0x02,
    ScPipeSubscribe = 0x03,
    ScMalloc = 0x0a,
    ScRealloc = 0x0b,
    ScFree = 0x0f,
    // Video + Audio Operations
    ScPrintString = 0x10,
    ScClearScreen = 0x11,
    ScWritePixel = 0x12,
    ScPlayFreq = 0x1a,
    ScPlayFile = 0x1b,
    ScPlayStop = 0x1f,
    // Filesystem IO Operations
    ScReadFile = 0x20,
    ScWriteFile = 0x21,
    ScRenameFile = 0x22,
    ScDeleteFile = 0x23,
    ScWriteSubdir = 0x27,
    ScListDir = 0x28,
    ScRunELF = 0x2a,
    ScRunFsCheck = 0x2b,
    // Port IO + Networking Operations
    ScWritePort = 0x30,
    ScReadPort = 0x31,
    ScSerialPort = 0x32,
    ScNewPacket = 0x33,
    ScSendPacket = 0x34,
    ScReceivePort = 0x35,
    ScSendPort = 0x36,
    ScNetRegister = 0x37
} SyscallNo_T;

/*
 *  int64_t syscall() prototype
 *
 *  Takes up to three valid arguments plus a <syscall_number> to call. All variables involved
 *  have to be 64bit (8 bytes long).
 *  The implemented function prototype is to wrap a raw interrupt settings provided via
 *  inline x86 assembly.
 */
int64_t syscall(SyscallNo_T number, int64_t arg1, int64_t arg2, int64_t arg3);

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
 *  int64_t read_rtc() prototype
 *
 *  Implementation of syscall 0x02 (arg1 0x01).
 */
int64_t read_rtc(RTC_T *rtc_data);

/*
 *  int64_t pipe_subscribe() prototype
 *
 *  Implementation of syscall 0x03 (arg1 0x01).
 */
int64_t pipe_subscribe(const uint8_t *buffer);

/*
 *  int64_t pipe_unsubscribe() prototype
 *
 *  Implementation of syscall 0x03 (arg1 0x02).
 */
int64_t pipe_unsubscribe(const uint8_t *buffer);

/*
 *  int64_t pipe_read() prototype
 *
 *  Implementation of syscall 0x03 (arg1 0x03).
 */
int64_t pipe_read(uint8_t *buffer);

/*
 *  int64_t print() prototype
 *
 *  Implementation of syscall 0x10.
 */
int64_t print(const uint8_t *str);

/*
 *  int64_t clear_screen() prototype
 *
 *  Implementation of syscall 0x11.
 */
int64_t clear_screen();

/*
 *  int64_t write_pixel() prototype
 *
 *  Implementation of syscall 0x12.
 */
int64_t write_pixel(uint32_t position, uint16_t color);

/*
 *  int64_t play_freq() prototype
 *
 *  Implementation of syscall 0x1a.
 */
int64_t play_freq(uint16_t freq, uint16_t duration);

/*
 *  int64_t play_midi_file() prototype
 *
 *  Implementation of syscall 0x1b (arg1 0x01).
 */
int64_t play_midi_file(const uint8_t *name);

/*
 *  int64_t stop_speaker() prototype
 *
 *  Implementation of syscall 0x1f.
 */
int64_t stop_speaker();

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

/*
 *  int64_t run_fs_check() prototype
 *
 *  Implementation of syscall 0x2B.
 */
int64_t run_fs_check(FsckReport_T *report);

/*
 *  int64_t read_port() prototype
 *
 *  Implementation of syscall 0x30!
 */
int64_t read_port(uint8_t port, uint64_t *value);

/*
 *  int64_t write_port() prototype
 *
 *  Implementation of syscall 0x31!
 */
int64_t write_port(uint8_t port, const uint64_t value);

/*
 *  int64_t serial_init() prototype
 *
 *  Implementation of syscall 0x32 (arg1 0x01).
 */
int64_t serial_init();

/*
 *  int64_t serial_read() prototype
 *
 *  Implementation of syscall 0x32 (arg1 0x02).
 */
int64_t serial_read(uint32_t *value);

/*
 *  int64_t serial_write() prototype
 *
 *  Implementation of syscall 0x32 (arg1 0x03).
 */
int64_t serial_write(const uint32_t value);

/*
 *  int64_t new_packet() prototype
 *
 *  Implementation of syscall 0x33. The buffer should contain a dummy packet header!
 */
int64_t new_packet(uint8_t type, uint8_t *buffer);

/*
 *  int64_t send_packet() prototype
 *
 *  Implementation of syscall 0x34. The buffer should contain a full packet header!
 */
int64_t send_packet(uint8_t type, uint8_t *buffer);

/*
 *  int64_t receive_data() prototype
 *
 *  Implementation of syscall 0x35.
 */
int64_t receive_data(uint8_t type, uint8_t *buffer);

/*
 *  int64_t send_data() prototype
 *
 *  Implementation of syscall 0x36.
 */
int64_t send_data(uint8_t type, uint8_t *buffer);

/*
 *  int64_t net_register() prototype
 *
 *  Implementation of syscall 0x37. Registers the calling process as the userland
 *  Ethernet driver. The kernel will deliver raw Ethernet frames to this process
 *  via receive_data() and initialise the RTL8139 NIC.
 */
int64_t net_register(void);

/*
 *  int64_t send_eth_frame() prototype
 *
 *  Send a raw Ethernet frame of exactly <len> bytes via the RTL8139.
 *  Uses ScSendPacket (0x34) with type 0x04; arg3 carries the frame length.
 */
int64_t send_eth_frame(const uint8_t *frame, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif
