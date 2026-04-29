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
    uint32_t system_path_cluster;
    uint32_t system_uptime;
    uint8_t ip_addr[4];
} __attribute__((packed)) SysInfo_T;

/*
 *  type NetStatus_T structure
 *
 *  Filled by syscall 0x38 (ScNetStatus).
 *  mac: RTL8139 hardware MAC (cached in kernel when ETH driver registers).
 *  ip:  IPv4 address (written by ETH driver via write_sysinfo / 0x01/0x02).
 *  drv_active: 1 if the global ETH driver process is registered.
 *  n_ports: number of active TCP port bindings.
 *  ports[16]: bound TCP destination ports (0-padded).
 */
typedef struct {
    uint8_t  mac[6];
    uint8_t  ip[4];
    uint8_t  drv_active;
    uint8_t  n_ports;
    uint16_t ports[16];
} __attribute__((packed)) NetStatus_T;

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
 *  type MountInfo_T structure
 *
 *  Describes one VFS mount point as returned by syscall 0x2C (ScListMounts).
 *  fs_type: 0=none, 1=rootfs, 2=fat12, 3=iso9660
 */
typedef struct {
    uint8_t path[32];
    uint8_t path_len;
    uint8_t fs_type;
} __attribute__((packed)) MountInfo_T;

/*
 *  type VfsDirEntry_T structure
 *
 *  A unified directory entry returned by syscall 0x2D (ScListDirPath).
 *  Works for both FAT12 and ISO9660.  name is NOT NUL-terminated; use name_len.
 */
typedef struct {
    uint8_t  name[32];
    uint8_t  name_len;
    uint8_t  is_dir;
    uint32_t size;
} __attribute__((packed)) VfsDirEntry_T;

/*
 *  type FBInfo_T structure
 *
 *  Returned by get_fb_info() / ScGetFBInfo (0x16).
 *  Describes the VESA linear framebuffer provided by the bootloader.
 */
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
} __attribute__((packed)) FBInfo_T;

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
    ScGetTicks = 0x04,
    ScSleep = 0x05,
    ScMalloc  = 0x0a,
    ScRealloc = 0x0b,
    ScFree    = 0x0f,
    // Video + Audio Operations
    ScPrintString = 0x10,
    ScClearScreen = 0x11,
    ScWritePixel = 0x12,
    ScWriteVGA = 0x13,
    ScMapVram = 0x14,
    ScSetVideoMode = 0x15,
    ScGetFBInfo = 0x16,
    ScBlitBuffer = 0x17,
    ScGetKernelFont = 0x18,
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
    ScListMounts = 0x2c,
    ScListDirPath = 0x2d,
    ScChdir = 0x2e,
    ScListTasks = 0x2f,
    // Port IO + Networking Operations
    ScWritePort = 0x30,
    ScReadPort = 0x31,
    ScSerialPort = 0x32,
    ScNewPacket = 0x33,
    ScSendPacket = 0x34,
    ScReceivePort = 0x35,
    ScSendPort = 0x36,
    ScNetRegister = 0x37,
    ScNetStatus   = 0x38
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
 *  uint64_t get_ticks() prototype
 *
 *  Implementation of syscall 0x04.
 *  Returns milliseconds elapsed since boot (10 ms resolution, PIT at 100 Hz).
 */
uint64_t get_ticks(void);

/*
 *  void sleep_ms() prototype
 *
 *  Implementation of syscall 0x05.
 *  Blocks the calling process for at least <ms> milliseconds (rounded up to
 *  the next 10 ms PIT tick).  Returns when the kernel has woken the process.
 */
void sleep_ms(uint64_t ms);

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
 *  int64_t write_vga() prototype
 *
 *  Implementation of syscall 0x13.
 *  Renders a 320×200 VGA mode-13h palette-indexed buffer to the kernel framebuffer.
 *  palette: pointer to 768 bytes (256×RGB), or NULL to use the default VGA palette.
 */
int64_t write_vga(const uint8_t *vga_buf, const uint8_t *palette);

/*
 *  uint64_t map_vram() prototype
 *
 *  Implementation of syscall 0x14.
 *  Maps physical VGA graphics RAM (0xA0000–0xAFFFF) into the calling
 *  process at virtual 0xA00_000 with USER+WRITE.
 *  Returns the virtual base address on success, 0 on failure. Idempotent.
 */
uint64_t map_vram(void);

/*
 *  int64_t get_fb_info() prototype
 *
 *  Implementation of syscall 0x16.
 *  Fills *info with width, height, pitch, and bpp of the VESA framebuffer.
 *  Returns 0 on success, 1 if no framebuffer is available.
 */
int64_t get_fb_info(FBInfo_T *info);

/*
 *  int64_t blit_buffer() prototype
 *
 *  Implementation of syscall 0x17.
 *  Blits a 32bpp (0x00RRGGBB) pixel buffer of fb.width × fb.height pixels
 *  to the VESA framebuffer.  One call per frame.
 */
int64_t blit_buffer(const uint32_t *pixels);
int64_t blit_buffer_scaled(const uint32_t *pixels, uint32_t src_w, uint32_t src_h);

/*
 *  int64_t get_kernel_font() prototype
 *
 *  Implementation of syscall 0x18.
 *  Copies the kernel's embedded PSF1 glyph data into buf (capacity buf_size bytes).
 *  Returns the char_size (bytes per glyph = font height in rows), or 0 on error.
 *  Glyph n occupies bytes [n*char_size .. (n+1)*char_size]; each row is one byte,
 *  MSB = leftmost pixel (8 px wide).
 */
