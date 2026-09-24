//
//  unit --- offline checks of the parts of the engine that need no network.
//

#include "../doc.h"
#include "../http.h"

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
}

int main()
{
    urls();
    http();
    html();
    css();
    forms();
    headlines();
    if (failures)
    {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
