#include "wgd.h"
#include "crypto.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>

static uint32_t now = 1000;
static uint64_t timestamp = 1;
void wireguard_random_bytes(void *out, size_t n) { assert(getrandom(out, n, 0) == (ssize_t)n); }
uint32_t wireguard_sys_now(void) { return now; }
bool wireguard_is_under_load(void) { return false; }
void wireguard_tai64n_now(uint8_t *out) { U64TO8_BIG(out, timestamp++); U32TO8_BIG(out+8, 0); }

struct packet { int from; uint8_t data[WGD_DATAGRAM_MAX]; size_t n; };
static struct packet packets[128];
static unsigned head, tail, injected, icmp_injected;
static int active;
static struct wgd engines[2];
static uint8_t outer[2][4] = {{192,0,2,1},{192,0,2,2}};
static bool send_packet(const uint8_t ip[4], uint16_t port, const uint8_t *p, size_t n) {
    assert(port == 51820 && !memcmp(ip, outer[1-active], 4)); assert(tail < 128);
    packets[tail].from = active; packets[tail].n = n; memcpy(packets[tail++].data, p, n); return true;
}
static bool inject(const uint8_t *ip, size_t n) { assert(n >= 28); if (ip[9] == 6) ++injected; else { assert(ip[9] == 1); ++icmp_injected; } return true; }
static void event(const char *s) { (void)s; }
static void pump(void) {
    while (head < tail) {
        struct packet p = packets[head++]; active = 1-p.from;
        wgd_receive(&engines[active], outer[p.from], 51820, p.data, p.n);
    }
}
static void hex(const char *s, uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; ++i) { unsigned v; assert(sscanf(s + i*2, "%2x", &v) == 1); out[i] = v; }
}
static void crypto_vectors(void) {
    uint8_t key[32], expected[32], result[32], base[32] = {9};
    hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", key, 32);
    hex("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a", expected, 32);
    wireguard_x25519(result, key, base); assert(!memcmp(result, expected, 32));
    hex("69217a3079908094e11121d042354a7c1f55b6482ca1a51e1b250dfd1ed0eef9", expected, 32);
    wireguard_blake2s(result, 32, NULL, 0, NULL, 0); assert(!memcmp(result, expected, 32));
    /* ChaCha20-Poly1305 vector independently produced with cryptography/OpenSSL. */
    uint8_t plain[114], ad[12], cipher[130], wanted[130], recovered[114];
    hex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f", key, 32);
    hex("50515253c0c1c2c3c4c5c6c7", ad, 12);
    memcpy(plain, "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.", 114);
    hex("663d7ec45b29ceaaa35505b8c1b3d94613a50fd7e315a748d35a378670746af867ab3404fe7b7655b904162b408190f3f8c781815bb8724e4ac22ea6351d38468cd370aa8ffb19e96edc915893cc6e1861c2af01ab0fb02df97ea145499bb87d44ec7d738272327290570a03658b27b116665c21ea189f9450ff121509fd8142befc", wanted, 130);
    wireguard_aead_encrypt(cipher, plain, 114, ad, 12, 0, key);
    assert(!memcmp(cipher, wanted, 130));
    assert(wireguard_aead_decrypt(recovered, cipher, 130, ad, 12, 0, key)); assert(!memcmp(recovered, plain, 114));
    memcpy(wanted, cipher, 130); wanted[129] ^= 1;
    assert(!wireguard_aead_decrypt(recovered, wanted, 130, ad, 12, 0, key));
}
static void configs(void) {
    char valid[] = "[Interface]\nAddress=10.77.0.1/32\nPrivateKey=dwdtCnMYpX08FsFyUbJmRd9ML4frwJkqsXf7pR25LCo=\nListenPort=51820\n[Peer]\nPublicKey=hSDwCYkwp1R0iLfctD73Wg2/Og0mOBr06kqY6qm05mo=\nAllowedIPs=10.77.0.2/32\n";
    struct wgd_config cfg; const char *error;
    assert(wgd_config_parse(&cfg, valid, &error)); assert(!error && cfg.listen_port == 51820);
    char routed[] = "[Interface]\r\nAddress = 10.3.255.253/24\r\nPrivateKey=dwdtCnMYpX08FsFyUbJmRd9ML4frwJkqsXf7pR25LCo=\n[Peer]\nPublicKey=hSDwCYkwp1R0iLfctD73Wg2/Og0mOBr06kqY6qm05mo=\nAllowedIPs = 10.3.255.253/24, 10.4.5.200/25, 10.4.6.68/24, 192.0.2.9/32\nEndpoint=192.0.2.10:34522\nPersistentKeepalive=30\n";
    assert(wgd_config_parse(&cfg, routed, &error));
    assert(cfg.address_prefix == 24 && cfg.address[3] == 253 && cfg.route_count == 4);
    assert(cfg.routes[0].network[3] == 0 && cfg.routes[1].network[3] == 128);
    assert(wgd_allowed(&cfg, (uint8_t[]){10,4,6,68}));
    assert(wgd_allowed(&cfg, (uint8_t[]){10,4,5,128}));
    assert(wgd_allowed(&cfg, (uint8_t[]){10,4,5,255}));
    assert(!wgd_allowed(&cfg, (uint8_t[]){10,4,5,127}));
    assert(!wgd_allowed(&cfg, (uint8_t[]){10,4,7,68}));
    assert(wgd_allowed(&cfg, (uint8_t[]){192,0,2,9}));
    assert(!wgd_allowed(&cfg, (uint8_t[]){192,0,2,10}));
    const char *prefix = "[Interface]\nAddress=10.0.0.1\nPrivateKey=dwdtCnMYpX08FsFyUbJmRd9ML4frwJkqsXf7pR25LCo=\n[Peer]\nPublicKey=hSDwCYkwp1R0iLfctD73Wg2/Og0mOBr06kqY6qm05mo=\nAllowedIPs=";
    char text[2048]; strcpy(text,prefix); strcat(text,"0.0.0.0/0\n");
    assert(wgd_config_parse(&cfg,text,&error));
    assert(wgd_allowed(&cfg,(uint8_t[]){203,0,113,9}));
    const char *bad_routes[] = {"10.0.0.0/33", "::/0", "10.0.0.0/24,", ",10.0.0.0/24", "10.0.0.0/24,,192.0.2.0/24"};
    for(unsigned i=0;i<sizeof(bad_routes)/sizeof(*bad_routes);++i) {
        strcpy(text,prefix); strcat(text,bad_routes[i]);
        assert(!wgd_config_parse(&cfg,text,&error));
    }
    strcpy(text,prefix);
    for(unsigned i=0;i<17;++i) strcat(text,i ? ",10.0.0.0/24":"10.0.0.0/24");
    assert(!wgd_config_parse(&cfg,text,&error));
    const char *bad[] = {
        "[Interface]\nAddress=256.1.1.1\n", "[Interface]\nListenPort=65536\n",
        "[Interface]\nAddress=10.0.0.1/33\n", "[Interface]\nAddress=10.0.0.1\nAddress=10.0.0.2\n",
        "[Interface]\nPrivateKey=AAAA\n", "[Interface]\n[Peer]\n[Peer]\n", "[Interface]\nUnknown=x\n"
    };
    for (unsigned i = 0; i < sizeof(bad)/sizeof(*bad); ++i) {
        char text[256]; strcpy(text, bad[i]); assert(!wgd_config_parse(&cfg, text, &error));
    }
}
static size_t ip_packet(uint8_t *p, unsigned proto, const uint8_t src[4], const uint8_t dst[4]) {
    memset(p, 0, 40); p[0] = 0x45; p[8] = 64; p[9] = proto; U16TO8_BIG(p+2, 40);
    memcpy(p+12, src, 4); memcpy(p+16, dst, 4);
    if (proto == 1) { p[20] = 8; U16TO8_BIG(p+22, wgd_checksum(p+20, 20)); }
    else p[32] = 0x50;
    U16TO8_BIG(p+10, wgd_checksum(p, 20)); return 40;
}
static void setup(void) {
    head = tail = injected = icmp_injected = 0;
    struct wgd_config cfg[2]; memset(cfg, 0, sizeof(cfg));
    uint8_t pub[2][32], base[32] = {9};
    for (unsigned i = 0; i < 2; ++i) {
        memset(cfg[i].private_key, (int)i+1, 32); wireguard_x25519(pub[i], cfg[i].private_key, base);
        cfg[i].address[0] = 10; cfg[i].address[1] = 77; cfg[i].address[3] = (uint8_t)i+1; cfg[i].listen_port = 51820;
    }
    for (unsigned i = 0; i < 2; ++i) {
        memcpy(cfg[i].public_key, pub[1-i], 32);
        cfg[i].route_count = 1; cfg[i].routes[0].prefix = 32;
        memcpy(cfg[i].routes[0].network, cfg[1-i].address, 4);
    }
    memcpy(cfg[0].endpoint, outer[1], 4); cfg[0].endpoint_port = 51820;
    struct wgd_io io = {send_packet, inject, event};
    assert(wgd_init(&engines[0], &cfg[0], io)); assert(wgd_init(&engines[1], &cfg[1], io));
}
static struct packet encrypted(const uint8_t *plain, size_t n) {
    struct packet p = {.from=0}; size_t padded = (n+15)&~(size_t)15; p.n = padded+32;
    memset(p.data, 0, p.n); p.data[0] = 4;
    struct wireguard_keypair *k = &engines[0].device.peers[0].curr_keypair;
    U32TO8_LITTLE(p.data+4, k->remote_index); U64TO8_LITTLE(p.data+8, k->sending_counter);
    if (n) memcpy(p.data+16, plain, n);
    wireguard_encrypt_packet(p.data+16, p.data+16, padded, k); return p;
}
static void receive_b(struct packet p, const uint8_t *ip) { active=1; wgd_receive(&engines[1], ip, 51820, p.data, p.n); }
static void sessions(void) {
    setup(); uint8_t ip[40]; active=0;
    size_t n = ip_packet(ip, 1, engines[0].config.address, engines[1].config.address);
    assert(!wgd_send_ip(&engines[0], ip, n)); pump();
    assert(engines[0].established && engines[1].established && engines[0].handshakes == 1);
    assert(engines[0].rx_packets == 1 && engines[1].rx_packets == 1);
    n = ip_packet(ip, 6, engines[0].config.address, engines[1].config.address);
    active=0; assert(wgd_send_ip(&engines[0], ip, n)); struct packet transport_packet=packets[tail-1]; pump(); assert(injected == 1);
    uint8_t spoof_endpoint[4] = {192,0,2,99};
    receive_b(transport_packet, spoof_endpoint); assert(injected == 1 && engines[1].replays == 1);
    assert(!memcmp(engines[1].device.peers[0].ip.bytes, outer[0], 4));
    struct packet keepalive = encrypted(NULL, 0); receive_b(keepalive, outer[0]);
    uint32_t last_rx=engines[1].device.peers[0].last_rx; now += 1000;
    receive_b(keepalive, spoof_endpoint); assert(engines[1].replays == 2 && engines[1].device.peers[0].last_rx == last_rx);
    ip[12]=11; ip[10]=ip[11]=0; U16TO8_BIG(ip+10, wgd_checksum(ip,20));
    unsigned drops=engines[1].drops; receive_b(encrypted(ip, n), outer[0]); assert(engines[1].drops == drops+1 && injected == 1);
    struct packet bad=encrypted(NULL,0); bad.data[bad.n-1]^=1; receive_b(bad, spoof_endpoint);
    assert(!memcmp(engines[1].device.peers[0].ip.bytes, outer[0], 4));
    for (size_t len=0; len<32; ++len) { uint8_t tiny[32]={4}; active=1; wgd_receive(&engines[1], outer[0],51820,tiny,len); }
    /* Rekey while old keys are still live. */
    now += 121000; n=ip_packet(ip,1,engines[0].config.address,engines[1].config.address);
    active=0; assert(wgd_send_ip(&engines[0],ip,n)); wgd_tick(&engines[0]); pump();
    assert(engines[0].handshakes == 2 && engines[1].handshakes == 2);
    active=0; assert(wgd_send_ip(&engines[0],ip,n)); pump();
    assert(engines[1].device.peers[0].curr_keypair.replay_bitmap != 0);
    now += 181000; active=1; wgd_tick(&engines[1]); assert(!engines[1].device.peers[0].curr_keypair.valid);
    wgd_close(&engines[0]); wgd_close(&engines[1]);
}
static void subnet_packets(void) {
    setup(); uint8_t ip[40]; active=0;
    size_t n=ip_packet(ip,1,engines[0].config.address,engines[1].config.address);
    wgd_send_ip(&engines[0],ip,n); pump();
    struct wgd_config *routes=&engines[1].config;
    routes->routes[1]=(struct wgd_route){{10,4,6,0},24,{0,0,0}}; routes->route_count=2;
    n=ip_packet(ip,6,(uint8_t[]){10,4,6,68},routes->address);
    receive_b(encrypted(ip,n),outer[0]); assert(injected==1);
    n=ip_packet(ip,1,(uint8_t[]){10,4,6,68},routes->address);
    ip[20]=0; ip[22]=ip[23]=0; U16TO8_BIG(ip+22,wgd_checksum(ip+20,20));
    unsigned before=icmp_injected;
    receive_b(encrypted(ip,n),outer[0]); assert(icmp_injected==before+1);
    ip[20]=11; ip[22]=ip[23]=0; U16TO8_BIG(ip+22,wgd_checksum(ip+20,20));
    receive_b(encrypted(ip,n),outer[0]); assert(icmp_injected==before+2);
    n=ip_packet(ip,6,(uint8_t[]){10,4,7,68},routes->address);
    unsigned drops=engines[1].drops;
    receive_b(encrypted(ip,n),outer[0]); assert(engines[1].drops==drops+1 && injected==1);
    routes->routes[0]=(struct wgd_route){{0,0,0,0},0,{0,0,0}}; routes->route_count=1;
    n=ip_packet(ip,6,routes->address,routes->address);
    receive_b(encrypted(ip,n),outer[0]); assert(engines[1].drops==drops+2 && injected==1);
    n=ip_packet(ip,6,(uint8_t[]){127,0,0,1},routes->address);
    receive_b(encrypted(ip,n),outer[0]); assert(engines[1].drops==drops+3 && injected==1);
    wgd_close(&engines[0]); wgd_close(&engines[1]);
}
static void replay_window(void) {
    struct wireguard_keypair k = {0};
    assert(wireguard_check_replay(&k,0)); assert(!wireguard_check_replay(&k,0));
    assert(wireguard_check_replay(&k,10)); assert(wireguard_check_replay(&k,9)); assert(!wireguard_check_replay(&k,9));
    assert(wireguard_check_replay(&k,UINT64_C(1)<<32)); assert(!wireguard_check_replay(&k,10));
    assert(!wireguard_check_replay(&k,UINT64_MAX));
}
int main(int argc, char **argv) {
    if (argc == 2) {
        char text[WGD_CONFIG_MAX+1]; FILE *f = fopen(argv[1], "rb"); assert(f);
        size_t n = fread(text,1,sizeof(text),f); fclose(f); assert(n <= WGD_CONFIG_MAX); text[n]=0;
        struct wgd_config cfg; const char *error;
        if (!wgd_config_parse(&cfg,text,&error)) { puts(error); return 1; }
        printf("PASS: configuration accepted (%u IPv4 prefixes)\n", cfg.route_count);
        crypto_zero(text,sizeof(text)); crypto_zero(&cfg,sizeof(cfg)); return 0;
    }
    crypto_vectors(); configs(); replay_window(); sessions(); subnet_packets();
    now=UINT32_MAX-1000; sessions();
    puts("PASS: crypto vectors, config, handshake, ping, TCP injection, replay, spoof rejection, rekey, expiry, tick wrap");
}
