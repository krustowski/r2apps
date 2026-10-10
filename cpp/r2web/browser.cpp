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
//   Ctrl+D                                 dark mode on/off (the "D"/"L" button too)
//   Ctrl+C                                 copy the focused link's address, or
//                                          the page's when no link is focused
//   Ctrl+V                                 paste, in the address bar, the find
//                                          box and a form's text field
//   Ctrl+Space, Shift+F10, right click     the menu: open, open in a new window
//                                          or copy the link, paste, back, ...
//                                          Arrows and Enter or the underlined
//                                          letter pick, Esc closes it.
//   / then n                               find on the page, find again
//   Esc                                    stop loading; close the window
// On a form field, Enter (or a click) starts typing into it; Enter again
// submits the form, Esc stops, Tab moves on.  Space or Enter ticks a box,
// picks a radio button, steps through a list's options (Shift: backwards)
// and presses a button.
// The address bar also takes ":dns <ip>", ":gw <ip>", ":css on|off",
// ":img on|off", ":dark on|off", "about:" pages, and "file:/mnt/..." (or just
// "/mnt/...") for a picture, an .htm(l) page (its relative pictures and links
// are files beside it) or text on a disk.
//
// Pictures (PNG, JPEG, GIF, BMP; web/image.h) are fetched one at a time
// after the page and its style sheets, and each takes the place of its alt
// text once it is in: a line of its own, as wide as it is or as the page,
// in the screen's colours.  A page shows its first MAX_PICS.  An address
// that answers with a picture shows the picture.
//

#include "../memento-hello/web/css.h"
#include "../memento-hello/web/doc.h"
#include "../memento-hello/web/image.h"
#include "../memento-hello/web/loader.h"
#include "../memento-hello/web/net_r2.h"
#include "ui/platform/impl/r2/R2_BitmapImpl.h"
#include "script.h"

class BrowserWindow
{
public:
    BrowserWindow() : loader(web::r2Net(), web::gatherEntropy, web::currentTime)
    {
        for (web::Buf &b : sheetBody)
            b.big = true;
    }

    ~BrowserWindow() { clearPics(); }

    static void onEvent(void *instance, struct PlatformWindowInterfaceInputEvent *data)
    {
        reinterpret_cast<BrowserWindow *>(instance)->onEvent_(data);
    }

    void SetWindow(PlatformWindow *w)
    {
        wnd = w;
        //  A window opened from another one's menu starts at the link.
        char first[ADDR_CAP];
        web::scopy(first, g_host->initialUrl, sizeof(first));
        openNext[0] = 0;
        navigate(first, true);
    }

private:
    static const int ADDR_CAP = 1200;
    static const int HIST_CAP = 16;
    static const int SCROLL_W = 5;
    static const int BUTTONS = 4; // back, forward, reload/stop, dark/light

    PlatformWindow *wnd = nullptr;

    web::Loader loader;
    web::Document doc;
    web::ScriptPage script;
    bool jsOn = true;
    int scriptNext = -1;
    //  The page's own loop: when it was last laid out again and when its
    //  animation frames last ran (both at a few per second at most: a layout
    //  is a whole re-read of the page).
    uint64_t lastScriptRender = 0, lastFrame = 0;
    char lastScriptError[96] = {};
    static const uint64_t SCRIPT_RENDER_MS = 150, FRAME_MS = 50;

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

    //  Pictures, kept by address so that the page read again (its style
    //  sheets came, CSS on or off) finds the ones it had.  One that could not
    //  be had stays here without pixels, so it is not asked for again.
    struct Pic
    {
        char url[ADDR_CAP];
        web::Picture pic;
    };
    static const int MAX_PICS = 16;
    static const int PIC_MAX_H = 480; // pixels; a taller one is made smaller
    static const int MAX_DOC_IMAGES = 256;
    Pic pics[MAX_PICS];
    int nPics = 0;
    int imgNext = -1; // the page's picture being fetched; -1 when none is
    bool imagesOn = true;
    int16_t picOf[MAX_DOC_IMAGES]; // the page's picture i is pics[picOf[i]], or -1
    bool pageIsFile = false;       // the page came from a disk: file:...
    char pageDir[ADDR_CAP] = {};   // and this is its directory, ending in '/'
    double pxPerUnit = 2;          // screen pixels a unit, as last painted
    char pageTiming[96] = {};
    char editBuf[ADDR_CAP] = {};
    int editLen = 0;
    bool editFresh = false; // the first key replaces the whole text

    char findText[64] = {};
    size_t findLine = (size_t)-1, findRun = 0;

    char message[96] = {};    // one-off status text ("not found", ...)
    //  The message was just set by something the user did ("Copied: ..."):
    //  it goes before the link under the pointer until the next key or move.
    bool messageFirst = false;

    //  Where the next BrowserWindow goes first; "Open in new window" sets it
    //  just before asking main.cpp for the window.
    static inline char openNext[ADDR_CAP] = {};

    //  The context menu.  Its items are made when it opens, from what it was
    //  opened on; its geometry when it is painted, and kept for the mouse.
    enum Action : uint8_t
    {
        A_SEP,
        A_OPEN,
        A_OPEN_NEW,
        A_COPY_LINK,
        A_COPY_TEXT,
        A_PASTE_FIELD,
        A_BACK,
        A_FORWARD,
        A_RELOAD,
        A_STOP,
        A_COPY_PAGE,
        A_PASTE_GO,
        A_FIND,
        A_DARK,
    };
    struct MenuItem
    {
        Action action;
        bool enabled;
        const char *label; // '&' before the letter that picks it
        const char *keys;  // the shortcut, shown on the right
    };
    static const int MENU_CAP = 16;
    MenuItem menu[MENU_CAP];
    int menuLen = 0;
    bool menuOpen = false;
    int menuSel = -1;
    int menuLink = -1;         // the link it was opened on, or -1 for the page
    double menuX = 0, menuY = 0; // where it was asked for
    double menuL = 0, menuT = 0, menuW = 0; // where it is drawn
    double winH = 0;
    char lastStatus[96] = {}; // what the loader said last time we painted
    bool imEnabled = false;
    uint64_t idleSince = 0;
    uint64_t lastRepaint = 0;

    //  Light or dark: which of the 16 EGA colours the window's own parts are
    //  drawn in.  The page's colours are not in here; see pageColour().
    struct Theme
    {
        uint8_t bg, text, faint, chrome, chromeDark, field, onFocus, mark, focus;
    };
    static constexpr Theme LIGHT = {15, 0, 8, 7, 8, 15, 15, 14, 1};
    static constexpr Theme DARK = {0, 15, 7, 8, 7, 0, 15, 6, 1};
    bool dark = false;

