#include "wgd.h"
#include "crypto.h"
#include <string.h>

uint16_t wgd_checksum(const uint8_t *p, size_t n) {
    uint32_t sum = 0;
    while (n >= 2) { sum += ((uint32_t)p[0] << 8) | p[1]; p += 2; n -= 2; }
    if (n) sum += (uint32_t)*p << 8;
    while (sum >> 16) sum = (sum & 65535) + (sum >> 16);
    return (uint16_t)~sum;
}

bool wgd_ipv4(const uint8_t *p, size_t n, size_t *total, size_t *ihl) {
    if (n < 20 || p[0] >> 4 != 4) return false;
    *ihl = (p[0] & 15) * 4;
    *total = ((size_t)p[2] << 8) | p[3];
    return *ihl >= 20 && *ihl <= n && *total >= *ihl && *total <= n &&
        !(p[6] & 0xbf) && !p[7] && p[8] && wgd_checksum(p, *ihl) == 0;
}

static bool send_to(struct wgd *wg, const uint8_t ip[4], uint16_t port,
                    const void *data, size_t n) {
    return port && wg->io.send(ip, port, data, n);
}
static bool transport(struct wgd *wg, const uint8_t *ip, size_t n) {
    struct wireguard_peer *p = &wg->device.peers[0];
    struct wireguard_keypair *k = &p->curr_keypair;
    if (!k->valid || !k->sending_valid || wireguard_expired(k->keypair_millis, REJECT_AFTER_TIME) ||
        k->sending_counter >= REJECT_AFTER_MESSAGES) return false;
    size_t padded = (n + 15) & ~(size_t)15;
    memset(wg->tx, 0, 16 + padded + 16);
    wg->tx[0] = MESSAGE_TRANSPORT_DATA;
    U32TO8_LITTLE(wg->tx + 4, k->remote_index);
    U64TO8_LITTLE(wg->tx + 8, k->sending_counter);
    if (n) memcpy(wg->tx + 16, ip, n);
    wireguard_encrypt_packet(wg->tx + 16, wg->tx + 16, padded, k);
    bool sent = send_to(wg, p->ip.bytes, p->port, wg->tx, 32 + padded);
    if (sent) { p->last_tx = k->last_tx = wireguard_sys_now(); if (n) ++wg->tx_packets; }
    if (k->sending_counter >= REKEY_AFTER_MESSAGES ||
        (k->initiator && wireguard_expired(k->keypair_millis, REKEY_AFTER_TIME))) p->send_handshake = true;
    return sent;
}
static void initiate(struct wgd *wg) {
    struct wireguard_peer *p = &wg->device.peers[0];
    if (!p->port || (p->last_initiation_tx && !wireguard_expired(p->last_initiation_tx, REKEY_TIMEOUT))) return;
    struct message_handshake_initiation msg;
    if (wireguard_create_handshake_initiation(&wg->device, p, &msg)) {
        /* Rate-limit attempts even when ARP/TX is temporarily unavailable. */
        p->last_initiation_tx = wireguard_sys_now();
        send_to(wg, p->ip.bytes, p->port, &msg, sizeof(msg));
    }
    crypto_zero(&msg, sizeof(msg));
}
static void endpoint(struct wireguard_peer *p, const uint8_t ip[4], uint16_t port) {
    memcpy(p->ip.bytes, ip, 4); p->port = port;
}
static void established(struct wgd *wg) {
    if (!wg->established) { wg->established = true; wg->io.event("session established"); }
    if (wg->pending_len && transport(wg, wg->pending, wg->pending_len)) {
        crypto_zero(wg->pending, wg->pending_len); wg->pending_len = 0;
    }
}

bool wgd_init(struct wgd *wg, const struct wgd_config *cfg, struct wgd_io io) {
    memset(wg, 0, sizeof(*wg)); wg->config = *cfg; wg->io = io;
    wireguard_init();
    if (!wireguard_device_init(&wg->device, cfg->private_key) ||
        !wireguard_peer_init(&wg->device, &wg->device.peers[0], cfg->public_key, cfg->preshared_key)) {
        wgd_close(wg); return false;
    }
    /* The protocol owns the key material; the routing config needs no copies. */
    crypto_zero(wg->config.private_key, sizeof(wg->config.private_key));
    crypto_zero(wg->config.preshared_key, sizeof(wg->config.preshared_key));
    struct wireguard_peer *p = &wg->device.peers[0];
    endpoint(p, cfg->endpoint, cfg->endpoint_port);
    memcpy(p->connect_ip.bytes, cfg->endpoint, 4); p->connect_port = cfg->endpoint_port;
    p->active = cfg->endpoint_port != 0; p->keepalive_interval = cfg->keepalive;
    return true;
}

