//
//  unit --- offline checks of the parts of the engine that need no network.
//

#include "../doc.h"
#include "../http.h"
#include "../image.h"
#include "../mp4.h"
#include "../png.h"

extern "C" unsigned char *stbi_load_from_memory(const unsigned char *, int, int *, int *, int *, int);
extern "C" void stbi_image_free(void *);

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

namespace web {
void *alloc(size_t n) { return ::malloc(n); }
void *realloc(void *p, size_t n) { return ::realloc(p, n); }
void free(void *p) { ::free(p); }
void *big_alloc(size_t n) { return ::malloc(n); }
void *big_realloc(void *p, size_t n) { return ::realloc(p, n); }
void big_free(void *p) { ::free(p); }
uint64_t now_ms() { return (uint64_t)time(nullptr) * 1000; }
} // namespace web

static int failures = 0;

#define CHECK(cond)                                                                                  \
    do                                                                                               \
    {                                                                                                \
        if (!(cond))                                                                                 \
        {                                                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                   \
            failures++;                                                                              \
        }                                                                                            \
    } while (0)

static void checkResolve(const char *base, const char *ref, const char *want)
{
    web::Url b, out;
    if (!web::urlFromInput(base, b))
    {
        printf("FAIL base %s\n", base);
        failures++;
        return;
    }
    char got[1200] = "(none)";
    if (web::urlResolve(b, ref, out))
        web::urlFormat(out, got, sizeof(got));
    if (strcmp(got, want))
    {
        printf("FAIL resolve %s + %s: got %s, want %s\n", base, ref, got, want);
        failures++;
    }
}

static void checkInput(const char *in, const char *want)
{
    web::Url u;
    char got[1200] = "(none)";
    if (web::urlFromInput(in, u))
        web::urlFormat(u, got, sizeof(got));
    if (strcmp(got, want))
    {
        printf("FAIL input '%s': got %s, want %s\n", in, got, want);
        failures++;
    }
}

static void urls()
{
    checkInput("example.com", "https://example.com/");
    checkInput("http://Example.COM:8080/a/../b?x=1#frag", "http://example.com:8080/b?x=1");
    checkInput("https://example.com:443/", "https://example.com/");
    checkInput("rou2exos wiki", "https://lite.duckduckgo.com/lite/?q=rou2exos+wiki");
    checkInput("localhost", "https://lite.duckduckgo.com/lite/?q=localhost");
    checkInput("ftp://x.org/", "(none)");
    checkInput("10.3.4.1:8080/status", "https://10.3.4.1:8080/status");

    // RFC 3986, 5.4.1, against "http://a/b/c/d;p?q"
    const char *B = "http://a/b/c/d;p?q";
    checkResolve(B, "g", "http://a/b/c/g");
    checkResolve(B, "./g", "http://a/b/c/g");
    checkResolve(B, "g/", "http://a/b/c/g/");
    checkResolve(B, "/g", "http://a/g");
    checkResolve(B, "//g", "http://g/");
    checkResolve(B, "?y", "http://a/b/c/d;p?y");
    checkResolve(B, "g?y", "http://a/b/c/g?y");
    checkResolve(B, "#s", "http://a/b/c/d;p?q");
    checkResolve(B, "g#s", "http://a/b/c/g");
    checkResolve(B, ";x", "http://a/b/c/;x");
    checkResolve(B, ".", "http://a/b/c/");
    checkResolve(B, "./", "http://a/b/c/");
    checkResolve(B, "..", "http://a/b/");
    checkResolve(B, "../", "http://a/b/");
    checkResolve(B, "../g", "http://a/b/g");
    checkResolve(B, "../..", "http://a/");
    checkResolve(B, "../../g", "http://a/g");
    checkResolve(B, "../../../g", "http://a/g");
    checkResolve(B, "g/../h", "http://a/b/c/h");
    checkResolve(B, "https://other.org/x y", "https://other.org/x%20y");
    checkResolve(B, "javascript:void(0)", "(none)");
    checkResolve(B, "mailto:a@b", "(none)");
}

