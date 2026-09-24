#pragma once

#include "url.h"

namespace web {

//
//  HTTP/1.1, one request per connection.
//
//  "Connection: close" keeps the client simple --- the end of the body is the
//  end of the stream when nothing else says where it is --- and costs a new
//  handshake per page, which a browser that fetches nothing but the document
//  pays once per click.  No compression is offered, so none should arrive.
//

//  The request for u.  With a body it is a POST of a urlencoded form, without
//  one a GET.  False when there was no memory for it.
bool httpBuildRequest(const Url &u, const uint8_t *body, size_t bodyLen, bool post, Buf &out);

class HttpResponse
{
public:
    //  Bigger pages are cut here and marked truncated.  The body lives in the
    //  big pool; the arena could not hold this much next to everything else.
    static const size_t MAX_BODY = 768 * 1024;
    static const size_t MAX_HEAD = 16 * 1024;

    int status = 0;
    char reason[48] = {};
    char location[Url::PATH_CAP] = {};
    char contentType[64] = {};
    char charset[24] = {};

    bool headersDone = false;
    bool done = false;      // the whole body is in
    bool failed = false;    // the stream could not be parsed
    bool truncated = false; // MAX_BODY was reached
    char error[64] = {};

    Buf body{true};

    void reset();

    //  Everything that arrives from the server goes through here.
    void feed(const uint8_t *data, size_t n);

    //  The server closed the connection.  That ends a body with no length.
    void onEof();

    bool isRedirect() const
    {
        return (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) &&
               location[0];
    }

private:
    Buf head_;
    long contentLength_ = -1;
    bool chunked_ = false;

    enum ChunkState
    {
        CH_SIZE,      // reading the hex size line
        CH_EXT,       // skipping a chunk extension up to the line end
        CH_DATA,      // copying chunk data
        CH_DATA_CRLF, // the CRLF after the data
        CH_TRAILER    // trailers after the last chunk, up to an empty line
    } chunkState_ = CH_SIZE;
    size_t chunkLeft_ = 0;
    int chunkDigits_ = 0;
    int trailerLineLen_ = 0;

    void parseHead();
    void takeBody(const uint8_t *data, size_t n);
    void fail(const char *why);
};

} // namespace web
