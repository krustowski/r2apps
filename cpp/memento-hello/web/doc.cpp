#include "doc.h"
#include "css.h"

namespace web {

#include "charmap.inc"

// ─── Characters ──────────────────────────────────────────────────────────────

uint8_t glyphFor(uint32_t cp)
{
    if (cp >= 0x20 && cp < 0x7F)
        return (uint8_t)cp;
    if (cp < 0x20 || cp == 0x7F)
        return 0;
    size_t lo = 0, hi = sizeof(kUniMap) / sizeof(kUniMap[0]);
    while (lo < hi)
    {
        size_t mid = (lo + hi) / 2;
        if (kUniMap[mid][0] == cp)
            return (uint8_t)kUniMap[mid][1];
        if (kUniMap[mid][0] < cp)
            lo = mid + 1;
        else
            hi = mid;
    }
    return '?';
}

enum Encoding
{
    ENC_UTF8,
    ENC_CP1252,
    ENC_CP1250,
    ENC_ISO88592,
};

static Encoding encodingFor(const char *name)
{
    if (!name || !name[0])
        return ENC_UTF8;
    if (ieq(name, "windows-1250") || ieq(name, "cp1250") || ieq(name, "x-cp1250"))
        return ENC_CP1250;
    if (ieq(name, "iso-8859-2") || ieq(name, "iso8859-2") || ieq(name, "latin2"))
        return ENC_ISO88592;
    if (ieq(name, "iso-8859-1") || ieq(name, "iso8859-1") || ieq(name, "latin1") ||
        ieq(name, "windows-1252") || ieq(name, "cp1252") || ieq(name, "us-ascii") || ieq(name, "ascii"))
        return ENC_CP1252; // what browsers do with Latin-1 too
    return ENC_UTF8;
}

//  One character at s[i], advancing i.  Invalid UTF-8 is read as windows-1252
//  instead of being thrown away: a page that says it is UTF-8 and is not is
//  nearly always one of those.
static uint32_t decodeAt(const uint8_t *s, size_t n, size_t &i, Encoding enc)
{
    uint8_t b = s[i++];
    if (b < 0x80)
        return b;
    switch (enc)
    {
    case ENC_CP1252:
        return kCp1252[b - 0x80];
    case ENC_CP1250:
        return kCp1250[b - 0x80];
    case ENC_ISO88592:
        return kIso88592[b - 0x80];
    case ENC_UTF8:
        break;
    }
    int more;
    uint32_t cp;
    if ((b & 0xE0) == 0xC0)
    {
        more = 1;
        cp = b & 0x1F;
    }
    else if ((b & 0xF0) == 0xE0)
    {
        more = 2;
        cp = b & 0x0F;
    }
    else if ((b & 0xF8) == 0xF0)
    {
        more = 3;
        cp = b & 0x07;
    }
    else
        return kCp1252[b - 0x80];
    if (i + (size_t)more > n)
        return kCp1252[b - 0x80];
    for (int k = 0; k < more; k++)
        if ((s[i + (size_t)k] & 0xC0) != 0x80)
            return kCp1252[b - 0x80];
    for (int k = 0; k < more; k++)
        cp = (cp << 6) | (s[i++] & 0x3F);
    return cp;
}

//  "&amp;", "&#233;", "&#xE9;" at s[i] (which is the '&').  Returns the code
//  point and moves i past the reference, or returns 0 and leaves i alone.
static uint32_t decodeEntity(const uint8_t *s, size_t n, size_t &i)
{
    size_t j = i + 1;
    if (j < n && s[j] == '#')
    {
        j++;
        uint32_t v = 0;
        bool hex = false, any = false;
        if (j < n && (s[j] == 'x' || s[j] == 'X'))
        {
            hex = true;
            j++;
        }
        while (j < n)
        {
            uint8_t c = s[j];
            int d = -1;
            if (c >= '0' && c <= '9')
                d = c - '0';
            else if (hex && lower((char)c) >= 'a' && lower((char)c) <= 'f')
                d = lower((char)c) - 'a' + 10;
            if (d < 0)
                break;
            v = v * (hex ? 16 : 10) + (uint32_t)d;
            if (v > 0x10FFFF)
                v = 0xFFFD;
            any = true;
            j++;
        }
        if (!any)
            return 0;
        if (j < n && s[j] == ';')
            j++;
        //  The C1 range is windows-1252 in every browser.
        if (v >= 0x80 && v < 0xA0)
            v = kCp1252[v - 0x80];
        i = j;
        return v ? v : 0xFFFD;
    }

    char name[12];
    size_t k = 0;
    while (j < n && k + 1 < sizeof(name) &&
           ((s[j] >= 'a' && s[j] <= 'z') || (s[j] >= 'A' && s[j] <= 'Z') || (s[j] >= '0' && s[j] <= '9')))
        name[k++] = (char)s[j++];
    name[k] = 0;
    if (!k)
        return 0;
    for (size_t e = 0; e < sizeof(kEntities) / sizeof(kEntities[0]); e++)
    {
        if (strcmp(kEntities[e].name, name) == 0)
        {
            if (j < n && s[j] == ';')
                j++;
            i = j;
            return kEntities[e].cp;
        }
    }
    return 0;
}

// ─── Parser ──────────────────────────────────────────────────────────────────

//  What the tags look like on their own, as a style sheet, so that a page's
//  own can say otherwise.  Structure --- headings, lists, tables, the blank
//  lines around paragraphs --- stays with the tag handlers below.
static const char kUaSheet[] = "b,strong,th,dt,h3,h4,h5,h6{font-weight:bold}"
                               "i,em,cite,var,dfn,address{font-style:italic}"
                               "code,tt,kbd,samp,pre,listing,xmp{color:#a00}"
                               "pre,listing,xmp,plaintext{white-space:pre}"
                               "mark{background-color:#ff5}"
                               "u,ins{text-decoration:underline}"
                               "center{text-align:center}"
                               "table{text-align:left}" // tables do not inherit it (a quirk every browser keeps)
                               "[hidden]{display:none}"
                               "dialog:not([open]){display:none}";

static bool isNameChar(uint8_t c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
           c == '_' || c == ':';
}

static bool is(const char *a, const char *b) { return strcmp(a, b) == 0; }

static bool isAny(const char *name, const char *const *list)
{
    for (; *list; list++)
        if (strcmp(name, *list) == 0)
            return true;
    return false;
}

static const char *const kVoid[] = {"area",  "base", "br",   "col",    "embed", "hr",    "img", "input",
                                    "link",  "meta", "param", "source", "track", "wbr", nullptr};

//  Blocks that start on a line of their own and need nothing more.
static const char *const kPlainBlocks[] = {"div",     "section", "article", "header",  "footer",
                                           "nav",     "main",    "aside",   "center",  "caption",
                                           "figcaption", "summary", "legend", "address", "hgroup",
                                           "details", "fieldset", "form",   "body",    "noscript",
                                           "search",  "menu",    "dialog",  nullptr};

//  Everything that is a block of some kind: what CSS "display: inline" can
//  turn into a run of text, and what "display: block" has nothing to add to.
static const char *const kBlockish[] = {"p",  "div", "section", "article", "header", "footer", "nav", "main",
                                        "aside", "center", "figure", "figcaption", "blockquote", "ul", "ol",
                                        "li", "dl", "dt", "dd", "h1", "h2", "h3", "h4", "h5", "h6", "pre",
                                        "table", "tr", "td", "th", "form", "fieldset", "address", "details",
                                        "summary", "hr", "menu", "dir", "hgroup", "body", "html", nullptr};

//  What an open <p> is closed by (HTML's "closes a p element" list).
static const char *const kClosesP[] = {"address", "article", "aside", "blockquote", "details", "div", "dl",
                                       "fieldset", "figcaption", "figure", "footer", "form", "h1", "h2", "h3",
                                       "h4", "h5", "h6", "header", "hgroup", "hr", "main", "menu", "nav",
                                       "ol", "p", "pre", "section", "table", "ul", nullptr};

//  Elements whose content is not text to show.
static const char *const kSkipped[] = {"script", "template", "svg",      "math",  "object",
                                       "noembed", "noframes", "canvas", "datalist", nullptr};

class HtmlParser
{
public:
    HtmlParser(Document &d, Encoding enc, bool css);
    ~HtmlParser();

    bool ok() const { return stack_ && css_; }
    void addSheet(const uint8_t *s, size_t n) { css_->addSheet((const char *)s, n, Css::AUTHOR); }
    void parseHtml(const uint8_t *s, size_t n);
    void parseText(const uint8_t *s, size_t n);

private:
    Document &d_;
    Encoding enc_;
    bool cssOn_;
    Css *css_ = nullptr;

    // ── The open elements ───────────────────────────────────────────────────
    enum Flags : uint8_t
    {
        F_BLOCKIFY = 1, // display: block on something inline
        F_INLINE = 2,   // display: inline on a block
        F_BUTTON = 4,   // a <button>: closes with "]"
        F_LINK = 8,     // restore the link on the way out
        F_FORM = 16,
        F_HREF = 32, // an <a> whose own href is link_ while it is open
    };
    struct Elem
    {
        char tag[12];
        CssElement sel;
        CssStyle cs;
        uint8_t bgEff; // the background text inside it is drawn on
        uint8_t flags;
        int8_t indentDelta;
        int8_t savedCtrl;
        int32_t savedLink;
    };
    static const int MAX_DEPTH = 128;
    Elem *stack_ = nullptr;
    int depth_ = 0;
    int overflow_ = 0; // elements past MAX_DEPTH, not tracked
    Elem root_ = {};
    const Elem &top() const { return depth_ ? stack_[depth_ - 1] : root_; }

