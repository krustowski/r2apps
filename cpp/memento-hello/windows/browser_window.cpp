//
// Window — Web browser
//
// The window is the browser's face and nothing more: the engine behind it is
// in web/ --- the TCP/IP stack (net_r2), TLS (tls.c, BearSSL), HTTP, and the
// HTML parser and layout (doc) --- and is described there.  What is here is
// the toolbar, the page, the status line and the keys.
//
// Keys, with the page focused:
//   Up/Down, PgUp/PgDn, Space, Home/End   scroll
//   Tab / Shift+Tab, Enter                 walk the links, follow one
//   Backspace, Alt+Left / Alt+Right        back, forward
//   Ctrl+L or F6                           edit the address
//   F5 or Ctrl+R                           reload
//   / then n                               find on the page, find again
//   Esc                                    stop loading; close the window
// On a form field, Enter (or a click) starts typing into it; Enter again
// submits the form, Esc stops, Tab moves on.  Space or Enter ticks a box,
// picks a radio button, steps through a list's options (Shift: backwards)
// and presses a button.
// The address bar also takes ":dns <ip>", ":gw <ip>", ":css on|off", and
// "about:" pages.
//

#include "../web/css.h"
#include "../web/doc.h"
#include "../web/loader.h"
#include "../web/net_r2.h"

class BrowserWindow
{
public:
    BrowserWindow() : loader(web::r2Net(), web::gatherEntropy, web::currentTime)
    {
        for (web::Buf &b : sheetBody)
            b.big = true;
    }

    static void onEvent(void *instance, struct PlatformWindowInterfaceInputEvent *data)
    {
        reinterpret_cast<BrowserWindow *>(instance)->onEvent_(data);
    }

    void SetWindow(PlatformWindow *w)
    {
        wnd = w;
        navigate("about:home", true);
    }

private:
    static const int ADDR_CAP = 1200;
    static const int HIST_CAP = 16;
    static const int SCROLL_W = 5;

    PlatformWindow *wnd = nullptr;

    web::Loader loader;
    web::Document doc;

    //  What the address bar shows when it is not being edited.
    char current[ADDR_CAP] = "about:home";
    //  A failed load leaves the page it failed from on the screen, but the
    //  bar and the insecure retry are about the address that failed.
    char attempted[ADDR_CAP] = {};
    web::Url attemptedUrl;

    struct Hist
    {
        char addr[ADDR_CAP];
        int scroll;
    };
    Hist hist[HIST_CAP];
    int histLen = 0;
    int histPos = -1;
    bool pushOnLoad = false;
    int scrollOnLoad = 0;

    int scrollRow = 0;
    int focusedLink = -1;
    int hoverLink = -1;

    enum Edit
    {
        EDIT_NONE,
        EDIT_ADDRESS,
        EDIT_FIND,
        EDIT_FIELD, // typing into a form's text field
    } edit = EDIT_NONE;
    int editControl = -1;
    bool formsTouched = false; // the user changed a control on this page

    //  The page as it came, kept so that it can be read again once its style
    //  sheets are in (or with CSS switched off), and the sheets themselves.
    web::Buf pageBody{true};
    char pageCharset[24] = {};
    web::Url pageUrl;
    bool cssOn = true;
    static const int MAX_SHEETS = 3;
    web::Buf sheetBody[MAX_SHEETS];
    int nSheets = 0;
    int sheetNext = -1; // the stylesheet href being fetched; -1 when none is
    char pageTiming[96] = {};
    char editBuf[ADDR_CAP] = {};
    int editLen = 0;
    bool editFresh = false; // the first key replaces the whole text

    char findText[64] = {};
    size_t findLine = (size_t)-1, findRun = 0;

    char message[96] = {};    // one-off status text ("not found", ...)
    char lastStatus[96] = {}; // what the loader said last time we painted
    bool imEnabled = false;
    uint64_t idleSince = 0;
    uint64_t lastRepaint = 0;

    // Colours and fonts, made on the first paint.
    PlatformColor *cBg = nullptr, *cText = nullptr, *cLink = nullptr, *cFaint = nullptr, *cCode = nullptr;
    PlatformColor *cChrome = nullptr, *cChromeDark = nullptr, *cWhite = nullptr, *cMark = nullptr;
    PlatformColor *cFocus = nullptr, *cHead = nullptr;
    PlatformColor *cPal[16] = {}; // the EGA palette, for the page's own colours
    PlatformFont *font = nullptr, *bigFont = nullptr;
    double cw = 3, ch = 6;     // one cell of the small font, in window units
    double rowH = 7;           // a row of text, with a unit of leading
    double bcw = 6;            // one cell of the big font

    // Geometry from the last paint, for hit testing.
    double contentX = 0, contentY = 0, contentW = 0, contentH = 0;
    double toolbarH = 0, addrX = 0, winW = 0;
    int visibleRows = 1;
    int buttonW = 0;

    // ── Navigation ────────────────────────────────────────────────────────────

    void ensureIdleLoop(bool on)
    {
        if (on == imEnabled || !wnd)
            return;
        wnd->SetImmediateMode(on);
        imEnabled = on;
    }

    void setTitle(const char *t)
    {
        char buf[48] = "Web";
        if (t && t[0])
        {
            web::scopy(buf, t, 40);
            if (strlen(t) > 39)
                web::scat(buf, "...", sizeof(buf));
        }
        wnd->SetTitle((const mchar *)buf);
    }

    void pushHistory(const char *addr)
    {
        if (histPos >= 0 && histPos < histLen)
            hist[histPos].scroll = scrollRow;
        if (histPos >= 0 && !strcmp(hist[histPos].addr, addr))
            return; // a reload
        if (histPos == HIST_CAP - 1)
        {
            memmove(hist, hist + 1, sizeof(Hist) * (HIST_CAP - 1));
            histPos--;
        }
        histPos++;
        histLen = histPos + 1;
        web::scopy(hist[histPos].addr, addr, ADDR_CAP);
        hist[histPos].scroll = 0;
    }

    void showDocument(const char *addr, bool push, int scroll)
    {
        if (push)
            pushHistory(addr);
        web::scopy(current, addr, sizeof(current));
        scrollRow = scroll;
        focusedLink = hoverLink = -1;
        findLine = (size_t)-1;
        doc.layout(0); // laid out again, for the real width, at the next paint
        setTitle(doc.title());
        wnd->Repaint();
    }

