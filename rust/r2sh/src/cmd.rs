use crate::abi::{self, print};

const KERNEL_VERSION: &[u8] = b"0.8.1";

struct Command {
    name: &'static [u8],
    description: &'static [u8],
    function: fn(args: &[u8]),
    hidden: bool,
}

static COMMANDS: &[Command] = &[
    Command {
        name: b"beep",
        description: b"beeps",
        function: cmd_beep,
        hidden: false,
    },
    Command {
        name: b"cd",
        description: b"changes the current directory",
        function: cmd_cd,
        hidden: false,
    },
    Command {
        name: b"cls",
        description: b"clears the screen",
        function: cmd_clear,
        hidden: false,
    },
    Command {
        name: b"debug",
        description: b"dumps the debug log into a file",
        function: cmd_debug,
        hidden: false,
    },
    Command {
        name: b"dir",
        description: b"lists the current directory",
        function: cmd_dir,
        hidden: false,
    },
    Command {
        name: b"echo",
        description: b"echos the arguments",
        function: cmd_echo,
        hidden: false,
    },
    Command {
        name: b"ed",
        description: b"runs a minimalistic text editor",
        function: cmd_ed,
        hidden: false,
    },
    Command {
        name: b"fsck",
        description: b"runs the filesystem check",
        function: cmd_fsck,
        hidden: false,
    },
    Command {
        name: b"help",
        description: b"shows this output",
        function: cmd_help,
        hidden: false,
    },
    Command {
        name: b"mkdir",
        description: b"creates a subdirectory",
        function: cmd_mkdir,
        hidden: false,
    },
    Command {
        name: b"mv",
        description: b"renames a file",
        function: cmd_mv,
        hidden: false,
    },
    Command {
        name: b"read",
        description: b"prints the output of a file",
        function: cmd_read,
        hidden: false,
    },
    Command {
        name: b"rm",
        description: b"removes a file",
        function: cmd_rm,
        hidden: false,
    },
    Command {
        name: b"run",
        description: b"loads the binary executable in memory and gives it the control",
        function: cmd_run,
        hidden: false,
    },
    Command {
        name: b"shutdown",
        description: b"shuts down the system",
        function: cmd_shutdown,
        hidden: false,
    },
    Command {
        name: b"snake",
        description: b"runs a simple VGA text mode snake-like game",
        function: cmd_snake,
        hidden: false,
    },
    Command {
        name: b"task",
        description: b"starts a simple task scheduler",
        function: cmd_task,
        hidden: true,
    },
    Command {
        name: b"tasks",
        description: b"lists currently running tasks",
        function: cmd_tasks,
        hidden: true,
    },
    Command {
        name: b"time",
        description: b"prints system time and date",
        function: cmd_time,
        hidden: false,
    },
    Command {
        name: b"version",
        description: b"prints the kernel version",
        function: cmd_version,
        hidden: false,
    },
    Command {
        name: b"write",
        description: b"writes arguments to a sample file on floppy",
        function: cmd_write,
        hidden: false,
    }
];

/// Handle takes in an input from keyboard and tries to match it to a defined Command to execute it
/// with given arguments.
pub fn handle(input: &[u8]) {
    let (cmd_name, cmd_args) = split_cmd(input);

    match find_cmd(cmd_name) {
        Some(cmd) => {
            // Call the command function
            (cmd.function)(cmd_args);
        }
        None => {
            if input.is_empty() {
                return;
            }

            // Echo back the input
            print(b"Unknown command: ");
            print(cmd_name);
            print(b"\n");
        }
    }
}

//
//  HELPER FUNCTIONS
//

#[allow(clippy::manual_find)]
/// Loops over the slice of defined commands and returns an Option of matching command via its
/// name, or None otherwise.
fn find_cmd(name: &[u8]) -> Option<&'static Command> {
    for cmd in COMMANDS {
        if cmd.name == name {
            return Some(cmd);
        }
    }
    None
}