    //  Formatting elements (<a>, <b>, <em>...) that something else closed
    //  before their own end tag came.  HTML reopens them where the text goes
    //  on ("reconstructs the active formatting elements"), which is why
    //  <p><a href><h2>Headline</h2></a> is a linked headline in every browser:
    //  the <h2> closes the <p>, and the <a> with it, and the <a> comes back
    //  inside the <h2>.  Table cells are the barriers, as in the spec.
    struct Active
    {
        Elem e;
        int32_t link; // an <a>'s own link, or -1
    };
    static const int MAX_ACTIVE = 6;
    Active active_[MAX_ACTIVE];
    int nActive_ = 0;
    void reconstruct();

    // ── Structure, as the tags make it ──────────────────────────────────────
    int indent_ = 0;
    int heading_ = 0;
    int faint_ = 0;
    int32_t link_ = -1;
    bool ctrl_ = false; // link_ is a control
    int form_ = -1;
    bool forcePre_ = false;

    struct List
    {
        bool ordered;
        int counter;
    };
    static const int MAX_LISTS = 12;
    List lists_[MAX_LISTS];
    int nLists_ = 0;
    int listOverflow_ = 0;

    static const int MAX_TABLES = 16;
    int tables_ = 0;
    bool cellOpen_[MAX_TABLES + 1] = {};
    int cellsInRow_ = 0;
    bool inCell() const { return tables_ > 0 && tables_ <= MAX_TABLES && cellOpen_[tables_]; }
    void setCell(bool open)
    {
        if (tables_ > 0 && tables_ <= MAX_TABLES)
            cellOpen_[tables_] = open;
    }

    //  While a tag is handled: "display: inline" on it, and a margin from CSS.
    bool suppress_ = false;
    int marginOverride_ = -1;

    bool itemOpen_ = false;
    Item cur_ = {};
    bool lastSpace_ = true;

    // ── The tag being read ──────────────────────────────────────────────────
    static const int MAX_ATTRS = 16;
    char attrPool_[4096];
    size_t poolLen_ = 0;
    struct Attr
    {
        char name[16];
        uint32_t value;
    } attrs_[MAX_ATTRS];
    int nAttrs_ = 0;

    const char *attr(const char *name) const
    {
        for (int i = 0; i < nAttrs_; i++)
            if (strcmp(attrs_[i].name, name) == 0)
                return attrPool_ + attrs_[i].value;
        return nullptr;
    }

    bool pre() const { return forcePre_ || top().cs.pre == 1; }

    uint8_t style() const
    {
        const CssStyle &cs = top().cs;
        uint8_t st = 0;
        if (cs.bold == 1)
            st |= ST_BOLD;
        if (cs.italic == 1)
            st |= ST_ITALIC;
        if (cs.underline == 1)
            st |= ST_UNDER;
        if (faint_)
            st |= ST_FAINT;
        if (link_ >= 0)
            st |= ctrl_ ? ST_CTRL : ST_LINK;
        return st;
    }

    void pushItem(const Item &it)
    {
        if (!d_.items_.append(&it, sizeof(it)))
            d_.oom_ = true;
    }

    void flush()
    {
        if (itemOpen_ && cur_.len)
            pushItem(cur_);
        itemOpen_ = false;
    }

    void appendByte(uint8_t b)
    {
        uint8_t st = style();
        uint8_t fg = top().cs.fg, bg = top().bgEff;
        if (itemOpen_ && (cur_.style != st || cur_.link != link_ || cur_.fg != fg || cur_.bg != bg))
            flush();
        if (!itemOpen_)
        {
            cur_ = Item{};
            cur_.kind = Item::TEXT;
            cur_.style = st;
            cur_.fg = fg;
            cur_.bg = bg;
            cur_.link = link_;
            cur_.off = (uint32_t)d_.text_.len;
            itemOpen_ = true;
        }
        if (!d_.text_.push(b))
        {
            d_.oom_ = true;
            return;
        }
        cur_.len++;
    }

    void appendAscii(const char *s)
    {
        while (*s)
            appendByte((uint8_t)*s++);
        lastSpace_ = false;
    }

    void appendFaint(const char *s)
    {
        faint_++;
        appendAscii(s);
        faint_--;
    }

    //  Text from an attribute (UTF-8, as the pool keeps it), shown as text.
    void appendUtf8(const char *s)
    {
        size_t l = strlen(s);
        for (size_t k = 0; k < l;)
            textChar(decodeAt((const uint8_t *)s, l, k, ENC_UTF8));
    }

    void textChar(uint32_t cp);

    void block(int margin, const char *marker = nullptr)
    {
        if (suppress_)
        {
            //  "display: inline" on a block: what would have been a line
            //  break is a space.
            if (!lastSpace_ && !pre())
                textChar(' ');
            return;
        }
        if (marginOverride_ >= 0)
            margin = marginOverride_;
        flush();
        Item it = {};
        it.kind = Item::BLOCK;
        it.heading = (uint8_t)heading_;
        it.pre = pre();
        it.margin = (uint8_t)margin;
        it.indent = (uint8_t)(indent_ < 0 ? 0 : (indent_ > 60 ? 60 : indent_));
        it.align = (uint8_t)(top().cs.align > 0 ? top().cs.align : 0);
        it.link = -1;
        if (marker)
        {
            it.off = (uint32_t)d_.text_.len;
            it.len = (uint32_t)strlen(marker);
            if (!d_.text_.append(marker, it.len))
                d_.oom_ = true;
        }
        pushItem(it);
        lastSpace_ = true;
    }

    //  A block boundary that is only a space when it falls inside a table cell.
    void softBlock(int margin)
    {
        if (inCell() && !pre())
        {
            if (!lastSpace_)
                textChar(' ');
            return;
        }
        block(margin);
    }

    void simple(Item::Kind k)
    {
        flush();
        Item it = {};
        it.kind = k;
        it.link = -1;
        pushItem(it);
        lastSpace_ = true;
    }

    int32_t addLink(const char *href)
    {
        uint32_t off = (uint32_t)d_.links_.len;
        if (!d_.links_.append(href, strlen(href) + 1) || !d_.linkOffs_.append(&off, sizeof(off)))
        {
            d_.oom_ = true;
            return -1;
        }
        return (int32_t)(d_.linkOffs_.len / sizeof(uint32_t)) - 1;
    }

    //  Forms.
    int addControl(Control &c, const char *name, const char *value);
    int32_t controlLink(int ci)
    {
        char h[16] = "\x01";
        scatInt(h, ci, sizeof(h));
        return addLink(h);
    }
    void cells(int ci, int width);
    void button(int ci, const char *label);
    size_t selectElement(const uint8_t *s, size_t n, size_t i);
    size_t textareaElement(const uint8_t *s, size_t n, size_t i);

    //  Decodes an attribute value (entities included) into the pool.
    uint32_t poolValue(const uint8_t *s, size_t n);

    size_t parseTag(const uint8_t *s, size_t n, size_t i);
    void startTag(const char *name, bool selfClosing, const uint8_t *s, size_t n, size_t &i);
    void endTag(const char *name);
    bool open(const char *name, const uint8_t *s, size_t n, size_t &i);
    void close(const char *name);
    void pop(bool semantics, bool implicit = false);
    void closeUntil(const char *const *targets, const char *const *stops);
    size_t skipRaw(const uint8_t *s, size_t n, size_t i, const char *name, bool collectTitle,
                   size_t *contentStart = nullptr, size_t *contentEnd = nullptr);
    size_t skipElement(const uint8_t *s, size_t n, size_t i, const char *name);
    void describe(const char *name, Elem &e);
};

HtmlParser::HtmlParser(Document &d, Encoding enc, bool css) : d_(d), enc_(enc), cssOn_(css)
{
    stack_ = (Elem *)big_alloc(sizeof(Elem) * MAX_DEPTH);
    css_ = new Css();
    if (css_)
        css_->addSheet(kUaSheet, sizeof(kUaSheet) - 1, Css::UA);
}

HtmlParser::~HtmlParser()
{
    big_free(stack_);
    delete css_;
}

void HtmlParser::textChar(uint32_t cp)
{
    if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == '\f')
    {
        if (pre())
        {
            if (cp == '\n')
                simple(Item::BR);
            else if (cp == '\t')
                appendAscii("    ");
            else if (cp == ' ')
                appendAscii(" ");
            return;
        }
        if (!lastSpace_)
        {
            appendByte(' ');
            lastSpace_ = true;
        }
        return;
    }

    //  Formatting that something closed too early comes back for the text.
    if (nActive_)
        reconstruct();

    //  The few characters that are worth more than one cell to get right.
    if (cp == 0x2026)
    {
        appendAscii("...");
        return;
    }
    if (cp == 0x20AC)
    {
        appendAscii("EUR");
        return;
    }
    if (cp == 0x2122)
    {
        appendAscii("TM");
        return;
    }
    if (cp == 0x00A9)
    {
        appendAscii("(c)");
        return;
    }
    if (cp == 0x00A0)
    {
        //  Not collapsible, but a space all the same.
        appendByte(' ');
        lastSpace_ = false;
        return;
    }
    if (cp >= 'a' && cp <= 'z' && top().cs.upper == 1)
        cp -= 32;

    uint8_t g = glyphFor(cp);
    if (!g)
        return;
    appendByte(g);
    lastSpace_ = false;
}

