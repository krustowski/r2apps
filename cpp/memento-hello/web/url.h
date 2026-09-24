#pragma once

#include "wbase.h"

namespace web {

//
//  An absolute http or https URL, split into the parts a request needs.  The
//  fragment is dropped on the way in: it never goes on the wire, and there is
//  nothing on a page here for it to scroll to.
//
struct Url
{
    static const size_t HOST_CAP = 128;
    static const size_t PATH_CAP = 1024;

    bool https = true;
    char host[HOST_CAP] = {};
    uint16_t port = 443;
    char path[PATH_CAP] = "/"; // always starts with '/', includes the query

    bool defaultPort() const { return port == (https ? 443 : 80); }
};

//
//  What the user typed.  A bare "example.com/x" is taken as https; words with
//  spaces and no dot are taken as a search.  Returns false for anything that
//  cannot become a URL (an unsupported scheme, a missing host).
//
bool urlFromInput(const char *text, Url &out);

//  A reference found on a page, resolved against the page it was found on.
bool urlResolve(const Url &base, const char *ref, Url &out);

//  "https://host[:port]/path"
void urlFormat(const Url &u, char *out, size_t cap);

//  Hosts written as a dotted quad need no resolver.
bool parseIPv4(const char *s, uint8_t ip[4]);

} // namespace web