static void http()
{
    const char *resp = "HTTP/1.1 200 OK\r\n"
                       "Content-Type: text/html; charset=ISO-8859-2\r\n"
                       "Transfer-Encoding: chunked\r\n"
                       "\r\n"
                       "5\r\nHello\r\n"
                       "7;ext=1\r\n, world\r\n"
                       "0\r\nX-Trailer: 1\r\n\r\n";
    //  Byte by byte: every boundary of the parser gets split somewhere.
    web::HttpResponse r;
    for (const char *p = resp; *p; p++)
        r.feed((const uint8_t *)p, 1);
    CHECK(r.done);
    CHECK(!r.failed);
    CHECK(r.status == 200);
    CHECK(!strcmp(r.contentType, "text/html"));
    CHECK(!strcmp(r.charset, "iso-8859-2"));
    CHECK(r.body.len == 12 && !memcmp(r.body.data, "Hello, world", 12));

    web::HttpResponse r2;
    const char *redir = "HTTP/1.0 301 Moved\r\nLocation: /new\r\nContent-Length: 3\r\n\r\nabcEXTRA";
    r2.feed((const uint8_t *)redir, strlen(redir));
    CHECK(r2.done && r2.isRedirect() && !strcmp(r2.location, "/new") && r2.body.len == 3);

    web::HttpResponse r3;
    const char *eof = "HTTP/1.1 200 OK\r\n\r\nuntil the end";
    r3.feed((const uint8_t *)eof, strlen(eof));
    CHECK(!r3.done);
    r3.onEof();
    CHECK(r3.done && !r3.truncated && r3.body.len == 13);

    web::HttpResponse r4;
    const char *cont = "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 204 No Content\r\n\r\n";
    r4.feed((const uint8_t *)cont, strlen(cont));
    CHECK(r4.done && r4.status == 204);
}

//  The laid-out page as plain text, one line per row.
static void flatten(web::Document &d, char *out, size_t cap)
{
    out[0] = 0;
    int row = 0;
    for (size_t li = 0; li < d.lineCount(); li++)
    {
        const web::Line &l = d.line(li);
        while (row < l.row)
        {
            web::scat(out, "\n", cap);
            row++;
        }
        char line[256];
        memset(line, ' ', sizeof(line));
        int end = 0;
        for (uint32_t r = 0; r < l.nRuns; r++)
        {
            const web::Run &ru = d.run(l.firstRun + r);
            memcpy(line + ru.col, d.text(ru.off), ru.len);
            if (ru.col + ru.len > end)
                end = ru.col + ru.len;
        }
        if (l.hr)
        {
            memset(line, '-', 10);
            end = 10;
        }
        line[end] = 0;
        web::scat(out, line, cap);
        web::scat(out, "\n", cap);
        row += l.big ? 2 : 1;
    }
}