uint32_t HtmlParser::poolValue(const uint8_t *s, size_t n)
{
    uint32_t start = (uint32_t)poolLen_;
    for (size_t i = 0; i < n && poolLen_ + 4 < sizeof(attrPool_);)
    {
        uint32_t cp;
        if (s[i] == '&')
        {
            cp = decodeEntity(s, n, i);
            if (!cp)
                cp = s[i++];
        }
        else
            cp = s[i++];
        //  Attribute values are mostly URLs; they stay as bytes (UTF-8 or
        //  not) because that is what goes back on the wire.
        if (cp < 0x80 || (cp < 0x100 && s[i - 1] == cp))
            attrPool_[poolLen_++] = (char)cp;
        else if (cp < 0x800)
        {
            attrPool_[poolLen_++] = (char)(0xC0 | (cp >> 6));
            attrPool_[poolLen_++] = (char)(0x80 | (cp & 0x3F));
        }
        else
        {
            attrPool_[poolLen_++] = (char)(0xE0 | ((cp >> 12) & 0x0F));
            attrPool_[poolLen_++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            attrPool_[poolLen_++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    attrPool_[poolLen_++] = 0;
    return start;
}

//  s[i] is just past the start tag; finds "</name", skips past its '>'.
//  For <title>, the text on the way is the page's title.  The content's
//  extent is reported when asked for.
size_t HtmlParser::skipRaw(const uint8_t *s, size_t n, size_t i, const char *name, bool collectTitle,
                           size_t *contentStart, size_t *contentEnd)
{
    char close[20] = "</";
    scat(close, name, sizeof(close));
    size_t cl = strlen(close);
    size_t start = i;
    size_t end = n;
    for (size_t k = i; k + cl <= n; k++)
    {
        if (s[k] == '<' && ieqn((const char *)s + k, close, cl) && (k + cl == n || !isNameChar(s[k + cl])))
        {
            end = k;
            break;
        }
    }
    if (contentStart)
        *contentStart = start;
    if (contentEnd)
        *contentEnd = end;

    if (collectTitle && !d_.title_[0])
    {
        size_t t = 0;
        bool space = true;
        for (size_t k = start; k < end && t + 1 < sizeof(d_.title_);)
        {
            uint32_t cp;
            if (s[k] == '&')
            {
                cp = decodeEntity(s, end, k);
                if (!cp)
                    cp = s[k++];
            }
            else
                cp = decodeAt(s, end, k, enc_);
            if (cp == ' ' || cp == '\n' || cp == '\t' || cp == '\r')
            {
                if (!space)
                    d_.title_[t++] = ' ';
                space = true;
                continue;
            }
            uint8_t g = glyphFor(cp);
            if (g)
            {
                d_.title_[t++] = (char)g;
                space = false;
            }
        }
        while (t && d_.title_[t - 1] == ' ')
            t--;
        d_.title_[t] = 0;
    }

    if (end >= n)
        return n;
    size_t k = end + cl;
    while (k < n && s[k] != '>')
        k++;
    return k < n ? k + 1 : n;
}

//  s[i] is just past a start tag; skips to the end of the element, counting
//  nested elements of the same name so that a hidden <div> full of <div>s
//  ends where it really ends.  Comments are stepped over as a whole.
size_t HtmlParser::skipElement(const uint8_t *s, size_t n, size_t i, const char *name)
{
    size_t nl = strlen(name);
    int depth = 1;
    while (i < n)
    {
        if (s[i] != '<')
        {
            i++;
            continue;
        }
        if (i + 3 < n && s[i + 1] == '!' && s[i + 2] == '-' && s[i + 3] == '-')
        {
            size_t k = i + 4;
            while (k + 2 < n && !(s[k] == '-' && s[k + 1] == '-' && s[k + 2] == '>'))
                k++;
            i = k + 3;
            continue;
        }
        bool close = i + 1 < n && s[i + 1] == '/';
        size_t at = i + (close ? 2 : 1);
        if (at + nl <= n && ieqn((const char *)s + at, name, nl) && (at + nl == n || !isNameChar(s[at + nl])))
        {
            size_t k = at + nl;
            while (k < n && s[k] != '>')
                k++;
            bool selfClosing = k > 0 && k < n && s[k - 1] == '/';
            if (close)
                depth--;
            else if (!selfClosing)
                depth++;
            i = k < n ? k + 1 : n;
            if (depth == 0)
                return i;
            continue;
        }
        i++;
    }
    return n;
}

//  s[i] is the '<'.  Returns the index just past the tag (or whatever it
//  turned out to be).
size_t HtmlParser::parseTag(const uint8_t *s, size_t n, size_t i)
{
    size_t j = i + 1;
    if (j >= n)
    {
        textChar('<');
        return n;
    }

    //  Comments, doctype, CDATA, processing instructions.
    if (s[j] == '!')
    {
        if (j + 2 < n && s[j + 1] == '-' && s[j + 2] == '-')
        {
            for (size_t k = j + 3; k + 2 < n; k++)
                if (s[k] == '-' && s[k + 1] == '-' && s[k + 2] == '>')
                    return k + 3;
            return n;
        }
        while (j < n && s[j] != '>')
            j++;
        return j < n ? j + 1 : n;
    }
    if (s[j] == '?')
    {
        while (j < n && s[j] != '>')
            j++;
        return j < n ? j + 1 : n;
    }

    bool end = false;
    if (s[j] == '/')
    {
        end = true;
        j++;
    }
    if (j >= n || !((s[j] >= 'a' && s[j] <= 'z') || (s[j] >= 'A' && s[j] <= 'Z')))
    {
        //  "a < b" in text.
        textChar('<');
        return i + 1;
    }

    char name[16];
    size_t k = 0;
    while (j < n && isNameChar(s[j]))
    {
        if (k + 1 < sizeof(name))
            name[k++] = lower((char)s[j]);
        j++;
    }
    name[k] = 0;

    //  Attributes.
    nAttrs_ = 0;
    poolLen_ = 0;
    bool selfClosing = false;
    while (j < n && s[j] != '>')
    {
        if (isSpace((char)s[j]))
        {
            j++;
            continue;
        }
        if (s[j] == '/')
        {
            selfClosing = true;
            j++;
            continue;
        }
        char an[16];
        size_t ak = 0;
        while (j < n && s[j] != '=' && s[j] != '>' && !isSpace((char)s[j]) && s[j] != '/')
        {
            if (ak + 1 < sizeof(an))
                an[ak++] = lower((char)s[j]);
            j++;
        }
        an[ak] = 0;
        if (!ak)
        {
            j++; // a stray character; step over it
            continue;
        }
        while (j < n && isSpace((char)s[j]))
            j++;
        const uint8_t *vs = s + j;
        size_t vl = 0;
        if (j < n && s[j] == '=')
        {
            j++;
            while (j < n && isSpace((char)s[j]))
                j++;
            if (j < n && (s[j] == '"' || s[j] == '\''))
            {
                uint8_t q = s[j++];
                vs = s + j;
                while (j < n && s[j] != q)
                    j++;
                vl = (size_t)((s + j) - vs);
                if (j < n)
                    j++;
            }
            else
            {
                vs = s + j;
                while (j < n && !isSpace((char)s[j]) && s[j] != '>')
                    j++;
                vl = (size_t)((s + j) - vs);
            }
        }
        if (nAttrs_ < MAX_ATTRS && !end)
        {
            Attr &a = attrs_[nAttrs_++];
            scopy(a.name, an, sizeof(a.name));
            a.value = poolValue(vs, vl);
        }
    }
    if (j < n)
        j++; // the '>'

    if (end)
        endTag(name);
    else
        startTag(name, selfClosing, s, n, j);
    return j;
}

//  The element as selectors see it: its name, id, classes and a few
//  attributes, hashed.
void HtmlParser::describe(const char *name, Elem &e)
{
    e = Elem{};
    scopy(e.tag, name, sizeof(e.tag));
    CssElement &c = e.sel;
    c.tag = cssHash(name, strlen(name), true);
    for (int a = 0; a < nAttrs_; a++)
    {
        const char *an = attrs_[a].name;
        const char *v = attrPool_ + attrs_[a].value;
        if (is(an, "id"))
            c.id = v[0] ? cssHash(v, strlen(v), false) : 0;
        else if (is(an, "class"))
        {
            const char *p = v;
            while (*p && c.nCls < CssElement::MAX_CLASSES)
            {
                while (isSpace(*p))
                    p++;
                const char *q = p;
                while (*q && !isSpace(*q))
                    q++;
                if (q > p)
                    c.cls[c.nCls++] = cssHash(p, (size_t)(q - p), false);
                p = q;
            }
        }
        else if (!is(an, "style") && c.nAttr < CssElement::MAX_ATTRS)
        {
            c.attrName[c.nAttr] = cssHash(an, strlen(an), true);
            c.attrValue[c.nAttr] = cssHash(v, strlen(v), false);
            c.nAttr++;
        }
    }
}

//  Pops the innermost open element, undoing what it did; `semantics` runs its
//  tag's end handling too (an element closed by its end tag, or implicitly by
//  something that cannot be inside it).
void HtmlParser::pop(bool semantics, bool implicit)
{
    if (overflow_)
    {
        overflow_--;
        return;
    }
    if (!depth_)
        return;
    Elem e = stack_[depth_ - 1];
    depth_--;
    indent_ -= e.indentDelta;

    static const char *const kFormatting[] = {"a",     "b",      "big",    "code", "em", "font", "i",
                                              "nobr",  "s",      "small",  "strike", "strong", "tt", "u",
                                              nullptr};
    if (implicit && isAny(e.tag, kFormatting))
    {
        if (nActive_ == MAX_ACTIVE)
        {
            memmove(active_, active_ + 1, sizeof(Active) * (MAX_ACTIVE - 1));
            nActive_--;
        }
        //  Outermost first: an element popped later was outside the ones
        //  popped before it.
        memmove(active_ + 1, active_, sizeof(Active) * (size_t)nActive_);
        active_[0].e = e;
        active_[0].link = (e.flags & F_HREF) ? link_ : -1;
        nActive_++;
    }

    //  The end handling sees the closing element's display and margin.
    suppress_ = (e.flags & F_INLINE) != 0;
    marginOverride_ = e.cs.marginBottom;
    if (semantics)
        close(e.tag);
    suppress_ = false;
    marginOverride_ = -1;

    if (e.flags & F_BUTTON)
        appendAscii("]");
    if (e.flags & F_LINK)
    {
        link_ = e.savedLink;
        ctrl_ = e.savedCtrl != 0;
    }
    if (e.flags & F_FORM)
        form_ = -1;
    if (e.flags & F_BLOCKIFY)
        softBlock(0);
}

void HtmlParser::reconstruct()
{
    int n = nActive_;
    nActive_ = 0;
    for (int k = 0; k < n && depth_ < MAX_DEPTH && !overflow_; k++)
    {
        Elem &e = stack_[depth_++];
        e = active_[k].e;
        e.flags &= F_LINK | F_HREF;
        e.indentDelta = 0;
        e.savedLink = link_;
        e.savedCtrl = ctrl_ ? 1 : 0;
        if (active_[k].link >= 0)
        {
            link_ = active_[k].link;
            ctrl_ = false;
        }
    }
}

//  Closes the innermost element named in `targets`, and everything inside it,
//  unless one named in `stops` comes first.
void HtmlParser::closeUntil(const char *const *targets, const char *const *stops)
{
    for (int k = depth_ - 1; k >= 0; k--)
    {
        if (isAny(stack_[k].tag, targets))
        {
            while (depth_ > k)
                pop(true, true);
            return;
        }
        if (stops && isAny(stack_[k].tag, stops))
            return;
    }
}

void HtmlParser::startTag(const char *name, bool selfClosing, const uint8_t *s, size_t n, size_t &i)
{
    (void)selfClosing; // "<div/>" is an open <div> in HTML
    if (is(name, "title"))
    {
        i = skipRaw(s, n, i, "title", true);
        return;
    }
    if (is(name, "style"))
    {
        size_t cs, ce;
        const char *media = attr("media");
        i = skipRaw(s, n, i, "style", false, &cs, &ce);
        if (cssOn_ && (!media || !ifind(media, strlen(media), "print")))
            css_->addSheet((const char *)s + cs, ce - cs, Css::AUTHOR);
        return;
    }
    if (is(name, "link"))
    {
        const char *rel = attr("rel");
        const char *href = attr("href");
        const char *media = attr("media");
        if (rel && href && href[0] && ifind(rel, strlen(rel), "stylesheet") &&
            !ifind(rel, strlen(rel), "alternate") && (!media || !ifind(media, strlen(media), "print")) &&
            d_.stylesheetCount() < 8)
        {
            uint32_t off = d_.addString(href, strlen(href));
            d_.sheetOffs_.append(&off, sizeof(off));
        }
        return;
    }
    if (isAny(name, kSkipped))
    {
        i = skipRaw(s, n, i, name, false);
        return;
    }
    if (is(name, "meta") || is(name, "base") || is(name, "head") || is(name, "html"))
        return;

    //  What this tag closes before it opens.
    static const char *const P[] = {"p", nullptr};
    static const char *const PSTOP[] = {"table", "td", "th", "button", "body", nullptr};
    static const char *const LI[] = {"li", nullptr};
    static const char *const LISTOP[] = {"ul", "ol", "menu", "dir", "table", nullptr};
    static const char *const DTDD[] = {"dt", "dd", nullptr};
    static const char *const DLSTOP[] = {"dl", "table", nullptr};
    static const char *const TR[] = {"tr", nullptr};
    static const char *const TRSTOP[] = {"table", nullptr};
    static const char *const CELL[] = {"td", "th", nullptr};
    static const char *const CELLSTOP[] = {"tr", "table", nullptr};
    static const char *const OPT[] = {"option", nullptr};
    static const char *const SECT[] = {"thead", "tbody", "tfoot", nullptr};
    if (isAny(name, kClosesP) || is(name, "li") || is(name, "dt") || is(name, "dd"))
        closeUntil(P, PSTOP);
    if (is(name, "li"))
        closeUntil(LI, LISTOP);
    else if (is(name, "dt") || is(name, "dd"))
        closeUntil(DTDD, DLSTOP);
    else if (is(name, "tr"))
        closeUntil(TR, TRSTOP);
    else if (is(name, "td") || is(name, "th"))
        closeUntil(CELL, CELLSTOP);
    else if (is(name, "option"))
        closeUntil(OPT, nullptr);
    else if (is(name, "thead") || is(name, "tbody") || is(name, "tfoot"))
        closeUntil(SECT, TRSTOP);
    if (is(name, "td") || is(name, "th") || is(name, "table") || is(name, "caption"))
        nActive_ = 0; // formatting does not cross into a cell
    else if (nActive_ && !isAny(name, kBlockish))
        reconstruct(); // an inline element goes inside the reopened ones

    //  The cascade, with the element in place at the top of the stack.
    Elem scratch;
    bool tracked = depth_ < MAX_DEPTH;
    Elem &e = tracked ? stack_[depth_] : scratch;
    describe(name, e);
    const Elem &parent = top();
    e.cs.inheritFrom(parent.cs);
    const CssElement *chain[MAX_DEPTH + 1];
    for (int k = 0; k < depth_; k++)
        chain[k] = &stack_[k].sel;
    chain[depth_] = &e.sel;
    css_->compute(chain, depth_ + 1, cssOn_ ? attr("style") : nullptr, e.cs);
    bool bodyish = is(name, "body") || is(name, "html");
    e.bgEff = e.cs.bg && !bodyish ? e.cs.bg : parent.bgEff;

    //  What the page says is not to be shown.
    const char *aria = attr("aria-hidden");
    if (!bodyish && (e.cs.display == CssStyle::D_NONE || e.cs.hidden == 1 ||
                     (aria && ieq(aria, "true") && !is(name, "svg"))))
    {
        if (!isAny(name, kVoid))
            i = skipElement(s, n, i, name);
        return;
    }

    bool voidTag = isAny(name, kVoid);
    bool blockish = isAny(name, kBlockish);
    e.savedLink = link_;
    e.savedCtrl = ctrl_ ? 1 : 0;
    if (tracked)
        depth_++;
    else
        overflow_++;

    //  A margin or padding on the left, as cells; lists and quotes count
    //  from the indent their tags give them anyway.
    if (tracked && e.cs.indent >= 0 && blockish)
    {
        int base = (is(name, "ul") || is(name, "ol") || is(name, "dir") || is(name, "menu")) ? 3
                   : (is(name, "blockquote") || is(name, "dd"))                           ? 4
                                                                                             : 0;
        e.indentDelta = (int8_t)(e.cs.indent - base);
        indent_ += e.indentDelta;
    }
    if (e.cs.display == CssStyle::D_BLOCK && !blockish && tracked)
    {
        e.flags |= F_BLOCKIFY;
        softBlock(0);
    }
    if (e.cs.display == CssStyle::D_INLINE && blockish && !is(name, "table") && !is(name, "tr"))
        e.flags |= F_INLINE;

    suppress_ = (e.flags & F_INLINE) != 0;
    marginOverride_ = e.cs.marginTop;
    bool consumed = open(name, s, n, i);
    suppress_ = false;
    marginOverride_ = -1;

    //  Elements with no content, and those whose content open() read
    //  through to the end tag, are done.
    if (voidTag || consumed)
        pop(false);
}

void HtmlParser::endTag(const char *name)
{
    for (int k = depth_ - 1; k >= 0; k--)
    {
        if (is(stack_[k].tag, name))
        {
            while (depth_ > k + 1)
                pop(true, true);
            pop(true);
            if (is(name, "td") || is(name, "th") || is(name, "table") || is(name, "caption"))
                nActive_ = 0;
            return;
        }
    }
    //  The end of a formatting element that was already closed for it: it is
    //  not to come back.
    for (int k = nActive_ - 1; k >= 0; k--)
        if (is(active_[k].e.tag, name))
        {
            memmove(active_ + k, active_ + k + 1, sizeof(Active) * (size_t)(nActive_ - k - 1));
            nActive_--;
            return;
        }
    //  An end tag with nothing open to end: the two that still mean something.
    if (is(name, "p"))
        softBlock(1); // "</p>" alone is read as an empty paragraph
    else if (is(name, "br"))
        simple(Item::BR);
}

// ─── Forms ───────────────────────────────────────────────────────────────────

int HtmlParser::addControl(Control &c, const char *name, const char *value)
{
    c.form = (int16_t)form_;
    c.name = d_.addString(name ? name : "", name ? strlen(name) : 0);
    c.value = d_.addString(value ? value : "", value ? strlen(value) : 0);
    if (c.isText())
    {
        size_t l = value ? strlen(value) : 0;
        c.editCap = (uint32_t)(l + 64);
        c.edit = (char *)big_alloc(c.editCap);
        if (c.edit)
        {
            memcpy(c.edit, value ? value : "", l);
            c.edit[l] = 0;
            c.editLen = (uint32_t)l;
        }
        else
            c.editCap = 0;
    }
    if (!d_.controls_.append(&c, sizeof(c)))
    {
        d_.oom_ = true;
        big_free(c.edit);
        return -1;
    }
    return d_.controlCount() - 1;
}

//  The cells a control takes in the text: a word of placeholders, drawn over
//  from the control's state.
void HtmlParser::cells(int ci, int width)
{
    int32_t lnk = controlLink(ci);
    int32_t savedLink = link_;
    bool savedCtrl = ctrl_;
    if (!lastSpace_)
        textChar(' ');
    flush();
    link_ = lnk;
    ctrl_ = true;
    d_.control(ci).textOff = (uint32_t)d_.text_.len;
    for (int k = 0; k < width; k++)
        appendByte('_');
    lastSpace_ = false;
    flush();
    link_ = savedLink;
    ctrl_ = savedCtrl;
}

void HtmlParser::button(int ci, const char *label)
{
    int32_t lnk = controlLink(ci);
    int32_t savedLink = link_;
    bool savedCtrl = ctrl_;
    if (!lastSpace_)
        textChar(' ');
    flush();
    link_ = lnk;
    ctrl_ = true;
    d_.control(ci).textOff = (uint32_t)d_.text_.len;
    appendAscii("[");
    appendUtf8(label);
    appendAscii("]");
    flush();
    link_ = savedLink;
    ctrl_ = savedCtrl;
}

//  A tag's attribute, read straight from the source (for <option>, whose
//  attributes arrive without going through parseTag).
static bool rawAttr(const uint8_t *t, size_t n, const char *name, char *out, size_t cap)
{
    size_t nl = strlen(name);
    for (size_t k = 0; k + nl <= n; k++)
    {
        if (!ieqn((const char *)t + k, name, nl) || (k && isNameChar(t[k - 1])) ||
            (k + nl < n && isNameChar(t[k + nl])))
            continue;
        size_t j = k + nl;
        while (j < n && isSpace((char)t[j]))
            j++;
        if (j >= n || t[j] != '=')
        {
            if (out && cap)
                out[0] = 0;
            return true; // present without a value
        }
        j++;
        while (j < n && isSpace((char)t[j]))
            j++;
        uint8_t q = (j < n && (t[j] == '"' || t[j] == '\'')) ? t[j++] : 0;
        size_t o = 0;
        while (j < n && (q ? t[j] != q : (!isSpace((char)t[j]) && t[j] != '>')))
        {
            if (o + 1 < cap)
                out[o++] = (char)t[j];
            j++;
        }
        if (cap)
            out[o] = 0;
        return true;
    }
    return false;
}

//  <select>: its options are read here, through to </select>.
size_t HtmlParser::selectElement(const uint8_t *s, size_t n, size_t i)
{
    Control c = {};
    c.type = Control::SELECT;
    c.options = (uint32_t)(d_.optionOffs_.len / (2 * sizeof(uint32_t)));
    const char *name = attr("name");
    char nameCopy[128];
    scopy(nameCopy, name ? name : "", sizeof(nameCopy));

    size_t cs, ce;
    size_t after = skipRaw(s, n, i, "select", false, &cs, &ce);
    int widest = 1;
    for (size_t k = cs; k < ce;)
    {
        const char *o = ifind((const char *)s + k, ce - k, "<option");
        if (!o)
            break;
        size_t ts = (size_t)((const uint8_t *)o - s) + 7;
        size_t te = ts;
        while (te < ce && s[te] != '>')
            te++;
        size_t ls = te + 1, le = ls;
        while (le < ce && s[le] != '<')
            le++;
        k = le;

        //  The label, as shown; the value, as sent.
        char label[64];
        size_t ln = 0;
        bool space = true;
        for (size_t q = ls; q < le && ln + 1 < sizeof(label);)
        {
            uint32_t cp;
            if (s[q] == '&')
            {
                cp = decodeEntity(s, le, q);
                if (!cp)
                    cp = s[q++];
            }
            else
                cp = decodeAt(s, le, q, enc_);
            if (cp <= ' ' || cp == 0xA0)
            {
                if (!space)
                    label[ln++] = ' ';
                space = true;
                continue;
            }
            uint8_t g = glyphFor(cp);
            if (g)
            {
                label[ln++] = (char)g;
                space = false;
            }
        }
        while (ln && label[ln - 1] == ' ')
            ln--;
        label[ln] = 0;
        char value[128];
        if (!rawAttr(s + ts, te - ts, "value", value, sizeof(value)))
        {
            //  No value: the text is the value, as the page has it.
            size_t a = ls, b = le;
            while (a < b && isSpace((char)s[a]))
                a++;
            while (b > a && isSpace((char)s[b - 1]))
                b--;
            scopyn(value, (const char *)s + a, b - a, sizeof(value));
        }
        if (rawAttr(s + ts, te - ts, "selected", nullptr, 0))
            c.selected = (int16_t)c.nOptions;
        uint32_t pair[2] = {d_.addString(value, strlen(value)), d_.addString(label, ln)};
        if (!d_.optionOffs_.append(pair, sizeof(pair)))
            break;
        c.nOptions++;
        if ((int)ln > widest)
            widest = (int)ln;
    }
    c.initialSelected = c.selected;
    c.width = (uint8_t)(widest + 4 > 40 ? 40 : widest + 4);
    int ci = addControl(c, nameCopy, nullptr);
    if (ci >= 0)
        cells(ci, d_.control(ci).width);
    return after;
}

//  <textarea>: its text is the initial value, read here through to the end tag.
size_t HtmlParser::textareaElement(const uint8_t *s, size_t n, size_t i)
{
    const char *name = attr("name");
    char nameCopy[128];
    scopy(nameCopy, name ? name : "", sizeof(nameCopy));
    int cols = 30;
    const char *ca = attr("cols");
    if (ca && *ca >= '0' && *ca <= '9')
    {
        cols = 0;
        while (*ca >= '0' && *ca <= '9')
            cols = cols * 10 + (*ca++ - '0');
    }
    size_t cs, ce;
    size_t after = skipRaw(s, n, i, "textarea", false, &cs, &ce);
    //  The content through the attribute decoder: entities resolved, the
    //  bytes otherwise as they are.
    poolLen_ = 0;
    size_t len = ce - cs;
    if (len > sizeof(attrPool_) / 2)
        len = sizeof(attrPool_) / 2;
    const char *value = attrPool_ + poolValue(s + cs, len);
    if (value[0] == '\n')
        value++; // a newline right after the tag is not part of the text
    Control c = {};
    c.type = Control::TEXTAREA;
    c.width = (uint8_t)(cols < 10 ? 10 : (cols > 60 ? 60 : cols));
    int ci = addControl(c, nameCopy, value);
    if (ci >= 0)
        cells(ci, d_.control(ci).width);
    return after;
}

//  A tag's own meaning.  Returns true when it read the element's content
//  through to its end tag (so it is to be popped again at once).
bool HtmlParser::open(const char *name, const uint8_t *s, size_t n, size_t &i)
{
    Elem &me = depth_ && !overflow_ ? stack_[depth_ - 1] : root_;
    if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2])
    {
        heading_ = name[1] - '0';
        block(1);
        return false;
    }
    if (is(name, "p"))
    {
        softBlock(1);
        return false;
    }
    if (is(name, "br"))
    {
        simple(Item::BR);
        return false;
    }
    if (is(name, "hr"))
    {
        simple(Item::HR);
        return false;
    }
    if (is(name, "pre") || is(name, "listing") || is(name, "xmp"))
    {
        block(1);
        return false;
    }
    if (is(name, "blockquote"))
    {
        indent_ += 4;
        block(1);
        return false;
    }
    if (is(name, "ul") || is(name, "ol") || is(name, "dir") || is(name, "menu"))
    {
        if (nLists_ < MAX_LISTS)
        {
            List &l = lists_[nLists_++];
            l.ordered = is(name, "ol");
            l.counter = 1;
            const char *st = attr("start");
            if (st && *st >= '0' && *st <= '9')
            {
                int v = 0;
                while (*st >= '0' && *st <= '9')
                    v = v * 10 + (*st++ - '0');
                l.counter = v;
            }
        }
        else
            listOverflow_++;
        indent_ += 3;
        block(nLists_ + listOverflow_ == 1 ? 1 : 0);
        return false;
    }
    if (is(name, "li"))
    {
        if (top().cs.listNone == 1 || suppress_)
        {
            block(0);
            return false;
        }
        char marker[12];
        if (nLists_ && lists_[nLists_ - 1].ordered)
        {
            marker[0] = 0;
            scatInt(marker, lists_[nLists_ - 1].counter++, sizeof(marker));
            scat(marker, ". ", sizeof(marker));
        }
        else
        {
            //  A bullet, a circle, then a dash for anything deeper.
            int depth = nLists_ > 0 ? nLists_ - 1 : 0;
            marker[0] = depth == 0 ? '\x07' : (depth == 1 ? '\x09' : '-');
            marker[1] = ' ';
            marker[2] = 0;
        }
        block(0, marker);
        return false;
    }
    if (is(name, "dl"))
    {
        block(1);
        return false;
    }
    if (is(name, "dt"))
    {
        block(0);
        return false;
    }
    if (is(name, "dd"))
    {
        indent_ += 4;
        block(0);
        return false;
    }
    if (is(name, "table"))
    {
        block(1);
        tables_++;
        setCell(false);
        return false;
    }
    if (is(name, "tr"))
    {
        setCell(false);
        cellsInRow_ = 0;
        block(0);
        return false;
    }
    if (is(name, "td") || is(name, "th"))
    {
        //  Cells are set apart by space alone: a rule between them is noise
        //  on the many pages that use tables only to place things.
        if (cellsInRow_++ > 0 && !lastSpace_)
            appendAscii("  ");
        lastSpace_ = true;
        setCell(true);
        return false;
    }
    if (is(name, "a"))
    {
        me.flags |= F_LINK;
        const char *href = attr("href");
        if (href && href[0])
        {
            link_ = addLink(href);
            ctrl_ = false;
            me.flags |= F_HREF;
        }
        return false;
    }
    if (is(name, "img"))
    {
        const char *alt = attr("alt");
        char buf[80];
        if (alt && alt[0])
        {
            scopy(buf, "[", sizeof(buf));
            scat(buf, alt, sizeof(buf) - 1);
            scat(buf, "]", sizeof(buf));
        }
        else if (link_ >= 0 && !ctrl_)
            scopy(buf, "[img]", sizeof(buf));
        else
            return false;
        flush();
        faint_++;
        appendUtf8(buf);
        faint_--;
        lastSpace_ = false;
        return false;
    }
    if (is(name, "form"))
    {
        uint32_t action = d_.addString(attr("action") ? attr("action") : "", attr("action") ? strlen(attr("action")) : 0);
        const char *method = attr("method");
        uint32_t rec[2] = {action, (uint32_t)(method && ieq(method, "post") ? 1 : 0)};
        if (d_.forms_.append(rec, sizeof(rec)))
        {
            form_ = (int)(d_.forms_.len / sizeof(rec)) - 1;
            me.flags |= F_FORM;
        }
        softBlock(0);
        return false;
    }
    if (is(name, "input"))
    {
        const char *type = attr("type");
        const char *value = attr("value");
        const char *name2 = attr("name");
        Control c = {};
        if (!type || !type[0])
            c.type = Control::TEXT;
        else if (ieq(type, "hidden"))
            c.type = Control::HIDDEN;
        else if (ieq(type, "password"))
            c.type = Control::PASSWORD;
        else if (ieq(type, "checkbox"))
            c.type = Control::CHECKBOX;
        else if (ieq(type, "radio"))
            c.type = Control::RADIO;
        else if (ieq(type, "submit"))
            c.type = Control::SUBMIT;
        else if (ieq(type, "reset"))
            c.type = Control::RESET;
        else if (ieq(type, "button"))
            c.type = Control::BUTTON;
        else if (ieq(type, "image"))
            c.type = Control::IMAGE;
        else if (ieq(type, "file") || ieq(type, "range") || ieq(type, "color"))
            return false; // nothing to type into here
        else
            c.type = Control::TEXT; // text, search, email, url, tel, number, date...
        if (attr("disabled"))
            return false;
        //  A box to tick with no name sends nothing anywhere: on today's pages
        //  that is a CSS toggle for a menu, not a question.
        if ((c.type == Control::CHECKBOX || c.type == Control::RADIO) && (!name2 || !name2[0]))
            return false;
        c.checked = c.initialChecked = attr("checked") ? 1 : 0;
        int size = 20;
        const char *sz = attr("size");
        if (sz && *sz >= '0' && *sz <= '9')
        {
            size = 0;
            while (*sz >= '0' && *sz <= '9')
                size = size * 10 + (*sz++ - '0');
        }
        c.width = (uint8_t)(c.type == Control::CHECKBOX || c.type == Control::RADIO
                                ? 3
                                : (size < 4 ? 4 : (size > 40 ? 40 : size)));
        int ci = addControl(c, name2, value);
        if (ci < 0 || c.type == Control::HIDDEN)
            return false;
        if (c.isButton())
        {
            const char *label = value && value[0] ? value
                                : c.type == Control::RESET ? "Reset"
                                : c.type == Control::IMAGE ? (attr("alt") && attr("alt")[0] ? attr("alt") : "Submit")
                                                           : "Submit";
            button(ci, label);
        }
        else
            cells(ci, c.width);
        return false;
    }
    if (is(name, "button"))
    {
        const char *type = attr("type");
        Control c = {};
        c.type = !type || ieq(type, "submit") ? Control::SUBMIT
                 : ieq(type, "reset")          ? Control::RESET
                                               : Control::BUTTON;
        int ci = addControl(c, attr("name"), attr("value"));
        if (ci < 0)
            return false;
        //  The button's content is its label, and all of it is the button.
        if (!lastSpace_)
            textChar(' ');
        flush();
        me.flags |= F_LINK | F_BUTTON;
        link_ = controlLink(ci);
        ctrl_ = true;
        d_.control(ci).textOff = (uint32_t)d_.text_.len;
        appendAscii("[");
        return false;
    }
    if (is(name, "select"))
    {
        i = selectElement(s, n, i);
        return true;
    }
    if (is(name, "textarea"))
    {
        i = textareaElement(s, n, i);
        return true;
    }
    if (is(name, "iframe") || is(name, "frame"))
    {
        const char *src = attr("src");
        if (src && src[0])
        {
            int32_t saved = link_;
            bool savedCtrl = ctrl_;
            link_ = addLink(src);
            ctrl_ = false;
            appendAscii("[frame]");
            link_ = saved;
            ctrl_ = savedCtrl;
        }
        if (is(name, "iframe"))
        {
            i = skipRaw(s, n, i, name, false);
            return true;
        }
        return false;
    }
    if (is(name, "video") || is(name, "audio"))
    {
        appendFaint(is(name, "video") ? "[video]" : "[audio]");
        i = skipRaw(s, n, i, name, false);
        return true;
    }
    if (is(name, "figure"))
    {
        softBlock(1);
        return false;
    }
    if (isAny(name, kPlainBlocks))
    {
        softBlock(0);
        return false;
    }
    return false;
}

//  A tag's own meaning, at its end.
void HtmlParser::close(const char *name)
{
    if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2])
    {
        heading_ = 0;
        block(1);
        return;
    }
    if (is(name, "p"))
    {
        softBlock(1);
        return;
    }
    if (is(name, "pre") || is(name, "listing") || is(name, "xmp"))
    {
        block(1);
        return;
    }
    if (is(name, "blockquote"))
    {
        indent_ -= 4;
        block(1);
        return;
    }
    if (is(name, "ul") || is(name, "ol") || is(name, "dir") || is(name, "menu"))
    {
        if (listOverflow_)
            listOverflow_--;
        else if (nLists_)
            nLists_--;
        indent_ -= 3;
        block(nLists_ + listOverflow_ == 0 ? 1 : 0);
        return;
    }
    if (is(name, "li") || is(name, "dt"))
    {
        block(0);
        return;
    }
    if (is(name, "dd"))
    {
        indent_ -= 4;
        block(0);
        return;
    }
    if (is(name, "dl"))
    {
        block(1);
        return;
    }
    if (is(name, "table"))
    {
        setCell(false);
        if (tables_ > 0)
            tables_--;
        block(1);
        return;
    }
    if (is(name, "tr"))
    {
        setCell(false);
        block(0);
        return;
    }
    if (is(name, "td") || is(name, "th"))
    {
        setCell(false);
        return;
    }
    if (is(name, "figure"))
    {
        softBlock(1);
        return;
    }
    if (isAny(name, kPlainBlocks))
    {
        softBlock(0);
        return;
    }
}

