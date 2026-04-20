#ifndef _GARN_HELPERS_INCLUDED_
#define _GARN_HELPERS_INCLUDED_

/*
 *  args.h
 *
 *  krusty@vxn.dev / Apr 20, 2026
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "helpers.h"
#include "mem.h"
#include "net.h"
#include "parser.h"
#include "string.h"
#include "syscall.h"
#include "types.h"

const uint8_t *ext_of(const uint8_t *name);
const uint8_t *content_type_for(const uint8_t *ext);
int fat_name_eq(const Entry_T *e, const uint8_t *name);
void respond(TcpSocket_T *client, uint16_t status, const uint8_t *reason, const uint8_t *content_type, const uint8_t *body, uint32_t body_len);

#ifdef __cplusplus
}
#endif

#endif
