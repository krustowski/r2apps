#ifndef WGD_PLATFORM_H
#define WGD_PLATFORM_H
#include "wgd.h"
bool wgd_platform_start(const struct wgd_config *cfg, const char **error);
bool wgd_platform_attach(const struct wgd_config *cfg);
void wgd_platform_detach(void);
void wgd_platform_poll(struct wgd *wg);
void wgd_platform_sleep(void);
bool wgd_platform_send(const uint8_t ip[4], uint16_t port, const uint8_t *data, size_t n);
bool wgd_platform_inject(const uint8_t *ip, size_t n);
void wgd_event(const char *text);
void wgd_stats(const struct wgd *wg);
int wgd_read_config(const char *path, char *text, size_t cap);
#endif
