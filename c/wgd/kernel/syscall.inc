        /* Userspace IPv4 tunnel (one daemon): probe, attach, detach, inject.
         * Attach reads Config (140 bytes): local IP, port, MTU and up to 16 prefixes.
         * Inject reads a complete Ethernet/IPv4 TCP or ICMP frame; RCX supplies its length.
         * Ops 4/5 reserve/release an ICMP probe for the calling process.
         * Process exit/crash releases the registration in netdrv::release_process.
         */
        0x44 => {
            let pid = unsafe { scheduler::get_current_pid() };
            return match arg1 {
                0 if arg2 == 0 && arg3 == 0 => crate::net::tunnel::CAPABILITY,
                1 if arg3 == 0 && user_buf_ok(arg2, core::mem::size_of::<crate::net::tunnel::Config>() as u64) => {
                    let cfg = unsafe { core::ptr::read_unaligned(arg2 as *const crate::net::tunnel::Config) };
                    if x86_64::instructions::interrupts::without_interrupts(|| unsafe { crate::net::tunnel::attach(pid, cfg) }) {
                        SyscallReturnCode::Ok as u64
                    } else { SyscallReturnCode::Busy as u64 }
                }
                2 if arg2 == 0 && arg3 == 0 => {
                    if x86_64::instructions::interrupts::without_interrupts(|| unsafe { crate::net::tunnel::detach(pid) }) {
                        SyscallReturnCode::Ok as u64
                    } else { SyscallReturnCode::Busy as u64 }
                }
                3 if (34..=1434).contains(&arg3) && user_buf_ok(arg2, arg3) => {
                    let frame = unsafe { core::slice::from_raw_parts(arg2 as *const u8, arg3 as usize) };
                    if x86_64::instructions::interrupts::without_interrupts(|| unsafe { crate::net::tunnel::inject(pid, frame) }) {
                        SyscallReturnCode::Ok as u64
                    } else { SyscallReturnCode::Busy as u64 }
                }
                4 if arg3 == 0 && user_buf_ok(arg2, 12) => {
                    let target = unsafe { core::ptr::read_unaligned(arg2 as *const [u8; 4]) };
                    let probe = x86_64::instructions::interrupts::without_interrupts(|| unsafe { crate::net::tunnel::claim_probe(pid, target) });
                    if let Some(probe) = probe {
                        unsafe { core::ptr::write_unaligned(arg2 as *mut crate::net::tunnel::Probe, probe); }
                        SyscallReturnCode::Ok as u64
                    } else { SyscallReturnCode::Busy as u64 }
                }
                5 if arg2 == 0 && arg3 == 0 => {
                    if x86_64::instructions::interrupts::without_interrupts(|| unsafe { crate::net::tunnel::release_probe(pid) }) {
                        SyscallReturnCode::Ok as u64
                    } else { SyscallReturnCode::Busy as u64 }
                }
                _ => SyscallReturnCode::InvalidInput as u64,
            };
        }