    //  Text that goes into a page made up here.
    static void escapeInto(web::Buf &b, const char *s)
    {
        for (; *s; s++)
        {
            if (*s == '<')
                b.appendStr("&lt;");
            else if (*s == '>')
                b.appendStr("&gt;");
            else if (*s == '&')
                b.appendStr("&amp;");
            else if (*s == '"')
                b.appendStr("&quot;");
            else
                b.push((uint8_t)*s);
        }
    }

    void showAbout(const char *what, bool push)
    {
        web::Buf b;
        pageBody.release();
        if (!strcmp(what, "about:net") || !strcmp(what, "about:"))
        {
            char net[400];
            web::r2NetDescribe(net, sizeof(net));
            b.appendStr("<title>About</title><h1>r2web</h1>"
                        "<p>A small web browser for rou2exOS: HTTP/1.1, TLS 1.2 by BearSSL, and HTML "
                        "without scripts or style sheets, in the kernel's own font.</p><h3>Network</h3><p>");
            escapeInto(b, net);
            b.appendStr("</p><p>Change with <code>:dns 9.9.9.9</code> or <code>:gw 10.3.4.1</code> in "
                        "the address bar; <code>:css off</code> shows pages without their style sheets. The guest needs a route to the internet: NAT on the host "
                        "for 10.3.4.0/24.</p>"
                        "<h3>Keys</h3><pre>"
                        "Up/Down PgUp/PgDn Space Home/End   scroll\n"
                        "Tab / Shift+Tab, Enter             walk links, follow\n"
                        "Backspace, Alt+Left / Alt+Right    back, forward\n"
                        "Ctrl+L or F6                       edit the address\n"
                        "F5 or Ctrl+R                       reload\n"
                        "Enter on a field, then type        fill in a form\n"
                        "Space on a box, list or button     tick, pick, press\n"
                        "/  then  n                         find, find again\n"
                        "Esc                                stop; close\n"
                        "</pre><p><a href=\"about:home\">Start page</a></p>");
        }
        else
        {
            b.appendStr("<title>Start</title><h1>r2web</h1>"
                        "<p>Type an address above, or words to search for.</p>"
                        "<h3>Pages that read well here</h3><ul>"
                        "<li><a href=\"https://lite.duckduckgo.com/lite/\">DuckDuckGo Lite</a> - search"
                        "<li><a href=\"https://text.npr.org/\">text.npr.org</a> - news, as text"
                        "<li><a href=\"https://lite.cnn.com/\">lite.cnn.com</a> - more news"
                        "<li><a href=\"https://news.ycombinator.com/\">Hacker News</a>"
                        "<li><a href=\"https://en.m.wikipedia.org/wiki/Special:Random\">A random Wikipedia "
                        "article</a>"
                        "<li><a href=\"http://info.cern.ch/hypertext/WWW/TheProject.html\">The first web "
                        "page</a>"
                        "</ul><p><a href=\"about:net\">About this browser, the network and the keys</a></p>");
        }
        doc.loadMessage(b.cstr());
        showDocument(what, push, 0);
    }

    void showError(const char *title, const char *detail, bool offerInsecure)
    {
        pageBody.release();
        web::Buf b;
        b.appendStr("<title>Problem loading page</title><h2>");
        escapeInto(b, title);
        b.appendStr("</h2><p>");
        escapeInto(b, attempted);
        b.appendStr("</p><p>");
        escapeInto(b, detail);
        b.appendStr("</p>");
        if (offerInsecure)
            b.appendStr("<p>The server's certificate does not lead to any of the roots this browser "
                        "carries. That may be an attacker, or a certificate authority not on the short "
                        "list.</p><p><a href=\"about:insecure\">Load it anyway, without knowing who "
                        "answers</a></p>");
        b.appendStr("<p><a href=\"about:retry\">Try again</a></p>");
        doc.loadMessage(b.cstr());
        //  The error is shown but not remembered: back goes to the page before.
        web::scopy(current, attempted, sizeof(current));
        scrollRow = 0;
        focusedLink = hoverLink = -1;
        doc.layout(0);
        setTitle("Problem loading page");
        wnd->Repaint();
    }

    void startLoad(const web::Url &u, bool insecure, bool push, int scroll, const web::Buf *post = nullptr)
    {
        attemptedUrl = u;
        web::urlFormat(u, attempted, sizeof(attempted));
        pushOnLoad = push;
        scrollOnLoad = scroll;
        message[0] = 0;
        sheetNext = -1;
        if (edit == EDIT_FIELD)
            edit = EDIT_NONE;
        if (post)
            loader.start(u, insecure, post->data ? post->data : (const uint8_t *)"", post->len);
        else
            loader.start(u, insecure);
        ensureIdleLoop(true);
        wnd->Repaint();
    }

    //  Anything typed in the address bar, or the href of a link.
    void navigate(const char *where, bool push, int scroll = 0)
    {
        //  A copy first: a link's href lives in the document, which the first
        //  thing a page made up here does is replace.
        char text[ADDR_CAP];
        while (*where == ' ')
            where++;
        web::scopy(text, where, sizeof(text));
        if (text[0] == ':')
        {
            command(text + 1);
            return;
        }
        if (web::istarts(text, "about:"))
        {
            if (!strcmp(text, "about:insecure"))
            {
                startLoad(attemptedUrl, true, true, 0);
                return;
            }
            if (!strcmp(text, "about:retry"))
            {
                startLoad(attemptedUrl, false, true, 0);
                return;
            }
            loader.cancel();
            showAbout(text, push);
            return;
        }
        web::Url u;
        if (!web::urlFromInput(text, u))
        {
            web::scopy(attempted, text, sizeof(attempted));
            showError("That is not an address this browser can open",
                      "Only http:// and https:// addresses work here.", false);
            return;
        }
        startLoad(u, false, push, scroll);
    }

