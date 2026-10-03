#ifndef _GARN_CONFIG_INCLUDED_
#define _GARN_CONFIG_INCLUDED_

/*
 *  config.h
 *
 *  krusty@vxn.dev / Apr 26, 2026
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"

typedef struct {
    uint16_t port;
    uint8_t  net[8];
    uint8_t  debug;
    uint8_t  path[64]; /* VFS prefix for route_file; empty = kernel cwd */
    uint8_t  ip[4];   /* static IPv4; all-zero = let ETH/DHCP manage the IP */
} GarnConfig_T;

void config_defaults(GarnConfig_T *cfg);
void config_load(const uint8_t *path, GarnConfig_T *cfg);

#ifdef __cplusplus
}
#endif

#endif