void HtmlParser::parseHtml(const uint8_t *s, size_t n)
{
    //  A byte order mark says UTF-8 whatever anything else says.
    size_t i = 0;
    if (n >= 3 && s[0] == 0xEF && s[1] == 0xBB && s[2] == 0xBF)
    {
        enc_ = ENC_UTF8;
        i = 3;
    }

    while (i < n && !d_.oom_)
    {
        uint8_t c = s[i];
        if (c == '<')
        {
            i = parseTag(s, n, i);
            continue;
        }
        if (c == '&')
        {
            uint32_t cp = decodeEntity(s, n, i);
            if (cp)
            {
                textChar(cp);
                continue;
            }
            i++;
            textChar('&');
            continue;
        }
        textChar(decodeAt(s, n, i, enc_));
    }
    while (depth_)
        pop(true);
    flush();
}

void HtmlParser::parseText(const uint8_t *s, size_t n)
{
    forcePre_ = true;
    block(0);
    size_t i = 0;
    if (n >= 3 && s[0] == 0xEF && s[1] == 0xBB && s[2] == 0xBF)
        i = 3;
    while (i < n && !d_.oom_)
        textChar(decodeAt(s, n, i, enc_));
    flush();
}

// ─── Document ────────────────────────────────────────────────────────────────