int64_t get_kernel_font(uint8_t *buf, uint64_t buf_size);

/*
 *  int64_t set_video_mode() prototype
 *
 *  Implementation of syscall 0x15.
 *  Programs VGA hardware registers for the given mode:
 *    0x03 — 80×25 color text  (restores kernel shell)
 *    0x0D — 320×200 16-color planar  (EGA-style, 4 planes at VRAM)
 *    0x12 — 640×480 16-color planar
 *    0x13 — 320×200 256-color unchained (classic mode 13h)
 *  Returns 0 on success, non-zero for an unrecognised mode.
 */
int64_t set_video_mode(uint8_t mode);

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
 *  void *malloc() prototype
 *
 *  Implementation of syscall 0x0A.
 *  Allocates <size> bytes from the userland heap (0xC00_000–0xFFF_FFF).
 *  Returns a pointer to zeroed memory, or NULL on failure.
 */
void *malloc(uint64_t size);

/*
 *  void *realloc() prototype
 *
 *  Implementation of syscall 0x0B.
 *  Resizes the block at <ptr> to <size> bytes.  ptr==NULL behaves like malloc.
 *  size==0 frees the block and returns NULL.
 */
void *realloc(void *ptr, uint64_t size);

/*
 *  void free() prototype
 *
 *  Implementation of syscall 0x0F.
 *  Frees a block previously returned by malloc or realloc.
 */
void free(void *ptr);

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
 *  Creates a subdirectory named <name> inside <parent_path> (absolute VFS path).
 */
int64_t write_subdir(const uint8_t *parent_path, const uint8_t *name);

/*
 *  int64_t chdir() prototype
 *
 *  Implementation of syscall 0x2E.
 *  Changes the kernel SYSTEM_CONFIG working directory to <path> (absolute VFS path).
 *  Returns 0 on success, non-zero on error.
 */
int64_t chdir(const uint8_t *path);

/*
 *  int64_t list_dir() prototype
 *
 *  Implementation of syscall 0x28.
 */
int64_t list_dir(int64_t cluster, Entry_T entries[32]);

/*
 *  type TaskInfo_T structure
 *
 *  One entry returned by list_tasks() / ScListTasks (0x2F).
 *  20 bytes: id(1) mode(1) status(1) _pad(1) name(16).
 *  mode:   0=Kernel  1=User
 *  status: 0=Ready 1=Running 2=Idle 3=Blocked 4=Crashed 5=Dead
 */
typedef struct {
    uint8_t id;
    uint8_t mode;
    uint8_t status;
    uint8_t _pad;
    uint8_t name[16];
} __attribute__((packed)) TaskInfo_T;

/*
 *  int64_t list_tasks() prototype
 *
 *  Implementation of syscall 0x2F.
 *  Fills buf with up to <max> TaskInfo_T entries (max 10).
 *  Returns the number of entries written.
 */
int64_t list_tasks(TaskInfo_T *buf, uint8_t max);

/*
 *  int64_t run_elf() prototype
 *
 *  Implementation of syscall 0x2A.
 *  Launches <name> as a background task.  <args> is a space-delimited string
 *  whose first token becomes argv[0] (pass NULL to use <name> as argv[0]).
 *  On success writes the new PID to *pid (if non-NULL) and returns 1.
 */
int64_t run_elf(const uint8_t *name, const uint8_t *args, uint8_t *pid);

/*
 *  int64_t run_fs_check() prototype
 *
 *  Implementation of syscall 0x2B.
 */
int64_t run_fs_check(FsckReport_T *report);

/*
 *  int64_t list_mounts() prototype
 *
 *  Implementation of syscall 0x2C.
 *  Fills buf with up to 8 MountInfo_T entries; returns the count of mounts.
 */
int64_t list_mounts(MountInfo_T *buf);

/*
 *  int64_t list_dir_path() prototype
 *
 *  Implementation of syscall 0x2D.
 *  Lists the directory at the given absolute VFS path (FAT12 or ISO9660).
 *  Fills buf with up to 32 VfsDirEntry_T entries (38 bytes each).
 *  Returns the number of entries written, or a negative error code.
 */
int64_t list_dir_path(const uint8_t *path, VfsDirEntry_T buf[32]);

/*
 *  int64_t write_port() prototype
 *
 *  Implementation of syscall 0x30.
 *  Kernel ABI: arg1=&port (u16), arg2=&value (u32).  Supports full 16-bit
 *  port space (0x0000–0xFFFF), e.g. VGA registers at 0x3C0–0x3DF.
 */
int64_t write_port(uint16_t port, uint32_t value);

/*
 *  int64_t read_port() prototype
 *
 *  Implementation of syscall 0x31.
 */
int64_t read_port(uint16_t port, uint32_t *value);

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
 *  Implementation of syscall 0x37 (arg1 = 0). Registers the calling process as the
 *  global userland Ethernet driver. The kernel will deliver all Ethernet frames that
 *  are not claimed by a port-specific binding to this process, and initialise the RTL8139.
 */
int64_t net_register(void);

/*
 *  int64_t net_bind_port() prototype
 *
 *  Implementation of syscall 0x37 (arg1 = port). Registers the calling process to
 *  receive only TCP frames whose destination port matches <port>. The global Ethernet
 *  driver (net_register) must already be running to handle ARP and ICMP.
 */
int64_t net_bind_port(uint16_t port);

/*
 *  int64_t get_net_status() prototype
 *
 *  Implementation of syscall 0x38.
 *  Fills *ns with the current network status (MAC, IP, driver active, port table).
 *  Returns 0 on success, negative on error.
 */
int64_t get_net_status(NetStatus_T *ns);

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