/// Splits the provided `input` in to tokens, where the delimitor is a single whitespace (space).
pub fn split_cmd(input: &[u8]) -> (&[u8], &[u8]) {
    // Find the first space
    if let Some(pos) = input.iter().position(|&c| c == b' ') {
        let (cmd, args) = input.split_at(pos);
        // Skip the space character for args
        let args_slice = args.get(1..).unwrap_or(&[]);
        (cmd, args_slice)
    } else {
        // No space found, entire input is the command
        (input, &[])
    }
}

/// Defines the maximum amount of IPv4 addresses that could be parsed from an input.
const MAX_IPS: usize = 4;

/// This function takes in an input (&[u8]) of various length, and parses it into IPv4 addresses
/// (up to MAX_IPS). Returns the parsed count of addresses.
fn parse_ip_args(input: &[u8], out: &mut [[u8; 4]; MAX_IPS]) -> usize {
    let mut ip_count = 0;
    let mut i = 0;
    let len = input.len();

    while i < len && ip_count < MAX_IPS {
        let mut ip = [0u8; 4];
        let mut octet = 0;
        let mut val = 0u16;
        let mut digit_seen = false;

        while i < len {
            match input[i] {
                b'0'..=b'9' => {
                    val = val * 10 + (input[i] - b'0') as u16;
                    if val > 255 {
                        break;
                    }
                    digit_seen = true;
                }
                b'.' => {
                    if !digit_seen || octet >= 3 {
                        break;
                    }
                    ip[octet] = val as u8;
                    octet += 1;
                    val = 0;
                    digit_seen = false;
                }
                b' ' => {
                    i += 1;
                    break;
                }
                _ => {
                    break;
                }
            }
            i += 1;
        }

        if digit_seen && octet == 3 {
            ip[3] = val as u8;
            out[ip_count] = ip;
            ip_count += 1;
        }

        while i < len && input[i] == b' ' {
            i += 1;
        }
    }

    ip_count
}

/// Used to make the FAT12-formatted filename into UPPERCASE.
pub fn to_uppercase_ascii(input: &mut [u8; 11]) {
    for byte in input.iter_mut() {
        if *byte >= b'a' && *byte <= b'z' {
            *byte -= 32;
        }
    }
}

//
//  COMMAND FUNCTIONS
//

/// Used to test the sound module, plays the mystery melody.
fn cmd_beep(_args: &[u8]) {
    //audio::midi::play_melody();
    //audio::beep::stop_beep();
    abi::audio_freq_play(144, 5000);
}

/// Changes the current directory to one matching an input from keyboard.
fn cmd_cd(args: &[u8]) {
    // 12 = name + extension + dot
    /*if args.len() == 0 || args.len() > 12 {
        unsafe {
            config::PATH_CLUSTER = 0;
            config::set_path(b"/");
        }
        return;
    }

    // This split_cmd invocation trims the b'\0' tail from the input args.
    let (filename_input, _) = keyboard::split_cmd(args);

    if filename_input.len() == 0 || filename_input.len() > 12 {
        warn!("Usage: cd <dirname>\n");
        return;
    }

    // 12 = filename + ext + dot
    let mut filename = [b' '; 12];
    if let Some(slice) = filename.get_mut(..filename_input.len()) {
        slice.copy_from_slice(filename_input);
    }

    let floppy = Floppy::init();

    // Init the filesystem to look for a match
    match Filesystem::new(&floppy) {
        Ok(fs) => {
            let mut cluster: u16 = 0;

            unsafe {
                fs.for_each_entry(config::PATH_CLUSTER, |entry| {
                    if entry.name.starts_with(&filename_input) {
                        cluster = entry.start_cluster;
                    }
                });

                if cluster > 0 {
                    config::PATH_CLUSTER = cluster as u16;
                    config::set_path(&filename_input);
                } else {
                    error!("No such directory\n");
                }
            }
        }
        Err(e) => {
            error!(e);
            error!();
        }
    }*/
}

/// This just clears the whole screen with black background color.
fn cmd_clear(_args: &[u8]) {
    abi::clear_screen();
}

/// Dumps the whole debug log to display and tries to write it to the DEBUG.TXT file too if
/// filesystem is reachable.
fn cmd_debug(_args: &[u8]) {
    //debug::dump_debug_log_to_file();
}