static bool check_handshake(struct wgd *wg, const uint8_t ip[4], uint16_t port,
                            const uint8_t *data, size_t n) {
    if (!wireguard_check_mac1(&wg->device, data, n - 32, data + n - 32)) return false;
    uint32_t now = wireguard_sys_now();
    if ((uint32_t)(now - wg->rate_at) >= 1000) { wg->rate_at = now; wg->rate_count = 0; }
    uint8_t source[6]; memcpy(source, ip, 4); U16TO8_BIG(source + 4, port);
    /* Cheap MAC verification precedes costly DH; bounded work even with valid cookies. */
    unsigned count = ++wg->rate_count;
    if (count > 20) return false;
    if (count > 4 && !wireguard_check_mac2(&wg->device, data, n - 16, source, sizeof(source), data + n - 16)) {
        struct message_cookie_reply cookie;
        wireguard_create_cookie_reply(&wg->device, &cookie, data + n - 32, U8TO32_LITTLE(data + 4), source, sizeof(source));
        send_to(wg, ip, port, &cookie, sizeof(cookie));
        crypto_zero(&cookie, sizeof(cookie)); return false;
    }
    return true;
}

void wgd_receive(struct wgd *wg, const uint8_t ip[4], uint16_t port,
                 const uint8_t *data, size_t n) {
    if (!port || n > WGD_DATAGRAM_MAX || ip[0] == 0 || ip[0] == 127 || ip[0] >= 224) { ++wg->drops; return; }
    struct wireguard_peer *p = &wg->device.peers[0];
    uint8_t type = wireguard_get_message_type(data, n);
    if (type == MESSAGE_HANDSHAKE_INITIATION) {
        struct message_handshake_initiation msg;
        memcpy(&msg, data, sizeof(msg));
        if (!check_handshake(wg, ip, port, data, n) || wireguard_process_initiation_message(&wg->device, &msg) != p) { ++wg->drops; return; }
        struct message_handshake_response response;
        if (wireguard_create_handshake_response(&wg->device, p, &response)) {
            endpoint(p, ip, port); wireguard_start_session(p, false); p->send_handshake = false; ++wg->handshakes;
            wg->io.event("handshake response");
            send_to(wg, ip, port, &response, sizeof(response));
        }
        crypto_zero(&response, sizeof(response));
    } else if (type == MESSAGE_HANDSHAKE_RESPONSE) {
        struct message_handshake_response msg; memcpy(&msg, data, sizeof(msg));
        if (!check_handshake(wg, ip, port, data, n) || !p->handshake.valid || !p->handshake.initiator ||
            msg.receiver != p->handshake.local_index || !wireguard_process_handshake_response(&wg->device, p, &msg)) { ++wg->drops; return; }
        endpoint(p, ip, port); wireguard_start_session(p, true); p->send_handshake = false; ++wg->handshakes;
        transport(wg, NULL, 0); established(wg);
    } else if (type == MESSAGE_COOKIE_REPLY) {
        struct message_cookie_reply msg; memcpy(&msg, data, sizeof(msg));
        if (!p->handshake.valid || msg.receiver != p->handshake.local_index ||
            memcmp(ip, p->ip.bytes, 4) || port != p->port ||
            !wireguard_process_cookie_message(&wg->device, p, &msg)) { ++wg->drops; return; }
        p->send_handshake = true;
    } else if (type == MESSAGE_TRANSPORT_DATA) {
        uint32_t idx = U8TO32_LITTLE(data + 4);
        uint64_t nonce = U8TO64_LITTLE(data + 8);
        size_t plain_len = n - 32;
        struct wireguard_keypair *k = get_peer_keypair_for_idx(p, idx);
        if (!k || !k->receiving_valid || wireguard_expired(k->keypair_millis, REJECT_AFTER_TIME) ||
            nonce >= REJECT_AFTER_MESSAGES || plain_len > sizeof(wg->plain) ||
            !wireguard_decrypt_packet(wg->plain, data + 16, n - 16, nonce, k)) { ++wg->drops; return; }
        /* Replay checks apply to keepalives too, before endpoint/timer/key changes. */
        if (!wireguard_check_replay(k, nonce)) { ++wg->replays; crypto_zero(wg->plain, sizeof(wg->plain)); return; }
        k->last_rx = p->last_rx = wireguard_sys_now();
        endpoint(p, ip, port);
        keypair_update(p, k);
        /* keypair_update may have wiped the object k pointed to. */
        k = get_peer_keypair_for_idx(p, idx);
        if (k->initiator && wireguard_expired(k->keypair_millis, REJECT_AFTER_TIME - KEEPALIVE_TIMEOUT - REKEY_TIMEOUT)) p->send_handshake = true;
        established(wg);
        if (plain_len) {
            size_t total, ihl;
            if (!wgd_ipv4(wg->plain, plain_len, &total, &ihl) || total > WGD_MTU ||
                !wg->plain[12] || wg->plain[12] == 127 || wg->plain[12] >= 224 ||
                !wgd_allowed(&wg->config, wg->plain + 12) || !memcmp(wg->plain + 12, wg->config.address, 4) ||
                memcmp(wg->plain + 16, wg->config.address, 4)) { ++wg->drops; }
            else {
                ++wg->rx_packets; wg->last_data_rx = wireguard_sys_now(); wg->need_keepalive = true;
                if (wg->plain[9] == 1 && total >= ihl + 8 && wg->plain[ihl] == 8 && wg->plain[ihl+1] == 0 &&
                    wgd_checksum(wg->plain + ihl, total - ihl) == 0) {
                    uint8_t remote[4]; memcpy(remote, wg->plain + 12, 4);
                    memcpy(wg->plain + 12, wg->config.address, 4); memcpy(wg->plain + 16, remote, 4);
                    wg->plain[ihl] = 0; wg->plain[ihl+2] = wg->plain[ihl+3] = 0;
                    uint16_t sum = wgd_checksum(wg->plain + ihl, total - ihl);
                    U16TO8_BIG(wg->plain + ihl + 2, sum);
                    wg->plain[10] = wg->plain[11] = 0; sum = wgd_checksum(wg->plain, ihl); U16TO8_BIG(wg->plain + 10, sum);
                    transport(wg, wg->plain, total);
                } else if (wg->plain[9] == 1 && total >= ihl + 8 &&
                           (wg->plain[ihl] == 0 || wg->plain[ihl] == 3 || wg->plain[ihl] == 11) &&
                           wgd_checksum(wg->plain + ihl, total - ihl) == 0) {
                    if (!wg->io.inject(wg->plain, total)) ++wg->drops;
                } else if (wg->plain[9] == 6 && total >= ihl + 20 &&
                           (wg->plain[ihl+12] >> 4) >= 5 && (size_t)(wg->plain[ihl+12] >> 4) * 4 <= total - ihl) {
                    if (!wg->io.inject(wg->plain, total)) ++wg->drops;
                } else ++wg->drops;
            }
        }
        crypto_zero(wg->plain, sizeof(wg->plain));
    } else ++wg->drops;
}