Document::Document() {}
Document::~Document() { clear(); }

void Document::clear()
{
    for (int c = 0; c < controlCount(); c++)
        big_free(control(c).edit);
    text_.release();
    items_.release();
    links_.release();
    linkOffs_.release();
    lines_.release();
    runs_.release();
    strings_.release();
    controls_.release();
    forms_.release();
    optionOffs_.release();
    sheetOffs_.release();
    title_[0] = 0;
    cols_ = rows_ = 0;
    oom_ = false;
}

uint32_t Document::addString(const char *s, size_t n)
{
    uint32_t off = (uint32_t)strings_.len;
    if (!strings_.append(s, n) || !strings_.push(0))
    {
        oom_ = true;
        return 0;
    }
    return off;
}

//  The charset a page declares for itself in a <meta>, when the server did not.
static void sniffCharset(const uint8_t *src, size_t n, char *out, size_t cap)
{
    size_t lim = n < 4096 ? n : 4096;
    const char *p = ifind((const char *)src, lim, "charset=");
    if (!p)
        return;
    p += 8;
    const char *end = (const char *)src + lim;
    while (p < end && (*p == '"' || *p == '\'' || *p == ' '))
        p++;
    size_t k = 0;
    while (p < end && k + 1 < cap && *p != '"' && *p != '\'' && *p != ';' && *p != '>' && *p != ' ' &&
           *p != '/')
        out[k++] = lower(*p++);
    out[k] = 0;
}