/// Prints the whole contents of the current directory.
fn cmd_dir(_args: &[u8]) {
    let mut entries: [abi::Entry; 32] = [Default::default(); 32];

    abi::file_list_dir(&mut entries);

    for entry in entries {
        print(&entry.name);
        print(b"\n");
    }
}

/// Echos the arguments back to the display.
fn cmd_echo(args: &[u8]) {
    print(args);
    print(b"\n");
}

/// Runs a simplistic text editor.
fn cmd_ed(args: &[u8]) {
    /*let (filename_input, _) = keyboard::split_cmd(args);

    if filename_input.len() == 0 || filename_input.len() > 12 {
        warn!("Usage: ed <filename>\n");
        return;
    }

    // Copy the input into a space-padded slice
    let mut filename = [b' '; 12];
    if let Some(slice) = filename.get_mut(..filename_input.len()) {
        slice.copy_from_slice(filename_input);
    }

    //to_uppercase_ascii(&mut filename);

    // Run the editor
    app::editor::edit_file(&filename);
    clear_screen!();*/
}

/// Experimental command function to test the Ethernet implementation.
fn cmd_ether(_args: &[u8]) {
    //app::ether::handle_packet();
}

/// Filesystem check utility.
fn cmd_fsck(_args: &[u8]) {
    //run_check();
}

/// Meta command to dump all non-hidden commands.
fn cmd_help(_args: &[u8]) {
    print(b"List of commands:\n");

    for cmd in COMMANDS {
        /*if cmd.hidden {
            continue;
        }*/

        // Print the command name and description
        print(b" ");
        //print(cmd.name);
        print(b": ");
        //print(cmd.description);
        print(b"\n");
    }
}

/// Creates new subdirectory in the current directory.
fn cmd_mkdir(args: &[u8]) {
    /*if args.len() == 0 || args.len() > 11 {
        warn!("Usage: mkdir <dirname>\n");
        return;
    }

    let floppy = Floppy;

    match Filesystem::new(&floppy) {
        Ok(fs) => {
            let mut filename: [u8; 11] = [b' '; 11];

            if let Some(slice) = filename.get_mut(..) {
                slice[..args.len()].copy_from_slice(args);
            }

            to_uppercase_ascii(&mut filename);
            unsafe {
                fs.create_subdirectory(&filename, PATH_CLUSTER);
            }
        }
        Err(e) => {
            error!(e);
            error!();
        }
    }*/
}

/// Renames given <old_name> to <new_name> in the current directory.
fn cmd_mv(args: &[u8]) {
    /*if args.len() == 0 {
        warn!("Usage: mv <old> <new>");
        return;
    }

    let floppy = Floppy::init();

    match Filesystem::new(&floppy) {
        Ok(fs) => {
            let (old, new) = split_cmd(args);

            let mut old_filename: [u8; 11] = [b' '; 11];
            let mut new_filename: [u8; 11] = [b' '; 11];

            if new.len() == 0 || old.len() == 0 || old.len() > 11 || new.len() > 11 {
                warn!("Usage: mv <old> <new>");
                return;
            }

            if let Some(slice) = old_filename.get_mut(..) {
                slice[..old.len()].copy_from_slice(old);
                slice[8..11].copy_from_slice(b"TXT");
            }

            if let Some(slice) = new_filename.get_mut(..) {
                slice[..new.len()].copy_from_slice(new);
                slice[8..11].copy_from_slice(b"TXT");
            }

            to_uppercase_ascii(&mut old_filename);
            to_uppercase_ascii(&mut new_filename);

            unsafe {
                fs.rename_file(PATH_CLUSTER, &old_filename, &new_filename);
            }
        }
        Err(e) => {
            error!(e);
            error!();
        }
    }*/
}