static void html()
{
    web::Document d;
    d.loadMessage("<html><head><title> A &amp; B </title><style>p{}</style><script>x<y</script></head>"
                  "<body><h1>Big</h1><p>one  two\n three</p><ul><li>a<li>b<ol><li>c</ol></ul>"
                  "<p>x&nbsp;y &lt;z&gt; caf\xC3\xA9 &eacute;&#x159;</p><hr><pre>  a\n    b</pre>"
                  "<table><tr><th>h1<th>h2<tr><td>1<td>2</table>"
                  "<a href=\"/x\">li<b>nk</b></a> <img alt=\"pic\"><br>end</body></html>");
    CHECK(!strcmp(d.title(), "A & B"));
    CHECK(d.linkCount() == 1 && !strcmp(d.linkHref(0), "/x"));

    d.layout(40);
    char flat[4096];
    flatten(d, flat, sizeof(flat));
    //  The heading is two rows tall and printed on the first of them.
    const char *want = "Big\n"
                       "\n"
                       "one two three\n"
                       "\n"
                       " \x07 a\n"
                       " \x07 b\n"
                       "   1. c\n"
                       "\n"
                       "x y <z> caf\x82 \x82r\n"
                       "\n"
                       "----------\n"
                       "\n"
                       "  a\n"
                       "    b\n"
                       "\n"
                       "h1 h2\n"
                       "1 2\n"
                       "\n"
                       "link [pic]\n"
                       "end\n";
    if (strcmp(flat, want))
    {
        printf("FAIL layout:\n---- got\n%s---- want\n%s----\n", flat, want);
        failures++;
    }

    //  Wrapping: words move to the next line whole, a word longer than the
    //  line is broken, and the indent survives the wrap.
    d.loadMessage("<blockquote>aaaa bbbb cccc dddddddddddddddddddd</blockquote>");
    d.layout(16);
    flatten(d, flat, sizeof(flat));
    const char *want2 = "    aaaa bbbb\n"
                        "    cccc\n"
                        "    dddddddddddd\n"
                        "    dddddddd\n";
    if (strcmp(flat, want2))
    {
        printf("FAIL wrap:\n---- got\n%s---- want\n%s----\n", flat, want2);
        failures++;
    }

    //  A word made of several items moves down whole.
    d.loadMessage("<p>aaaa bbbb (<a href=x>cccc</a>), dd</p>");
    d.layout(12);
    flatten(d, flat, sizeof(flat));
    if (strcmp(flat, "aaaa bbbb\n(cccc), dd\n"))
    {
        printf("FAIL glue:\n---- got\n%s----\n", flat);
        failures++;
    }

    //  Windows-1250, the way older Czech pages come.
    const uint8_t cz[] = {'<', 'p', '>', 0xE8, 0x9A, 0xF8, 0};
    d.loadHtml(cz, 6, "windows-1250");
    d.layout(40);
    flatten(d, flat, sizeof(flat));
    CHECK(!strcmp(flat, "csr\n"));
}

static void css()
{
    web::Document d;
    char flat[4096];
    d.loadMessage("<style>.x{display:none} p.c{text-align:center;color:red}"
                  "@media (max-width:400px){.w{display:none}}"
                  "@media (max-width:800px){.n{display:none}}"
                  "div > p.q{display:none} .k:not(.show){visibility:hidden}"
                  "ul.m{list-style:none;padding-left:0} ul.m li{display:inline}"
                  "em{font-style:normal;font-weight:700}</style>"
                  "<p>a<span class=x>hidden</span>b</p><p class=c>mid</p>"
                  "<p class=w>wide</p><p class=n>narrow</p>"
                  "<div><p class=q>child</p><section><p class=q>grandchild</p></section></div>"
                  "<p class=k>k1</p><p class=\"k show\">k2</p>"
                  "<ul class=m><li>A<li>B</ul><p><em>E</em></p>");
    d.layout(20);
    flatten(d, flat, sizeof(flat));
    const char *want = "ab\n"
                       "\n"
                       "        mid\n"
                       "\n"
                       "wide\n"
                       "\n"
                       "grandchild\n"
                       "\n"
                       "k2\n"
                       "\n"
                       "A B\n"
                       "\n"
                       "E\n";
    if (strcmp(flat, want))
    {
        printf("FAIL css layout:\n---- got\n%s---- want\n%s----\n", flat, want);
        failures++;
    }
    //  red quantises to EGA dark red (index 4), stored as index + 1.
    bool sawRed = false, sawBoldE = false;
    for (size_t li = 0; li < d.lineCount(); li++)
        for (uint32_t r = 0; r < d.line(li).nRuns; r++)
        {
            const web::Run &ru = d.run(d.line(li).firstRun + r);
            if (!memcmp(d.text(ru.off), "mid", 3))
                sawRed = ru.fg == 5;
            if (d.text(ru.off)[0] == 'E' && ru.len == 1)
                sawBoldE = (ru.style & web::ST_BOLD) && !(ru.style & web::ST_ITALIC);
        }
    CHECK(sawRed);
    CHECK(sawBoldE);

    //  With CSS off, only the built-in defaults: everything shows.
    const char *page = "<style>.x{display:none}</style><p>a<span class=x>b</span></p>";
    d.loadHtml((const uint8_t *)page, strlen(page), "utf-8", nullptr, 0, false);
    d.layout(20);
    flatten(d, flat, sizeof(flat));
    CHECK(!strcmp(flat, "ab\n"));

    //  A linked sheet, handed in separately.
    const char *sheet = "#t{display:none}";
    web::StyleSheetText st = {(const uint8_t *)sheet, strlen(sheet)};
    const char *page2 = "<link rel=stylesheet href=/s.css><p>x<b id=t>y</b></p>";
    d.loadHtml((const uint8_t *)page2, strlen(page2), "utf-8", &st, 1, true);
    d.layout(20);
    flatten(d, flat, sizeof(flat));
    CHECK(!strcmp(flat, "x\n"));
    CHECK(d.stylesheetCount() == 1 && !strcmp(d.stylesheetHref(0), "/s.css"));
}