bool wgd_send_ip(struct wgd *wg, const uint8_t *ip, size_t n) {
    size_t total, ihl;
    if (!wgd_ipv4(ip, n, &total, &ihl) || total > WGD_MTU ||
        memcmp(ip + 12, wg->config.address, 4) || !memcmp(ip + 16, wg->config.address, 4) ||
        !wgd_allowed(&wg->config, ip + 16)) { ++wg->drops; return false; }
    if (transport(wg, ip, total)) return true;
    if (!wg->pending_len) { memcpy(wg->pending, ip, total); wg->pending_len = total; }
    wg->device.peers[0].send_handshake = true;
    initiate(wg); return false;
}

void wgd_tick(struct wgd *wg) {
    uint32_t now = wireguard_sys_now();
    if ((uint32_t)(now - wg->timer_at) < 100) return;
    wg->timer_at = now;
    struct wireguard_peer *p = &wg->device.peers[0];
    struct wireguard_keypair *keys[] = {&p->prev_keypair, &p->curr_keypair, &p->next_keypair};
    for (unsigned i = 0; i < 3; ++i)
        if (keys[i]->valid && wireguard_expired(keys[i]->keypair_millis, REJECT_AFTER_TIME)) keypair_destroy(keys[i]);
    if (p->handshake.valid && wireguard_expired(p->last_initiation_tx, REJECT_AFTER_TIME)) crypto_zero(&p->handshake, sizeof(p->handshake));
    if (wg->established && !p->curr_keypair.valid) { wg->established = false; wg->io.event("session expired"); }
    if ((p->keepalive_interval && wireguard_expired(p->last_tx, p->keepalive_interval)) ||
        (wg->need_keepalive && (uint32_t)(now - wg->last_data_rx) >= KEEPALIVE_TIMEOUT * 1000 &&
         (int32_t)(p->last_tx - wg->last_data_rx) < 0)) {
        if (transport(wg, NULL, 0)) wg->need_keepalive = false;
    }
    if (wg->pending_len && p->curr_keypair.valid) established(wg);
    if (p->send_handshake || (p->active && !p->curr_keypair.valid)) initiate(wg);
}

void wgd_close(struct wgd *wg) { crypto_zero(wg, sizeof(*wg)); }