/// This command function takes the argument, then tries to find a matching filename in the current
/// directory, and finally it dumps its content to screen.
fn cmd_read(args: &[u8]) {
    /*if args.len() == 0 || args.len() > 11 {
        warn!("Usage: read <filename>\n");
        return;
    }

    let floppy = Floppy::init();

    match Filesystem::new(&floppy) {
        Ok(fs) => {
            let mut filename = [b' '; 11];

            filename[..args.len()].copy_from_slice(args);
            filename[8..11].copy_from_slice(b"TXT");

            to_uppercase_ascii(&mut filename);

            unsafe {
                // TODO: tix this
                //let cluster = fs.list_dir(config::PATH_CLUSTER, &filename);
                let cluster = 0;

                if cluster > 0 {
                    let mut buf = [0u8; 512];

                    fs.read_file(cluster as u16, &mut buf);

                    print!("Dumping file raw contents:\n", video::vga::Color::DarkYellow);
                    printb!(&buf);
                    println!();
                } else {
                    error!("No such file found");
                }
            }
        }
        Err(e) => {
            error!(e);
            error!();
        }
    }*/
}

/// Removes a file in the current directory according to the input.
fn cmd_rm(args: &[u8]) {
    /*if args.len() == 0 || args.len() > 11 {
        warn!("Usage: rm <filename>\n");
        return;
    }

    let floppy = Floppy::init();

    match Filesystem::new(&floppy) {
        Ok(fs) => {
            let mut filename: [u8; 11] = [b' '; 11];

            if let Some(slice) = filename.get_mut(..) {
                slice[..args.len()].copy_from_slice(args);
                slice[8..11].copy_from_slice(b"TXT");
            }

            to_uppercase_ascii(&mut filename);

            unsafe {
                fs.delete_file(PATH_CLUSTER, &filename);
            }
        }
        Err(e) => {
            error!(e);
            error!();
        }
    }*/
}

fn cmd_run(args: &[u8]) {
    /*if args.len() == 0 || args.len() > 12 {
        warn!("usage: run <binary name>");
        return;
    }

    // This split_cmd invocation trims the b'\0' tail from the input args.
    let (filename_input, _) = keyboard::split_cmd(args);

    if filename_input.len() == 0 || filename_input.len() > 12 {
        warn!("Usage: run <binary name>\n");
        return;
    }

    // 12 = filename + ext + dot
    let mut filename = [b' '; 12];
    if let Some(slice) = filename.get_mut(..filename_input.len()) {
        slice.copy_from_slice(filename_input);
    }

    let floppy = Floppy::init();

    // Init the filesystem to look for a match
    match Filesystem::new(&floppy) {
        Ok(fs) => {
            unsafe {
                let mut cluster: u16 = 0;
                let mut offset = 0;
                let mut size = 0;

                fs.for_each_entry(config::PATH_CLUSTER, |entry| {
                    if entry.name.starts_with(&filename_input) && entry.ext.starts_with(b"ELF") {
                        cluster = entry.start_cluster;
                        size = entry.file_size;
                        return;
                    }
                });

                rprint!("Size: ");
                rprintn!(size);
                rprint!("\n");

                if cluster == 0 {
                    error!("no such file found");
                    error!();
                    return;
                }

                let load_addr: u64 = 0x690_000;

                while size - offset > 0 {
                    let lba = fs.cluster_to_lba(cluster);
                    let mut sector = [0u8; 512];

                    fs.device.read_sector(lba, &mut sector);

                    let dst = load_addr as *mut u8;

                    //core::ptr::copy_nonoverlapping(sector.as_ptr(), dst.add(offset as usize), 512.min((size - offset) as usize));

                    rprint!("Loading ELF image to memory segment\n");
                    for i in 0..512 {
                        if let Some(byte) = sector.get(i) {
                            *dst.add(i + offset as usize) = *byte;
                        }
                    }

                    cluster = fs.read_fat12_entry(cluster);

                    rprint!("Cluster: ");
                    rprintn!(cluster);
                    rprint!("\n");

                    if cluster >= 0xFF8 || cluster == 0 {
                        break;
                    }

                    //rprint!("offset++\n");
                    offset += 512;
                }

                let arg: u64 = 555;
                //let result: u32;

                //let entry_ptr = (load_addr + 0x18) as *const u64;
                //let entry_addr = *entry_ptr;
                //let entry_addr = core::ptr::read_unaligned((load_addr + 0x18) as *const u64);
                let entry_ptr = (load_addr + 0x18) as *const u8;

                rprint!("First 16 bytes (load_addr + 0x18): ");
                for i in 0..16 {
                    rprintn!(*(entry_ptr as *const u8).add(i));
                    rprint!(" ");
                }
                rprint!("\n");

                /*let entry_addr = u64::from_le_bytes([
                    *entry_ptr.add(0),
                    *entry_ptr.add(1),
                    *entry_ptr.add(2),
                    *entry_ptr.add(3),
                    *entry_ptr.add(4),
                    *entry_ptr.add(5),
                    *entry_ptr.add(6),
                    *entry_ptr.add(7),
                ]);*/

                // assume `elf_image` is a pointer to the loaded ELF file in memory
                let entry_addr = super::elf::load_elf64(load_addr as usize);

                rprint!("ELF entry point: ");
                rprintn!(entry_addr);
                rprint!("\n");

                let stack_top = 0x700000;

                // cast and jump
                let entry_fn: extern "C" fn() -> u64 = core::mem::transmute(entry_addr as *const ());

                rprint!("Jumping to the program entry point...\n");
                //prg_fn();
                //super::elf::jump_to_elf(entry_fn, stack_top);
                super::elf::jump_to_elf(entry_fn, stack_top, arg);

                //let entry: extern "C" fn(u32) -> u32 = core::mem::transmute((load_addr + 0x41) as *mut u8);

                //let result = run_program(entry, arg);
                //let result = entry(arg);
            }
        }
        Err(e) => {
            error!(e);
            error!();
        }
    }*/
}