    void command(const char *cmd)
    {
        bool ok = false;
        if (web::istarts(cmd, "dns "))
            ok = web::r2NetSetDns(cmd + 4);
        else if (web::istarts(cmd, "gw "))
            ok = web::r2NetSetGateway(cmd + 3);
        else if (web::istarts(cmd, "css "))
        {
            ok = web::ieq(cmd + 4, "on") || web::ieq(cmd + 4, "off");
            if (ok && cssOn != web::ieq(cmd + 4, "on"))
            {
                cssOn = !cssOn;
                //  The current page again, the other way.
                if (pageBody.len)
                {
                    if (cssOn && !nSheets)
                    {
                        renderPage(nullptr, 0);
                        sheetNext = 0;
                        fetchNextSheet();
                    }
                    else
                        applySheets();
                }
            }
        }
        web::scopy(message, ok ? "Setting changed." : "Unknown command; try :dns <ip>, :gw <ip> or :css on|off",
                   sizeof(message));
        wnd->Repaint();
    }

    void followLink(int idx)
    {
        if (idx < 0 || idx >= doc.linkCount())
            return;
        int ci = doc.linkControl(idx);
        if (ci >= 0)
        {
            focusedLink = idx;
            activateControl(ci, false);
            return;
        }
        const char *href = doc.linkHref(idx);
        if (web::istarts(href, "about:"))
        {
            navigate(href, true);
            return;
        }
        if (href[0] == '#')
        {
            //  An anchor on this page; there is nowhere to scroll to.
            return;
        }
        web::Url base;
        if (!web::urlFromInput(current, base))
        {
            //  A page made up here: its links are absolute or about:.
            navigate(href, true);
            return;
        }
        web::Url u;
        if (!web::urlResolve(base, href, u))
        {
            web::scopy(message, "That link goes somewhere this browser cannot follow.", sizeof(message));
            wnd->Repaint();
            return;
        }
        startLoad(u, false, true, 0);
    }

    void goHistory(int delta)
    {
        int to = histPos + delta;
        if (to < 0 || to >= histLen)
            return;
        hist[histPos].scroll = scrollRow;
        histPos = to;
        navigate(hist[histPos].addr, false, hist[histPos].scroll);
    }

    void reload()
    {
        if (histPos >= 0)
            navigate(hist[histPos].addr, false, scrollRow);
    }

    // ── Forms ─────────────────────────────────────────────────────────────────

    void activateControl(int ci, bool backwards)
    {
        web::Control &c = doc.control(ci);
        if (c.isText())
        {
            edit = EDIT_FIELD;
            editControl = ci;
        }
        else if (c.type == web::Control::SUBMIT || c.type == web::Control::IMAGE)
        {
            submitForm(c.form, ci);
            return;
        }
        else if (c.type == web::Control::RESET)
        {
            doc.resetForm(c.form);
            formsTouched = true;
        }
        else if (c.type == web::Control::BUTTON)
            web::scopy(message, "This button does its work with JavaScript, which this browser has not got.",
                       sizeof(message));
        else
        {
            doc.controlActivate(ci, backwards);
            formsTouched = true;
        }
        wnd->Repaint();
    }

    //  Sends a form: GET puts its data in the address, POST in the body.
    //  `submitter` is the button pressed, or -1 for Enter in a text field,
    //  which presses the form's first button the way browsers do.
    void submitForm(int form, int submitter)
    {
        if (form < 0)
        {
            web::scopy(message, "This is not part of a form; the page does its work with JavaScript.",
                       sizeof(message));
            wnd->Repaint();
            return;
        }
        if (submitter < 0)
            for (int k = 0; k < doc.controlCount() && submitter < 0; k++)
                if (doc.control(k).form == form &&
                    (doc.control(k).type == web::Control::SUBMIT || doc.control(k).type == web::Control::IMAGE))
                    submitter = k;
        web::Buf data;
        doc.formData(form, submitter, data);
        web::Url base, u;
        const char *action = doc.formAction(form);
        if (!web::urlFromInput(current, base) || !web::urlResolve(base, action[0] ? action : current, u))
        {
            web::scopy(message, "The form goes somewhere this browser cannot follow.", sizeof(message));
            wnd->Repaint();
            return;
        }
        edit = EDIT_NONE;
        if (doc.formPost(form))
        {
            startLoad(u, false, true, 0, &data);
            return;
        }
        char *q = strchr(u.path, '?');
        if (q)
            *q = 0;
        if (strlen(u.path) + 1 + data.len >= sizeof(u.path))
        {
            web::scopy(message, "Too much to send in an address.", sizeof(message));
            wnd->Repaint();
            return;
        }
        web::scat(u.path, "?", sizeof(u.path));
        web::scat(u.path, data.cstr(), sizeof(u.path));
        startLoad(u, false, true, 0);
    }

    // ── Style sheets ──────────────────────────────────────────────────────────

    //  The kept page, read again with the sheets there are.
    void renderPage(const web::StyleSheetText *sheets, int n)
    {
        doc.loadHtml(pageBody.data, pageBody.len, pageCharset, sheets, n, cssOn);
        doc.layout(0);
        focusedLink = hoverLink = -1;
        findLine = (size_t)-1;
        formsTouched = false;
        setTitle(doc.title());
    }

    void applySheets()
    {
        sheetNext = -1;
        if (!pageBody.len)
            return;
        if (formsTouched)
        {
            //  Reading the page again would throw away what was typed.
            web::scopy(message, "Style sheets not applied: the form has been filled in.", sizeof(message));
            wnd->Repaint();
            return;
        }
        web::StyleSheetText texts[MAX_SHEETS];
        for (int k = 0; k < nSheets; k++)
            texts[k] = web::StyleSheetText{sheetBody[k].data, sheetBody[k].len};
        int keep = scrollRow;
        renderPage(texts, cssOn ? nSheets : 0);
        scrollRow = keep; // clamped to the new length at the next paint
        web::scopy(message, pageTiming, sizeof(message));
        size_t ml = strlen(message);
        if (ml && message[ml - 1] == '.')
            message[ml - 1] = 0;
        if (cssOn && nSheets)
        {
            web::scat(message, ", +", sizeof(message));
            web::scatInt(message, nSheets, sizeof(message));
            web::scat(message, nSheets == 1 ? " style sheet" : " style sheets", sizeof(message));
        }
        wnd->Repaint();
    }

