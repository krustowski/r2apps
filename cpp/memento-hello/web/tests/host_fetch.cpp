//
//  host_fetch --- the browser engine on the host, against real servers.
//
//  Everything between the network and the screen --- URLs, TLS (the same
//  BearSSL configuration and trust anchors as on r2), HTTP, the HTML parser
//  and the layout --- is exercised here through ordinary sockets, and the
//  laid-out page is printed as text.  What it does not exercise is the r2 TCP
//  stack and the Memento window.
//
//      ./host_fetch https://example.com/ [columns]
//      ./host_fetch --file page.html [columns]
//      ./host_fetch --insecure https://self-signed.example/
//

#include "../doc.h"
#include "../loader.h"
#include "../tls.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

// ─── The platform, host edition ──────────────────────────────────────────────

namespace web {
void *alloc(size_t n) { return ::malloc(n); }
void *realloc(void *p, size_t n) { return ::realloc(p, n); }
void free(void *p) { ::free(p); }
void *big_alloc(size_t n) { return ::malloc(n); }
void *big_realloc(void *p, size_t n) { return ::realloc(p, n); }
void big_free(void *p) { ::free(p); }
uint64_t now_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}
} // namespace web

//  The anchor file: $WEB_CACERTS, or the one tools/mkcacerts.sh writes.
static const char *anchorPath()
{
    const char *p = getenv("WEB_CACERTS");
    return p && p[0] ? p : "../cacerts.bin";
}

extern "C" const char *web_tls_platform_anchor_path(void) { return anchorPath(); }

extern "C" int web_tls_platform_anchors(const unsigned char **data, unsigned long *len)
{
    FILE *f = fopen(anchorPath(), "rb");
    if (!f)
        return -1;
    static unsigned char buf[512 * 1024];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    *data = buf;
    *len = (unsigned long)n;
    return n ? 0 : -1;
}

extern "C" void *web_tls_alloc(unsigned long n) { return malloc(n); }
extern "C" void web_tls_release(void *p) { free(p); }

static void hostSeed(uint8_t *out, size_t n)
{
    if (getrandom(out, n, 0) != (ssize_t)n)
        abort();
}

static void hostTime(unsigned long *days, unsigned long *secs)
{
    time_t t = time(nullptr);
    *days = (unsigned long)(t / 86400) + 719528; // BearSSL counts from 0000-01-01
    *secs = (unsigned long)(t % 86400);
}

class HostNet : public web::NetIf
{
public:
    void poll() override {}

    int resolve(const char *host, uint8_t ip[4]) override
    {
        struct addrinfo hints = {}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        int r = getaddrinfo(host, nullptr, &hints, &res);
        if (r || !res)
        {
            snprintf(err_, sizeof(err_), "%s", gai_strerror(r));
            return -1;
        }
        memcpy(ip, &((struct sockaddr_in *)res->ai_addr)->sin_addr, 4);
        freeaddrinfo(res);
        return 1;
    }

    int connect(const uint8_t ip[4], uint16_t port) override
    {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return -1;
        fcntl(fd, F_SETFL, O_NONBLOCK);
        struct sockaddr_in sa = {};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(port);
        memcpy(&sa.sin_addr, ip, 4);
        if (::connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 && errno != EINPROGRESS)
        {
            snprintf(err_, sizeof(err_), "%s", strerror(errno));
            ::close(fd);
            return -1;
        }
        peerClosed_ = false;
        return fd;
    }

    int status(int h) override
    {
        if (failed_)
            return FAILED;
        if (peerClosed_)
            return PEER_CLOSED;
        int e = 0;
        socklen_t l = sizeof(e);
        getsockopt(h, SOL_SOCKET, SO_ERROR, &e, &l);
        if (e)
        {
            snprintf(err_, sizeof(err_), "%s", strerror(e));
            return FAILED;
        }
        struct sockaddr_in sa;
        socklen_t sl = sizeof(sa);
        return getpeername(h, (struct sockaddr *)&sa, &sl) == 0 ? OPEN : CONNECTING;
    }

    size_t send(int h, const uint8_t *d, size_t n) override
    {
        ssize_t r = ::send(h, d, n, MSG_NOSIGNAL);
        return r > 0 ? (size_t)r : 0;
    }