static void forms()
{
    web::Document d;
    d.loadMessage("<form action=\"/s\" method=post><input name=q value=\"a b\" size=6>"
                  "<input type=checkbox name=c checked><input type=radio name=r value=1 checked>"
                  "<input type=radio name=r value=2><select name=s><option value=1>One<option selected>Two</select>"
                  "<input type=hidden name=h value=\"x&amp;y\"><textarea name=t cols=12>hi\nthere</textarea>"
                  "<input type=submit name=go value=Go><button name=b value=v>Press</button></form>");
    CHECK(d.controlCount() == 9);
    CHECK(!strcmp(d.formAction(0), "/s") && d.formPost(0));
    int go = -1, radio2 = -1, sel = -1, q = -1;
    for (int k = 0; k < d.controlCount(); k++)
    {
        const web::Control &c = d.control(k);
        if (c.type == web::Control::SUBMIT && !strcmp(d.str(c.name), "go"))
            go = k;
        if (c.type == web::Control::RADIO && !strcmp(d.str(c.value), "2"))
            radio2 = k;
        if (c.type == web::Control::SELECT)
            sel = k;
        if (c.type == web::Control::TEXT)
            q = k;
    }
    web::Buf out;
    CHECK(d.formData(0, go, out));
    const char *want = "q=a+b&c=on&r=1&s=Two&h=x%26y&t=hi%0D%0Athere&go=Go";
    if (strcmp(out.cstr(), want))
    {
        printf("FAIL form data: got %s, want %s\n", out.cstr(), want);
        failures++;
    }
    //  Typing, a radio group, a select cycling round.
    d.controlBackspace(q);
    d.controlInsert(q, 'z');
    d.controlActivate(radio2);
    d.controlActivate(sel);
    CHECK(d.formData(0, -1, out));
    const char *want2 = "q=a+z&c=on&r=2&s=1&h=x%26y&t=hi%0D%0Athere";
    if (strcmp(out.cstr(), want2))
    {
        printf("FAIL form data 2: got %s, want %s\n", out.cstr(), want2);
        failures++;
    }
    char cells[64];
    CHECK(d.controlCells(q, cells, sizeof(cells)) && !strcmp(cells, "a z___"));
    CHECK(d.controlCells(sel, cells, sizeof(cells)) && !strcmp(cells, "[One v]"));
    d.resetForm(0);
    CHECK(d.formData(0, -1, out) && !strcmp(out.cstr(), "q=a+b&c=on&r=1&s=Two&h=x%26y&t=hi%0D%0Athere"));

    //  The controls are in the link table, in order, and laid out as cells.
    int ctrlLinks = 0;
    for (int k = 0; k < d.linkCount(); k++)
        ctrlLinks += d.linkControl(k) >= 0;
    CHECK(ctrlLinks == 8); // everything but the hidden field
    d.layout(60);
    char flat[1024];
    flatten(d, flat, sizeof(flat));
    const char *wantFlat = "______ ___ ___ ___ _______ ____________ [Go] [Press]\n";
    if (strcmp(flat, wantFlat))
    {
        printf("FAIL form layout:\n---- got\n%s---- want\n%s----\n", flat, wantFlat);
        failures++;
    }
}

//  The link each run of text carries, as "text=link" pairs.
static void linkMap(web::Document &d, char *out, size_t cap)
{
    out[0] = 0;
    for (size_t li = 0; li < d.lineCount(); li++)
        for (uint32_t r = 0; r < d.line(li).nRuns; r++)
        {
            const web::Run &ru = d.run(d.line(li).firstRun + r);
            char one[96];
            web::scopyn(one, d.text(ru.off), ru.len, 64);
            web::scat(one, "=", sizeof(one));
            web::scat(one, ru.link >= 0 ? d.linkHref(ru.link) : "-", sizeof(one));
            if (out[0])
                web::scat(out, " | ", cap);
            web::scat(out, one, cap);
        }
}