/// Experimental command function to demonstrate the current state of the shutdown process
/// implemented.
fn cmd_shutdown(_args: &[u8]) {
    /*print!("\n\n --- Shutting down the system", video::vga::Color::DarkCyan);

    // Burn some CPU time
    for _ in 0..3 {
        for _ in 0..3_500_000 {
            unsafe {
                core::arch::asm!("nop");
            }
        }
        printb!(b". ");
    }

    // Invoke the ACPI shutdown attempt (if present)
    acpi::shutdown::shutdown();*/
}

/// Meta command to run the Snake game.
fn cmd_snake(_args: &[u8]) {
    //app::snake::menu::menu_loop();
}

fn cmd_task(_args: &[u8]) {
    //crate::task::task::run_scheduler();
}

fn cmd_tasks(_args: &[u8]) {
    //crate::task::task::status();
}

/// Prints current time and date in UTC as read from RTC in CMOS.
fn cmd_time(_args: &[u8]) {
    /*let (y, mo, d, h, m, s) = time::rtc::read_rtc_full();

    print!("RTC Time: ");

    // Hours
    printn!(h as u64);
    print!(":");

    // Minutes
    if m < 10 { 
        print!("0");
    }
    printn!(m as u64);
    print!(":");

    // Seconds
    if s < 10 { 
        print!("0");
    }
    printn!(s as u64);
    println!();

    print!("RTC Date: ");

    // Day of month
    if d < 10 {
        print!("0");
    }
    printn!(d as u64);
    print!("/");

    // Months
    if mo < 10 {
        print!("0");
    }
    printn!(mo as u64);
    print!("/");

    printn!(y as u64);
    println!();*/
}

/// Prints system information set, mainly version and name.
fn cmd_version(_args: &[u8]) {
    /*print!("Version: ");
    printb!(KERNEL_VERSION);
    println!();*/
}

/// Experimental command function to demonstrate the possibility of writing to files in FAT12 filesystem.
fn cmd_write(args: &[u8]) {
    /*let floppy = Floppy::init();

    match Filesystem::new(&floppy) {
        Ok(fs) => {
            let (filename, content) = split_cmd(args);

            if filename.len() == 0 || content.len() == 0 {
                warn!("Usage <filename> <content>\n");
                return;
            }

            if filename.len() > 8 {
                error!("Filename too long (>8)\n");
                return;
            }

            let mut name = [b' '; 11];

            if let Some(slice) = name.get_mut(..) {
                slice[..filename.len()].copy_from_slice(filename);
                slice[8..11].copy_from_slice(b"TXT");
            }

            to_uppercase_ascii(&mut name);

            unsafe {
                fs.write_file(PATH_CLUSTER, &name, content);
            }
        }
        Err(e) => {
            error!(e);
            error!();
        }
    }*/
}