    //  The page's linked style sheets, one after the other, the first few.
    void fetchNextSheet()
    {
        while (sheetNext >= 0 && sheetNext < doc.stylesheetCount() && nSheets < MAX_SHEETS)
        {
            web::Url u;
            if (web::urlResolve(pageUrl, doc.stylesheetHref(sheetNext), u))
            {
                loader.start(u, false);
                ensureIdleLoop(true);
                web::scopy(message, "Style sheet ", sizeof(message));
                web::scatInt(message, nSheets + 1, sizeof(message));
                web::scat(message, "...", sizeof(message));
                wnd->Repaint();
                return;
            }
            sheetNext++;
        }
        applySheets();
    }

    void onSheetLoaded()
    {
        web::HttpResponse &r = loader.response();
        if (loader.phase() == web::Loader::FAILED && !strcmp(loader.error(), "Stopped"))
        {
            applySheets(); // with what there is
            return;
        }
        if (loader.phase() == web::Loader::DONE && r.status == 200 && r.body.len &&
            (!r.contentType[0] || web::istarts(r.contentType, "text/css") || web::istarts(r.contentType, "text/plain")))
        {
            web::Buf &b = sheetBody[nSheets++];
            b.release();
            b.data = r.body.data; // taken over, not copied
            b.len = r.body.len;
            b.cap = r.body.cap;
            r.body.data = nullptr;
            r.body.len = r.body.cap = 0;
        }
        sheetNext++;
        fetchNextSheet();
    }

    void onLoaded()
    {
        if (sheetNext >= 0)
        {
            onSheetLoaded();
            return;
        }
        if (loader.phase() == web::Loader::FAILED)
        {
            if (!strcmp(loader.error(), "Stopped"))
            {
                web::scopy(message, "Stopped.", sizeof(message));
                wnd->Repaint();
                return;
            }
            showError("Could not load the page", loader.error(), loader.untrusted());
            return;
        }

        web::HttpResponse &r = loader.response();
        char addr[ADDR_CAP];
        web::urlFormat(loader.url(), addr, sizeof(addr));

        const char *ct = r.contentType;
        bool html = !ct[0] || web::istarts(ct, "text/html") || web::istarts(ct, "application/xhtml");
        pageBody.release();
        for (web::Buf &b : sheetBody)
            b.release();
        nSheets = 0;
        if (html)
        {
            //  The page is kept, taken over from the response rather than
            //  copied: its style sheets come next, and then it is read again.
            pageBody.data = r.body.data;
            pageBody.len = r.body.len;
            pageBody.cap = r.body.cap;
            r.body.data = nullptr;
            r.body.len = r.body.cap = 0;
            web::scopy(pageCharset, r.charset, sizeof(pageCharset));
            pageUrl = loader.url();
            renderPage(nullptr, 0);
        }
        else if (web::istarts(ct, "text/") || web::istarts(ct, "application/json") ||
                 web::istarts(ct, "application/xml") || web::istarts(ct, "application/javascript"))
            doc.loadText(r.body.data, r.body.len, r.charset);
        else
        {
            web::Buf b;
            char kib[32] = {};
            web::scatInt(kib, (long)((r.body.len + 1023) / 1024), sizeof(kib));
            b.appendStr("<title>");
            escapeInto(b, ct);
            b.appendStr("</title><h2>Nothing to show</h2><p>The address answered with <code>");
            escapeInto(b, ct);
            b.appendStr("</code> (");
            b.appendStr(kib);
            b.appendStr(" KiB), which this browser cannot display.</p>");
            doc.loadMessage(b.cstr());
        }

        //  Where the time went, until something more important replaces it.
        loader.timing(message, sizeof(message));
        if (doc.outOfMemory())
            web::scopy(message, "Out of memory: only part of the page is shown.", sizeof(message));
        else if (r.truncated)
            web::scopy(message, "The page was cut short.", sizeof(message));
        else if (r.status >= 400)
        {
            web::scopy(message, "The server says: ", sizeof(message));
            web::scatInt(message, r.status, sizeof(message));
            web::scat(message, " ", sizeof(message));
            web::scat(message, r.reason, sizeof(message));
        }
        r.body.release();
        web::scopy(pageTiming, message, sizeof(pageTiming));
        showDocument(addr, pushOnLoad, scrollOnLoad);
        if (html && cssOn && doc.stylesheetCount())
        {
            sheetNext = 0;
            fetchNextSheet();
        }
    }

    // ── Scrolling and links ───────────────────────────────────────────────────

    int maxScroll() const
    {
        int m = doc.rows() - visibleRows + 1;
        return m > 0 ? m : 0;
    }

    void scrollTo(int row)
    {
        if (row > maxScroll())
            row = maxScroll();
        if (row < 0)
            row = 0;
        if (row != scrollRow)
        {
            scrollRow = row;
            wnd->Repaint();
        }
    }

    void focusLink(int dir)
    {
        int n = doc.linkCount();
        if (!n)
            return;
        int start = focusedLink;
        //  Starting fresh, begin with the first link on the screen.
        if (start < 0 || doc.linkRow(start) < scrollRow || doc.linkRow(start) >= scrollRow + visibleRows)
        {
            start = -1;
            for (int i = 0; i < n; i++)
            {
                int r = doc.linkRow(i);
                if (r >= scrollRow && r < scrollRow + visibleRows)
                {
                    start = dir > 0 ? i - 1 : i + 1;
                    break;
                }
            }
            if (start == -1 && dir < 0)
                start = n;
        }
        for (int i = start + dir; i >= 0 && i < n; i += dir)
        {
            int r = doc.linkRow(i);
            if (r < 0)
                continue; // not laid out (an empty link)
            focusedLink = i;
            if (r < scrollRow)
                scrollTo(r);
            else if (r >= scrollRow + visibleRows - 1)
                scrollTo(r - visibleRows / 2);
            wnd->Repaint();
            return;
        }
    }

    void findNext(bool fromStart)
    {
        if (!findText[0])
            return;
        size_t li = 0, ri = 0;
        if (!fromStart && findLine != (size_t)-1)
        {
            li = findLine;
            ri = findRun + 1;
        }
        else
            li = doc.lineAtRow(scrollRow);
        if (!doc.find(findText, li, ri) && !doc.find(findText, li = 0, ri = 0))
        {
            web::scopy(message, "Not found.", sizeof(message));
            findLine = (size_t)-1;
            wnd->Repaint();
            return;
        }
        findLine = li;
        findRun = ri;
        int r = doc.line(li).row;
        if (r < scrollRow || r >= scrollRow + visibleRows - 1)
            scrollTo(r - visibleRows / 3);
        message[0] = 0;
        wnd->Repaint();
    }

