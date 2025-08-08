#[unsafe(no_mangle)]
pub fn syscall(num: u64, arg1: u64, arg2: u64) -> u64 {
    let ret: u64;
    unsafe {
        core::arch::asm!(
            "mov rdi, {0:r}",
            "mov rsi, {1:r}",
            "mov rax, {2:r}",
            "int 0x7f",
            in(reg) arg1,
            in(reg) arg2,
            in(reg) num,
            lateout("rax") ret,
            options(nostack),
        );
    }
    ret
}

pub fn exit(code: u64) -> ! {
    syscall(0x00, code, 0);

    loop {}
}

/*
 *
 *  SysInfo (0x01)
 *
 */

#[repr(C, packed)]
#[derive(Default)]
pub struct SysInfo {
    pub system_name: [u8; 32],
    pub system_user: [u8; 32],
    pub system_version: [u8; 8],
    pub system_uptime: u32,
}

pub fn sysinfo() -> Option<SysInfo> {
    let mut sysinfo: SysInfo = Default::default();

    let ret = syscall(0x01, 0x01, &mut sysinfo as *mut SysInfo as u64);

    if ret == 0 {
        return Some(sysinfo);
    }
    None
}

/*
 *
 *  Print to console (0x10)
 *
 */

pub fn print(input: &[u8]) -> u64 {
    syscall(0x10, input.as_ptr() as u64, input.len() as u64)
}

pub fn clear_screen() {
    syscall(0x11, 0x00, 0x00);
}

/*
 *
 *  File operations (0x20)
 *
 */

#[repr(C, packed)]
#[derive(Default,Copy,Clone)]
pub struct Entry {
    pub name: [u8; 8],
    pub ext: [u8; 3],
    pub attr: u8,
    pub reserved: u8,
    pub create_time_tenths: u8,
    pub create_time: u16,
    pub create_date: u16,
    pub last_access_date: u16,
    pub high_cluster: u16,
    pub write_time: u16,
    pub write_date: u16,
    pub start_cluster: u16,
    pub file_size: u32,
}

pub fn file_list_dir(entries: &mut [Entry; 32]) {
    syscall(0x28, 0x00, entries.as_mut_ptr() as *mut _ as u64);
}

/*
 *
 *  Networking (0x30)
 *
 */

pub fn port_write(port: u16, value: u32) {
    syscall(0x30, &port as *const _ as u64, &value as *const _ as u64);
}

pub fn port_read(port: u16) -> u32 {
    let mut value: u64 = 0;

    syscall(0x31, &port as *const _ as u64, &mut value as *mut _ as u64);

    value as u32
}

/*
 *
 *  Audio operations (0x40)
 *
 */

pub fn audio_freq_play(freq: u64, duration: u64) {
    syscall(0x40, freq, duration);
}

pub fn audio_file_play() {
    syscall(0x41, 0x01, 0x00);
}
