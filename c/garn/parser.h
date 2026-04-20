#ifndef _GARN_PARSER_INCLUDED_
#define _GARN_PARSER_INCLUDED_

/*
 *  args.h
 *
 *  krusty@vxn.dev / Apt 20, 2026
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"

/* Append a null-terminated string into buf at offset off, return new offset. */
uint32_t str_append(uint8_t *buf, uint32_t off, const uint8_t *s);

/*
 *  parse_token()
 *
 *  Reads from buf[*off] up to delim (or \r), writes into out (null-terminated).
 *  Advances *off past the delimiter.
 */
void parse_token(const uint8_t *buf, uint32_t len, uint32_t *off, uint8_t *out, uint32_t max, uint8_t delim);

#ifdef __cplusplus
}
#endif

#endif
