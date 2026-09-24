#include "url.h"

namespace web {

//  The search a line of words turns into.  DuckDuckGo's "lite" front end is a
//  plain HTML form over a plain HTML result list: no script, no CSS layout to
//  speak of, which is exactly what this browser can show.
static const char *SEARCH_PREFIX = "https://lite.duckduckgo.com/lite/?q=";

bool parseIPv4(const char *s, uint8_t ip[4])
{
    for (int o = 0; o < 4; o++)
    {
        if (*s < '0' || *s > '9')
            return false;
        int v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9')
        {
            v = v * 10 + (*s++ - '0');
            if (++digits > 3)
                return false;
        }
        if (v > 255)
            return false;
        ip[o] = (uint8_t)v;
        if (o < 3 && *s++ != '.')
            return false;
    }
    return *s == 0;
}

//  Writes one byte of a path, escaping what may not appear raw in a request
//  line.  Existing %XX escapes are kept as they are.
static void pathPut(char *out, size_t &n, size_t cap, char c)
{
    static const char hex[] = "0123456789ABCDEF";
    unsigned char u = (unsigned char)c;
    if (u <= ' ' || u >= 0x7F || c == '"' || c == '<' || c == '>' || c == '\\' || c == '^' ||
        c == '`' || c == '{' || c == '|' || c == '}')
    {
        if (n + 3 < cap)
        {
            out[n++] = '%';
            out[n++] = hex[u >> 4];
            out[n++] = hex[u & 15];
        }
        return;
    }
    if (n + 1 < cap)
        out[n++] = c;
}

//
//  Removes "." and ".." segments (RFC 3986, 5.2.4) from a path that starts
//  with '/'.  The query, when there is one, is left alone.
//
static void normalizePath(char *path)
{
    char *q = strchr(path, '?');
    char query[Url::PATH_CAP] = {};
    if (q)
    {
        scopy(query, q, sizeof(query));
        *q = 0;
    }

    //  Where each kept segment starts in out, so ".." can pop one.
    static const int MAX_SEGS = 128;
    size_t starts[MAX_SEGS];
    int nsegs = 0;

    char out[Url::PATH_CAP];
    size_t n = 0;
    bool trailing = false;
    const char *p = path;
    while (*p == '/')
    {
        const char *seg = p + 1;
        const char *end = seg;
        while (*end && *end != '/')
            end++;
        size_t sl = (size_t)(end - seg);
        bool last = !*end;

        if (sl == 1 && seg[0] == '.')
            trailing = last;
        else if (sl == 2 && seg[0] == '.' && seg[1] == '.')
        {
            if (nsegs)
                n = starts[--nsegs];
            trailing = last;
        }
        else if (sl == 0)
            trailing = last; // "a//b" keeps nothing extra; a final "/" marks a directory
        else
        {
            if (nsegs < MAX_SEGS)
                starts[nsegs++] = n;
            if (n + 1 < sizeof(out))
                out[n++] = '/';
            for (size_t i = 0; i < sl && n + 1 < sizeof(out); i++)
                out[n++] = seg[i];
            trailing = false;
        }
        p = end;
    }
    if (n == 0 || trailing)
    {
        if (n + 1 < sizeof(out))
            out[n++] = '/';
    }
    out[n] = 0;

    scopy(path, out, Url::PATH_CAP);
    scat(path, query, Url::PATH_CAP);
}

//  "host[:port]" up to the first '/', '?' or '#'.  Returns the rest.
static const char *parseAuthority(const char *s, Url &u)
{
    //  userinfo is not something this browser will send anywhere.
    const char *at = nullptr;
    for (const char *p = s; *p && *p != '/' && *p != '?' && *p != '#'; p++)
        if (*p == '@')
            at = p;
    if (at)
        s = at + 1;

    size_t n = 0;
    while (*s && *s != '/' && *s != '?' && *s != '#' && *s != ':')
    {
        if (n + 1 < Url::HOST_CAP)
            u.host[n++] = lower(*s);
        s++;
    }
    u.host[n] = 0;
    //  A trailing dot names the same host and only confuses SNI.
    if (n && u.host[n - 1] == '.')
        u.host[n - 1] = 0;

    u.port = u.https ? 443 : 80;
    if (*s == ':')
    {
        s++;
        unsigned v = 0;
        bool any = false;
        while (*s >= '0' && *s <= '9')
        {
            v = v * 10 + (unsigned)(*s++ - '0');
            any = true;
            if (v > 65535)
                return nullptr;
        }
        if (any && v)
            u.port = (uint16_t)v;
    }
    return s;
}

//  Copies path and query, escaping as it goes and stopping at the fragment.
static void setPath(Url &u, const char *s, bool addSlash)
{
    size_t n = 0;
    if (addSlash)
        u.path[n++] = '/';
    while (*s && *s != '#')
        pathPut(u.path, n, Url::PATH_CAP, *s++);
    u.path[n] = 0;
    if (!u.path[0])
        scopy(u.path, "/", Url::PATH_CAP);
}

static bool parseAbsolute(const char *s, Url &u)
{
    if (istarts(s, "https://"))
    {
        u.https = true;
        s += 8;
    }
    else if (istarts(s, "http://"))
    {
        u.https = false;
        s += 7;
    }
    else
        return false;

    s = parseAuthority(s, u);
    if (!s || !u.host[0])
        return false;
    setPath(u, s, *s != '/');
    normalizePath(u.path);
    return true;
}

bool urlFromInput(const char *text, Url &out)
{
    while (isSpace(*text))
        text++;
    char in[Url::PATH_CAP];
    scopy(in, text, sizeof(in));
    size_t l = strlen(in);
    while (l && isSpace(in[l - 1]))
        in[--l] = 0;
    if (!l)
        return false;

    Url u;
    if (istarts(in, "http://") || istarts(in, "https://"))
    {
        if (!parseAbsolute(in, u))
            return false;
        out = u;
        return true;
    }

    //  Some other scheme ("ftp:", "mailto:") is not something to guess at.
    for (size_t i = 0; i < l; i++)
    {
        if (in[i] == ':' && i + 2 < l && in[i + 1] == '/' && in[i + 2] == '/')
            return false;
        if (in[i] == '/' || in[i] == '.')
            break;
    }

    bool hasSpace = false, hasDot = false;
    for (size_t i = 0; i < l; i++)
    {
        if (in[i] == ' ')
            hasSpace = true;
        if (in[i] == '.' || in[i] == ':')
            hasDot = true;
    }
    if (hasSpace || !hasDot)
    {
        //  A search.  Spaces become '+', everything else unsafe is escaped.
        char q[Url::PATH_CAP];
        size_t n = 0;
        static const char hex[] = "0123456789ABCDEF";
        for (size_t i = 0; i < l && n + 4 < sizeof(q); i++)
        {
            unsigned char c = (unsigned char)in[i];
            if (c == ' ')
                q[n++] = '+';
            else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                     c == '-' || c == '_' || c == '.' || c == '~')
                q[n++] = (char)c;
            else
            {
                q[n++] = '%';
                q[n++] = hex[c >> 4];
                q[n++] = hex[c & 15];
            }
        }
        q[n] = 0;
        char full[Url::PATH_CAP];
        scopy(full, SEARCH_PREFIX, sizeof(full));
        scat(full, q, sizeof(full));
        return parseAbsolute(full, out);
    }

