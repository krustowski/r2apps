#include "parser.h"

/* Append a null-terminated string into buf at offset off, return new offset. */
uint32_t str_append(uint8_t *buf, uint32_t off, const uint8_t *s) {
    while (*s)
        buf[off++] = *s++;
    return off;
}

/*
 *  parse_token()
 *
 *  Reads from buf[*off] up to delim (or \r), writes into out (null-terminated).
 *  Advances *off past the delimiter.
 */
void parse_token(const uint8_t *buf, uint32_t len, uint32_t *off, uint8_t *out, uint32_t max, uint8_t delim) {
    uint32_t j = 0;
    while (*off < len && buf[*off] != delim && buf[*off] != '\r' && j < max - 1)
        out[j++] = buf[(*off)++];
    out[j] = '\0';
    if (*off < len && buf[*off] == delim)
        (*off)++;
}