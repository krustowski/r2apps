#include "syscall.h"

int64_t syscall(SyscallNo_T number, int64_t arg1, int64_t arg2, int64_t arg3) {
    int64_t ret;
    asm volatile("int $0x7f"
                 : "=a"(ret)
                 /* RDX, RDI, RSI, RCX */
                 : "d"(number), "D"(arg1), "S"(arg2), "c"(arg3)
                 : "r11", "memory");
    return ret;
}

void exit(int64_t pid, int64_t code) {
    syscall(ScExit, pid, code, 0);

    for (;;) {
    }
}

int64_t read_sysinfo(SysInfo_T *sysinfo) {
    if (syscall(ScSysInfo, 0x01, (int64_t)sysinfo, 0)) {
        return 0;
    }

    return 1;
}

int64_t write_sysinfo(const SysInfo_T *sysinfo) {
    if (syscall(ScSysInfo, 0x02, (int64_t)sysinfo, 0)) {
        return 0;
    }

    return 1;
}

int64_t read_rtc(RTC_T *rtc_data) {
    if (syscall(ScRTC, 0x01, (int64_t)rtc_data, 0)) {
        return 0;
    }

    return 1;
}

uint64_t get_ticks(void) {
    return (uint64_t)syscall(ScGetTicks, 0, 0, 0);
}

void sleep_ms(uint64_t ms) {
    syscall(ScSleep, (int64_t)ms, 0, 0);
}

int64_t pipe_subscribe(const uint8_t *buffer) {
    if (syscall(ScPipeSubscribe, 0x01, (int64_t)buffer, 0)) {
        return 0;
    }

    return 1;
}

int64_t pipe_unsubscribe(const uint8_t *buffer) {
    if (syscall(ScPipeSubscribe, 0x02, (int64_t)buffer, 0)) {
        return 0;
    }

    return 1;
}

int64_t pipe_read(uint8_t *buffer) {
    if (syscall(ScPipeSubscribe, 0x03, (int64_t)buffer, 0)) {
        return 0;
    }

    return 1;
}

int64_t print(const uint8_t *str) {
    int64_t len = 0;
    while (str[len])
        ++len;

    if (len > 0) {
        /* Pass len+1 so the kernel slice includes the null terminator;
         * the kernel's b=='\0' guard then fires correctly at the boundary. */
        if (syscall(ScPrintString, (int64_t)str, len + 1, 0)) {
            return 0;
        }
    }

    return len;
}

int64_t clear_screen() {
    if (syscall(ScClearScreen, 0, 0, 0)) {
        return 0;
    }

    return 1;
}

int64_t write_pixel(uint32_t position, uint16_t color) {
    if (syscall(ScWritePixel, (int64_t)position, (int64_t)color, 0)) {
        return 0;
    }

    return 1;
}

int64_t write_vga(const uint8_t *vga_buf, const uint8_t *palette) {
    syscall(ScWriteVGA, (int64_t)vga_buf, (int64_t)palette, 0);
    return 1;
}

uint64_t map_vram(void) {
    uint64_t base = 0;
    syscall(ScMapVram, 0, (int64_t)&base, 0);
    return base;
}

int64_t set_video_mode(uint8_t mode) { return syscall(ScSetVideoMode, (int64_t)mode, 0, 0); }

int64_t get_fb_info(FBInfo_T *info) { return syscall(ScGetFBInfo, (int64_t)info, 0, 0); }

int64_t blit_buffer(const uint32_t *pixels) { return syscall(ScBlitBuffer, (int64_t)pixels, 0, 0); }

int64_t blit_buffer_scaled(const uint32_t *pixels, uint32_t src_w, uint32_t src_h) {
    int64_t dims = ((int64_t)src_w << 16) | (int64_t)src_h;
    return syscall(ScBlitBuffer, (int64_t)pixels, dims, 0);
}

int64_t get_kernel_font(uint8_t *buf, uint64_t buf_size) {
    return syscall(ScGetKernelFont, (int64_t)buf, (int64_t)buf_size, 0);
}

int64_t play_freq(uint16_t freq, uint16_t duration) {
    if (syscall(ScPlayFreq, (int64_t)freq, (int64_t)duration, 0)) {
        return 0;
    }

    return 1;
}

int64_t play_midi_file(const uint8_t *name) {
    int64_t len = 0;
    while (name[len])
        ++len;

    if (len > 0) {
        if (syscall(ScPlayFile, 0x01, (int64_t)name, 0)) {
            return 0;
        }

        return 1;
    }

    return 0;
}

int64_t stop_speaker() {
    if (syscall(ScPlayStop, 0, 0, 0)) {
        return 0;
    }

    return 1;
}

void *malloc(uint64_t size) { return (void *)syscall(ScMalloc, (int64_t)size, 0, 0); }

void *realloc(void *ptr, uint64_t size) {
    return (void *)syscall(ScRealloc, (int64_t)ptr, (int64_t)size, 0);
}

void free(void *ptr) { syscall(ScFree, (int64_t)ptr, 0, 0); }

int64_t read_file(const uint8_t *name, uint8_t *buffer) {
    int64_t len = 0;
    while (name[len])
        ++len;

    if (len > 0) {
        if (syscall(ScReadFile, (int64_t)name, (int64_t)buffer, 0)) {
            return 0;
        }

        return 1;
    }

    return 0;
}