    //  The link under a point in the content area, or -1.
    int linkAt(double x, double y)
    {
        if (x < contentX || y < contentY || x >= contentX + contentW || y >= contentY + contentH)
            return -1;
        int row = scrollRow + (int)((y - contentY) / rowH);
        size_t li = doc.lineAtRow(row);
        if (li >= doc.lineCount())
            return -1;
        const web::Line &l = doc.line(li);
        if (row < l.row || row >= l.row + (l.big ? 2 : 1))
            return -1;
        double cell = l.big ? bcw : cw;
        int col = (int)((x - contentX - 1) / cell);
        for (uint32_t r = 0; r < l.nRuns; r++)
        {
            const web::Run &ru = doc.run(l.firstRun + r);
            if (col >= ru.col && col < ru.col + ru.len)
                return ru.link;
        }
        return -1;
    }

    // ── Editing the address bar ───────────────────────────────────────────────

    void beginEdit(Edit what)
    {
        edit = what;
        if (what == EDIT_ADDRESS)
        {
            web::scopy(editBuf, current, sizeof(editBuf));
            editFresh = true;
        }
        else
        {
            editBuf[0] = 0;
            editFresh = false;
        }
        editLen = (int)strlen(editBuf);
        wnd->Repaint();
    }

    //  Typing into a form's text field.
    void onFieldKey(PlatformKey *key)
    {
        int ci = editControl;
        if (ci < 0 || ci >= doc.controlCount())
        {
            edit = EDIT_NONE;
            return;
        }
        web::Control &c = doc.control(ci);
        if (key->isEscape)
            edit = EDIT_NONE;
        else if (key->isTab)
        {
            edit = EDIT_NONE;
            focusLink((key->isLeftShift || key->isRightShift) ? -1 : +1);
            return;
        }
        else if (key->isEnter)
        {
            if (c.type == web::Control::TEXTAREA && !(key->isLeftControl || key->isRightControl))
                doc.controlInsert(ci, '\n');
            else
            {
                submitForm(c.form, -1);
                return;
            }
        }
        else if (key->isBackspace)
            doc.controlBackspace(ci);
        else if (key->isChar && key->theChar >= ' ' && (unsigned char)key->theChar < 0x7F)
            doc.controlInsert(ci, (char)key->theChar);
        else
            return;
        formsTouched = true;
        wnd->Repaint();
    }

    void onEditKey(PlatformKey *key)
    {
        if (edit == EDIT_FIELD)
        {
            onFieldKey(key);
            return;
        }
        if (key->isEscape)
        {
            edit = EDIT_NONE;
            wnd->Repaint();
            return;
        }
        if (key->isEnter)
        {
            Edit was = edit;
            edit = EDIT_NONE;
            if (was == EDIT_ADDRESS)
                navigate(editBuf, true);
            else
            {
                web::scopy(findText, editBuf, sizeof(findText));
                findNext(true);
            }
            wnd->Repaint();
            return;
        }
        if (key->isBackspace)
        {
            if (editFresh)
                editLen = 0;
            else if (editLen > 0)
                editLen--;
            editBuf[editLen] = 0;
            editFresh = false;
            wnd->Repaint();
            return;
        }
        if (key->isArrowRight || key->isEnd || key->isHome || key->isArrowLeft)
        {
            //  No cursor to move; these only keep the old text for editing.
            editFresh = false;
            wnd->Repaint();
            return;
        }
        if (key->isChar && key->theChar >= ' ' && (unsigned char)key->theChar < 0x7F)
        {
            if (editFresh)
                editLen = 0;
            editFresh = false;
            if (editLen < ADDR_CAP - 1)
            {
                editBuf[editLen++] = (char)key->theChar;
                editBuf[editLen] = 0;
            }
            wnd->Repaint();
        }
    }

    // ── Events ────────────────────────────────────────────────────────────────

    void onEvent_(struct PlatformWindowInterfaceInputEvent *data)
    {
        switch (data->type)
        {
        case PlatformWindowInputEventType::OnImmediateModeIdleLoop:
            onIdle();
            return;
        case PlatformWindowInputEventType::OnPaint:
            OnPaint(data->Data.OnPaint.ctx, data->Data.OnPaint.target);
            return;
        case PlatformWindowInputEventType::OnMouseMove:
        {
            Coord mx = data->Data.OnMouseMove.mouseX, my = data->Data.OnMouseMove.mouseY;
            int l = linkAt(COORD_VAL(mx), COORD_VAL(my));
            if (l != hoverLink)
            {
                hoverLink = l;
                wnd->Repaint();
            }
            return;
        }
        case PlatformWindowInputEventType::OnMouseWheel:
            scrollTo(scrollRow + (data->Data.OnMouseWheel.up ? -3 : 3));
            return;
        case PlatformWindowInputEventType::OnMouseClick:
            if (data->Data.OnMouseClick.state == PlatformWindowButtonState::Pressed)
            {
                Coord mx = data->Data.OnMouseClick.mouseX, my = data->Data.OnMouseClick.mouseY;
                onClick(COORD_VAL(mx), COORD_VAL(my));
            }
            return;
        case PlatformWindowInputEventType::OnKeyEvent:
            if (data->Data.OnKeyEvent.key->isKeyDown)
                onKey(data->Data.OnKeyEvent.key);
            return;
        default:
            return;
        }
    }

    void onIdle()
    {
        if (loader.busy())
        {
            loader.step();
            if (!loader.busy())
            {
                onLoaded();
                idleSince = web::now_ms();
            }
            else if (strcmp(lastStatus, loader.status()) && web::now_ms() - lastRepaint >= 500)
            {
                //  A repaint is tens of milliseconds of not reading the
                //  network (see RX_WINDOW in web/net_r2.cpp), so the status
                //  line is brought up to date twice a second at most.
                lastRepaint = web::now_ms();
                wnd->Repaint();
            }
            return;
        }
        //  Keep the stack turning for a while after a load, so that closing
        //  connections finish and ARP gets answered; then let the loop rest.
        web::r2Net().poll();
        if (web::now_ms() - idleSince > 5000)
            ensureIdleLoop(false);
    }