    // Colours and fonts, made on the first paint; the theme picks from cPal.
    PlatformColor *cBg = nullptr, *cText = nullptr, *cFaint = nullptr;
    PlatformColor *cChrome = nullptr, *cChromeDark = nullptr, *cMark = nullptr, *cFocus = nullptr;
    PlatformColor *cField = nullptr;   // the buttons and the address box
    PlatformColor *cOnFocus = nullptr; // text on cFocus
    PlatformColor *cPal[16] = {};      // the EGA palette, for the page's own colours
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
        menuOpen = false;
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
        script.clear(); scriptNext = -1;
        web::Buf b;
        pageBody.release();
        if (!strcmp(what, "about:console"))
        {
            size_t n = 0;
            const char *log = web::scriptConsole(&n);
            b.appendStr("<title>Console</title><h2>What page scripts logged</h2><pre>");
            web::Buf text;
            text.append(log, n);
            escapeInto(b, n ? text.cstr() : "(nothing yet)");
            b.appendStr("</pre>");
            doc.loadHtml(b.data, b.len, "utf-8");
            showDocument(what, push, 0);
            return;
        }
        if (!strcmp(what, "about:net") || !strcmp(what, "about:"))
        {
            char net[400];
            web::r2NetDescribe(net, sizeof(net));
            b.appendStr("<title>About</title><h1>r2web</h1>"
                        "<p>A small web browser for rou2exOS: HTTP/1.1, TLS 1.2 by BearSSL, and HTML "
                        "with CSS, pictures and JavaScript (QuickJS: ES2023, a DOM, timers, fetch, EventSource), "
                        "in the kernel's own font.</p><h3>Network</h3><p>");
            escapeInto(b, net);
            b.appendStr("</p><p>Address, gateway and DNS are the ones the eth driver got by DHCP (or was "
                        "given). Override with <code>:dns 9.9.9.9</code> or <code>:gw 192.168.1.1</code> in "
                        "the address bar; <code>:css off</code> shows pages without their style sheets, "
                        "<code>:img off</code> without their pictures (PNG, JPEG, GIF and BMP, the first "
                        "16 on a page), and "
                        "<code>:js off</code> disables page scripts (what they logged is on "
                        "<a href=\"about:console\">about:console</a>), and "
                        "<code>:dark on</code> (or Ctrl+D) draws them light on dark. Under "
                        "QEMU with a tap, the guest needs NAT on the host for its network.</p>"
                        "<h3>Keys</h3><pre>"
                        "Up/Down PgUp/PgDn Space Home/End   scroll\n"
                        "Tab / Shift+Tab, Enter             walk links, follow\n"
                        "Backspace, Alt+Left / Alt+Right    back, forward\n"
                        "Ctrl+L or F6                       edit the address\n"
                        "F5 or Ctrl+R                       reload\n"
                        "Ctrl+D                             dark mode on/off\n"
                        "Ctrl+C                             copy the link's (or page's) address\n"
                        "Ctrl+V                             paste\n"
                        "Ctrl+Space, Shift+F10, right click the menu\n"
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
        script.clear(); scriptNext = -1;
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
        menuOpen = false;
        doc.layout(0);
        setTitle("Problem loading page");
        wnd->Repaint();
    }

    void startLoad(const web::Url &u, bool insecure, bool push, int scroll, const web::Buf *post = nullptr)
    {
        script.clear(); scriptNext = -1;
        attemptedUrl = u;
        web::urlFormat(u, attempted, sizeof(attempted));
        pushOnLoad = push;
        scrollOnLoad = scroll;
        message[0] = 0;
        sheetNext = -1;
        imgNext = -1;
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
        if (web::istarts(text, "file:"))
        {
            openFile(text, push, scroll);
            return;
        }
        if (text[0] == '/')
        {
            //  A path, as the shell would take it: a file on a disk.
            char addr[ADDR_CAP] = "file:";
            web::scat(addr, text, sizeof(addr));
            openFile(addr, push, scroll);
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
        if (web::istarts(cmd, "js ")) {
            ok = web::ieq(cmd + 3, "on") || web::ieq(cmd + 3, "off");
            if (ok) { jsOn = web::ieq(cmd + 3, "on"); reload(); }
        }
        else if (web::istarts(cmd, "dns "))
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
        else if (web::istarts(cmd, "img "))
        {
            ok = web::ieq(cmd + 4, "on") || web::ieq(cmd + 4, "off");
            if (ok && imagesOn != web::ieq(cmd + 4, "on"))
            {
                imagesOn = !imagesOn;
                if (imgNext >= 0)
                {
                    loader.cancel();
                    imgNext = -1;
                }
                applyPictures();
                if (imagesOn)
                    startImages();
                wnd->Repaint();
            }
        }
        else if (web::istarts(cmd, "dark "))
        {
            ok = web::ieq(cmd + 5, "on") || web::ieq(cmd + 5, "off");
            if (ok)
                setDark(web::ieq(cmd + 5, "on"));
        }
        web::scopy(message,
                   ok ? "Setting changed." : "Unknown command; try :dns <ip>, :gw <ip>, :css on|off, :img on|off or :dark on|off",
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
        //  A link of a page with scripts: the page has the click first.
        int node = scriptNode(doc.linkHandler(idx));
        char href[ADDR_CAP];
        web::scopy(href, doc.linkHref(idx), sizeof(href));
        if (node >= 0)
        {
            bool go = script.click(node);
            if (updateScriptPage() || !go)
                return;
        }
        else if (jsOn && script.active() && web::istarts(href, "javascript:"))
        {
            script.eval(href + 11);
            updateScriptPage();
            return;
        }
        followHref(href);
    }

    void followHref(const char *href)
    {
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
        char local[ADDR_CAP];
        if (localAddress(href, local, sizeof(local)))
        {
            navigate(local, true);
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

    //  The page's node behind an "r2:N" handler, when its scripts run.
    int scriptNode(const char *handler) const
    {
        return jsOn && script.active() ? web::ScriptPage::nodeOf(handler) : -1;
    }

    //  The control for node N after the page was laid out again, or -1.
    int controlOfNode(int node) const
    {
        for (int k = 0; k < doc.controlCount(); k++)
            if (web::ScriptPage::nodeOf(doc.str(doc.control(k).onclick)) == node)
                return k;
        return -1;
    }

    void activateControl(int ci, bool backwards)
    {
        int node = scriptNode(doc.str(doc.control(ci).onclick));
        if (node >= 0)
        {
            web::Control &k = doc.control(ci);
            if (k.type == web::Control::CHECKBOX || k.type == web::Control::RADIO || k.type == web::Control::SELECT)
            {
                //  The change first, as a browser makes it; the page may take
                //  it back by cancelling the click.
                doc.controlActivate(ci, backwards);
                formsTouched = true;
                if (!script.state(node, doc.control(ci).checked, doc.control(ci).selected))
                    doc.controlActivate(ci, !backwards);
                updateScriptPage();
                wnd->Repaint();
                return;
            }
            bool go = script.click(node);
            if (updateScriptPage() || !go)
                return;
            ci = controlOfNode(node);
            if (ci < 0)
                return;
        }
        web::Control &c = doc.control(ci);
        if (c.isText())
        {
            edit = EDIT_FIELD;
            editControl = ci;
            if (node >= 0)
                script.focus(node);
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
            web::scopy(message, "This button has no supported JavaScript handler.",
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
    void renderPage(const web::StyleSheetText *sheets, int n, bool preserveControls = false)
    {
        // A script can change markup after the user has typed into a form.
        // Keep surviving controls' values across the HTML layout rebuild.
        struct Saved { uint32_t index, type, checked; int32_t selected; char id[128]; uint32_t len; };
        web::Buf saved{true};
        if (preserveControls) for (int i = 0; i < doc.controlCount(); ++i) {
            const auto &c = doc.control(i);
            Saved s{}; s.index = i; s.type = c.type; s.checked = c.checked; s.selected = c.selected;
            web::scopy(s.id, doc.str(c.id), sizeof(s.id));
            s.len = c.isText() ? c.editLen : 0;
            saved.append(&s, sizeof(s));
            if (s.len) saved.append(c.edit, s.len);
            saved.push(0);
        }
        doc.loadHtml(pageBody.data, pageBody.len, pageCharset, sheets, n, cssOn);
        if (!saved.failed) for (size_t at = 0; at + sizeof(Saved) < saved.len;) {
            Saved s; memcpy(&s, saved.data + at, sizeof(s)); at += sizeof(s);
            int i = -1;
            if (s.id[0]) { for (int k = 0; k < doc.controlCount(); ++k) if (!strcmp(s.id, doc.str(doc.control(k).id))) { i = k; break; } }
            else if (s.index < (uint32_t)doc.controlCount()) i = s.index;
            if (i >= 0 && doc.control(i).type == s.type) {
                doc.control(i).checked = s.checked; doc.control(i).selected = s.selected;
                if (doc.control(i).isText()) doc.controlSetText(i, (const char *)saved.data + at);
            }
            at += s.len + 1;
        }
        doc.layout(0);
        applyPictures();
        focusedLink = hoverLink = -1;
        menuOpen = false;
        findLine = (size_t)-1;
        formsTouched = false;
        setTitle(doc.title());
    }

    //  The page's DOM laid out again, focus and typing kept on the same nodes.
    void renderScriptPage()
    {
        int focusNode = focusedLink >= 0 ? web::ScriptPage::nodeOf(doc.linkHandler(focusedLink)) : -1;
        int editNode = edit == EDIT_FIELD && editControl >= 0 && editControl < doc.controlCount()
                           ? web::ScriptPage::nodeOf(doc.str(doc.control(editControl).onclick))
                           : -1;
        int scroll = scrollRow;
        web::StyleSheetText sheets[MAX_SHEETS];
        for (int i = 0; i < nSheets; ++i) sheets[i] = {sheetBody[i].data, sheetBody[i].len};
        web::scopy(pageCharset, "utf-8", sizeof(pageCharset));
        renderPage(sheets, cssOn ? nSheets : 0, false);
        scrollRow = scroll;
        if (focusNode >= 0)
            for (int i = 0; i < doc.linkCount(); i++)
                if (web::ScriptPage::nodeOf(doc.linkHandler(i)) == focusNode)
                {
                    focusedLink = i;
                    break;
                }
        if (editNode >= 0)
        {
            editControl = controlOfNode(editNode);
            if (editControl < 0)
                edit = EDIT_NONE;
        }
        lastScriptRender = web::now_ms();
        //  Pictures the page's scripts added are fetched like the page's own.
        if (imagesOn && imgNext < 0 && sheetNext < 0 && scriptNext < 0 && !loader.busy())
            startImages();
    }

    //  What the page's scripts did since we last looked: a new layout, a
    //  message, an address, a form to send, a page to go to.  True when the
    //  browser went somewhere else.
    bool updateScriptPage(bool renderNow = true)
    {
        if (!script.active())
        {
            char why[96];
            if (script.takeStatus(why, sizeof(why)))
                web::scopy(message, why, sizeof(message));
            return false;
        }
        bool repaint = false;
        if (renderNow && script.changed() && script.render(pageBody, nullptr, 0))
        {
            renderScriptPage();
            repaint = true;
        }
        if (script.error()[0] && strcmp(script.error(), lastScriptError))
        {
            web::scopy(lastScriptError, script.error(), sizeof(lastScriptError));
            web::scopy(message, script.error(), sizeof(message));
            repaint = true;
        }
        char t[ADDR_CAP];
        if (script.takeStatus(t, sizeof(t)))
        {
            web::scopy(message, t, sizeof(message));
            messageFirst = true;
            repaint = true;
        }
        if (script.takeUrl(t, sizeof(t)))
        {
            //  pushState or a new #fragment: the same page at a new address.
            web::scopy(current, t, sizeof(current));
            if (histPos >= 0)
                web::scopy(hist[histPos].addr, t, ADDR_CAP);
            repaint = true;
        }
        if (script.takeOpen(t, sizeof(t)))
            openInNewWindow(t);
        if (int h = script.takeHistory())
        {
            goHistory(h);
            return true;
        }
        web::ScriptPage::Submit sub;
        if (script.takeSubmit(sub))
        {
            web::Url u;
            if (!web::urlFromInput(sub.action, u))
            {
                web::scopy(message, "The form goes somewhere this browser cannot follow.", sizeof(message));
                wnd->Repaint();
                return false;
            }
            edit = EDIT_NONE;
            if (sub.post)
                startLoad(u, false, true, 0, &sub.data);
            else
            {
                char *q = strchr(u.path, '?');
                if (q)
                    *q = 0;
                if (sub.data.len && strlen(u.path) + 1 + sub.data.len < sizeof(u.path))
                {
                    web::scat(u.path, "?", sizeof(u.path));
                    web::scat(u.path, sub.data.cstr(), sizeof(u.path));
                }
                startLoad(u, false, true, 0);
            }
            return true;
        }
        if (script.navigation()[0]) {
            char target[ADDR_CAP]; web::scopy(target, script.navigation(), sizeof(target));
            bool replace = script.navigationReplaces();
            script.clearNavigation();
            char local[ADDR_CAP]; web::Url u;
            if (localAddress(target, local, sizeof(local))) navigate(local, !replace);
            else if (web::urlResolve(pageUrl, target, u)) { char addr[ADDR_CAP]; web::urlFormat(u, addr, sizeof(addr)); navigate(addr, !replace); }
            else navigate(target, !replace);
            return true;
        }
        if (repaint)
            wnd->Repaint();
        return false;
    }

    //  The page's own life between the user's actions: its timers, network
    //  and animation frames, and a new layout when its DOM changed.
    void scriptLoop()
    {
        uint64_t now = web::now_ms();
        script.tick();
        if (script.wantsFrame() && now - lastFrame >= FRAME_MS)
        {
            lastFrame = now;
            script.frame((double)now);
        }
        updateScriptPage(now - lastScriptRender >= SCRIPT_RENDER_MS);
    }

    void startScripts()
    {
        scriptNext = -1;
        lastScriptError[0] = 0;
        if (jsOn && script.start(pageBody, pageCharset, current))
        {
            scriptNext = 0;
            ensureIdleLoop(true);
            fetchNextScript();
            return;
        }
        char why[96];
        if (script.takeStatus(why, sizeof(why)))
        {
            web::scopy(message, why, sizeof(message));
            wnd->Repaint();
        }
        afterScripts();
    }
    void afterScripts()
    {
        if (!pageIsFile && cssOn && doc.stylesheetCount()) { sheetNext = 0; fetchNextSheet(); }
        else startImages();
    }
    void fetchNextScript()
    {
        while (scriptNext >= 0 && scriptNext < script.count()) {
            if (!script.src(scriptNext)[0]) {
                const char *code = script.source(scriptNext);
                script.run(scriptNext, code, strlen(code));
                ++scriptNext;
                if (updateScriptPage(false)) return;
                continue;
            }
            char local[ADDR_CAP];
            if (localAddress(script.src(scriptNext), local, sizeof(local))) {
                web::Buf body{true}; bool tooBig;
                if (readLocal(local, body, tooBig)) script.run(scriptNext, body.cstr(), body.len);
                ++scriptNext;
                if (updateScriptPage(false)) return;
                continue;
            }
            web::Url u;
            if (web::urlFromInput(script.src(scriptNext), u)) {
                loader.start(u, false); ensureIdleLoop(true); return;
            }
            ++scriptNext;
        }
        if (scriptNext >= 0)
        {
            scriptNext = -1;
            script.parsed();
            if (updateScriptPage())
                return;
            ensureIdleLoop(true);
        }
        afterScripts();
    }
    void onScriptLoaded()
    {
        auto &r = loader.response();
        if (loader.phase() == web::Loader::FAILED && !strcmp(loader.error(), "Stopped")) {
            r.body.release(); scriptNext = -1;
            web::scopy(message, "Stopped.", sizeof(message));
            wnd->Repaint(); return;
        }
        if (loader.phase() == web::Loader::DONE && r.status == 200 && !r.truncated)
            script.run(scriptNext, r.body.cstr(), r.body.len);
        else web::scopy(message, "Could not load a page script.", sizeof(message));
        r.body.release(); ++scriptNext;
        if (!updateScriptPage(false)) fetchNextScript();
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
        renderPage(texts, cssOn ? nSheets : 0, true);
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
        startImages();
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

    // ── Pictures ──────────────────────────────────────────────────────────────

    void clearPics()
    {
        for (int k = 0; k < nPics; k++)
            pics[k].pic.release();
        nPics = 0;
        imgNext = -1;
    }

    int findPic(const char *url) const
    {
        for (int k = 0; k < nPics; k++)
            if (!strcmp(pics[k].url, url))
                return k;
        return -1;
    }

    //  An address on a page from a disk made into a file: one, when it is
    //  one: "file:..." as it is, and a relative one beside the page.
    bool localAddress(const char *ref, char *out, size_t cap)
    {
        if (web::istarts(ref, "file:"))
        {
            web::scopy(out, ref, cap);
            return true;
        }
        if (!pageIsFile || strstr(ref, "://") || web::istarts(ref, "about:") || !ref[0])
            return false;
        web::scopy(out, "file:", cap);
        if (ref[0] != '/')
            web::scat(out, pageDir, cap);
        web::scat(out, ref, cap);
        return true;
    }

    //  The address of the page's picture i, made whole; `local` when it is
    //  a file.
    bool imageUrl(int i, web::Url &u, char *out, size_t cap, bool *local = nullptr)
    {
        bool isFile = localAddress(doc.imageSrc(i), out, cap);
        if (local)
            *local = isFile;
        if (isFile)
            return true;
        if (!web::urlResolve(pageUrl, doc.imageSrc(i), u))
            return false;
        web::urlFormat(u, out, cap);
        return true;
    }

    //  A file from a disk into `out` (big pool), as much as a response may be.
    bool readLocal(const char *addr, web::Buf &out, bool &tooBig)
    {
        const char *path = addr + 5; // past "file:"
        while (path[0] == '/' && path[1] == '/')
            path++;
        tooBig = false;
        auto size = r2::fs::size_of(path);
        if (!size || !*size)
            return false;
        if (*size > web::HttpResponse::MAX_BODY)
        {
            tooBig = true;
            return false;
        }
        out.release();
        out.big = true;
        if (!out.reserve(*size))
            return false;
        int64_t got = r2::fs::read_at(path, r2::byte_span(out.data, *size), 0);
        if (got <= 0)
            return false;
        out.len = (size_t)got;
        return true;
    }

    static bool hasExt(const char *s, const char *ext)
    {
        size_t n = strlen(s), e = strlen(ext);
        return n >= e && web::ieq(s + n - e, ext);
    }

    static bool isPictureName(const char *s)
    {
        return hasExt(s, ".png") || hasExt(s, ".jpg") || hasExt(s, ".jpeg") || hasExt(s, ".gif") ||
               hasExt(s, ".bmp");
    }

    void openFile(const char *addr, bool push, int scroll)
    {
        script.clear(); scriptNext = -1;
        loader.cancel();
        sheetNext = imgNext = -1;
        web::scopy(attempted, addr, sizeof(attempted));
        web::Buf data{true};
        bool tooBig;
        if (!readLocal(addr, data, tooBig))
        {
            showError("Could not open the file", tooBig ? "It is too big (768 KiB at most)." : "It is not there, or cannot be read.", false);
            return;
        }
        pageBody.release();
        for (web::Buf &b : sheetBody)
            b.release();
        nSheets = 0;
        clearPics();
        pageIsFile = true;
        const char *path = addr + 5;
        while (path[0] == '/' && path[1] == '/')
            path++;
        web::scopy(pageDir, path, sizeof(pageDir));
        char *slash = strrchr(pageDir, '/');
        if (slash)
            slash[1] = 0;
        else
            web::scopy(pageDir, "/", sizeof(pageDir));

        bool html = hasExt(addr, ".htm") || hasExt(addr, ".html");
        if (isPictureName(addr))
            showPicture(addr, data.data, data.len, false);
        else if (html)
        {
            pageBody.data = data.data; // taken over
            pageBody.len = data.len;
            pageBody.cap = data.cap;
            data.data = nullptr;
            data.len = data.cap = 0;
            pageCharset[0] = 0;
            renderPage(nullptr, 0);
        }
        else
            doc.loadText(data.data, data.len, "");
        web::scopy(message, "From the disk, ", sizeof(message));
        web::scatInt(message, (long)((pageBody.len + data.len + 1023) / 1024), sizeof(message));
        web::scat(message, " KiB.", sizeof(message));
        web::scopy(pageTiming, message, sizeof(pageTiming));
        showDocument(addr, push, scroll);
        if (html)
            startScripts();
    }

    //  Tells the page how big each picture it has is (0 for those it has
    //  not), which lays it out again at the next paint.
    void applyPictures()
    {
        for (int i = 0; i < doc.imageCount(); i++)
        {
            int k = -1;
            web::Url u;
            char url[ADDR_CAP];
            if (imagesOn && i < MAX_DOC_IMAGES && imageUrl(i, u, url, sizeof(url)))
                k = findPic(url);
            if (k >= 0 && !pics[k].pic.px)
                k = -1;
            if (i < MAX_DOC_IMAGES)
                picOf[i] = (int16_t)k;
            doc.setImageSize(i, k >= 0 ? pics[k].pic.w : 0, k >= 0 ? pics[k].pic.h : 0);
        }
    }

    void startImages()
    {
        if (!imagesOn || !doc.imageCount() || loader.busy())
            return;
        imgNext = 0;
        fetchNextImage();
    }

    //  The next picture the page has and this window has not, the first
    //  MAX_PICS of them.
    void fetchNextImage()
    {
        while (imgNext >= 0 && imgNext < doc.imageCount() && imgNext < MAX_DOC_IMAGES)
        {
            web::Url u;
            char url[ADDR_CAP];
            bool local = false;
            if (imageUrl(imgNext, u, url, sizeof(url), &local) && findPic(url) < 0)
            {
                if (nPics == MAX_PICS)
                    break;
                Pic &p = pics[nPics++];
                web::scopy(p.url, url, sizeof(p.url));
                p.pic.release();
                if (local)
                {
                    //  From the disk: no waiting, so at once.
                    web::Buf data{true};
                    bool tooBig;
                    if (readLocal(url, data, tooBig))
                        decodeInto(p.pic, data.data, data.len);
                    applyPictures();
                    imgNext++;
                    continue;
                }
                loader.start(u, false);
                ensureIdleLoop(true);
                web::scopy(message, "Picture ", sizeof(message));
                web::scatInt(message, imgNext + 1, sizeof(message));
                web::scat(message, " of ", sizeof(message));
                web::scatInt(message, doc.imageCount(), sizeof(message));
                web::scat(message, "...", sizeof(message));
                wnd->Repaint();
                return;
            }
            imgNext++;
        }
        imgNext = -1;
        int shown = 0;
        for (int k = 0; k < nPics; k++)
            shown += pics[k].pic.px != nullptr;
        web::scopy(message, pageTiming, sizeof(message));
        if (shown)
        {
            size_t ml = strlen(message);
            if (ml && message[ml - 1] == '.')
                message[ml - 1] = 0;
            web::scat(message, ml ? ", " : "", sizeof(message));
            web::scatInt(message, shown, sizeof(message));
            web::scat(message, shown == 1 ? " picture" : " pictures", sizeof(message));
        }
        wnd->Repaint();
    }

    //  Into the screen's colours, as wide as the page is now at most, over
    //  the page's background where it is transparent.
    const char *decodeInto(web::Picture &pic, const uint8_t *data, size_t len)
    {
        int maxW = (int)((contentW - 3) * pxPerUnit);
        if (maxW < 16)
            maxW = 16;
        const char *why = web::decodePicture(data, len, maxW, PIC_MAX_H,
                                             (int)MementoR2Impl::R2_Palette::Count(), dark ? 0x000000 : 0xFFFFFF, pic);
        //  Spacers and counters: a pixel or two, nothing to see.
        if (!why && (pic.w <= 2 || pic.h <= 2))
            pic.release();
        return why;
    }

    void onImageLoaded()
    {
        web::HttpResponse &r = loader.response();
        if (loader.phase() == web::Loader::FAILED && !strcmp(loader.error(), "Stopped"))
        {
            imgNext = -1;
            web::scopy(message, "Stopped.", sizeof(message));
            wnd->Repaint();
            return;
        }
        Pic &p = pics[nPics - 1];
        if (loader.phase() == web::Loader::DONE && r.status == 200 && r.body.len && !r.truncated)
            decodeInto(p.pic, r.body.data, r.body.len);
        r.body.release();
        applyPictures();
        imgNext++;
        fetchNextImage();
    }

    //  An address that answered with a picture, or a picture file: a page
    //  with just that on it.
    void showPicture(const char *addr, const uint8_t *data, size_t len, bool truncated)
    {
        clearPics();
        Pic &p = pics[nPics++];
        web::scopy(p.url, addr, sizeof(p.url));
        const char *why = truncated ? "too big to download (768 KiB at most)" : decodeInto(p.pic, data, len);
        web::Buf b;
        b.appendStr("<title>");
        escapeInto(b, addr);
        b.appendStr("</title>");
        if (why)
        {
            b.appendStr("<h2>Cannot show the picture</h2><p>");
            escapeInto(b, why);
            b.appendStr(".</p>");
        }
        else
        {
            b.appendStr("<p align=center><img src=\"");
            escapeInto(b, addr);
            b.appendStr("\"></p>");
        }
        doc.loadMessage(b.cstr());
        applyPictures();
    }

    void onLoaded()
    {
        if (scriptNext >= 0) { onScriptLoaded(); return; }
        if (sheetNext >= 0)
        {
            onSheetLoaded();
            return;
        }
        if (imgNext >= 0)
        {
            onImageLoaded();
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
        pageIsFile = false;
        if (html)
        {
            clearPics();
            //  The page is kept, taken over from the response rather than
            //  copied: its style sheets come next, and then it is read again.
            r.body.shrink(); // kept for as long as the page is: without its growing room
            pageBody.data = r.body.data;
            pageBody.len = r.body.len;
            pageBody.cap = r.body.cap;
            r.body.data = nullptr;
            r.body.len = r.body.cap = 0;
            web::scopy(pageCharset, r.charset, sizeof(pageCharset));
            pageUrl = loader.url();
            renderPage(nullptr, 0);
        }
        else if (web::istarts(ct, "image/"))
        {
            pageUrl = loader.url();
            showPicture(addr, r.body.data, r.body.len, r.truncated);
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
        if (html) startScripts();
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
        if (row < l.row || row >= l.row + l.height())
            return -1;
        if (l.img)
        {
            double px = (x - contentX - 1) * pxPerUnit;
            return px >= l.imgX && px < l.imgX + l.imgW ? l.link : -1;
        }
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

    static bool isCtrlKey(PlatformKey *key, char lower)
    {
        return (key->isLeftControl || key->isRightControl) && key->isChar &&
               (key->theChar == lower || key->theChar == lower - 'a' + 'A');
    }

    void pasteIntoField(int ci)
    {
        for (const char *p = clipboardGet(); *p; p++)
            if (*p >= ' ' && (unsigned char)*p < 0x7F)
                doc.controlInsert(ci, *p);
        formsTouched = true;
    }

    void say(const char *prefix, const char *what)
    {
        web::scopy(message, prefix, sizeof(message));
        web::scat(message, what, sizeof(message));
        messageFirst = true;
        wnd->Repaint();
    }

    //  Ctrl+C on the page: the focused link's address, made whole against the
    //  page's, or the page's own when no link (or only a form control) is
    //  focused.
    void copyAddress()
    {
        char s[ADDR_CAP];
        int l = focusedLink >= 0 ? focusedLink : hoverLink;
        if (l >= 0 && doc.linkControl(l) < 0)
            linkForDisplay(l, s, sizeof(s));
        else
            web::scopy(s, current, sizeof(s));
        clipboardSet(s);
        say("Copied: ", s);
    }

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
        int node = scriptNode(doc.str(c.onclick));
        if (key->isEscape)
        {
            edit = EDIT_NONE;
            if (node >= 0)
            {
                script.key("Escape");
                script.controlChanged(node);
                updateScriptPage();
            }
        }
        else if (key->isTab)
        {
            edit = EDIT_NONE;
            if (node >= 0)
            {
                script.controlChanged(node);
                if (updateScriptPage())
                    return;
            }
            focusLink((key->isLeftShift || key->isRightShift) ? -1 : +1);
            return;
        }
        else if (key->isEnter)
        {
            if (c.type == web::Control::TEXTAREA && !(key->isLeftControl || key->isRightControl))
                doc.controlInsert(ci, '\n');
            else
            {
                //  The page hears Enter first (a chat box sends on it), then
                //  the form is sent as the page's DOM says.
                if (node >= 0)
                {
                    script.controlChanged(node);
                    bool taken = script.key("Enter");
                    if (taken || !script.submitFrom(node))
                    {
                        updateScriptPage();
                        return;
                    }
                }
                submitForm(c.form, -1);
                return;
            }
        }
        else if (key->isBackspace)
            doc.controlBackspace(ci);
        else if (isCtrlKey(key, 'v'))
            pasteIntoField(ci);
        else if (isCtrlKey(key, 'c'))
            return;
        else if (key->isChar && key->theChar >= ' ' && (unsigned char)key->theChar < 0x7F)
            doc.controlInsert(ci, (char)key->theChar);
        else
            return;
        formsTouched = true;
        if (node >= 0 && ci < doc.controlCount())
        {
            //  Every change reaches the page as it is typed (search boxes).
            script.input(node, doc.control(ci).edit ? doc.control(ci).edit : "");
            updateScriptPage();
        }
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
        if (isCtrlKey(key, 'c'))
        {
            clipboardSet(editBuf);
            return;
        }
        if (isCtrlKey(key, 'v'))
        {
            if (editFresh)
                editLen = 0;
            editFresh = false;
            for (const char *p = clipboardGet(); *p && editLen < ADDR_CAP - 1; p++)
                if (*p >= ' ' && (unsigned char)*p < 0x7F)
                    editBuf[editLen++] = *p;
            editBuf[editLen] = 0;
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
            if (menuOpen)
            {
                int i = menuItemAt(COORD_VAL(mx), COORD_VAL(my));
                if (i >= 0 && i != menuSel)
                {
                    menuSel = i;
                    wnd->Repaint();
                }
                return;
            }
            int l = linkAt(COORD_VAL(mx), COORD_VAL(my));
            if (l != hoverLink)
            {
                hoverLink = l;
                messageFirst = false;
                wnd->Repaint();
            }
            return;
        }
        case PlatformWindowInputEventType::OnMouseWheel:
            closeMenu();
            scrollTo(scrollRow + (data->Data.OnMouseWheel.up ? -3 : 3));
            return;
        case PlatformWindowInputEventType::OnMouseClick:
            if (data->Data.OnMouseClick.state == PlatformWindowButtonState::Pressed)
            {
                Coord mx = data->Data.OnMouseClick.mouseX, my = data->Data.OnMouseClick.mouseY;
                double x = COORD_VAL(mx), y = COORD_VAL(my);
                if (menuOpen)
                {
                    //  A click on an item picks it; anywhere else only closes
                    //  the menu, as a click beside a menu does everywhere.
                    int i = menuItemAt(x, y);
                    if (i >= 0)
                        pickMenu(i);
                    else
                        closeMenu();
                }
                else if (data->Data.OnMouseClick.button == PlatformWindowMouseButton::Right)
                    openMenu(linkAt(x, y), x, y, false);
                else
                    onClick(x, y);
            }
            return;
        case PlatformWindowInputEventType::OnKeyEvent:
            if (data->Data.OnKeyEvent.key->isKeyDown)
            {
                messageFirst = false;
                onKey(data->Data.OnKeyEvent.key);
            }
            return;
        default:
            return;
        }
    }

    void onIdle()
    {
        if (script.active())
        {
            scriptLoop();
            if (script.busy() || script.changed())
                idleSince = web::now_ms();
        }
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
            else if (x < 4 * buttonW)
                setDark(!dark);
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
        if (menuOpen)
        {
            onMenuKey(key);
            return;
        }
        //  The menu, on the focused link or the field being typed into.
        if ((edit == EDIT_NONE || edit == EDIT_FIELD) &&
            (((key->isLeftControl || key->isRightControl) && key->isChar && key->theChar == ' ') ||
             ((key->isLeftShift || key->isRightShift) && key->isF && key->f == 10)))
        {
            double x = contentX + 1, y = contentY;
            if (focusedLink >= 0)
                linkPoint(focusedLink, x, y);
            openMenu(focusedLink, x, y, true);
            return;
        }
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
        if (ctrl && key->isChar && (key->theChar == 'd' || key->theChar == 'D'))
        {
            setDark(!dark);
            return;
        }
        if (isCtrlKey(key, 'c'))
        {
            copyAddress();
            return;
        }
        if (isCtrlKey(key, 'v'))
        {
            //  Pasted on the page: an address to go to, in the bar for Enter.
            beginEdit(EDIT_ADDRESS);
            onEditKey(key);
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

    // ── The context menu ──────────────────────────────────────────────────────

    void addItem(Action a, bool enabled, const char *label, const char *keys = "")
    {
        if (menuLen < MENU_CAP)
            menu[menuLen++] = {a, enabled, label, keys};
    }

    //  `link` is what it was opened on (-1: the page), (x, y) where: under
    //  the pointer, or under the focused link when a key opened it.
    void openMenu(int link, double x, double y, bool fromKey)
    {
        if (link >= doc.linkCount())
            link = -1;
        menuLen = 0;
        menuLink = link;
        int ci = link >= 0 ? doc.linkControl(link) : -1;
        bool clip = clipboardGet()[0] != 0;
        if (link >= 0 && ci < 0)
        {
            addItem(A_OPEN, true, "&Open link", "Enter");
            addItem(A_OPEN_NEW, true, "Open in &new window");
            addItem(A_COPY_LINK, true, "&Copy link address", "Ctrl+C");
            addItem(A_COPY_TEXT, true, "Copy link &text");
            addItem(A_SEP, false, "");
        }
        else if (ci >= 0 && doc.control(ci).isText())
        {
            addItem(A_PASTE_FIELD, clip, "&Paste", "Ctrl+V");
            addItem(A_SEP, false, "");
        }
        addItem(A_BACK, histPos > 0, "&Back", "Alt+Left");
        addItem(A_FORWARD, histPos + 1 < histLen, "&Forward", "Alt+Right");
        if (loader.busy())
            addItem(A_STOP, true, "&Stop", "Esc");
        else
            addItem(A_RELOAD, true, "&Reload", "F5");
        addItem(A_SEP, false, "");
        addItem(A_COPY_PAGE, true, "Copy page &address");
        addItem(A_PASTE_GO, clip, "Paste and &go");
        addItem(A_FIND, true, "F&ind on page", "/");
        addItem(A_DARK, true, dark ? "&Light mode" : "&Dark mode", "Ctrl+D");

        if (link >= 0)
            focusedLink = link; // so it is drawn as the one the menu is about
        menuOpen = true;
        menuX = x;
        menuY = y;
        menuSel = -1;
        if (fromKey)
            moveMenuSel(+1);
        wnd->Repaint();
    }

    void closeMenu()
    {
        if (!menuOpen)
            return;
        menuOpen = false;
        wnd->Repaint();
    }

    void moveMenuSel(int dir)
    {
        for (int n = 0; n < menuLen; n++)
        {
            menuSel = menuSel < 0 ? (dir > 0 ? 0 : menuLen - 1) : (menuSel + dir + menuLen) % menuLen;
            if (menu[menuSel].enabled)
                return;
        }
        menuSel = -1;
    }

    static char accel(const char *label)
    {
        const char *a = strchr(label, '&');
        if (!a || !a[1])
            return 0;
        char c = a[1];
        return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
    }

    void onMenuKey(PlatformKey *key)
    {
        if (key->isEscape || key->isTab)
            closeMenu();
        else if (key->isArrowDown)
            moveMenuSel(+1), wnd->Repaint();
        else if (key->isArrowUp)
            moveMenuSel(-1), wnd->Repaint();
        else if (key->isHome)
            menuSel = -1, moveMenuSel(+1), wnd->Repaint();
        else if (key->isEnd)
            menuSel = -1, moveMenuSel(-1), wnd->Repaint();
        else if (key->isEnter || (key->isChar && key->theChar == ' ' && !(key->isLeftControl || key->isRightControl)))
        {
            if (menuSel >= 0)
                pickMenu(menuSel);
        }
        else if (key->isChar && !(key->isLeftControl || key->isRightControl))
        {
            char c = (char)key->theChar;
            if (c >= 'A' && c <= 'Z')
                c = (char)(c - 'A' + 'a');
            for (int i = 0; i < menuLen; i++)
                if (menu[i].enabled && accel(menu[i].label) == c)
                {
                    pickMenu(i);
                    return;
                }
        }
        else if ((key->isLeftControl || key->isRightControl) && key->isChar && key->theChar == ' ')
            closeMenu(); // the key that opened it closes it again
    }

    void pickMenu(int i)
    {
        if (i < 0 || i >= menuLen || !menu[i].enabled)
            return;
        Action a = menu[i].action;
        int l = menuLink;
        menuOpen = false;
        char s[ADDR_CAP];
        switch (a)
        {
        case A_SEP:
            break;
        case A_OPEN:
            followLink(l);
            break;
        case A_OPEN_NEW:
            linkForDisplay(l, s, sizeof(s));
            openInNewWindow(s);
            break;
        case A_COPY_LINK:
            linkForDisplay(l, s, sizeof(s));
            clipboardSet(s);
            say("Copied: ", s);
            break;
        case A_COPY_TEXT:
            linkText(l, s, sizeof(s));
            clipboardSet(s);
            say("Copied: ", s);
            break;
        case A_PASTE_FIELD:
        {
            int ci = doc.linkControl(l);
            if (edit != EDIT_FIELD || editControl != ci)
                activateControl(ci, false); // starts typing into it
            pasteIntoField(ci);
            break;
        }
        case A_BACK:
            goHistory(-1);
            break;
        case A_FORWARD:
            goHistory(+1);
            break;
        case A_RELOAD:
            reload();
            break;
        case A_STOP:
            loader.cancel();
            onLoaded();
            break;
        case A_COPY_PAGE:
            web::scopy(s, loader.busy() ? attempted : current, sizeof(s));
            clipboardSet(s);
            say("Copied: ", s);
            break;
        case A_PASTE_GO:
            //  Through the address bar, so what is pasted is cleaned the
            //  same way as what is typed there.
            beginEdit(EDIT_ADDRESS);
            editLen = 0;
            for (const char *p = clipboardGet(); *p && editLen < ADDR_CAP - 1; p++)
                if ((*p > ' ' || (*p == ' ' && editLen)) && (unsigned char)*p < 0x7F)
                    editBuf[editLen++] = *p;
            while (editLen && editBuf[editLen - 1] == ' ')
                editLen--;
            editBuf[editLen] = 0;
            edit = EDIT_NONE;
            navigate(editBuf, true);
            break;
        case A_FIND:
            beginEdit(EDIT_FIND);
            break;
        case A_DARK:
            setDark(!dark);
            break;
        }
        wnd->Repaint();
    }

    void openInNewWindow(const char *addr)
    {
        web::scopy(openNext, addr, sizeof(openNext));
        if (openBrowserWindow(openNext)) openNext[0] = 0;
        //  SetWindow takes it; still here, the window was never made.
        if (openNext[0])
        {
            openNext[0] = 0;
            say("", "No room for another window.");
        }
    }

    //  The words of link l, as they read on the page (all its runs, which
    //  wrapping may have put on several lines).
    void linkText(int l, char *out, size_t cap)
    {
        out[0] = 0;
        int row = doc.linkRow(l);
        if (row < 0)
            return;
        bool found = false;
        for (size_t li = doc.lineAtRow(row); li < doc.lineCount(); li++)
        {
            const web::Line &ln = doc.line(li);
            bool here = false;
            for (uint32_t r = 0; r < ln.nRuns; r++)
            {
                const web::Run &ru = doc.run(ln.firstRun + r);
                if (ru.link != l)
                    continue;
                if (found && !here && out[0])
                    web::scat(out, " ", cap); // a line break inside the link
                here = found = true;
                size_t n = strlen(out);
                if (n + 1 < cap)
                    web::scopyn(out + n, doc.text(ru.off), ru.len, cap - n);
            }
            if (found && !here)
                break;
        }
    }

    //  Just below where link l is drawn, for a menu opened by key.
    void linkPoint(int l, double &x, double &y)
    {
        int row = doc.linkRow(l);
        if (row < scrollRow || row >= scrollRow + visibleRows)
            return;
        size_t li = doc.lineAtRow(row);
        if (li >= doc.lineCount())
            return;
        const web::Line &ln = doc.line(li);
        if (ln.img && ln.link == l)
        {
            x = contentX + 1 + ln.imgX / pxPerUnit;
            y = contentY + (ln.row - scrollRow + ln.height()) * rowH;
            if (y > contentY + contentH - rowH)
                y = contentY + contentH - rowH;
            return;
        }
        double cell = ln.big ? bcw : cw;
        for (uint32_t r = 0; r < ln.nRuns; r++)
        {
            const web::Run &ru = doc.run(ln.firstRun + r);
            if (ru.link == l)
            {
                x = contentX + 1 + ru.col * cell;
                y = contentY + (ln.row - scrollRow + ln.height()) * rowH;
                return;
            }
        }
    }

    double menuItemH(int i) const { return menu[i].action == A_SEP ? 3 : rowH + 1; }

    //  Where the menu goes: at the point asked for, moved in to fit.
    void layoutMenu()
    {
        size_t lw = 0, kw = 0;
        for (int i = 0; i < menuLen; i++)
        {
            size_t a = strlen(menu[i].label) - (strchr(menu[i].label, '&') ? 1 : 0);
            size_t b = strlen(menu[i].keys);
            lw = a > lw ? a : lw;
            kw = b > kw ? b : kw;
        }
        menuW = (lw + (kw ? kw + 3 : 0)) * cw + 8;
        double h = 4;
        for (int i = 0; i < menuLen; i++)
            h += menuItemH(i);
        menuL = menuX;
        menuT = menuY;
        if (menuL + menuW > winW - 1)
            menuL = winW - 1 - menuW;
        if (menuT + h > winH - 1)
            menuT = menuY - h > 0 ? menuY - h : winH - 1 - h;
        if (menuL < 1)
            menuL = 1;
        if (menuT < 1)
            menuT = 1;
    }

    int menuItemAt(double x, double y) const
    {
        if (!menuOpen || x < menuL || x >= menuL + menuW)
            return -1;
        double iy = menuT + 2;
        for (int i = 0; i < menuLen; i++)
        {
            double h = menuItemH(i);
            if (y >= iy && y < iy + h)
                return menu[i].action == A_SEP ? -1 : i;
            iy += h;
        }
        return -1;
    }

    void paintMenu(PlatformBitmap *t)
    {
        layoutMenu();
        double h = 4;
        for (int i = 0; i < menuLen; i++)
            h += menuItemH(i);
        //  A shadow, a border, then the items.
        t->FillRect(Coord(menuL + 1.5), Coord(menuT + 1.5), Coord(menuW), Coord(h), cChromeDark, false);
        t->FillRect(Coord(menuL), Coord(menuT), Coord(menuW), Coord(h), cText, false);
        t->FillRect(Coord(menuL + 0.5), Coord(menuT + 0.5), Coord(menuW - 1), Coord(h - 1), cField, false);
        double iy = menuT + 2;
        for (int i = 0; i < menuLen; i++)
        {
            const MenuItem &m = menu[i];
            double ih = menuItemH(i);
            if (m.action == A_SEP)
            {
                t->FillRect(Coord(menuL + 2), Coord(iy + 1), Coord(menuW - 4), Coord(0.5), cChrome, false);
                iy += ih;
                continue;
            }
            bool sel = i == menuSel;
            if (sel)
                t->FillRect(Coord(menuL + 1), Coord(iy), Coord(menuW - 2), Coord(ih), cFocus, false);
            PlatformColor *c = !m.enabled ? cFaint : sel ? cOnFocus : cText;
            char label[40];
            int ai = -1, n = 0;
            for (const char *p = m.label; *p && n < (int)sizeof(label) - 1; p++)
            {
                if (*p == '&' && ai < 0)
                {
                    ai = n;
                    continue;
                }
                label[n++] = *p;
            }
            label[n] = 0;
            double tx = menuL + 4, ty = iy + 0.5;
            text(t, tx, ty, menuW - 8, ch, label, c, font, false);
            if (ai >= 0 && m.enabled)
                t->FillRect(Coord(tx + ai * cw), Coord(ty + ch - 0.5), Coord(cw - 0.5), Coord(0.5), c, false);
            if (m.keys[0])
            {
                double kx = menuL + menuW - 4 - strlen(m.keys) * cw;
                text(t, kx, ty, menuW, ch, m.keys, sel ? cOnFocus : cFaint, font, false);
            }
            iy += ih;
        }
    }

    // ── Paint ─────────────────────────────────────────────────────────────────

    void makeResources(PlatformDrawingContext *dc)
    {
        if (cPal[0])
            return;
        //  Everything is quantised to the 16 EGA colours, so the palette is
        //  all there is; the theme picks from it.
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

    void applyTheme()
    {
        const Theme &th = dark ? DARK : LIGHT;
        cBg = cPal[th.bg];
        cText = cPal[th.text];
        cFaint = cPal[th.faint];
        cChrome = cPal[th.chrome];
        cChromeDark = cPal[th.chromeDark];
        cField = cPal[th.field];
        cOnFocus = cPal[th.onFocus];
        cMark = cPal[th.mark];
        cFocus = cPal[th.focus];
    }

    //  A colour the page asked for, as it is drawn.  Dark mode does not
    //  repaint the page in two colours: each EGA colour gives way to its
    //  opposite in brightness --- black and white, the two greys, and each
    //  dark colour and its bright one --- so a page keeps its colours, and a
    //  dark heading on a pale box becomes a bright heading on a dark one.
    int pageColour(int idx) const
    {
        static const uint8_t opposite[16] = {15, 9, 10, 11, 12, 13, 14, 8, 7, 1, 2, 3, 4, 5, 6, 0};
        return dark ? opposite[idx & 15] : idx;
    }

    void setDark(bool on)
    {
        if (dark == on)
            return;
        dark = on;
        wnd->Repaint();
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
        //  The last button says what it switches to: Dark, or Light.
        const char *labels[BUTTONS] = {"<", ">", loader.busy() ? "x" : "R", dark ? "L" : "D"};
        bool enabled[BUTTONS] = {histPos > 0, histPos + 1 < histLen, true, true};
        for (int i = 0; i < BUTTONS; i++)
        {
            double bx = i * buttonW + 1;
            t->FillRect(Coord(bx), 1.5, Coord(buttonW - 1), Coord(toolbarH - 3), cField, false);
            text(t, bx + (buttonW - 1 - cw) / 2, 2 + (toolbarH - 4 - ch) / 2, cw * 2, ch, labels[i],
                 enabled[i] ? cText : cChrome, font, true);
        }
        addrX = BUTTONS * buttonW + 2;
        double aw = W - addrX - 2;
        t->FillRect(Coord(addrX), 1.5, Coord(aw), Coord(toolbarH - 3), cField, false);

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
            text(t, addrX + 1.5, ty, aw - 3, ch, shown, cOnFocus, font, false);
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
                bool scripted = scriptNode(doc.str(c.onclick)) >= 0;
                web::scat(out, c.form < 0 ? (scripted ? ": for the page's script" : " (needs JavaScript)")
                               : doc.formPost(c.form) ? ": sends the form (POST)"
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
        if (web::istarts(href, "javascript:") || !strcmp(href, "#r2"))
        {
            //  What a script does, not an address.
            web::scopy(out, !strcmp(href, "#r2") ? "Runs the page's script" : href, cap);
            return;
        }
        if (localAddress(href, out, cap))
            return;
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
        else if (messageFirst && message[0])
            web::scopy(s, message, sizeof(s));
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

        //  The pixels a cell and a row take, which is what pictures are
        //  measured in.  From the DPI, not from the bitmap: Memento allocates
        //  bitmaps in steps of 150 pixels, so one is usually wider than its
        //  window, and a scale worked out from its width puts everything
        //  drawn straight into the pixels too far right and down.
        auto *bm = static_cast<MementoR2Impl::R2_BitmapImpl *>(t);
        if (wnd->GetEffectiveDPI() > 0)
            pxPerUnit = wnd->GetEffectiveDPI() / 96.0;
        doc.setCellPixels((int)(cw * pxPerUnit + 0.5), (int)(rowH * pxPerUnit + 0.5));

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
            if (l.img)
            {
                paintPicture(bm, l, y);
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
                    text(t, x, y, w + cell, lh, buf, focused || typing ? cOnFocus : cText, font, !live);
                    continue;
                }

                //  The page's colours, kept readable: a colour too close to
                //  what is behind it (the find mark, when there is one)
                //  gives way to black or white.
                int bgIdx = pageColour(ru.bg ? ru.bg - 1 : 15);
                int fgIdx = isLink ? 1 : ru.fg ? ru.fg - 1 : l.big ? 1 : (ru.style & web::ST_FAINT) ? 8 : 0;
                fgIdx = pageColour(fgIdx);
                int under = found ? (dark ? DARK : LIGHT).mark : bgIdx;
                int dl = web::cssLuma((uint8_t)fgIdx) - web::cssLuma((uint8_t)under);
                if (dl < 0)
                    dl = -dl;
                if (dl < 90)
                    fgIdx = web::cssLuma((uint8_t)under) < 128 ? 15 : 0;
                PlatformColor *fg = cPal[fgIdx];
                if (focused)
                {
                    t->FillRect(Coord(x), Coord(y), Coord(w), Coord(lh - 1), cFocus, false);
                    fg = cOnFocus;
                }
                else if (found)
                    t->FillRect(Coord(x), Coord(y), Coord(w), Coord(lh - 1), cMark, false);
                else if (bgIdx != pageColour(15))
                    t->FillRect(Coord(x), Coord(y), Coord(w), Coord(lh - 1), cPal[bgIdx], false);

                text(t, x, y, w + cell, lh, buf, fg, l.big ? bigFont : font, (ru.style & web::ST_BOLD) || l.big);
                if ((isLink || (ru.style & web::ST_UNDER)) && !focused)
                    t->FillRect(Coord(x), Coord(y + lh - 1.5), Coord(w), Coord(0.5),
                                isLink && ru.link == hoverLink ? cText : fg, false);
            }
        }
        t->ClearClip();

        //  A frame around a picture that is the focused link, over the rest.
        if (focusedLink >= 0)
            for (size_t li = doc.lineAtRow(scrollRow); li < doc.lineCount(); li++)
            {
                const web::Line &l = doc.line(li);
                if (l.row >= scrollRow + visibleRows)
                    break;
                if (!l.img || l.link != focusedLink)
                    continue;
                double x = contentX + 1 + l.imgX / pxPerUnit, y = contentY + (l.row - scrollRow) * rowH;
                double w = l.imgW / pxPerUnit, h = l.imgH / pxPerUnit;
                t->SetClip(Coord(contentX), Coord(contentY), Coord(contentW), Coord(contentH), false);
                t->FillRect(Coord(x - 1), Coord(y - 1), Coord(w + 2), Coord(1), cFocus, false);
                t->FillRect(Coord(x - 1), Coord(y + h), Coord(w + 2), Coord(1), cFocus, false);
                t->FillRect(Coord(x - 1), Coord(y), Coord(1), Coord(h), cFocus, false);
                t->FillRect(Coord(x + w), Coord(y), Coord(1), Coord(h), cFocus, false);
                t->ClearClip();
            }

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

    //  A picture line, straight into the window's pixels (one palette index a
    //  pixel), scaled by nearest pixel when the page is narrower than it and
    //  cut to the page area.
    void paintPicture(MementoR2Impl::R2_BitmapImpl *bm, const web::Line &l, double y)
    {
        int i = l.img - 1;
        int k = i < MAX_DOC_IMAGES ? picOf[i] : -1;
        if (k < 0 || k >= nPics || !pics[k].pic.px)
            return;
        const web::Picture &p = pics[k].pic;
        uint8 *px = bm->GetPixels();
        int bw = bm->GetRealWidth().intValue(), bh = bm->GetRealHeight().intValue();
        if (!px || bw <= 0)
            return;
        int x0 = (int)((contentX + 1) * pxPerUnit) + l.imgX, y0 = (int)(y * pxPerUnit);
        int top = (int)(contentY * pxPerUnit), bottom = (int)((contentY + contentH) * pxPerUnit);
        int right = (int)((contentX + contentW) * pxPerUnit);
        if (bottom > bh)
            bottom = bh;
        if (right > bw)
            right = bw;
        for (int dy = 0; dy < l.imgH; dy++)
        {
            int py = y0 + dy;
            if (py < top)
                continue;
            if (py >= bottom)
                break;
            const uint8_t *src = p.px + (size_t)(dy * p.h / l.imgH) * p.w;
            uint8 *dst = px + (size_t)py * bw;
            for (int dx = 0; dx < l.imgW && x0 + dx < right; dx++)
                if (x0 + dx >= 0)
                    dst[x0 + dx] = src[l.imgW == p.w ? dx : dx * p.w / l.imgW];
        }
    }

    void OnPaint(PlatformDrawingContext *dc, PlatformBitmap *target)
    {
        if (!target)
            return;
        makeResources(dc);
        if (!cPal[0] || !font || !bigFont)
            return;
        applyTheme();

        Coord Wc = target->GetWidth();
        Coord Hc = target->GetHeight();
        double W = COORD_VAL(Wc), H = COORD_VAL(Hc);
        winW = W;
        winH = H;

        paintChrome(target, W);
        double sh = rowH + 2;
        contentX = 2;
        contentY = toolbarH + 1;
        contentW = W - contentX - SCROLL_W - 1;
        contentH = H - contentY - sh - 1;
        visibleRows = (int)(contentH / rowH);
        if (visibleRows < 1)
            visibleRows = 1;
        //  What layout queries and media queries see (window.innerWidth...).
        script.setViewport((int)(contentW / cw), visibleRows, (int)(cw * pxPerUnit + 0.5), (int)(rowH * pxPerUnit + 0.5),
                           dark);
        paintPage(target);
        paintStatus(target, W, H);
        if (menuOpen)
            paintMenu(target);
    }
};
