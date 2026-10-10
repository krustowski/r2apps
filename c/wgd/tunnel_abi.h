#ifndef WGD_TUNNEL_ABI_H
#define WGD_TUNNEL_ABI_H
#ifndef _R2_TYPES_INCLUDED_
#include <stdint.h>
#endif
#define WGD_SYSCALL 0x44
#define WGD_CAPABILITY 0x52325748u /* ABI 2: IPv4 prefixes and ICMP probes */
#define WGD_ROUTES_MAX 16
struct wgd_route { uint8_t network[4], prefix, reserved[3]; };
struct wgd_registration {
    uint8_t local[4];
    uint16_t port, mtu;
    uint8_t count, reserved[3];
    struct wgd_route routes[WGD_ROUTES_MAX];
};
struct wgd_probe_registration { uint8_t target[4], local[4]; uint16_t id, reserved; };
#endif