void Document::loadHtml(const uint8_t *src, size_t n, const char *charset, const StyleSheetText *sheets,
                        int nSheets, bool css)
{
    clear();
    char cs[24] = {};
    if (charset && charset[0])
        scopy(cs, charset, sizeof(cs));
    else
        sniffCharset(src, n, cs, sizeof(cs));
    HtmlParser *p = new HtmlParser(*this, encodingFor(cs), css);
    if (!p || !p->ok())
    {
        delete p;
        oom_ = true;
        return;
    }
    if (css)
        for (int k = 0; k < nSheets; k++)
            p->addSheet(sheets[k].data, sheets[k].len);
    p->parseHtml(src, n);
    delete p;
}

void Document::loadText(const uint8_t *src, size_t n, const char *charset)
{
    clear();
    HtmlParser *p = new HtmlParser(*this, encodingFor(charset), false);
    if (!p || !p->ok())
    {
        delete p;
        oom_ = true;
        return;
    }
    p->parseText(src, n);
    delete p;
}

const char *Document::linkHref(int i) const
{
    if (i < 0 || i >= linkCount())
        return "";
    return (const char *)links_.data + ((const uint32_t *)linkOffs_.data)[i];
}

int Document::linkControl(int i) const
{
    const char *h = linkHref(i);
    if (h[0] != '\x01')
        return -1;
    int v = 0;
    for (h++; *h >= '0' && *h <= '9'; h++)
        v = v * 10 + (*h - '0');
    return v < controlCount() ? v : -1;
}