static void checkLinks(const char *html, const char *want)
{
    web::Document d;
    d.loadMessage(html);
    d.layout(60);
    char got[1024];
    linkMap(d, got, sizeof(got));
    if (strcmp(got, want))
    {
        printf("FAIL links in %s\n  got  %s\n  want %s\n", html, got, want);
        failures++;
    }
}

static void headlines()
{
    checkLinks("<h2><a href=/a>Inner</a></h2>", "Inner=/a");
    checkLinks("<a href=/b><h2>Outer</h2></a>", "Outer=/b");
    //  The <h2> closes the <p> and the <a> in it; the <a> comes back for the
    //  headline and for the teaser after it, as in every browser.
    checkLinks("<p><a href=/c><h2>Head</h2>teaser</a></p><p>after", "Head=/c | teaser=/c | after=-");
    checkLinks("<p><b><a href=/d><h3>Bold head</h3></a></b>", "Bold head=/d");
    checkLinks("<style>a{display:block}</style><a href=/e><h3>Card</h3><p>text</p></a>", "Card=/e | text=/e");
    checkLinks("<style>.t a{color:#333;text-decoration:none}</style><h1 class=t><a href=/f>Styled</a></h1>",
               "Styled=/f");
    //  Formatting does not cross into a table cell.
    checkLinks("<table><tr><td><a href=/g>x<td>y</table>", "x=/g | y=-");
    //  A card made clickable by an empty <a> stretched over it (krusty.space,
    //  Hugo's PaperMod): the heading takes the link.
    checkLinks("<article><header><h2>Card title</h2></header><div><p>Teaser</p></div>"
               "<footer>June</footer><a class=entry-link aria-label=\"post link\" href=/h></a></article>"
               "<article><h2>Second</h2><p>More</p><a href=/i></a></article>",
               "Card title=/h | Teaser=- | June=- | Second=/i | More=-");
    //  A heading that is a link already keeps its own.
    checkLinks("<article><h2><a href=/j>Own</a></h2><a href=/k aria-label=Card></a></article>", "Own=/j");
    //  No heading to take: the label stands in for the text.
    checkLinks("<p>Share <a href=/l aria-label=Mastodon><svg></svg></a> <a href=/m title=Feed></a>",
               "Share=- | [Mastodon]=/l | [Feed]=/m");
    //  No heading, no label: nothing to show, as before.
    checkLinks("<p>x<a href=/n></a>y", "xy=-");
}

//  A test picture from img/ (made by Pillow), into `buf`; its length.
static size_t readImg(const char *name, uint8_t *buf, size_t cap)
{
    char path[128];
    snprintf(path, sizeof(path), "img/%s", name);
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        printf("FAIL cannot open %s\n", path);
        failures++;
        return 0;
    }
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    return n;
}

