#include "printf.h"

int main(void) {
    const uint8_t *filename = "CTEST.TXT";
    const uint8_t *wbuffer = "Written by C using the rou2exOS ABI\n";

    uint8_t rbuffer[512];
    uint8_t elf_pid = 0;

    SysInfo_T sysinfo;
    RTC_T rtc_data;
    Entry_T entries[32];

    /* Test printing to standard output (console, syscall 0x10) */
    print("*** Hello from C\n");

    printf((const uint8_t *)"Hello %s %d %x %c %%!\n", "world", 123, 0xAB, 'A');

    /* Test System Information gathering and handling (syscall 0x01) */
    if (read_sysinfo(&sysinfo)) {
        print("*** Reading system information\n");

        printf("System rou2exOS %s\n", sysinfo.system_version);
        printf("[%s@%s:%s] > ...\n", sysinfo.system_user, sysinfo.system_name, sysinfo.system_path);
    }

    /* Test RTC clock read (syscall 0x02) */
    if (read_rtc(&rtc_data)) {
        printf("System time: %d:%d:%d %d-%d-%d\n", rtc_data.hours, rtc_data.minutes, rtc_data.seconds, rtc_data.year, rtc_data.month, rtc_data.day);
    }

    /* Test writing to a file (syscall 0x21) */
    if (write_file(filename, wbuffer)) {
        print("*** Written to a file successfully\n");
    }

    /* Test reading such file back (syscall 0x20) */
    if (read_file(filename, rbuffer)) {
        print("*** Reading the file contents\n");
        print(rbuffer);
    }

    /* Test writing/creating new subdirectory (syscall 0x27) */
    if (write_subdir(0, (const uint8_t *)"TESTDIR")) {
        print("*** Created a new subdirectory\n");
    }

    /* Test entry renaming (syscall 0x22) */
    if (rename_file((const uint8_t *)"TESTDIR", (const uint8_t *)"NEWDIR")) {
        print("*** Subdirectory renamed successfully\n");
    }

    /* Test entry deletion (syscall 0x23) */
    if (delete_file(filename)) {
        print("*** Text file deleted successfully\n");
    }

    /* Test listing a directory (syscall 0x28) */
    if (list_dir(0, entries)) {
        print("*** Listing current directory\n");

        for (uint8_t i = 0; i < 32; i++) {
            if (entries[i].name[0] == 0x00) {
                continue;
            }

            print(" ");

            if (entries[i].attr & 0x10) {
                entries[i].attr = 0x00;
                printf(" %s   <DIR>\n", entries[i].name);
                continue;
            } else {
                entries[i].attr = 0x00;
                // print( entries[i].name );
            }

            printf(" %s   %d bytes\n", (const uint8_t *)(entries[i].name), entries[i].file_size);
        }
    }

    /* Test running an ELF executable (syscall 0x2A) */
    print("*** Running external ELF executable...\n");

    if (run_elf((const uint8_t *)"PRINT.ELF", &elf_pid)) {
        printf("*** External ELF executed successfully (PID: %d)\n", elf_pid);
    }

    print("*** Exit\n");

    return 0;
}
