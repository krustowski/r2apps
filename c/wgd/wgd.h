#ifndef WGD_H
#define WGD_H
#include "wireguard.h"
#include "tunnel_abi.h"

#define WGD_MTU 1420
#define WGD_DATAGRAM_MAX 1472
#define WGD_CONFIG_MAX 2048

struct wgd_config {
    uint8_t private_key[32], public_key[32], preshared_key[32];
    uint8_t address[4], endpoint[4], address_prefix, route_count;
    struct wgd_route routes[WGD_ROUTES_MAX];
    uint16_t listen_port, endpoint_port, keepalive;
};

struct wgd_io {
    bool (*send)(const uint8_t ip[4], uint16_t port, const uint8_t *data, size_t n);
    bool (*inject)(const uint8_t *ip, size_t n);
    void (*event)(const char *event);
};

struct wgd {
    struct wireguard_device device;
    struct wgd_config config;
    struct wgd_io io;
    uint8_t tx[WGD_DATAGRAM_MAX], plain[WGD_MTU + 16];
    uint8_t pending[WGD_MTU];
    size_t pending_len;
    uint32_t handshakes, rx_packets, tx_packets, drops, replays;
    uint32_t rate_at, rate_count, timer_at, last_data_rx;
    bool established, need_keepalive;
};

bool wgd_config_parse(struct wgd_config *cfg, char *text, const char **error);
bool wgd_allowed(const struct wgd_config *cfg, const uint8_t ip[4]);
bool wgd_init(struct wgd *wg, const struct wgd_config *cfg, struct wgd_io io);
void wgd_receive(struct wgd *wg, const uint8_t ip[4], uint16_t port,
                 const uint8_t *data, size_t n);
bool wgd_send_ip(struct wgd *wg, const uint8_t *ip, size_t n);
void wgd_tick(struct wgd *wg);
void wgd_close(struct wgd *wg);
uint16_t wgd_checksum(const uint8_t *p, size_t n);
bool wgd_ipv4(const uint8_t *p, size_t n, size_t *total, size_t *ihl);

#endif