static void pictures()
{
    static uint8_t buf[4096];
    web::Picture p;
    size_t n;

    //  Black and white come out as exactly black and white: the dither
    //  cannot move them to another colour.
    n = readImg("bw.png", buf, sizeof(buf));
    CHECK(!web::decodePicture(buf, n, 100, 100, 16, 0xFFFFFF, p));
    CHECK(p.w == 8 && p.h == 4);
    bool bw = true;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 8; x++)
            bw &= p.px[y * 8 + x] == (x < 4 ? 0 : 15);
    CHECK(bw);

    //  Red: dithered between the two reds of the 16; in the cube, a red with
    //  no green or blue in it.
    n = readImg("red.jpg", buf, sizeof(buf));
    CHECK(!web::decodePicture(buf, n, 100, 100, 16, 0xFFFFFF, p));
    bool reds = p.w == 16 && p.h == 16;
    for (int k = 0; reds && k < 256; k++)
        reds = p.px[k] == 4 || p.px[k] == 12;
    CHECK(reds);
    CHECK(!web::decodePicture(buf, n, 100, 100, 256, 0xFFFFFF, p));
    bool cube = true;
    for (int k = 0; cube && k < 256; k++)
        cube = p.px[k] >= 16 + 36 * 4 && p.px[k] <= 231 && (p.px[k] - 16) % 36 == 0;
    CHECK(cube);

    //  Transparent: whatever it is laid over.
    n = readImg("clear.png", buf, sizeof(buf));
    CHECK(!web::decodePicture(buf, n, 100, 100, 16, 0xFFFFFF, p) && p.px[0] == 15);
    CHECK(!web::decodePicture(buf, n, 100, 100, 16, 0x000000, p) && p.px[5] == 0);

    //  Made to fit, keeping its shape; never enlarged.
    n = readImg("wide.gif", buf, sizeof(buf));
    int w, h;
    CHECK(web::pictureSize(buf, n, w, h) && w == 100 && h == 50);
    CHECK(!web::decodePicture(buf, n, 40, 40, 16, 0, p));
    CHECK(p.w == 40 && p.h == 20 && p.px[0] == 15);
    CHECK(!web::decodePicture(buf, n, 1000, 10, 16, 0, p));
    CHECK(p.w == 20 && p.h == 10);

    n = readImg("blue.bmp", buf, sizeof(buf));
    CHECK(!web::decodePicture(buf, n, 100, 100, 16, 0, p) && p.w == 6 && p.h == 3 && p.px[0] == 1);

    //  Not a picture, or cut short: an answer, not a crash.
    CHECK(web::decodePicture((const uint8_t *)"<html>", 6, 100, 100, 16, 0, p) != nullptr);
    n = readImg("red.jpg", buf, sizeof(buf));
    CHECK(web::decodePicture(buf, n / 3, 100, 100, 16, 0, p) != nullptr || p.w == 16);
    p.release();
}

//  Animated GIFs: every frame, each held as long as the file says (0 ms as
//  browsers take it, 100), and when they do not all fit, every other one
//  left out as often as it takes, its time given to the one before.
static void animations()
{
    static uint8_t buf[4096];
    web::Animation a;
    size_t n = readImg("anim.gif", buf, sizeof(buf));
    CHECK(web::isGif(buf, n));
    CHECK(!web::decodeAnimation(buf, n, 100, 100, 16, 0xFFFFFF, 64 * 1024, a));
    CHECK(a.w == 20 && a.h == 10 && a.frames == 3);
    CHECK(a.delay[0] == 50 && a.delay[1] == 100 && a.delay[2] == 100 && a.length == 250);
    CHECK((a.frame(0)[0] == 4 || a.frame(0)[0] == 12) && a.frame(1)[0] == 0 && a.frame(2)[199] == 15);
    CHECK(a.frameAt(0) == 0 && a.frameAt(49) == 0 && a.frameAt(50) == 1 && a.frameAt(149) == 1);
    CHECK(a.frameAt(150) == 2 && a.frameAt(250) == 0 && a.frameAt(250 * 7 + 60) == 1);
    //  Made smaller like any picture.
    CHECK(!web::decodeAnimation(buf, n, 10, 10, 16, 0xFFFFFF, 64 * 1024, a) && a.w == 10 && a.h == 5);

    //  Ten frames of 8x8, a white column moving right, in room for four:
    //  frames 0, 4 and 8, the last of them with 9's time too.
    n = readImg("ten.gif", buf, sizeof(buf));
    CHECK(!web::decodeAnimation(buf, n, 100, 100, 16, 0, 4 * 64, a));
    CHECK(a.frames == 3 && a.length == 400);
    CHECK(a.frames == 3 && a.delay[0] == 160 && a.delay[1] == 160 && a.delay[2] == 80);
    CHECK(a.frame(0)[0] == 15 && a.frame(0)[1] == 0 && a.frame(1)[4] == 15 && a.frame(1)[0] == 0 &&
          a.frame(2)[0] == 15);
    //  All of them when there is room.
    CHECK(!web::decodeAnimation(buf, n, 100, 100, 16, 0, 64 * 1024, a) && a.frames == 10 && a.length == 400);
    CHECK(a.frame(9)[1] == 15 && a.frame(9)[0] == 0);
    //  Not room for two frames: not an animation.
    CHECK(web::decodeAnimation(buf, n, 100, 100, 16, 0, 100, a) != nullptr && !a.px);

    //  A still GIF is one frame; anything else is not a GIF; a cut one
    //  keeps the frames before the cut.
    n = readImg("wide.gif", buf, sizeof(buf));
    CHECK(!web::decodeAnimation(buf, n, 100, 100, 16, 0, 64 * 1024, a) && a.frames == 1 && a.frameAt(12345) == 0);
    n = readImg("red.jpg", buf, sizeof(buf));
    CHECK(!web::isGif(buf, n) && web::decodeAnimation(buf, n, 100, 100, 16, 0, 64 * 1024, a) != nullptr);
    n = readImg("ten.gif", buf, sizeof(buf));
    CHECK(!web::decodeAnimation(buf, n * 2 / 3, 100, 100, 16, 0, 64 * 1024, a) && a.frames >= 1 &&
          a.frames < 10);
    CHECK(web::decodeAnimation(buf, 20, 100, 100, 16, 0, 64 * 1024, a) != nullptr);
    a.release();
}

