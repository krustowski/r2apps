#include "wgd.h"
#include "crypto.h"
#include <string.h>

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r') ++s;
    size_t n = strlen(s);
    while (n && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r')) s[--n] = 0;
    return s;
}
static bool number(const char *s, unsigned max, uint16_t *out) {
    unsigned n = 0;
    if (!*s) return false;
    do {
        if (*s < '0' || *s > '9') return false;
        n = n * 10 + (unsigned)(*s++ - '0');
        if (n > max) return false;
    } while (*s);
    *out = (uint16_t)n; return true;
}
static bool ip4(char *s, uint8_t ip[4], bool unicast) {
    for (unsigned i = 0; i < 4; ++i) {
        char *end = s;
        while (*end >= '0' && *end <= '9') ++end;
        char sep = *end; *end = 0;
        uint16_t octet;
        if (!number(s, 255, &octet)) return false;
        ip[i] = (uint8_t)octet;
        if (i < 3 ? sep != '.' : sep != 0) return false;
        s = end + 1;
    }
    return !unicast || (ip[0] != 0 && ip[0] != 127 && ip[0] < 224);
}
static bool address(char *s, uint8_t ip[4], uint8_t *prefix, bool unicast) {
    char *slash = s;
    while (*slash && *slash != '/') ++slash;
    if (*slash) {
        *slash++ = 0;
        uint16_t bits;
        if (!number(slash, 32, &bits)) return false;
        *prefix = (uint8_t)bits;
    } else {
        *prefix = 32;
    }
    return ip4(s, ip, unicast);
}
bool wgd_allowed(const struct wgd_config *cfg, const uint8_t ip[4]) {
    for (unsigned i = 0; i < cfg->route_count; ++i) {
        const struct wgd_route *r = &cfg->routes[i];
        bool match = true;
        for (unsigned b = 0; b < 4; ++b) {
            unsigned bits = r->prefix > b * 8 ? r->prefix - b * 8 : 0;
            uint8_t mask = bits >= 8 ? 255 : bits ? (uint8_t)(255u << (8 - bits)) : 0;
            if ((ip[b] & mask) != r->network[b]) match = false;
        }
        if (match) return true;
    }
    return false;
}
static bool routes(char *text, struct wgd_config *cfg) {
    for (char *s = text; ; ) {
        char *next = s;
        while (*next && *next != ',') ++next;
        bool more = *next != 0;
        *next++ = 0;
        if (cfg->route_count == WGD_ROUTES_MAX) return false;
        struct wgd_route *r = &cfg->routes[cfg->route_count++];
        if (!address(trim(s), r->network, &r->prefix, false)) return false;
        for (unsigned b = 0; b < 4; ++b) {
            unsigned bits = r->prefix > b * 8 ? r->prefix - b * 8 : 0;
            r->network[b] &= bits >= 8 ? 255 : bits ? (uint8_t)(255u << (8 - bits)) : 0;
        }
        if (!more) return true;
        s = next;
    }
}
static bool key(const char *s, uint8_t out[32], bool allow_zero) {
    /* Canonical 32-byte WireGuard base64; no partial/extra keys. */
    if (strlen(s) != 44 || s[43] != '=') return false;
    for (unsigned i = 0; i < 43; ++i)
        if (!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z') ||
              (s[i] >= '0' && s[i] <= '9') || s[i] == '+' || s[i] == '/')) return false;
    size_t n = 32;
    if (!wireguard_base64_decode(s, out, &n) || n != 32) return false;
    char canonical[45]; size_t cap = sizeof(canonical);
    if (!wireguard_base64_encode(out, 32, canonical, &cap) || strcmp(s, canonical)) return false;
    uint8_t nonzero = 0;
    for (unsigned i = 0; i < 32; ++i) nonzero |= out[i];
    return allow_zero || nonzero;
}

bool wgd_config_parse(struct wgd_config *cfg, char *text, const char **error) {
    unsigned section = 0, seen = 0;
    memset(cfg, 0, sizeof(*cfg));
    cfg->listen_port = 51820;
    *error = "invalid or unsupported configuration";
    for (char *line = text; *line;) {
        char *next = line;
        while (*next && *next != '\n') ++next;
        if (*next) *next++ = 0;
        char *comment = line;
        while (*comment && *comment != '#' && *comment != ';') ++comment;
        *comment = 0;
        line = trim(line);
        if (!*line) { line = next; continue; }
        if (*line == '[') {
            if (!strcmp(line, "[Interface]") && section == 0) section = 1;
            else if (!strcmp(line, "[Peer]") && section == 1) section = 2;
            else return false;
            line = next; continue;
        }
        char *value = line;
        while (*value && *value != '=') ++value;
        if (!*value) return false;
        *value++ = 0;
        char *name = trim(line); value = trim(value);
        unsigned bit = 0; bool ok = false;
        if (section == 1 && !strcmp(name, "PrivateKey")) { bit = 1; ok = key(value, cfg->private_key, false); }
        else if (section == 1 && !strcmp(name, "Address")) { bit = 2; ok = address(value, cfg->address, &cfg->address_prefix, true); }
        else if (section == 1 && !strcmp(name, "ListenPort")) { bit = 4; ok = number(value, 65535, &cfg->listen_port) && cfg->listen_port; }
        else if (section == 2 && !strcmp(name, "PublicKey")) { bit = 8; ok = key(value, cfg->public_key, false); }
        else if (section == 2 && !strcmp(name, "AllowedIPs")) { bit = 16; ok = routes(value, cfg); }
        else if (section == 2 && !strcmp(name, "PresharedKey")) { bit = 32; ok = key(value, cfg->preshared_key, true); }
        else if (section == 2 && !strcmp(name, "PersistentKeepalive")) { bit = 64; ok = number(value, 60, &cfg->keepalive); }
        else if (section == 2 && !strcmp(name, "Endpoint")) {
            bit = 128;
            char *colon = value;
            while (*colon && *colon != ':') ++colon;
            if (*colon) { *colon++ = 0; ok = ip4(value, cfg->endpoint, true) && number(colon, 65535, &cfg->endpoint_port) && cfg->endpoint_port; }
        }
        if (!ok || (seen & bit)) {
            if (seen & bit) *error = "duplicate configuration field";
            else if (bit == 1 || bit == 8 || bit == 32) *error = "invalid base64 WireGuard key";
            else if (bit == 2) *error = "Address must be one IPv4 address with an optional /0..32 prefix";
            else if (bit == 16) *error = "AllowedIPs requires 1..16 comma-separated IPv4 prefixes";
            else if (bit == 128) *error = "Endpoint requires numeric IPv4:port";
            else if (!bit) *error = "unknown option or option in the wrong section";
            else *error = "invalid port or keepalive (maximum 60 seconds)";
            return false;
        }
        seen |= bit; line = next;
    }
    if ((seen & 27) != 27) { *error = "PrivateKey, Address, PublicKey and AllowedIPs are required"; return false; }
    if (cfg->endpoint_port && !memcmp(cfg->endpoint, cfg->address, 4)) {
        *error = "Endpoint must use the physical network address"; return false;
    }
    *error = NULL; return true;
}