    void onClick(double x, double y)
    {
        if (y < toolbarH)
        {
            if (x < buttonW)
                goHistory(-1);
            else if (x < 2 * buttonW)
                goHistory(+1);
            else if (x < 3 * buttonW)
            {
                if (loader.busy())
                    loader.cancel(), onLoaded();
                else
                    reload();
            }
            else if (x >= addrX)
                beginEdit(EDIT_ADDRESS);
            return;
        }
        if (edit != EDIT_NONE)
        {
            edit = EDIT_NONE;
            wnd->Repaint();
        }
        //  The scroll bar: a click above or below the thumb pages, a click on
        //  the track anywhere else jumps there.
        if (x >= contentX + contentW && y >= contentY && y < contentY + contentH)
        {
            int m = maxScroll();
            if (m <= 0)
                return;
            double frac = (y - contentY) / contentH;
            scrollTo((int)(frac * (m + visibleRows)) - visibleRows / 2);
            return;
        }
        int l = linkAt(x, y);
        if (l >= 0)
        {
            focusedLink = l;
            followLink(l);
        }
    }

    void onKey(PlatformKey *key)
    {
        if (edit != EDIT_NONE)
        {
            onEditKey(key);
            return;
        }
        bool ctrl = key->isLeftControl || key->isRightControl;
        bool alt = key->isLeftAlt || key->isRightAlt;
        bool shift = key->isLeftShift || key->isRightShift;
        int page = visibleRows > 2 ? visibleRows - 2 : 1;

        if (key->isEscape)
        {
            if (loader.busy())
            {
                loader.cancel();
                onLoaded();
                return;
            }
            ensureIdleLoop(false);
            wnd->Close();
            return;
        }
        if ((ctrl && key->isChar && (key->theChar == 'l' || key->theChar == 'L')) ||
            (key->isF && key->f == 6))
        {
            beginEdit(EDIT_ADDRESS);
            return;
        }
        if ((ctrl && key->isChar && (key->theChar == 'r' || key->theChar == 'R')) || (key->isF && key->f == 5))
        {
            reload();
            return;
        }
        if (alt && key->isArrowLeft)
        {
            goHistory(-1);
            return;
        }
        if (alt && key->isArrowRight)
        {
            goHistory(+1);
            return;
        }
        if (key->isBackspace)
        {
            goHistory(-1);
            return;
        }
        if (key->isTab)
        {
            focusLink(shift ? -1 : +1);
            return;
        }
        if (key->isEnter)
        {
            if (focusedLink >= 0)
                followLink(focusedLink);
            return;
        }
        //  Space presses a focused button or box instead of scrolling.
        int fc = focusedLink >= 0 ? doc.linkControl(focusedLink) : -1;
        if (fc >= 0 && key->isChar && key->theChar == ' ' && !doc.control(fc).isText())
        {
            activateControl(fc, shift);
            return;
        }
        if (key->isArrowUp)
            scrollTo(scrollRow - 1);
        else if (key->isArrowDown)
            scrollTo(scrollRow + 1);
        else if (key->isPageUp)
            scrollTo(scrollRow - page);
        else if (key->isPageDown || (key->isChar && key->theChar == ' '))
            scrollTo(scrollRow + page);
        else if (key->isHome)
            scrollTo(0);
        else if (key->isEnd)
            scrollTo(maxScroll());
        else if (key->isChar && key->theChar == '/')
            beginEdit(EDIT_FIND);
        else if (key->isChar && key->theChar == 'n')
            findNext(false);
    }

    // ── Paint ─────────────────────────────────────────────────────────────────

    void makeResources(PlatformDrawingContext *dc)
    {
        if (cBg)
            return;
        //  Everything is quantised to the 16 EGA colours, so these are those.
        cBg = dc->CreateColor(0xFFFFFFFF, nullptr, nullptr);
        cText = dc->CreateColor(0xFF000000, nullptr, nullptr);
        cLink = dc->CreateColor(0xFF0000AA, nullptr, nullptr);
        cFaint = dc->CreateColor(0xFF555555, nullptr, nullptr);
        cCode = dc->CreateColor(0xFFAA0000, nullptr, nullptr);
        cHead = dc->CreateColor(0xFF0000AA, nullptr, nullptr);
        cChrome = dc->CreateColor(0xFFAAAAAA, nullptr, nullptr);
        cChromeDark = dc->CreateColor(0xFF555555, nullptr, nullptr);
        cWhite = dc->CreateColor(0xFFFFFFFF, nullptr, nullptr);
        cMark = dc->CreateColor(0xFFFFFF55, nullptr, nullptr);
        cFocus = dc->CreateColor(0xFF0000AA, nullptr, nullptr);
        static const uint32_t ega[16] = {0xFF000000, 0xFF0000AA, 0xFF00AA00, 0xFF00AAAA, 0xFFAA0000, 0xFFAA00AA,
                                         0xFFAA5500, 0xFFAAAAAA, 0xFF555555, 0xFF5555FF, 0xFF55FF55, 0xFF55FFFF,
                                         0xFFFF5555, 0xFFFF55FF, 0xFFFFFF55, 0xFFFFFFFF};
        for (int k = 0; k < 16; k++)
            cPal[k] = dc->CreateColor(ega[k], nullptr, nullptr);
        font = dc->CreateFont(6, nullptr, false, false, false, nullptr, nullptr);
        bigFont = dc->CreateFont(12, nullptr, false, false, false, nullptr, nullptr);
        if (font)
        {
            Coord w, h;
            if (font->GetDrawnTextSize("MMMMMMMMMM", w, h))
            {
                cw = COORD_VAL(w) / 10;
                ch = COORD_VAL(h);
            }
        }
        if (bigFont)
        {
            Coord w, h;
            if (bigFont->GetDrawnTextSize("MMMMMMMMMM", w, h))
                bcw = COORD_VAL(w) / 10;
        }
        if (cw <= 0)
            cw = 3;
        if (ch <= 0)
            ch = 6;
        if (bcw <= 0)
            bcw = cw * 2;
        rowH = ch + 1;
    }