//  An MP4 of H.264 (what Telegram makes of a GIF): decoded a step at a
//  time into an Animation, cut to the size the stream says is shown, each
//  frame as long as its sample.  colours.mp4 is 36x20 at 10 frames a second,
//  black, white, red, blue, white (Baseline, from openh264); high.mp4 the same
//  with its avcC saying High, which is turned away before anything is decoded.
static bool mostly(const web::Animation &a, int f, int c1, int c2)
{
    int n = 0;
    for (int k = 0; k < a.w * a.h; k++)
        n += a.frame(f)[k] == c1 || a.frame(f)[k] == c2;
    return n * 10 >= a.w * a.h * 9;
}

static const char *decodeMp4(const char *name, size_t cut, int maxW, size_t budget, web::Animation &a)
{
    static uint8_t buf[8192];
    size_t n = readImg(name, buf, sizeof(buf));
    web::Buf file{true};
    file.append(buf, cut ? cut : n);
    web::Mp4Animation m;
    const char *why = m.start(file, maxW, 100, 16, 0xFFFFFF, budget);
    if (why)
        return why;
    CHECK(!file.data && m.busy());
    int steps = 0;
    while (m.step(0) && steps < 1000)
        steps++;
    return m.finish(a);
}

static void mp4s()
{
    web::Animation a;
    const char *why = decodeMp4("colours.mp4", 0, 100, 64 * 1024, a);
    CHECK(!why);
    if (why)
        printf("  colours.mp4: %s\n", why);
    CHECK(a.w == 36 && a.h == 20 && a.frames == 5 && a.length == 500);
    if (a.frames != 5)
    {
        printf("  colours.mp4: %dx%d, %d frames, %u ms\n", a.w, a.h, a.frames, a.length);
        return;
    }
    CHECK(a.delay[0] == 100 && a.delay[4] == 100);
    CHECK(a.frames == 5 && mostly(a, 0, 0, 0) && mostly(a, 1, 15, 15) && mostly(a, 2, 4, 12) &&
          mostly(a, 3, 1, 9) && mostly(a, 4, 15, 15));
    //  Made smaller, and fewer frames kept where they do not fit.
    CHECK(!decodeMp4("colours.mp4", 0, 18, 64 * 1024, a) && a.w == 18 && a.h == 10);
    CHECK(!decodeMp4("colours.mp4", 0, 100, 36 * 20 * 2, a) && a.frames == 2 && a.length == 500);
    int w, h, profile;
    static uint8_t buf[8192];
    size_t n = readImg("colours.mp4", buf, sizeof(buf));
    CHECK(web::Mp4Animation::probe(buf, n, w, h, profile) && w == 36 && h == 20 && profile == 66);

    a.release();
    why = decodeMp4("high.mp4", 0, 100, 64 * 1024, a);
    CHECK(why && strstr(why, "High") && !a.px);
    //  Cut short, or not an MP4 at all: an answer, not a crash.
    CHECK(decodeMp4("colours.mp4", 600, 100, 64 * 1024, a) != nullptr || a.frames <= 5);
    CHECK(decodeMp4("colours.mp4", 40, 100, 64 * 1024, a) != nullptr);
    CHECK(decodeMp4("anim.gif", 0, 100, 64 * 1024, a) != nullptr);
    a.release();
}

