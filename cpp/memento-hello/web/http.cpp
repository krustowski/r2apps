#include "http.h"

namespace web {

bool httpBuildRequest(const Url &u, const uint8_t *body, size_t bodyLen, bool post, Buf &req)
{
    char out[Url::PATH_CAP + Url::HOST_CAP + 512];
    size_t cap = sizeof(out);
    out[0] = 0;
    scat(out, post ? "POST " : "GET ", cap);
    scat(out, u.path, cap);
    scat(out, " HTTP/1.1\r\nHost: ", cap);
    scat(out, u.host, cap);
    if (!u.defaultPort())
    {
        scat(out, ":", cap);
        scatInt(out, u.port, cap);
    }
    if (post)
    {
        scat(out, "\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: ", cap);
        scatInt(out, (long)bodyLen, cap);
    }
    scat(out,
         "\r\n"
         "User-Agent: r2web/0.1 (rou2exOS; Memento)\r\n"
         "Accept: text/html,text/plain;q=0.9,*/*;q=0.1\r\n"
         "Accept-Language: en,cs;q=0.8\r\n"
         "Accept-Encoding: identity\r\n"
         "Connection: close\r\n"
         "\r\n",
         cap);
    size_t n = strlen(out);
    //  A request that filled the buffer was cut somewhere, and a cut request
    //  is worse than none.
    if (n + 1 >= cap)
        return false;
    req.clear();
    req.append(out, n);
    if (post && bodyLen)
        req.append(body, bodyLen);
    return !req.failed;
}

void HttpResponse::reset()
{
    status = 0;
    reason[0] = location[0] = contentType[0] = charset[0] = 0;
    headersDone = done = failed = truncated = false;
    error[0] = 0;
    body.release();
    head_.release();
    contentLength_ = -1;
    chunked_ = false;
    chunkState_ = CH_SIZE;
    chunkLeft_ = 0;
    chunkDigits_ = 0;
    trailerLineLen_ = 0;
}

void HttpResponse::fail(const char *why)
{
    if (failed)
        return;
    failed = true;
    scopy(error, why, sizeof(error));
}

//  The value of a header line "Name: value", trimmed, or nullptr.
static const char *headerValue(const char *line, const char *name, char *out, size_t cap)
{
    size_t n = strlen(name);
    if (!ieqn(line, name, n) || line[n] != ':')
        return nullptr;
    const char *v = line + n + 1;
    while (*v == ' ' || *v == '\t')
        v++;
    scopy(out, v, cap);
    size_t l = strlen(out);
    while (l && (out[l - 1] == ' ' || out[l - 1] == '\t'))
        out[--l] = 0;
    return out;
}

void HttpResponse::parseHead()
{
    char *text = (char *)head_.cstr();

    //  Status line: "HTTP/1.1 200 OK"
    char *eol = strstr(text, "\r\n");
    if (!eol || !istarts(text, "HTTP/"))
    {
        fail("not an HTTP response");
        return;
    }
    *eol = 0;
    const char *p = strchr(text, ' ');
    if (!p)
    {
        fail("bad status line");
        return;
    }
    p++;
    int code = 0;
    for (int i = 0; i < 3; i++)
    {
        if (p[i] < '0' || p[i] > '9')
        {
            fail("bad status code");
            return;
        }
        code = code * 10 + (p[i] - '0');
    }
    status = code;
    scopy(reason, p[3] == ' ' ? p + 4 : "", sizeof(reason));

    char *line = eol + 2;
    while (*line)
    {
        char *end = strstr(line, "\r\n");
        if (end)
            *end = 0;

        char v[Url::PATH_CAP];
        if (headerValue(line, "Content-Length", v, sizeof(v)))
        {
            long l = 0;
            for (const char *d = v; *d >= '0' && *d <= '9'; d++)
                l = l * 10 + (*d - '0');
            contentLength_ = l;
        }
        else if (headerValue(line, "Transfer-Encoding", v, sizeof(v)))
        {
            if (ifind(v, strlen(v), "chunked"))
                chunked_ = true;
        }
        else if (headerValue(line, "Location", v, sizeof(v)))
        {
            scopy(location, v, sizeof(location));
        }
        else if (headerValue(line, "Content-Type", v, sizeof(v)))
        {
            //  "text/html; charset=utf-8"
            size_t k = 0;
            while (v[k] && v[k] != ';' && v[k] != ' ' && k + 1 < sizeof(contentType))
            {
                contentType[k] = lower(v[k]);
                k++;
            }
            contentType[k] = 0;
            const char *cs = ifind(v, strlen(v), "charset=");
            if (cs)
            {
                cs += 8;
                if (*cs == '"')
                    cs++;
                size_t j = 0;
                while (cs[j] && cs[j] != ';' && cs[j] != '"' && cs[j] != ' ' && j + 1 < sizeof(charset))
                {
                    charset[j] = lower(cs[j]);
                    j++;
                }
                charset[j] = 0;
            }
        }
        else if (headerValue(line, "Content-Encoding", v, sizeof(v)))
        {
            //  Asked for identity; a server that sends gzip anyway has sent
            //  something this browser cannot read.
            if (!ieq(v, "identity"))
                fail("server sent a compressed body");
        }

        if (!end)
            break;
        line = end + 2;
    }

    headersDone = true;

    //  No body at all: 1xx, 204, 304, or an explicit zero length.
    if ((status >= 100 && status < 200) || status == 204 || status == 304 ||
        (!chunked_ && contentLength_ == 0))
        done = true;
}

void HttpResponse::takeBody(const uint8_t *data, size_t n)
{
    if (!n || done)
        return;
    if (body.len + n > MAX_BODY)
    {
        n = MAX_BODY - body.len;
        truncated = true;
    }
    if (n && !body.append(data, n))
    {
        //  Out of memory: keep what there is and call it a page.
        truncated = true;
        done = true;
        return;
    }
    if (truncated)
        done = true;
}

void HttpResponse::feed(const uint8_t *data, size_t n)
{
    if (failed || done)
        return;

    size_t i = 0;
    if (!headersDone)
    {
        //  Collect up to the blank line.  The head is small; byte by byte is
        //  fine and handles a terminator split across two reads.
        while (i < n && !headersDone)
        {
            if (!head_.push(data[i++]) || head_.len > MAX_HEAD)
            {
                fail("response headers too large");
                return;
            }
            size_t l = head_.len;
            if (l >= 4 && head_.data[l - 4] == '\r' && head_.data[l - 3] == '\n' &&
                head_.data[l - 2] == '\r' && head_.data[l - 1] == '\n')
            {
                head_.len -= 2; // keep the last header's CRLF, drop the blank line
                parseHead();
                if (failed)
                    return;
                //  An interim 100 Continue is followed by the real response.
                if (status >= 100 && status < 200)
                {
                    head_.clear();
                    headersDone = false;
                    done = false;
                }
            }
        }
        if (!headersDone || done)
            return;
    }

    data += i;
    n -= i;

    if (!chunked_)
    {
        if (contentLength_ >= 0)
        {
            size_t want = (size_t)contentLength_ - body.len;
            if (n > want)
                n = want;
            takeBody(data, n);
            if (body.len >= (size_t)contentLength_)
                done = true;
        }
        else
            takeBody(data, n);
        return;
    }

    //  Chunked: "<hex>[;ext]\r\n<data>\r\n" ... "0\r\n" [trailers] "\r\n"
    for (size_t k = 0; k < n && !done && !failed; )
    {
        uint8_t c = data[k];
        switch (chunkState_)
        {
        case CH_SIZE:
        {
            int d = -1;
            if (c >= '0' && c <= '9')
                d = c - '0';
            else if (lower((char)c) >= 'a' && lower((char)c) <= 'f')
                d = lower((char)c) - 'a' + 10;
            if (d >= 0)
            {
                if (++chunkDigits_ > 8)
                {
                    fail("chunk size too large");
                    break;
                }
                chunkLeft_ = chunkLeft_ * 16 + (size_t)d;
            }
            else if (c == '\n')
            {
                if (!chunkDigits_)
                {
                    fail("bad chunk size");
                    break;
                }
                chunkState_ = chunkLeft_ ? CH_DATA : CH_TRAILER;
                trailerLineLen_ = 0;
            }
            else if (c == ';' || c == ' ' || c == '\t')
                chunkState_ = CH_EXT;
            else if (c != '\r')
                fail("bad chunk size");
            k++;
            break;
        }
        case CH_EXT:
            if (c == '\n')
            {
                chunkState_ = chunkLeft_ ? CH_DATA : CH_TRAILER;
                trailerLineLen_ = 0;
            }
            k++;
            break;
        case CH_DATA:
        {
            size_t take = n - k;
            if (take > chunkLeft_)
                take = chunkLeft_;
            takeBody(data + k, take);
            chunkLeft_ -= take;
            k += take;
            if (!chunkLeft_)
                chunkState_ = CH_DATA_CRLF;
            break;
        }
        case CH_DATA_CRLF:
            if (c == '\n')
            {
                chunkState_ = CH_SIZE;
                chunkLeft_ = 0;
                chunkDigits_ = 0;
            }
            k++;
            break;
        case CH_TRAILER:
            if (c == '\n')
            {
                if (trailerLineLen_ == 0)
                    done = true;
                trailerLineLen_ = 0;
            }
            else if (c != '\r')
                trailerLineLen_++;
            k++;
            break;
        }
    }
}

void HttpResponse::onEof()
{
    if (done || failed)
        return;
    if (!headersDone)
    {
        fail(head_.len ? "connection closed in the headers" : "empty response");
        return;
    }
    //  A body with no length ends here.  One with a length, or a chunked one,
    //  that ends here was cut short --- show what came, but say so.
    if (chunked_ || (contentLength_ >= 0 && body.len < (size_t)contentLength_))
        truncated = true;
    done = true;
}

} // namespace web
