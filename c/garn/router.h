#ifndef _GARN_ROUTER_INCLUDED_
#define _GARN_ROUTER_INCLUDED_

/*
 *  args.h
 *
 *  krusty@vxn.dev / Apr 20, 2026
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "helpers.h"
#include "net.h"
#include "string.h"
#include "syscall.h"
#include "types.h"

#define CHUNK_SIZE 128 /* max bytes per write() to stay within kernel packet buffer */

void route_file(TcpSocket_T *client, const uint8_t *name, uint8_t *buf, uint32_t cap);
void route_events(TcpSocket_T *client);
void route_root(TcpSocket_T *client);
void route_info(TcpSocket_T *client);

#ifdef __cplusplus
}
#endif

#endif