const char *Document::formAction(int f) const
{
    if (f < 0 || (size_t)f >= forms_.len / 8)
        return "";
    return str(((const uint32_t *)forms_.data)[2 * f]);
}

bool Document::formPost(int f) const
{
    if (f < 0 || (size_t)f >= forms_.len / 8)
        return false;
    return ((const uint32_t *)forms_.data)[2 * f + 1] != 0;
}

const char *Document::controlText(int ci) const
{
    const Control &c = control(ci);
    if (c.isText())
        return c.edit ? c.edit : "";
    return str(c.value);
}

bool Document::controlCells(int ci, char *out, size_t cap) const
{
    const Control &c = control(ci);
    size_t w = c.width < cap ? c.width : cap - 1;
    for (size_t k = 0; k < w; k++)
        out[k] = '_';
    out[w] = 0;
    switch (c.type)
    {
    case Control::CHECKBOX:
        memcpy(out, c.checked ? "[x]" : "[ ]", 3 < w ? 3 : w);
        return true;
    case Control::RADIO:
        memcpy(out, c.checked ? "(*)" : "( )", 3 < w ? 3 : w);
        return true;
    case Control::SELECT:
    {
        const uint32_t *pairs = (const uint32_t *)optionOffs_.data + 2 * c.options;
        const char *label = c.nOptions ? str(pairs[2 * c.selected + 1]) : "";
        memset(out, ' ', w);
        out[0] = '[';
        size_t ll = strlen(label);
        if (ll > w - 4)
            ll = w - 4;
        memcpy(out + 1, label, ll);
        memcpy(out + w - 3, " v]", 3);
        return true;
    }
    case Control::TEXT:
    case Control::PASSWORD:
    case Control::TEXTAREA:
    {
        //  The text as glyphs, the end of it when it is longer than the field.
        char glyphs[512];
        size_t g = 0;
        const char *t = controlText(ci);
        size_t tl = strlen(t);
        for (size_t k = 0; k < tl && g + 1 < sizeof(glyphs);)
        {
            uint32_t cp = decodeAt((const uint8_t *)t, tl, k, ENC_UTF8);
            uint8_t ch = cp == '\n' || cp == '\r' ? ' ' : glyphFor(cp);
            if (ch)
                glyphs[g++] = c.type == Control::PASSWORD ? '*' : (char)ch;
        }
        size_t from = g >= w ? g - (w - 1) : 0;
        memcpy(out, glyphs + from, g - from);
        return true;
    }
    default:
        return false;
    }
}

void Document::controlInsert(int ci, char ch)
{
    Control &c = control(ci);
    if (!c.isText())
        return;
    if (c.editLen + 2 > c.editCap)
    {
        uint32_t cap = c.editCap ? c.editCap * 2 : 64;
        char *p = (char *)big_realloc(c.edit, cap);
        if (!p)
            return;
        c.edit = p;
        c.editCap = cap;
    }
    c.edit[c.editLen++] = ch;
    c.edit[c.editLen] = 0;
}

void Document::controlBackspace(int ci)
{
    Control &c = control(ci);
    if (!c.isText() || !c.editLen)
        return;
    //  A whole UTF-8 character, not its last byte.
    do
        c.editLen--;
    while (c.editLen && (c.edit[c.editLen] & 0xC0) == 0x80);
    c.edit[c.editLen] = 0;
}

void Document::controlActivate(int ci, bool backwards)
{
    Control &c = control(ci);
    switch (c.type)
    {
    case Control::CHECKBOX:
        c.checked = !c.checked;
        break;
    case Control::RADIO:
        for (int k = 0; k < controlCount(); k++)
        {
            Control &o = control(k);
            if (o.type == Control::RADIO && o.form == c.form && !strcmp(str(o.name), str(c.name)))
                o.checked = 0;
        }
        c.checked = 1;
        break;
    case Control::SELECT:
        if (c.nOptions)
            c.selected = (int16_t)((c.selected + (backwards ? c.nOptions - 1 : 1)) % c.nOptions);
        break;
    default:
        break;
    }
}

void Document::resetForm(int f)
{
    for (int k = 0; k < controlCount(); k++)
    {
        Control &c = control(k);
        if (c.form != f)
            continue;
        c.checked = c.initialChecked;
        c.selected = c.initialSelected;
        if (c.isText() && c.edit)
        {
            scopy(c.edit, str(c.value), c.editCap);
            c.editLen = (uint32_t)strlen(c.edit);
        }
    }
}

//  application/x-www-form-urlencoded, byte by byte.
static void formEncode(Buf &out, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    unsigned char prev = 0;
    for (; *s; prev = (unsigned char)*s, s++)
    {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '*')
            out.push(c);
        else if (c == ' ')
            out.push('+');
        else if (c == '\n' && prev != '\r')
            out.appendStr("%0D%0A");
        else
        {
            out.push('%');
            out.push((uint8_t)hex[c >> 4]);
            out.push((uint8_t)hex[c & 15]);
        }
    }
}

static void formPair(Buf &out, const char *name, const char *value)
{
    if (out.len)
        out.push('&');
    formEncode(out, name);
    out.push('=');
    formEncode(out, value);
}

bool Document::formData(int f, int submitter, Buf &out) const
{
    out.clear();
    for (int k = 0; k < controlCount(); k++)
    {
        const Control &c = control(k);
        if (c.form != f)
            continue;
        const char *name = str(c.name);
        if (!name[0])
            continue;
        switch (c.type)
        {
        case Control::TEXT:
        case Control::PASSWORD:
        case Control::TEXTAREA:
            formPair(out, name, controlText(k));
            break;
        case Control::HIDDEN:
            formPair(out, name, str(c.value));
            break;
        case Control::CHECKBOX:
        case Control::RADIO:
            if (c.checked)
                formPair(out, name, str(c.value)[0] ? str(c.value) : "on");
            break;
        case Control::SELECT:
            if (c.nOptions)
                formPair(out, name, str(((const uint32_t *)optionOffs_.data)[2 * (c.options + c.selected)]));
            break;
        case Control::SUBMIT:
            if (k == submitter)
                formPair(out, name, str(c.value));
            break;
        case Control::IMAGE:
            if (k == submitter)
            {
                char nx[160];
                scopy(nx, name, sizeof(nx) - 3);
                scat(nx, ".x", sizeof(nx));
                formPair(out, nx, "0");
                nx[strlen(nx) - 1] = 'y';
                formPair(out, nx, "0");
            }
            break;
        default:
            break;
        }
    }
    return !out.failed;
}

// ─── Layout ──────────────────────────────────────────────────────────────────

class Layout
{
public:
    Layout(Document &d, int cols) : d_(d), cols_(cols < 16 ? 16 : cols) {}
    void run();

private:
    Document &d_;
    int cols_;

    int row_ = 0;
    bool open_ = false;
    Line cur_ = {};
    int col_ = 0;
    int startCol_ = 0;
    bool big_ = false;
    bool pre_ = false;
    int indent_ = 0;
    int margin_ = 0;
    bool atStart_ = true;
    bool space_ = false;
    uint32_t markerOff_ = 0, markerLen_ = 0;

    //  Where the word being placed started on this line.  A word can be made
    //  of several items --- "(" then a link then ")" --- and when its last part
    //  does not fit, all of it moves down, not just that part.
    int wordCol_ = 0;

    //  The colours of the item being placed, and the current block's alignment.
    uint8_t fg_ = 0, bg_ = 0;
    int align_ = 0;

    int width() const { return big_ ? cols_ / 2 : cols_; }

    int indentCells() const
    {
        int ind = big_ ? indent_ / 2 : indent_;
        //  Deep nesting on a narrow window would leave nothing for the text.
        int maxInd = width() / 2;
        return ind > maxInd ? maxInd : ind;
    }

    Run *lastRun()
    {
        if (!open_ || !cur_.nRuns)
            return nullptr;
        return (Run *)d_.runs_.data + (d_.runs_.len / sizeof(Run) - 1);
    }

    void addRun(uint32_t off, int len, int col, uint8_t style, int32_t link)
    {
        Run r = {};
        r.off = off;
        r.len = (uint16_t)len;
        r.col = (uint16_t)col;
        r.style = style | (big_ ? ST_BIG : 0);
        r.fg = fg_;
        r.bg = bg_;
        r.link = link;
        if (!d_.runs_.append(&r, sizeof(r)))
        {
            d_.oom_ = true;
            return;
        }
        cur_.nRuns++;
    }

