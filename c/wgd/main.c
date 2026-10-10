#include "platform.h"
#include "crypto.h"
#include "entropy.h"
#include <string.h>

static struct wgd wg;
static struct wgd_config config;
static char text[WGD_CONFIG_MAX + 1];
int main(int argc, char **argv) {
    wgd_event("starting");
    bool check = argc > 1 && !strcmp(argv[1], "--check");
    unsigned index = check ? 2 : 1;
    if (argc > (int)index + 1) { wgd_event("usage: wgd [--check] [config path]"); return 1; }
    int n;
    if (argc > (int)index) n = wgd_read_config(argv[index], text, sizeof(text));
    else {
        n = wgd_read_config("WGD.CFG", text, sizeof(text));
        if (n < 0) n = wgd_read_config("/mnt/fat/WGD.CFG", text, sizeof(text));
        if (n < 0) n = wgd_read_config("/mnt/tar/opt/wgd/wgd.cfg", text, sizeof(text));
    }
    if (n < 0 || n > WGD_CONFIG_MAX) { wgd_event("configuration missing or exceeds 2048 bytes"); return 1; }
    /* Embedded NUL must not silently hide a second peer or invalid settings. */
    for (int i = 0; i < n; ++i) if (!text[i]) { crypto_zero(text, sizeof(text)); wgd_event("configuration contains NUL bytes"); return 1; }
    text[n] = 0;
    const char *error;
    bool ok = wgd_config_parse(&config, text, &error); crypto_zero(text, sizeof(text));
    if (!ok) { crypto_zero(&config, sizeof(config)); wgd_event(error); return 1; }
    if (check) { crypto_zero(&config, sizeof(config)); wgd_event("configuration valid"); return 0; }
    if (!wgd_platform_start(&config, &error)) { crypto_zero(&config, sizeof(config)); wgd_entropy_stop(); wgd_event(error); return 1; }
    struct wgd_io io = {wgd_platform_send, wgd_platform_inject, wgd_event};
    if (!wgd_init(&wg, &config, io)) { crypto_zero(&config, sizeof(config)); wgd_entropy_stop(); wgd_event("invalid key or peer initialization failed"); return 1; }
    if (!wgd_platform_attach(&config)) { wgd_close(&wg); crypto_zero(&config, sizeof(config)); wgd_entropy_stop(); wgd_event("another tunnel is registered or tunnel registration failed"); return 1; }
    crypto_zero(&config, sizeof(config));
    char public_key[45]; size_t key_len = sizeof(public_key);
    wireguard_base64_encode(wg.device.public_key, 32, public_key, &key_len);
    wgd_event("listening; public key follows"); wgd_event(public_key); wgd_stats(&wg);
    uint32_t stats_at = wireguard_sys_now();
    for (;;) {
        wgd_platform_poll(&wg); wgd_tick(&wg);
        uint32_t now = wireguard_sys_now();
        if ((uint32_t)(now - stats_at) >= 5000) { wgd_stats(&wg); stats_at = now; }
        wgd_platform_sleep();
    }
}