    void text(PlatformBitmap *t, double x, double y, double w, double h, const char *s, PlatformColor *c,
              PlatformFont *f, bool bold)
    {
        PlatformDrawTextOptions o{};
        o.font = f;
        o.foreground = c;
        o.horizontalAlign = PlatformAlign::Begin;
        o.verticalAlign = PlatformAlign::Begin;
        t->DrawText(Coord(x), Coord(y), Coord(w), Coord(h), (const mchar *)s, &o, false);
        //  The font has no bold: the same glyphs one pixel (half a unit at
        //  this DPI) to the right thicken every vertical stroke.
        if (bold)
            t->DrawText(Coord(x + 0.5), Coord(y), Coord(w), Coord(h), (const mchar *)s, &o, false);
    }

    //  Fits text into a number of cells, keeping its end when it is too long.
    static void fitTail(char *out, const char *s, int cells)
    {
        int l = (int)strlen(s);
        if (cells < 4)
            cells = 4;
        if (l <= cells)
        {
            web::scopy(out, s, (size_t)cells + 1);
            return;
        }
        web::scopy(out, "...", 4);
        web::scat(out, s + l - (cells - 3), (size_t)cells + 1);
    }

    static void fitHead(char *out, const char *s, int cells)
    {
        int l = (int)strlen(s);
        if (cells < 4)
            cells = 4;
        if (l <= cells)
        {
            web::scopy(out, s, (size_t)cells + 1);
            return;
        }
        web::scopyn(out, s, (size_t)(cells - 3), (size_t)cells + 1);
        web::scat(out, "...", (size_t)cells + 1);
    }

    void paintChrome(PlatformBitmap *t, double W)
    {
        //  Toolbar: back, forward, reload/stop, then the address.
        toolbarH = rowH + 4;
        t->FillRect(0, 0, Coord(W), Coord(toolbarH), cChrome, false);
        t->FillRect(0, Coord(toolbarH - 0.5), Coord(W), Coord(0.5), cChromeDark, false);
        buttonW = (int)(cw * 3 + 3);
        const char *labels[3] = {"<", ">", loader.busy() ? "x" : "R"};
        bool enabled[3] = {histPos > 0, histPos + 1 < histLen, true};
        for (int i = 0; i < 3; i++)
        {
            double bx = i * buttonW + 1;
            t->FillRect(Coord(bx), 1.5, Coord(buttonW - 1), Coord(toolbarH - 3), cWhite, false);
            text(t, bx + (buttonW - 1 - cw) / 2, 2 + (toolbarH - 4 - ch) / 2, cw * 2, ch, labels[i],
                 enabled[i] ? cText : cChrome, font, true);
        }
        addrX = 3 * buttonW + 2;
        double aw = W - addrX - 2;
        t->FillRect(Coord(addrX), 1.5, Coord(aw), Coord(toolbarH - 3), cWhite, false);

        int cells = (int)((aw - 3) / cw) - 1;
        char shown[ADDR_CAP + 16];
        if (edit == EDIT_FIND)
        {
            char tmp[ADDR_CAP + 8] = "Find: ";
            web::scat(tmp, editBuf, sizeof(tmp));
            web::scat(tmp, "_", sizeof(tmp));
            fitTail(shown, tmp, cells);
        }
        else if (edit == EDIT_ADDRESS)
        {
            char tmp[ADDR_CAP + 2];
            web::scopy(tmp, editBuf, sizeof(tmp));
            if (!editFresh)
                web::scat(tmp, "_", sizeof(tmp));
            fitTail(shown, tmp, cells);
        }
        else
            fitHead(shown, loader.busy() ? attempted : current, cells);
        double ty = 2 + (toolbarH - 4 - ch) / 2;
        if (edit == EDIT_ADDRESS && editFresh)
        {
            //  Selected: the next key replaces it.
            double tw = strlen(shown) * cw;
            t->FillRect(Coord(addrX + 1.5), Coord(ty), Coord(tw), Coord(ch), cFocus, false);
            text(t, addrX + 1.5, ty, aw - 3, ch, shown, cWhite, font, false);
        }
        else
            text(t, addrX + 1.5, ty, aw - 3, ch, shown, cText, font, false);
    }

    //  Where a link goes, the way the address bar would show it once there.
    void linkForDisplay(int idx, char *out, size_t cap)
    {
        int ci = doc.linkControl(idx);
        if (ci >= 0)
        {
            static const char *const kinds[] = {"Text field",   "Password field", "Text area", "Checkbox",
                                                "Radio button", "List",           "Button",    "Reset button",
                                                "Button",       "Button",         ""};
            const web::Control &c = doc.control(ci);
            web::scopy(out, kinds[c.type], cap);
            if (doc.str(c.name)[0])
            {
                web::scat(out, " \"", cap);
                web::scat(out, doc.str(c.name), cap);
                web::scat(out, "\"", cap);
            }
            if (c.type == web::Control::SUBMIT || c.type == web::Control::IMAGE)
            {
                web::scat(out, c.form < 0 ? " (needs JavaScript)" : doc.formPost(c.form) ? ": sends the form (POST)"
                                                                                       : ": sends the form",
                          cap);
            }
            else if (c.isText())
                web::scat(out, edit == EDIT_FIELD && editControl == ci ? ": typing; Enter sends, Esc stops"
                                                                       : ": Enter to type into it",
                          cap);
            else if (c.type == web::Control::SELECT)
                web::scat(out, ": Space or Enter for the next option", cap);
            return;
        }
        const char *href = doc.linkHref(idx);
        web::Url base, u;
        if (!web::istarts(href, "about:") && web::urlFromInput(current, base) && web::urlResolve(base, href, u))
            web::urlFormat(u, out, cap);
        else
            web::scopy(out, href, cap);
    }

    void paintStatus(PlatformBitmap *t, double W, double H)
    {
        double sh = rowH + 2;
        t->FillRect(0, Coord(H - sh), Coord(W), Coord(sh), cChrome, false);
        char s[ADDR_CAP];
        if (loader.busy())
            web::scopy(s, loader.status(), sizeof(s));
        else if (edit == EDIT_FIELD && focusedLink >= 0)
            linkForDisplay(focusedLink, s, sizeof(s));
        else if (hoverLink >= 0)
            linkForDisplay(hoverLink, s, sizeof(s));
        else if (focusedLink >= 0)
            linkForDisplay(focusedLink, s, sizeof(s));
        else if (message[0])
            web::scopy(s, message, sizeof(s));
        else
        {
            s[0] = 0;
            if (doc.rows() > visibleRows)
            {
                int pct = maxScroll() ? scrollRow * 100 / maxScroll() : 100;
                web::scatInt(s, pct, sizeof(s));
                web::scat(s, "%  ", sizeof(s));
            }
            web::scatInt(s, doc.linkCount(), sizeof(s));
            web::scat(s, doc.linkCount() == 1 ? " link" : " links", sizeof(s));
        }
        web::scopy(lastStatus, loader.status(), sizeof(lastStatus));
        char shown[ADDR_CAP];
        fitHead(shown, s, (int)((W - 4) / cw) - 1);
        text(t, 2, H - sh + 1, W - 4, ch, shown, cText, font, false);
    }