    size_t recv(int h, uint8_t *d, size_t n) override
    {
        ssize_t r = ::recv(h, d, n, 0);
        if (r == 0)
            peerClosed_ = true;
        else if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        {
            snprintf(err_, sizeof(err_), "%s", strerror(errno));
            failed_ = true;
        }
        return r > 0 ? (size_t)r : 0;
    }

    void close(int h) override { ::close(h); }
    const char *lastError() override { return err_; }

private:
    char err_[128] = {};
    bool peerClosed_ = false;
    bool failed_ = false;
};

// ─── Printing a laid-out page ────────────────────────────────────────────────

//  The document is in CP437; the terminal wants UTF-8.
static void putCp437(unsigned char c)
{
    static const char *const hi[128] = {
        "Ç", "ü", "é", "â", "ä", "à", "å", "ç", "ê", "ë", "è", "ï", "î", "ì", "Ä", "Å", "É", "æ", "Æ", "ô", "ö", "ò",
        "û", "ù", "ÿ", "Ö", "Ü", "¢", "£", "¥", "₧", "ƒ", "á", "í", "ó", "ú", "ñ", "Ñ", "ª", "º", "¿", "⌐", "¬", "½",
        "¼", "¡", "«", "»", "░", "▒", "▓", "│", "┤", "╡", "╢", "╖", "╕", "╣", "║", "╗", "╝", "╜", "╛", "┐", "└", "┴",
        "┬", "├", "─", "┼", "╞", "╟", "╚", "╔", "╩", "╦", "╠", "═", "╬", "╧", "╨", "╤", "╥", "╙", "╘", "╒", "╓", "╫",
        "╪", "┘", "┌", "█", "▄", "▌", "▐", "▀", "α", "ß", "Γ", "π", "Σ", "σ", "µ", "τ", "Φ", "Θ", "Ω", "δ", "∞", "φ",
        "ε", "∩", "≡", "±", "≥", "≤", "⌠", "⌡", "÷", "≈", "°", "∙", "·", "√", "ⁿ", "²", "■", " "};
    if (c >= 0x80)
        fputs(hi[c - 0x80], stdout);
    else if (c == 0x07)
        fputs("•", stdout);
    else if (c == 0x09)
        fputs("○", stdout);
    else if (c >= 0x20)
        putchar(c);
    else
        putchar('?');
}

static void printDoc(web::Document &doc, int cols)
{
    doc.layout(cols);
    printf("== title: ");
    for (const char *t = doc.title(); *t; t++)
        putCp437((unsigned char)*t);
    printf("\n== %zu lines, %d rows, %d links%s\n", doc.lineCount(), doc.rows(), doc.linkCount(),
           doc.outOfMemory() ? " (OUT OF MEMORY)" : "");

    int row = 0;
    for (size_t li = 0; li < doc.lineCount(); li++)
    {
        const web::Line &l = doc.line(li);
        while (row < l.row)
        {
            putchar('\n');
            row++;
        }
        if (l.hr)
        {
            for (int i = 0; i < cols; i++)
                fputs("─", stdout);
        }
        int col = 0;
        int scale = l.big ? 2 : 1;
        for (uint32_t r = 0; r < l.nRuns; r++)
        {
            const web::Run &ru = doc.run(l.firstRun + r);
            while (col < ru.col * scale)
            {
                putchar(' ');
                col++;
            }
            bool link = ru.style & web::ST_LINK;
            if (link)
                fputs("\033[4;34m", stdout);
            else if (ru.style & web::ST_BOLD)
                fputs("\033[1m", stdout);
            else if (ru.style & web::ST_FAINT)
                fputs("\033[2m", stdout);
            else if (ru.fg)
                fputs("\033[33m", stdout);
            for (int k = 0; k < ru.len; k++)
            {
                putCp437((unsigned char)doc.text(ru.off)[k]);
                if (l.big)
                    putchar(' ');
            }
            fputs("\033[0m", stdout);
            col += ru.len * scale;
        }
        putchar('\n');
        row += l.big ? 2 : 1;
        if (l.big)
        {
            putchar('\n');
        }
    }
    printf("\n== links:\n");
    for (int i = 0; i < doc.linkCount() && i < 40; i++)
        printf("  [%d] %s\n", i, doc.linkHref(i));
}