//  Pictures on a page: alt text until the window has the picture, then a
//  line of their own, as many rows as they are tall.
static void pageImages()
{
    web::Document d;
    d.loadMessage("<p>before<img src=a.png alt=Cat>after</p><a href=/x><img src=b.gif></a>"
                  "<img src=\"data:image/png;base64,AAAA\"><img>");
    CHECK(d.imageCount() == 2 && !strcmp(d.imageSrc(0), "a.png") && !strcmp(d.imageSrc(1), "b.gif"));
    d.setCellPixels(6, 14);
    d.layout(40);
    char got[1024];
    linkMap(d, got, sizeof(got));
    CHECK(!strcmp(got, "before=- | [Cat]=- | after=- | [img]=/x"));

    d.setImageSize(0, 60, 30);   // 3 rows of 14 pixels
    d.setImageSize(1, 600, 100); // wider than the 240 pixels of 40 cells
    CHECK(d.layoutCols() != 40); // to be laid out again
    d.layout(40);
    linkMap(d, got, sizeof(got));
    CHECK(!strcmp(got, "before=- | after=-"));
    int pics = 0;
    for (size_t li = 0; li < d.lineCount(); li++)
    {
        const web::Line &l = d.line(li);
        if (l.img == 1)
        {
            CHECK(l.rows == 3 && l.imgW == 60 && l.imgH == 30 && l.height() == 3 && l.link == -1);
            pics++;
        }
        if (l.img == 2)
        {
            CHECK(l.imgW == 240 && l.imgH == 40 && l.rows == 3 && l.link == 0);
            pics++;
        }
    }
    CHECK(pics == 2);
    //  The linked picture is where its link is.
    CHECK(d.linkRow(0) >= 0 && d.line(d.lineAtRow(d.linkRow(0))).img == 2);
}

//  A screenshot's PNG, read back by stb_image: every pixel its palette colour.
static void pngRoundTrip(int w, int h, int colours)
{
    static uint8_t px[640 * 400];
    uint8_t pal[768];
    for (int i = 0; i < 256; i++)
        pal[i * 3] = (uint8_t)i, pal[i * 3 + 1] = (uint8_t)(255 - i), pal[i * 3 + 2] = (uint8_t)(i * 7);
    //  Flat areas, a few windows and some noise, like a screen.
    unsigned seed = 1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            seed = seed * 1103515245 + 12345;
            int v = (x / 40 + y / 30) % colours;
            if (y > h / 2 && x < w / 3)
                v = (seed >> 16) % colours;
            px[y * w + x] = (uint8_t)v;
        }
    web::Buf png{true};
    CHECK(web::encodePng(px, w, h, pal, colours, png));
    int gw = 0, gh = 0, comp = 0;
    unsigned char *rgb = stbi_load_from_memory(png.data, (int)png.len, &gw, &gh, &comp, 3);
    CHECK(rgb && gw == w && gh == h);
    bool same = rgb != nullptr;
    for (int i = 0; same && i < w * h; i++)
        same = rgb[i * 3] == pal[px[i] * 3] && rgb[i * 3 + 1] == pal[px[i] * 3 + 1] && rgb[i * 3 + 2] == pal[px[i] * 3 + 2];
    CHECK(same);
    if (rgb)
        stbi_image_free(rgb);
    printf("png %dx%d, %d colours: %zu bytes (raw %d)\n", w, h, colours, png.len, w * h);
}

int main()
{
    urls();
    http();
    html();
    css();
    forms();
    headlines();
    pictures();
    animations();
    mp4s();
    pageImages();
    pngRoundTrip(640, 400, 16);
    pngRoundTrip(333, 211, 256);
    pngRoundTrip(3, 1, 16);
    if (failures)
    {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