    char full[Url::PATH_CAP];
    scopy(full, "https://", sizeof(full));
    scat(full, in, sizeof(full));
    return parseAbsolute(full, out);
}

bool urlResolve(const Url &base, const char *ref, Url &out)
{
    while (isSpace(*ref))
        ref++;
    char r[Url::PATH_CAP];
    //  Browsers drop tabs and newlines inside a URL; so does this.
    size_t n = 0;
    for (const char *p = ref; *p && n + 1 < sizeof(r); p++)
        if (*p != '\n' && *p != '\r' && *p != '\t')
            r[n++] = *p;
    while (n && r[n - 1] == ' ')
        n--;
    r[n] = 0;

    if (istarts(r, "http://") || istarts(r, "https://"))
        return parseAbsolute(r, out);

    //  Any other scheme ("javascript:", "mailto:", "data:") goes nowhere.
    for (size_t i = 0; r[i]; i++)
    {
        char c = r[i];
        if (c == ':')
            return false;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '+' || c == '-' || c == '.'))
            break;
    }

    Url u = base;
    if (r[0] == '/' && r[1] == '/')
    {
        const char *s = parseAuthority(r + 2, u);
        if (!s || !u.host[0])
            return false;
        setPath(u, s, *s != '/');
        normalizePath(u.path);
        out = u;
        return true;
    }
    if (r[0] == '#' || r[0] == 0)
    {
        out = base;
        return true;
    }
    if (r[0] == '/')
    {
        setPath(u, r, false);
        normalizePath(u.path);
        out = u;
        return true;
    }
    if (r[0] == '?')
    {
        //  Same path, new query.
        char p[Url::PATH_CAP];
        scopy(p, base.path, sizeof(p));
        char *q = strchr(p, '?');
        if (q)
            *q = 0;
        size_t pl = strlen(p);
        size_t k = 0;
        char tmp[Url::PATH_CAP];
        for (const char *s = r; *s && *s != '#'; s++)
            pathPut(tmp, k, sizeof(tmp), *s);
        tmp[k] = 0;
        scopy(u.path, p, Url::PATH_CAP);
        if (pl + k + 1 < Url::PATH_CAP)
            scat(u.path, tmp, Url::PATH_CAP);
        out = u;
        return true;
    }

    //  Relative path: the base's directory, then the reference.
    char dir[Url::PATH_CAP];
    scopy(dir, base.path, sizeof(dir));
    char *q = strchr(dir, '?');
    if (q)
        *q = 0;
    char *slash = strrchr(dir, '/');
    if (slash)
        slash[1] = 0;
    else
        scopy(dir, "/", sizeof(dir));

    size_t k = strlen(dir);
    for (const char *s = r; *s && *s != '#'; s++)
        pathPut(dir, k, sizeof(dir), *s);
    dir[k] = 0;
    scopy(u.path, dir, Url::PATH_CAP);
    normalizePath(u.path);
    out = u;
    return true;
}

void urlFormat(const Url &u, char *out, size_t cap)
{
    scopy(out, u.https ? "https://" : "http://", cap);
    scat(out, u.host, cap);
    if (!u.defaultPort())
    {
        scat(out, ":", cap);
        scatInt(out, u.port, cap);
    }
    scat(out, u.path, cap);
}

} // namespace web
