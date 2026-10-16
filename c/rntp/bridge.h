#ifndef R2_RNTP_BRIDGE_H
#define R2_RNTP_BRIDGE_H
#include "types.h"
#include "syscall.h"

#define NETDISK_ABI 0x52324e44ULL
#define NETDISK_MAX (64ULL << 20)
typedef struct {
    uint64_t sectors, generation;
    uint8_t boot[512];
} NetDiskAttach;
typedef struct {
    NetDiskAttach attach;
    uint64_t free_bytes;
} NetDiskAttachInfo;
typedef struct {
    uint64_t generation, free_bytes;
} NetDiskUsageUpdate;
typedef struct {
    uint64_t id, lba;
    uint32_t op, status;
    uint8_t data[512];
} NetDiskRequest;
typedef struct { uint64_t lba; uint8_t data[512]; } NetDiskWrite;
typedef struct {
    uint64_t id;
    uint32_t count, reserved;
    NetDiskWrite writes[32];
} NetDiskBatch;
typedef struct { uint64_t id; uint32_t count, status; } NetDiskBatchReply;
typedef struct {
    uint64_t id, lba;
    uint32_t count, status;
    uint8_t data[32][512];
} NetDiskReadAheadReply;
_Static_assert(sizeof(NetDiskReadAheadReply) == 16408, "netdisk read-ahead completion ABI");
_Static_assert(sizeof(NetDiskBatch) == 16656, "netdisk batch ABI");
_Static_assert(sizeof(NetDiskBatchReply) == 16, "netdisk batch completion ABI");
_Static_assert(sizeof(NetDiskAttach) == 528, "netdisk attach ABI");
_Static_assert(sizeof(NetDiskAttachInfo) == 536, "netdisk attach with usage ABI");
_Static_assert(sizeof(NetDiskUsageUpdate) == 16, "netdisk usage update ABI");
_Static_assert(sizeof(NetDiskRequest) == 536, "netdisk request ABI");
static inline uint64_t netdisk_call(uint64_t op, void *buffer, uint64_t length) {
    return (uint64_t)syscall((SyscallNo_T)0x47, op, (int64_t)buffer, length);
}
#endif