    void paintPage(PlatformBitmap *t)
    {
        t->FillRect(Coord(contentX - 1), Coord(contentY - 1), Coord(contentW + 1), Coord(contentH + 1), cBg, false);

        int cols = (int)((contentW - 2) / cw);
        if (cols != doc.layoutCols())
        {
            doc.layout(cols);
            if (scrollRow > maxScroll())
                scrollRow = maxScroll();
        }

        t->SetClip(Coord(contentX), Coord(contentY), Coord(contentW), Coord(contentH), false);
        char buf[512];
        for (size_t li = doc.lineAtRow(scrollRow); li < doc.lineCount(); li++)
        {
            const web::Line &l = doc.line(li);
            if (l.row >= scrollRow + visibleRows)
                break;
            double y = contentY + (l.row - scrollRow) * rowH;
            if (l.hr)
            {
                t->FillRect(Coord(contentX + 1), Coord(y + rowH / 2), Coord(cols * cw), Coord(0.5), cFaint, false);
                continue;
            }
            double cell = l.big ? bcw : cw;
            double lh = l.big ? rowH * 2 : rowH;
            for (uint32_t r = 0; r < l.nRuns; r++)
            {
                const web::Run &ru = doc.run(l.firstRun + r);
                size_t n = ru.len < sizeof(buf) - 1 ? ru.len : sizeof(buf) - 1;
                memcpy(buf, doc.text(ru.off), n);
                buf[n] = 0;
                double x = contentX + 1 + ru.col * cell;
                double w = n * cell;

                bool isLink = ru.link >= 0;
                bool focused = isLink && ru.link == focusedLink;
                bool found = li == findLine && r == findRun;
                int ci = (ru.style & web::ST_CTRL) ? doc.linkControl(ru.link) : -1;

                if (ci >= 0)
                {
                    //  A form control: its cells come from its state, not the text.
                    char cells[256];
                    const web::Control &c = doc.control(ci);
                    bool live = doc.controlCells(ci, cells, sizeof(cells));
                    if (live)
                    {
                        size_t from = ru.off - c.textOff;
                        size_t cl = strlen(cells);
                        size_t k = 0;
                        for (; k < n && from + k < cl; k++)
                            buf[k] = cells[from + k];
                        buf[k] = 0;
                    }
                    bool typing = edit == EDIT_FIELD && editControl == ci;
                    PlatformColor *box = focused || typing ? cFocus : cChrome;
                    t->FillRect(Coord(x), Coord(y), Coord(w), Coord(lh - 1), box, false);
                    text(t, x, y, w + cell, lh, buf, focused || typing ? cWhite : cText, font, !live);
                    continue;
                }

                //  The page's colours, kept readable: a colour too close to
                //  what is behind it gives way to black or white.
                int bgIdx = ru.bg ? ru.bg - 1 : 15;
                int fgIdx = isLink ? 1 : ru.fg ? ru.fg - 1 : l.big ? 1 : (ru.style & web::ST_FAINT) ? 8 : 0;
                int dl = web::cssLuma((uint8_t)fgIdx) - web::cssLuma((uint8_t)bgIdx);
                if (dl < 0)
                    dl = -dl;
                if (dl < 90)
                    fgIdx = web::cssLuma((uint8_t)bgIdx) < 128 ? 15 : 0;
                PlatformColor *fg = cPal[fgIdx];
                if (focused)
                {
                    t->FillRect(Coord(x), Coord(y), Coord(w), Coord(lh - 1), cFocus, false);
                    fg = cWhite;
                }
                else if (found)
                    t->FillRect(Coord(x), Coord(y), Coord(w), Coord(lh - 1), cMark, false);
                else if (bgIdx != 15)
                    t->FillRect(Coord(x), Coord(y), Coord(w), Coord(lh - 1), cPal[bgIdx], false);

                text(t, x, y, w + cell, lh, buf, fg, l.big ? bigFont : font, (ru.style & web::ST_BOLD) || l.big);
                if ((isLink || (ru.style & web::ST_UNDER)) && !focused)
                    t->FillRect(Coord(x), Coord(y + lh - 1.5), Coord(w), Coord(0.5),
                                isLink && ru.link == hoverLink ? cText : fg, false);
            }
        }
        t->ClearClip();

        //  The scroll bar.
        double sx = contentX + contentW;
        t->FillRect(Coord(sx), Coord(contentY - 1), Coord(SCROLL_W), Coord(contentH + 1), cChrome, false);
        int total = doc.rows() > 0 ? doc.rows() : 1;
        if (total > visibleRows)
        {
            double th = contentH * visibleRows / total;
            if (th < 4)
                th = 4;
            double ty = contentY + (contentH - th) * scrollRow / (maxScroll() ? maxScroll() : 1);
            t->FillRect(Coord(sx + 1), Coord(ty), Coord(SCROLL_W - 2), Coord(th), cChromeDark, false);
        }
    }

    void OnPaint(PlatformDrawingContext *dc, PlatformBitmap *target)
    {
        if (!target)
            return;
        makeResources(dc);
        if (!cBg || !font || !bigFont)
            return;

        Coord Wc = target->GetWidth();
        Coord Hc = target->GetHeight();
        double W = COORD_VAL(Wc), H = COORD_VAL(Hc);
        winW = W;

        paintChrome(target, W);
        double sh = rowH + 2;
        contentX = 2;
        contentY = toolbarH + 1;
        contentW = W - contentX - SCROLL_W - 1;
        contentH = H - contentY - sh - 1;
        visibleRows = (int)(contentH / rowH);
        if (visibleRows < 1)
            visibleRows = 1;
        paintPage(target);
        paintStatus(target, W, H);
    }
};
