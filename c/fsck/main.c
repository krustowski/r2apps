#include "printf.h"
#include "syscall.h"
#include "types.h"

/*
 *  fsck
 *
 *  FAT12 filesystem checker for rou2exOS.
 *  Invokes kernel syscall 0x2B which runs check::run_check() and fills
 *  the report struct with error counts.
 *
 *  krusty@vxn.dev / Apr 19, 2026
 */

typedef struct {
    uint64_t errors;
    uint64_t orphan_clusters;
    uint64_t cross_linked;
    uint64_t invalid_entries;
} FsckReport_T;

int main(void) {
    FsckReport_T report;

    print((const uint8_t *)"fsck: checking FAT12 filesystem...\n");

    syscall(0x2B, 0, (int64_t)&report, 0);

    printf((const uint8_t *)"  errors:          %u\n", (uint32_t)report.errors);
    printf((const uint8_t *)"  orphan clusters: %u\n", (uint32_t)report.orphan_clusters);
    printf((const uint8_t *)"  cross-linked:    %u\n", (uint32_t)report.cross_linked);
    printf((const uint8_t *)"  invalid entries: %u\n", (uint32_t)report.invalid_entries);

    if (report.errors == 0 && report.orphan_clusters == 0 && report.cross_linked == 0 && report.invalid_entries == 0) {
        print((const uint8_t *)"fsck: filesystem is clean.\n");
    } else {
        print((const uint8_t *)"fsck: filesystem has issues.\n");
    }

    return 0;
}
