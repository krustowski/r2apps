//
// netmux — one process, two network stacks, one frame queue.
//
// The kernel queues a process's network frames in one queue (syscall 0x35).
// Memento runs two TCP/IP stacks: the web browser's (web/net_r2.cpp) and
// c/libcr2's, which the Chat and IRC windows use.  If both read the queue,
// each takes frames that were the other's and throws them away --- a page
// that loads while IRC is connected loses its segments, and IRC its lines.
//
// So neither reads it.  Both ask here: the queue is read in one place, each
// frame goes to the stack that owns its destination port (the browser claims
// its ports with netmux_claim()), and a frame for the stack that is not asking
// just now waits for it in a small queue of its own.  ARP goes to both, since
// both keep a cache of addresses; ping to one, so it is answered once.
//
// A third client, the Network window's host scan (nsk.cpp, after c/nsk), is
// no stack: it takes the replies to its own probes --- echo replies with its
// ICMP id --- and a copy of every ARP frame while it is asking, and nothing
// else.
//
#pragma once

#include <r2/types.hpp>

enum NetmuxClient
{
    NETMUX_WEB = 0,
    NETMUX_CR2 = 1, // c/libcr2: Chat, IRC
    NETMUX_NSK = 2, // the host scan in the Network window
    NETMUX_CLIENTS = 3,
};

//  The ICMP echo id of the host scan's probes: echo replies carrying it are
//  NETMUX_NSK's.  'NS', as c/nsk has it.
#define NETMUX_NSK_ICMP_ID 0x4e53

//  One frame for `who`: copied into `buf` (at most `cap` bytes), its length
//  returned, or 0 when there is none just now.
extern "C" int64_t netmux_pull(int who, uint8_t *buf, uint32_t cap);

//  TCP and UDP destination ports lo..hi (inclusive) are `who`'s.  Everything
//  not claimed is c/libcr2's.
extern "C" void netmux_claim(int who, uint16_t lo, uint16_t hi);

//  Frames each stack got, and frames dropped because its queue here was full
//  or it had stopped asking --- for about:net.
struct NetmuxStats
{
    uint64_t frames[NETMUX_CLIENTS];
    uint64_t dropped[NETMUX_CLIENTS];
};
extern "C" NetmuxStats netmux_stats();

//  The global Ethernet driver registration (syscall 0x37) is the process's,
//  not a stack's: the kernel says only that somebody holds it.  Whoever takes
//  it here says so, so that the others know it was this process.
//  netmux_take_driver() registers when nobody holds it and answers whether
//  this process is the driver now.
extern "C" bool netmux_take_driver();
extern "C" bool netmux_is_driver();

//  Hands c/libcr2 its frames through the above (net_set_frame_source).
void netmux_install_cr2();
