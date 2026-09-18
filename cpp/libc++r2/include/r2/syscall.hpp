#ifndef _R2CXX_SYSCALL_HPP_
#define _R2CXX_SYSCALL_HPP_

/*
 *  syscall.hpp
 *
 *  The r2 kernel ABI, as C++ sees it: the syscall numbers, the structures the
 *  kernel fills in, and the one inline routine everything else is built on.
 *
 *  Calling convention (software interrupt 0x7f):
 *
 *      RDX, RAX = syscall number     RAX = return value
 *      RDI      = arg1
 *      RSI      = arg2
 *      RCX      = arg3
 *
 *  The number is loaded into both RDX and RAX on purpose.  c/libcr2 passes it
 *  in RDX; the kernel's interrupt entry in src/abi/syscall.rs reads it from
 *  RAX (`mov rdx, rax` before the call into syscall_inner).  Writing both
 *  costs one instruction and leaves this library working against either, which
 *  matters because an app built today may well be run on a kernel built a year
 *  ago --- the ISO and the app suite ship separately.
 *
 *  Every pointer handed to the kernel must point inside 0x600_000-0xA00_000:
 *  the ABI range-checks arguments and returns InvalidInput for anything else.
 *  That is the whole reason r2::heap allocates from an arena inside the image
 *  rather than from the kernel's own 0xC00_000 heap --- see heap.hpp.
 */

#include "types.hpp"

namespace r2 {

/*  Software interrupt used to enter the kernel.  */
inline constexpr int ABI_INTERRUPT = 0x7f;

/*  Bounds the kernel enforces on every pointer argument.  */
inline constexpr uintptr_t USERLAND_START = 0x600000;
inline constexpr uintptr_t USERLAND_END = 0xA00000;

enum class Sys : int64_t {
    Exit = 0x00,

    /*  System and memory  */
    SysInfo = 0x01,
    Rtc = 0x02,
    PipeSubscribe = 0x03,
    GetTicks = 0x04,
    Sleep = 0x05,
    Malloc = 0x0a,
    Realloc = 0x0b,
    Free = 0x0f,

    /*  Video and audio  */
    PrintString = 0x10,
    ClearScreen = 0x11,
    WritePixel = 0x12,
    WriteVga = 0x13,
    MapVram = 0x14,
    SetVideoMode = 0x15,
    GetFbInfo = 0x16,
    BlitBuffer = 0x17,
    GetKernelFont = 0x18,
    PlayFreq = 0x1a,
    PlayFile = 0x1b,
    PlayStop = 0x1f,

    /*  Filesystem  */
    ReadFile = 0x20,
    WriteFile = 0x21,
    RenameFile = 0x22,
    DeleteFile = 0x23,
    WriteSubdir = 0x27,
    ListDir = 0x28,
    RunElf = 0x2a,
    RunFsCheck = 0x2b,
    ListMounts = 0x2c,
    ListDirPath = 0x2d,
    Chdir = 0x2e,
    ListTasks = 0x2f,

    /*  Port IO and networking  */
    WritePort = 0x30,
    ReadPort = 0x31,
    SerialPort = 0x32,
    NewPacket = 0x33,
    SendPacket = 0x34,
    ReceivePort = 0x35,
    SendPort = 0x36,
    NetRegister = 0x37,
    NetStatus = 0x38,
    ReadFileAt = 0x39,
};

/*  Return codes the ABI uses in place of the 0 it returns on success.  */
enum class SysError : int64_t {
    Ok = 0x00,
    Busy = 0xfa,
    NotImplemented = 0xfb,
    InvalidInput = 0xfc,
    FilesystemError = 0xfd,
    FileNotFound = 0xfe,
};

/*
 *  The raw entry point.  Returns whatever the kernel leaves in RAX; the
 *  convention differs per syscall, so the typed wrappers in fs.hpp, gfx.hpp
 *  and the rest are what callers should normally reach for.
 */
inline int64_t raw_syscall(Sys number, int64_t arg1 = 0, int64_t arg2 = 0,
                           int64_t arg3 = 0) noexcept {
    int64_t ret;
    asm volatile("int $0x7f"
                 : "=a"(ret)
                 : "a"(number), "d"(number), "D"(arg1), "S"(arg2), "c"(arg3)
                 : "r11", "memory");
    return ret;
}

/*  True when a pointer may legally be passed to the kernel.  */
inline bool is_kernel_addressable(const void *p) noexcept {
    uintptr_t addr = (uintptr_t)p;
    return addr >= USERLAND_START && addr <= USERLAND_END;
}

/* ------------------------------------------------------------------------ *
 *  Structures the kernel reads and writes.  Layout must match
 *  c/libcr2/syscall.h exactly; these cross the ABI boundary by address.
 * ------------------------------------------------------------------------ */

struct __attribute__((packed)) SysInfo {
    uint8_t system_name[32];
    uint8_t system_user[32];
    uint8_t system_path[32];
    uint8_t system_version[8];
    uint32_t system_path_cluster;
    uint32_t system_uptime;
    uint8_t ip_addr[4];
};

struct __attribute__((packed)) RtcTime {
    uint8_t seconds;
    uint8_t minutes;
    uint8_t hours;
    uint8_t day;
    uint8_t month;
    uint16_t year;
};

struct __attribute__((packed)) DirEntry {
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
};

struct __attribute__((packed)) FsckReport {
    uint64_t errors;
    uint64_t orphan_clusters;
    uint64_t cross_linked;
    uint64_t invalid_entries;
};

struct __attribute__((packed)) MountInfo {
    uint8_t path[32];
    uint8_t path_len;
    uint8_t fs_type; /*  0 none, 1 rootfs, 2 fat12, 3 iso9660  */
};

struct __attribute__((packed)) VfsDirEntry {
    uint8_t name[32]; /*  not NUL-terminated --- use name_len  */
    uint8_t name_len;
    uint8_t is_dir;
    uint32_t size;
};

struct __attribute__((packed)) FbInfo {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
};

struct __attribute__((packed)) MousePacket {
    uint8_t buttons; /*  bit 0 left, bit 1 right, bit 2 middle  */
    int8_t dx;
    int8_t dy; /*  positive is up, the PS/2 way  */
};

struct __attribute__((packed)) NetStatus {
    uint8_t mac[6];
    uint8_t ip[4];
    uint8_t drv_active;
    uint8_t n_ports;
    uint16_t ports[16];
};

struct __attribute__((packed)) TaskInfo {
    uint8_t id;
    uint8_t mode;   /*  0 kernel, 1 user  */
    uint8_t status; /*  0 ready, 1 running, 2 idle, 3 blocked, 4 crashed, 5 dead  */
    uint8_t _pad;
    uint8_t name[16];
};

struct ReadRange {
    uint64_t buffer;
    uint64_t offset;
    uint64_t length;
};

static_assert(sizeof(SysInfo) == 116, "SysInfo must match the kernel's layout");
static_assert(sizeof(VfsDirEntry) == 38, "VfsDirEntry must match the kernel's layout");
static_assert(sizeof(MousePacket) == 3, "MousePacket must match the kernel's layout");
static_assert(sizeof(TaskInfo) == 20, "TaskInfo must match the kernel's layout");

} // namespace r2

#endif