    void openLine()
    {
        if (open_)
            return;
        if (!atStart_)
            row_ += margin_;
        margin_ = 0;
        atStart_ = false;
        cur_ = Line{};
        cur_.firstRun = (uint32_t)(d_.runs_.len / sizeof(Run));
        cur_.big = big_;
        cur_.row = row_;
        open_ = true;
        startCol_ = col_ = indentCells();
        space_ = false;
        wordCol_ = col_;
        if (markerLen_)
        {
            int mc = startCol_ - (int)markerLen_;
            if (mc < 0)
                mc = 0;
            uint8_t fg = fg_, bg = bg_;
            fg_ = bg_ = 0;
            addRun(markerOff_, (int)markerLen_, mc, ST_FAINT, -1);
            fg_ = fg;
            bg_ = bg;
            if (mc + (int)markerLen_ > col_)
                startCol_ = col_ = mc + (int)markerLen_;
            wordCol_ = col_;
            markerLen_ = 0;
        }
    }

    void closeLine()
    {
        if (!open_)
            return;
        if (align_ && !pre_ && cur_.nRuns)
        {
            //  Centred or right-aligned: the whole line moves over.
            Run *runs = (Run *)d_.runs_.data + cur_.firstRun;
            int end = 0;
            for (uint32_t k = 0; k < cur_.nRuns; k++)
                if (runs[k].col + runs[k].len > end)
                    end = runs[k].col + runs[k].len;
            int free = width() - end;
            int shift = align_ == 1 ? free / 2 : free;
            if (shift > 0)
                for (uint32_t k = 0; k < cur_.nRuns; k++)
                    runs[k].col = (uint16_t)(runs[k].col + shift);
        }
        if (!d_.lines_.append(&cur_, sizeof(cur_)))
            d_.oom_ = true;
        row_ += cur_.big ? 2 : 1;
        open_ = false;
    }

    //  Puts text [off, off+len) at the current column, after a space if one is
    //  pending, joining the previous run when it can.
    void put(uint32_t off, int len, uint8_t style, int32_t link, bool withSpace)
    {
        Run *r = lastRun();
        uint8_t st = style | (big_ ? ST_BIG : 0);
        if (withSpace)
        {
            if (r && r->style == st && r->link == link && r->fg == fg_ && r->bg == bg_ && r->off + r->len + 1 == off &&
                d_.text_.data[off - 1] == ' ' && r->col + r->len == col_)
            {
                r->len = (uint16_t)(r->len + 1 + len);
                col_ += 1 + len;
                return;
            }
            col_++;
        }
        else if (r && r->style == st && r->link == link && r->fg == fg_ && r->bg == bg_ && r->off + r->len == off &&
                 r->col + r->len == col_)
        {
            r->len = (uint16_t)(r->len + len);
            col_ += len;
            return;
        }
        addRun(off, len, col_, style, link);
        col_ += len;
    }

    bool wrapGlued();
    void text(const Item &it);
};

//  Starts a new line and takes the beginning of the current word along, when
//  the word began on this line in an earlier item.  Returns false (and does
//  nothing) when there is no such beginning or it would not fit either.
bool Layout::wrapGlued()
{
    if (!open_ || wordCol_ <= startCol_ || wordCol_ >= col_)
        return false;
    int moved = col_ - wordCol_;
    int newStart = indentCells();
    if (newStart + moved >= width())
        return false;

    //  The pieces of the runs that lie at or after wordCol_.
    static const int MAX_PIECES = 16;
    Run pieces[MAX_PIECES];
    int np = 0;
    Run *runs = (Run *)d_.runs_.data;
    size_t total = d_.runs_.len / sizeof(Run);
    size_t first = cur_.firstRun;
    size_t k = total;
    while (k > first && runs[k - 1].col + runs[k - 1].len > wordCol_)
        k--;
    if (total - k > (size_t)MAX_PIECES)
        return false;
    for (size_t r = k; r < total; r++)
    {
        Run piece = runs[r];
        if (piece.col < wordCol_)
        {
            int cut = wordCol_ - piece.col;
            piece.off += (uint32_t)cut;
            piece.len = (uint16_t)(piece.len - cut);
            piece.col = (uint16_t)wordCol_;
            //  What stays behind, without the space it ended on.
            int keep = cut;
            while (keep > 0 && d_.text_.data[runs[r].off + (uint32_t)keep - 1] == ' ')
                keep--;
            runs[r].len = (uint16_t)keep;
        }
        else
        {
            cur_.nRuns--;
            d_.runs_.len -= sizeof(Run);
        }
        pieces[np++] = piece;
    }
    //  Runs that were split stay on this line; the loop above removed only
    //  the ones that moved whole, which are all at the end.
    //  (A split run is always the first piece, so this ordering holds.)

    col_ = wordCol_;
    closeLine();
    openLine();
    for (int i = 0; i < np; i++)
    {
        const Run &pc = pieces[i];
        uint8_t fg = fg_, bg = bg_;
        fg_ = pc.fg;
        bg_ = pc.bg;
        addRun(pc.off, pc.len, col_ + (pc.col - (int)pieces[0].col), pc.style & (uint8_t)~ST_BIG, pc.link);
        fg_ = fg;
        bg_ = bg;
    }
    col_ += moved;
    wordCol_ = startCol_;
    return true;
}

void Layout::text(const Item &it)
{
    fg_ = it.fg;
    bg_ = it.bg;
    const char *t = (const char *)d_.text_.data;
    uint32_t i = it.off, end = it.off + it.len;

    if (pre_)
    {
        //  Kept as it is; a line wider than the window wraps rather than
        //  being cut, so nothing on it is lost.
        while (i < end)
        {
            openLine();
            int room = width() - col_;
            if (room <= 0)
            {
                closeLine();
                continue;
            }
            int take = (int)(end - i) < room ? (int)(end - i) : room;
            put(i, take, it.style, it.link, false);
            i += (uint32_t)take;
        }
        return;
    }

    while (i < end)
    {
        if (t[i] == ' ')
        {
            if (open_ && col_ > startCol_)
                space_ = true;
            i++;
            continue;
        }
        uint32_t j = i;
        while (j < end && t[j] != ' ')
            j++;
        int wl = (int)(j - i);

        openLine();
        int need = (space_ ? 1 : 0) + wl;
        if (col_ + need > width() && col_ > startCol_)
        {
            if (space_ || !wrapGlued())
            {
                closeLine();
                openLine();
            }
            space_ = false;
        }
        if (space_ || col_ == startCol_)
            wordCol_ = col_ + (space_ ? 1 : 0);
        //  A word longer than the line is broken where the line ends.
        while (col_ + (space_ ? 1 : 0) + wl > width())
        {
            int room = width() - col_ - (space_ ? 1 : 0);
            if (room <= 0)
            {
                closeLine();
                openLine();
                space_ = false;
                continue;
            }
            put(i, room, it.style, it.link, space_);
            space_ = false;
            i += (uint32_t)room;
            wl -= room;
            closeLine();
            openLine();
        }
        put(i, wl, it.style, it.link, space_);
        space_ = false;
        i = j;
    }
}

void Layout::run()
{
    d_.lines_.clear();
    d_.runs_.clear();

    const Item *items = (const Item *)d_.items_.data;
    size_t count = d_.items_.len / sizeof(Item);
    for (size_t k = 0; k < count && !d_.oom_; k++)
    {
        const Item &it = items[k];
        switch (it.kind)
        {
        case Item::BLOCK:
            closeLine();
            if (it.margin > margin_)
                margin_ = it.margin;
            indent_ = it.indent;
            big_ = it.heading == 1 || it.heading == 2;
            pre_ = it.pre;
            align_ = it.align;
            if (it.len)
            {
                markerOff_ = it.off;
                markerLen_ = it.len;
            }
            break;
        case Item::BR:
            if (open_)
                closeLine();
            else
            {
                openLine();
                closeLine();
            }
            break;
        case Item::HR:
            closeLine();
            if (margin_ < 1)
                margin_ = 1;
            openLine();
            cur_.hr = 1;
            closeLine();
            margin_ = 1;
            break;
        case Item::TEXT:
            text(it);
            break;
        }
    }
    closeLine();
    d_.rows_ = row_;
}

void Document::layout(int cols)
{
    Layout l(*this, cols);
    l.run();
    cols_ = cols;
}

size_t Document::lineAtRow(int row) const
{
    size_t lo = 0, hi = lineCount();
    while (lo < hi)
    {
        size_t mid = (lo + hi) / 2;
        const Line &l = line(mid);
        if (l.row + (l.big ? 2 : 1) <= row)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

int Document::linkRow(int i) const
{
    for (size_t li = 0; li < lineCount(); li++)
    {
        const Line &l = line(li);
        for (uint32_t r = 0; r < l.nRuns; r++)
            if (run(l.firstRun + r).link == i)
                return l.row;
    }
    return -1;
}

bool Document::find(const char *needle, size_t &lineIdx, size_t &runIdx) const
{
    size_t nl = strlen(needle);
    if (!nl)
        return false;
    for (size_t li = lineIdx; li < lineCount(); li++)
    {
        const Line &l = line(li);
        for (uint32_t r = (li == lineIdx ? (uint32_t)runIdx : 0); r < l.nRuns; r++)
        {
            const Run &ru = run(l.firstRun + r);
            if (ifind(text(ru.off), ru.len, needle))
            {
                lineIdx = li;
                runIdx = r;
                return true;
            }
        }
    }
    return false;
}

} // namespace web