//  The page, then its linked style sheets (the first three, as the browser
//  does), then the page again with them.
static void render(web::Loader &loader, HostNet &net, const char *urlText, int cols, bool css)
{
    web::HttpResponse &resp = loader.response();
    printf("== %s -> %d %s, %s%s%s, %zu bytes%s\n", urlText, resp.status, resp.reason, resp.contentType,
           resp.charset[0] ? "; charset=" : "", resp.charset, resp.body.len, resp.truncated ? " (truncated)" : "");
    web::Document doc;
    if (resp.contentType[0] && !web::istarts(resp.contentType, "text/html") &&
        !web::istarts(resp.contentType, "application/xhtml"))
    {
        doc.loadText(resp.body.data, resp.body.len, resp.charset);
        printDoc(doc, cols);
        return;
    }
    web::Buf page(true);
    page.append(resp.body.data, resp.body.len);
    char charset[24];
    web::scopy(charset, resp.charset, sizeof(charset));
    web::Url base = loader.url();
    doc.loadHtml(page.data, page.len, charset, nullptr, 0, css);

    static web::Buf sheets[3];
    web::StyleSheetText texts[3];
    int n = 0;
    for (int k = 0; css && k < doc.stylesheetCount() && n < 3; k++)
    {
        web::Url u;
        if (!web::urlResolve(base, doc.stylesheetHref(k), u))
            continue;
        loader.start(u, false);
        while (loader.busy())
        {
            loader.step();
            usleep(1000);
        }
        char text[1200];
        web::urlFormat(u, text, sizeof(text));
        web::HttpResponse &r = loader.response();
        fprintf(stderr, "   .. style sheet %s: %d, %zu bytes\n", text, r.status, r.body.len);
        if (loader.phase() != web::Loader::DONE || r.status != 200)
            continue;
        sheets[n].clear();
        sheets[n].append(r.body.data, r.body.len);
        texts[n].data = sheets[n].data;
        texts[n].len = sheets[n].len;
        n++;
    }
    (void)net;
    if (n)
        doc.loadHtml(page.data, page.len, charset, texts, n, true);
    printDoc(doc, cols);
}

int main(int argc, char **argv)
{
    bool insecure = false;
    int a = 1;
    if (a < argc && !strcmp(argv[a], "--insecure"))
    {
        insecure = true;
        a++;
    }
    if (a < argc && !strcmp(argv[a], "--file"))
    {
        if (a + 1 >= argc)
            return 2;
        FILE *f = fopen(argv[a + 1], "rb");
        if (!f)
            return 1;
        static uint8_t buf[4 << 20];
        size_t n = fread(buf, 1, sizeof(buf), f);
        fclose(f);
        web::Document doc;
        doc.loadHtml(buf, n, nullptr);
        printDoc(doc, a + 2 < argc ? atoi(argv[a + 2]) : 78);
        return 0;
    }
    if (a >= argc)
    {
        fprintf(stderr, "usage: %s [--insecure] <url> [cols] | --file <page.html> [cols]\n", argv[0]);
        return 2;
    }

    web::Url u;
    if (!web::urlFromInput(argv[a], u))
    {
        fprintf(stderr, "not a URL: %s\n", argv[a]);
        return 2;
    }
    int cols = a + 1 < argc ? atoi(argv[a + 1]) : 78;

    HostNet net;
    web::Loader loader(net, hostSeed, hostTime);
    loader.start(u, insecure);
    const char *last = "";
    while (loader.busy())
    {
        loader.step();
        if (strcmp(last, loader.status()))
        {
            fprintf(stderr, "   .. %s\n", loader.status());
            last = loader.status();
        }
        usleep(1000);
    }
    char final[1200];
    web::urlFormat(loader.url(), final, sizeof(final));
    if (loader.phase() == web::Loader::FAILED)
    {
        printf("FAILED: %s%s\n", loader.error(), loader.untrusted() ? " [untrusted]" : "");
        return 1;
    }
    render(loader, net, final, cols, !getenv("WEB_NOCSS"));
    return 0;
}