int64_t write_file(const uint8_t *name, const uint8_t *buffer) {
    int64_t len = 0;
    while (name[len])
        ++len;

    if (len > 0) {
        if (syscall(ScWriteFile, (int64_t)name, (int64_t)buffer, 0)) {
            return 0;
        }

        return 1;
    }

    return 0;
}

int64_t rename_file(const uint8_t *old_name, const uint8_t *new_name) {
    uint8_t len_old = 0, len_new = 0;
    while (old_name[len_old])
        ++len_old;
    while (new_name[len_new])
        ++len_new;

    if (len_old > 0 && len_new > 0) {
        if (syscall(ScRenameFile, (int64_t)old_name, (int64_t)new_name, 0)) {
            return 0;
        }

        return 1;
    }

    return 0;
}

int64_t delete_file(const uint8_t *name) {
    uint8_t len = 0;
    while (name[len])
        ++len;

    if (len > 0) {
        if (syscall(ScDeleteFile, (int64_t)name, 0, 0)) {
            return 0;
        }

        return 1;
    }

    return 0;
}

int64_t write_subdir(const uint8_t *parent_path, const uint8_t *name) {
    uint8_t len = 0;
    while (name[len])
        ++len;

    if (len > 0) {
        if (syscall(ScWriteSubdir, (int64_t)parent_path, (int64_t)name, 0)) {
            return 0;
        }

        return 1;
    }

    return 0;
}

int64_t chdir(const uint8_t *path) {
    return syscall(ScChdir, (int64_t)path, 0, 0);
}

int64_t list_dir(int64_t cluster, Entry_T entries[32]) {
    if (syscall(ScListDir, cluster, (int64_t)entries, 0)) {
        return 0;
    }

    return 1;
}

int64_t list_tasks(TaskInfo_T *buf, uint8_t max) {
    return syscall(ScListTasks, (int64_t)buf, (int64_t)max, 0);
}

int64_t run_elf(const uint8_t *name, const uint8_t *args, uint8_t *pid) {
    int64_t r = syscall(ScRunELF, (int64_t)name, (int64_t)args, 0);
    if (r == 0)
        return 0;
    if (pid)
        *pid = (uint8_t)r;
    return 1;
}

int64_t run_fs_check(FsckReport_T *report) {
    if (syscall(ScRunFsCheck, 0, (int64_t)report, 0)) {
        return 0;
    }

    return 1;
}

int64_t list_mounts(MountInfo_T *buf) {
    return syscall(ScListMounts, 0, (int64_t)buf, 0);
}

int64_t list_dir_path(const uint8_t *path, VfsDirEntry_T buf[32]) {
    return syscall(ScListDirPath, (int64_t)path, (int64_t)buf, 0);
}

/* Kernel PORT_WRITE (0x30) ABI: arg1=&port (u16 ptr), arg2=&value (u32 ptr). */
int64_t write_port(uint16_t port, uint32_t value) { return syscall(ScWritePort, (int64_t)&port, (int64_t)&value, 0); }

/* Kernel PORT_READ (0x31) ABI: arg1=&port (u16 ptr), arg2=value ptr (u32). */
int64_t read_port(uint16_t port, uint32_t *value) { return syscall(ScReadPort, (int64_t)&port, (int64_t)value, 0); }

int64_t serial_init() {
    if (syscall(ScSerialPort, 0x01, 0x00, 0)) {
        return 0;
    }

    return 1;
}

int64_t serial_read(uint32_t *value) {
    if (syscall(ScSerialPort, 0x02, (int64_t)value, 0)) {
        return 0;
    }

    return 1;
}

int64_t serial_write(const uint32_t value) {
    if (syscall(ScSerialPort, 0x03, (int64_t)value, 0)) {
        return 0;
    }

    return 1;
}

int64_t new_packet(uint8_t type, uint8_t *buffer) {
    if (syscall(ScNewPacket, (int64_t)type, (int64_t)buffer, 0)) {
        return 0;
    }

    return 1;
}

int64_t send_packet(uint8_t type, uint8_t *buffer) {
    if (syscall(ScSendPacket, (int64_t)type, (int64_t)buffer, 0)) {
        return 0;
    }

    return 1;
}

int64_t receive_data(uint8_t type, uint8_t *buffer) {
    /* Returns the Ethernet frame length on success, 0 while blocked. */
    return syscall(ScReceivePort, (int64_t)type, (int64_t)buffer, 0);
}

int64_t send_data(uint8_t type, uint8_t *buffer) {
    if (syscall(ScSendPort, (int64_t)type, (int64_t)buffer, 0)) {
        return 0;
    }

    return 1;
}

int64_t net_register(void) { return syscall(ScNetRegister, 0, 0, 0); }

int64_t net_bind_port(uint16_t port) { return syscall(ScNetRegister, (int64_t)port, 0, 0); }

int64_t get_net_status(NetStatus_T *ns) { return syscall(ScNetStatus, (int64_t)ns, 0, 0); }

int64_t send_eth_frame(const uint8_t *frame, uint32_t len) {
    (void)len;
    return syscall(ScSendPacket, 0x04, (int64_t)frame, 0);
}
