//
//  htmlparse.cpp --- HTML into dom.js's operations.  See htmlparse.h.
//
//  The tokenizer reads tags, text, comments and character references and
//  hands them to the Builder, which keeps the stack of open elements and
//  decides where each goes: what ends a paragraph, which tags imply others,
//  when an end tag is to be ignored.  Neither recurses, so a page nested
//  thousands deep costs a deep stack of names, not of frames; past MaxDepth
//  elements are still read, just not nested further.
//
#include "htmlparse.h"
#include "../memento-hello/web/doc.h"

namespace web {
namespace {

const int MaxDepth = 256;
const int NameCap = 32;

bool alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool alnum(char c) { return alpha(c) || (c >= '0' && c <= '9'); }
bool space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r'; }

bool in(const char *name, const char *const *list)
{
    for (; *list; list++)
        if (!strcmp(name, *list))
            return true;
    return false;
}

const char *const VOIDS[] = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "source",
                             "track", "wbr", "param", "keygen", "basefont", "bgsound", "frame", nullptr};
const char *const HEAD_CONTENT[] = {"base", "basefont", "bgsound", "link", "meta", "noscript", "script", "style",
                                    "template", "title", nullptr};
const char *const CLOSES_P[] = {"address", "article", "aside", "blockquote", "center", "details", "dialog", "dir",
                                "div", "dl", "fieldset", "figcaption", "figure", "footer", "form", "h1", "h2", "h3",
                                "h4", "h5", "h6", "header", "hgroup", "hr", "main", "menu", "nav", "ol", "p", "pre",
                                "listing", "section", "summary", "table", "ul", "search", "plaintext", "xmp", "li",
                                "dd", "dt", nullptr};
//  What an end tag does not reach past, unless it is the one being closed.
const char *const SCOPE[] = {"applet", "caption", "html", "table", "td", "th", "marquee", "object", "template", nullptr};
//  Tags that end foreign content (SVG, MathML) and are HTML again.
const char *const BREAKOUT[] = {"b", "big", "blockquote", "body", "br", "center", "code", "dd", "div", "dl", "dt",
                                "em", "embed", "h1", "h2", "h3", "h4", "h5", "h6", "head", "hr", "i", "img", "li",
                                "listing", "menu", "meta", "nobr", "ol", "p", "pre", "ruby", "s", "small", "span",
                                "strong", "strike", "sub", "sup", "table", "tt", "u", "ul", "var", nullptr};
const char *const HEADINGS[] = {"h1", "h2", "h3", "h4", "h5", "h6", nullptr};
const char *const SECTIONS[] = {"tbody", "thead", "tfoot", nullptr};

void putUtf8(Buf &b, uint32_t c)
{
    uint8_t u[4];
    size_t k;
    if (c < 0x80)
        u[0] = (uint8_t)c, k = 1;
    else if (c < 0x800)
        u[0] = (uint8_t)(0xC0 | (c >> 6)), u[1] = (uint8_t)(0x80 | (c & 0x3F)), k = 2;
    else if (c < 0x10000)
        u[0] = (uint8_t)(0xE0 | (c >> 12)), u[1] = (uint8_t)(0x80 | ((c >> 6) & 0x3F)), u[2] = (uint8_t)(0x80 | (c & 0x3F)), k = 3;
    else
        u[0] = (uint8_t)(0xF0 | (c >> 18)), u[1] = (uint8_t)(0x80 | ((c >> 12) & 0x3F)),
        u[2] = (uint8_t)(0x80 | ((c >> 6) & 0x3F)), u[3] = (uint8_t)(0x80 | (c & 0x3F)), k = 4;
    b.append(u, k);
}

//  A tag as the tokenizer read it.  Names and values are NUL-separated in
//  `attrs`, name first; `raw` keeps the case the page wrote, for SVG.
struct Tag
{
    char name[NameCap], raw[NameCap];
    Buf attrs;
    int nAttrs = 0;
    bool selfClosing = false;
};

class Builder
{
public:
    enum Raw { NONE, RAWTEXT, RCDATA, PLAINTEXT };

    Builder(JSContext *ctx, bool fragment) : ctx_(ctx), fragment_(fragment)
    {
        ops_ = JS_NewArray(ctx);
        mode_ = fragment ? IN_BODY : INITIAL;
    }

    JSValue finish()
    {
        if (!fragment_)
            body();
        flush();
        while (depth_ + overflow_ > 0)
            pop();
        return ops_;
    }

    //  Text, already decoded.
    void text(const char *s, size_t n)
    {
        if (!n)
            return;
        //  Inside an element of the head (a title, a style sheet) text is that
        //  element's.
        bool inHeadElement = depth_ && strcmp(top(), "head") && strcmp(top(), "html");
        if (!fragment_ && mode_ != IN_BODY && !inHeadElement)
        {
            //  Spaces before the body stay where they are (or vanish before the
            //  head); anything else starts the body.
            size_t k = 0;
            while (k < n && space(s[k]))
                k++;
            if (mode_ == IN_HEAD || mode_ == AFTER_HEAD)
                text_.append(s, k);
            if (k == n)
                return;
            s += k, n -= k;
            body();
        }
        text_.append(s, n);
    }

    void comment(const char *s, size_t n)
    {
        flush();
        JSValue c = JS_NewArray(ctx_);
        JS_SetPropertyUint32(ctx_, c, 0, JS_NewInt32(ctx_, 8));
        JS_SetPropertyUint32(ctx_, c, 1, JS_NewStringLen(ctx_, s, n));
        push(c);
    }

    void doctype(const char *name)
    {
        if (fragment_ || mode_ != INITIAL)
            return;
        flush();
        JSValue d = JS_NewArray(ctx_);
        JS_SetPropertyUint32(ctx_, d, 0, JS_NewInt32(ctx_, 10));
        JS_SetPropertyUint32(ctx_, d, 1, JS_NewString(ctx_, name[0] ? name : "html"));
        push(d);
    }

    //  How the tokenizer goes on after this tag: as raw text up to its end
    //  tag, or as markup.
    Raw start(Tag &t)
    {
        const char *name = t.name;
        if (!fragment_)
        {
            if (!strcmp(name, "html"))
            {
                if (mode_ == INITIAL)
                {
                    open(t, 0);
                    mode_ = BEFORE_HEAD;
                }
                return NONE;
            }
            if (mode_ == INITIAL)
                html();
            if (!strcmp(name, "head"))
            {
                if (mode_ == BEFORE_HEAD)
                {
                    open(t, 0);
                    mode_ = IN_HEAD;
                }
                return NONE;
            }
            if (in(name, HEAD_CONTENT) && (mode_ == BEFORE_HEAD || mode_ == IN_HEAD) && currentNs() == 0)
            {
                head();
                return element(t);
            }
            //  A template's content is its own, wherever the template is.
            if (mode_ != IN_BODY && inTemplate())
                return element(t);
            if (!strcmp(name, "body") || !strcmp(name, "frameset"))
            {
                if (mode_ != IN_BODY)
                {
                    head();
                    if (mode_ == IN_HEAD)
                        closeTo("head");
                    open(t, 0);
                    mode_ = IN_BODY;
                }
                return NONE;
            }
            body();
        }
        else if (!strcmp(name, "html") || !strcmp(name, "head") || !strcmp(name, "body"))
            return NONE;
        return element(t);
    }

    void end(Tag &t)
    {
        const char *name = t.name;
        if (!fragment_)
        {
            if (!strcmp(name, "head"))
            {
                if (mode_ == BEFORE_HEAD)
                    head();
                if (mode_ == IN_HEAD)
                {
                    closeTo("head");
                    mode_ = AFTER_HEAD;
                }
                return;
            }
            //  Kept open: whatever follows them still belongs to the body.
            if (!strcmp(name, "body") || !strcmp(name, "html"))
                return;
            if (mode_ != IN_BODY && strcmp(name, "title") && strcmp(name, "style") && strcmp(name, "script") &&
                strcmp(name, "noscript") && strcmp(name, "template") && !inTemplate())
                body();
        }
        if (!strcmp(name, "br"))
        {
            Tag br;
            scopy(br.name, "br", sizeof(br.name));
            element(br);
            return;
        }
        if (!strcmp(name, "p") && find("p", SCOPE) < 0 && currentNs() == 0)
        {
            Tag p;
            scopy(p.name, "p", sizeof(p.name));
            open(p, 0);
            pop();
            return;
        }
        int k;
        if (!strcmp(name, "tr") || !strcmp(name, "table") || in(name, SECTIONS))
        {
            static const char *const TABLE_SCOPE[] = {"html", "template", nullptr};
            k = find(name, TABLE_SCOPE);
        }
        else
            k = find(name, SCOPE, t.raw);
        if (k >= 0)
            closeAt(k);
    }

private:
    enum Mode { INITIAL, BEFORE_HEAD, IN_HEAD, AFTER_HEAD, IN_BODY };
    struct Open
    {
        char name[NameCap];
        uint8_t ns;
    };

    JSContext *ctx_;
    JSValue ops_;
    uint32_t count_ = 0;
    bool fragment_;
    Mode mode_;
    Buf text_;
    Open stack_[MaxDepth];
    int depth_ = 0, overflow_ = 0;
    bool haveForm_ = false;

    void push(JSValue v) { JS_SetPropertyUint32(ctx_, ops_, count_++, v); }
    void flush()
    {
        if (!text_.len)
            return;
        push(JS_NewStringLen(ctx_, (const char *)text_.data, text_.len));
        text_.clear();
    }
    int currentNs() const { return depth_ ? stack_[depth_ - 1].ns : 0; }
    bool inTemplate() const
    {
        for (int k = 0; k < depth_; k++)
            if (!strcmp(stack_[k].name, "template"))
                return true;
        return false;
    }
    const char *top() const { return depth_ ? stack_[depth_ - 1].name : ""; }

    void open(const Tag &t, int ns)
    {
        flush();
        JSValue el = JS_NewArray(ctx_), attrs = JS_NewArray(ctx_);
        JS_SetPropertyUint32(ctx_, el, 0, JS_NewInt32(ctx_, 1));
        JS_SetPropertyUint32(ctx_, el, 1, JS_NewString(ctx_, ns ? (t.raw[0] ? t.raw : t.name) : t.name));
        const char *a = t.attrs.data ? (const char *)t.attrs.data : "";
        for (int i = 0; i < t.nAttrs * 2; i++)
        {
            size_t len = strlen(a);
            if (!(i & 1) && !ns)
            {
                //  HTML attribute names are lower case; SVG keeps viewBox.
                char name[64];
                size_t m = 0;
                for (; m < len && m + 1 < sizeof(name); m++)
                    name[m] = lower(a[m]);
                JS_SetPropertyUint32(ctx_, attrs, (uint32_t)i, JS_NewStringLen(ctx_, name, m));
            }
            else
                JS_SetPropertyUint32(ctx_, attrs, (uint32_t)i, JS_NewStringLen(ctx_, a, len));
            a += len + 1;
        }
        JS_SetPropertyUint32(ctx_, el, 2, attrs);
        JS_SetPropertyUint32(ctx_, el, 3, JS_NewInt32(ctx_, ns));
        push(el);
        if (depth_ < MaxDepth)
        {
            scopy(stack_[depth_].name, ns ? (t.raw[0] ? t.raw : t.name) : t.name, NameCap);
            stack_[depth_].ns = (uint8_t)ns;
            depth_++;
        }
        else
            overflow_++;
        if (!strcmp(t.name, "form") && !ns)
            haveForm_ = true;
    }

    void pop()
    {
        flush();
        push(JS_NewInt32(ctx_, 2));
        if (overflow_)
            overflow_--;
        else if (depth_)
        {
            if (!strcmp(stack_[depth_ - 1].name, "form"))
                haveForm_ = false;
            depth_--;
        }
    }

    void closeAt(int k)
    {
        while (overflow_)
            pop();
        while (depth_ > k)
            pop();
    }

    void closeTo(const char *name)
    {
        for (int k = depth_ - 1; k >= 0; k--)
            if (!strcmp(stack_[k].name, name))
            {
                closeAt(k);
                return;
            }
    }

    //  The open element called `name`, searching down from the top no
    //  further than a scope boundary (unless that is the one wanted).  Foreign
    //  elements match regardless of case.
    int find(const char *name, const char *const *boundary, const char *raw = nullptr) const
    {
        for (int k = depth_ - 1; k >= 0; k--)
        {
            const Open &o = stack_[k];
            if (o.ns ? (ieq(o.name, raw && raw[0] ? raw : name)) : !strcmp(o.name, name))
                return k;
            if (!o.ns && in(o.name, boundary))
                return -1;
        }
        return -1;
    }

    void html()
    {
        Tag t;
        scopy(t.name, "html", sizeof(t.name));
        open(t, 0);
        mode_ = BEFORE_HEAD;
    }
    void head()
    {
        if (fragment_)
            return;
        if (mode_ == INITIAL)
            html();
        if (mode_ == BEFORE_HEAD)
        {
            Tag t;
            scopy(t.name, "head", sizeof(t.name));
            open(t, 0);
            mode_ = IN_HEAD;
        }
    }
    void body()
    {
        if (fragment_ || mode_ == IN_BODY)
            return;
        head();
        if (mode_ == IN_HEAD)
        {
            closeTo("head");
            mode_ = AFTER_HEAD;
        }
        Tag t;
        scopy(t.name, "body", sizeof(t.name));
        open(t, 0);
        mode_ = IN_BODY;
    }

    void closeP()
    {
        int k = find("p", SCOPE);
        if (k >= 0)
            closeAt(k);
    }

    Raw element(Tag &t)
    {
        const char *name = t.name;
        int ns = currentNs();
        if (ns)
        {
            bool fontBreak = !strcmp(name, "font");
            if (in(name, BREAKOUT) && (!fontBreak || t.nAttrs))
            {
                while (depth_ && stack_[depth_ - 1].ns)
                    pop();
                ns = 0;
            }
            else
            {
                open(t, ns);
                if (t.selfClosing)
                    pop();
                return NONE;
            }
        }
        if (!strcmp(name, "svg") || !strcmp(name, "math"))
        {
            open(t, name[0] == 's' ? 1 : 2);
            if (t.selfClosing)
                pop();
            return NONE;
        }

        //  What this tag ends first.
        if (in(name, CLOSES_P))
            closeP();
        if (in(name, HEADINGS) && in(top(), HEADINGS))
            pop();
        if (!strcmp(name, "li") || !strcmp(name, "dd") || !strcmp(name, "dt"))
        {
            for (int k = depth_ - 1; k >= 0; k--)
            {
                const char *o = stack_[k].name;
                bool same = !strcmp(name, "li") ? !strcmp(o, "li") : (!strcmp(o, "dd") || !strcmp(o, "dt"));
                if (same)
                {
                    closeAt(k);
                    break;
                }
                if (!strcmp(o, "ul") || !strcmp(o, "ol") || !strcmp(o, "dl") || !strcmp(o, "menu") || in(o, SCOPE))
                    break;
            }
        }
        if (!strcmp(name, "option") && !strcmp(top(), "option"))
            pop();
        if (!strcmp(name, "optgroup"))
        {
            if (!strcmp(top(), "option"))
                pop();
            if (!strcmp(top(), "optgroup"))
                pop();
        }
        if (!strcmp(name, "a"))
        {
            int k = find("a", SCOPE);
            if (k >= 0)
                closeAt(k);
        }
        if (!strcmp(name, "button") || !strcmp(name, "nobr"))
        {
            int k = find(name, SCOPE);
            if (k >= 0)
                closeAt(k);
        }
        if (!strcmp(name, "select"))
        {
            int k = find("select", SCOPE);
            if (k >= 0)
            {
                closeAt(k);
                return NONE;
            }
        }
        if (!strcmp(name, "form") && haveForm_)
            return NONE;
        if (!strcmp(name, "tr"))
        {
            static const char *const B[] = {"table", "html", "template", nullptr};
            int k = find("tr", B);
            if (k >= 0)
                closeAt(k);
            if (!strcmp(top(), "table"))
                implied("tbody");
        }
        if (!strcmp(name, "td") || !strcmp(name, "th"))
        {
            static const char *const B[] = {"table", "html", "template", nullptr};
            int k = find("td", B);
            if (k < 0)
                k = find("th", B);
            if (k >= 0)
                closeAt(k);
            if (!strcmp(top(), "table"))
                implied("tbody");
            if (in(top(), SECTIONS))
                implied("tr");
        }
        if (in(name, SECTIONS) || !strcmp(name, "caption") || !strcmp(name, "colgroup"))
        {
            static const char *const B[] = {"html", "template", nullptr};
            for (int k = depth_ - 1; k >= 0; k--)
            {
                const char *o = stack_[k].name;
                if (!strcmp(o, "table"))
                {
                    closeAt(k + 1);
                    break;
                }
                if (in(o, B))
                    break;
            }
        }
        if (!strcmp(name, "col") && !strcmp(top(), "table"))
            implied("colgroup");

        open(t, 0);
        if (in(name, VOIDS))
        {
            pop();
            return NONE;
        }
        if (!strcmp(name, "script") || !strcmp(name, "style") || !strcmp(name, "xmp") || !strcmp(name, "iframe") ||
            !strcmp(name, "noembed") || !strcmp(name, "noframes") || !strcmp(name, "noscript"))
            return RAWTEXT;
        if (!strcmp(name, "textarea") || !strcmp(name, "title"))
            return RCDATA;
        if (!strcmp(name, "plaintext"))
            return PLAINTEXT;
        return NONE;
    }

    void implied(const char *name)
    {
        Tag t;
        scopy(t.name, name, sizeof(t.name));
        open(t, 0);
    }
};

//  Appends the text s[0..n) with CR LF and lone CR made LF.
void appendText(Buf &out, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        if (s[i] == '\r')
        {
            out.push('\n');
            if (i + 1 < n && s[i + 1] == '\n')
                i++;
        }
        else if (s[i])
            out.push((uint8_t)s[i]);
    }
}

//  A character reference at s[i] into out; false when there is none (the
//  '&' is then text).  In an attribute value, a reference without ';'
//  followed by '=' or a letter stays as written, for URLs like ?a=1&copy=2.
bool reference(const char *s, size_t n, size_t &i, bool attr, Buf &out)
{
    size_t j = i;
    uint32_t cp = characterReference((const uint8_t *)s, n, j);
    if (!cp)
        return false;
    if (attr && s[j - 1] != ';' && j < n && (s[j] == '=' || alnum(s[j])))
        return false;
    putUtf8(out, cp);
    i = j;
    return true;
}

class Tokenizer
{
public:
    Tokenizer(const char *s, size_t n, Builder &b) : s_(s), n_(n), b_(b) {}

    void run(Builder::Raw initial, const char *initialTag)
    {
        size_t i = 0;
        //  A byte order mark is not text.
        if (n_ >= 3 && (uint8_t)s_[0] == 0xEF && (uint8_t)s_[1] == 0xBB && (uint8_t)s_[2] == 0xBF)
            i = 3;
        if (initial != Builder::NONE)
            i = raw(i, initial, initialTag);
        while (i < n_)
        {
            char c = s_[i];
            if (c == '<')
            {
                i = markup(i);
                continue;
            }
            if (c == '&')
            {
                text_.clear();
                if (!reference(s_, n_, i, false, text_))
                {
                    text_.push('&');
                    i++;
                }
                b_.text((const char *)text_.data, text_.len);
                continue;
            }
            size_t j = i;
            while (j < n_ && s_[j] != '<' && s_[j] != '&')
                j++;
            emit(i, j);
            i = j;
        }
    }

private:
    const char *s_;
    size_t n_;
    Builder &b_;
    Buf text_;
    Tag tag_;
    bool skipLF_ = false;

    void emit(size_t from, size_t to)
    {
        if (skipLF_)
        {
            skipLF_ = false;
            if (from < to && s_[from] == '\r')
                from++;
            if (from < to && s_[from] == '\n')
                from++;
        }
        text_.clear();
        appendText(text_, s_ + from, to - from);
        b_.text((const char *)text_.data, text_.len);
    }

    //  "<...": a tag, a comment, a doctype --- or text that starts with '<'.
    size_t markup(size_t i)
    {
        size_t j = i + 1;
        if (j < n_ && s_[j] == '!')
        {
            if (j + 2 < n_ && s_[j + 1] == '-' && s_[j + 2] == '-')
            {
                size_t start = j + 3, k = start;
                while (k < n_ && !(s_[k] == '-' && k + 2 < n_ && s_[k + 1] == '-' && (s_[k + 2] == '>' ||
                                                                                       (s_[k + 2] == '!' && k + 3 < n_ && s_[k + 3] == '>'))))
                    k++;
                //  "<!-->" and "<!--->" are empty comments.
                if (n_ - start >= 1 && s_[start] == '>')
                {
                    b_.comment("", 0);
                    return start + 1;
                }
                if (n_ - start >= 2 && s_[start] == '-' && s_[start + 1] == '>')
                {
                    b_.comment("", 0);
                    return start + 2;
                }
                b_.comment(s_ + start, (k < n_ ? k : n_) - start);
                if (k >= n_)
                    return n_;
                return k + (s_[k + 2] == '!' ? 4 : 3);
            }
            if (j + 8 <= n_ && ieqn(s_ + j + 1, "DOCTYPE", 7))
            {
                size_t k = j + 8;
                while (k < n_ && space(s_[k]))
                    k++;
                char name[NameCap];
                size_t m = 0;
                while (k < n_ && !space(s_[k]) && s_[k] != '>' && m + 1 < sizeof(name))
                    name[m++] = lower(s_[k++]);
                name[m] = 0;
                while (k < n_ && s_[k] != '>')
                    k++;
                b_.doctype(name);
                return k < n_ ? k + 1 : n_;
            }
            if (j + 8 <= n_ && !memcmp(s_ + j + 1, "[CDATA[", 7))
            {
                size_t start = j + 8;
                const char *end = ifind(s_ + start, n_ - start, "]]>");
                size_t stop = end ? (size_t)(end - s_) : n_;
                b_.text(s_ + start, stop - start);
                return end ? stop + 3 : n_;
            }
            return bogus(j + 1);
        }
        if (j < n_ && s_[j] == '?')
            return bogus(j);
        if (j < n_ && s_[j] == '/')
        {
            if (j + 1 < n_ && alpha(s_[j + 1]))
            {
                size_t k = tagName(j + 1);
                while (k < n_ && s_[k] != '>')
                    k++;
                b_.end(tag_);
                return k < n_ ? k + 1 : n_;
            }
            if (j + 1 < n_ && s_[j + 1] == '>')
                return j + 2;
            return bogus(j + 1);
        }
        if (j < n_ && alpha(s_[j]))
        {
            size_t k = tagName(j);
            tag_.attrs.clear();
            tag_.nAttrs = 0;
            tag_.selfClosing = false;
            k = attributes(k);
            if (k > n_)
                return n_; // an unfinished tag at the end is dropped
            Builder::Raw r = b_.start(tag_);
            if (!strcmp(tag_.name, "pre") || !strcmp(tag_.name, "textarea") || !strcmp(tag_.name, "listing"))
                skipLF_ = true;
            if (r != Builder::NONE)
            {
                char name[NameCap];
                scopy(name, tag_.name, sizeof(name));
                return raw(k, r, name);
            }
            return k;
        }
        b_.text("<", 1);
        return j;
    }

    size_t bogus(size_t start)
    {
        size_t k = start;
        while (k < n_ && s_[k] != '>')
            k++;
        b_.comment(s_ + start, k - start);
        return k < n_ ? k + 1 : n_;
    }

    size_t tagName(size_t k)
    {
        size_t m = 0;
        while (k < n_ && !space(s_[k]) && s_[k] != '/' && s_[k] != '>')
        {
            if (m + 1 < NameCap)
            {
                tag_.raw[m] = s_[k];
                tag_.name[m] = lower(s_[k]);
                m++;
            }
            k++;
        }
        tag_.name[m] = tag_.raw[m] = 0;
        return k;
    }

    //  The attributes up to '>'; the position after it, or past the end
    //  when the tag never closes.
    size_t attributes(size_t k)
    {
        for (;;)
        {
            while (k < n_ && (space(s_[k]) || s_[k] == '/'))
            {
                if (s_[k] == '/' && k + 1 < n_ && s_[k + 1] == '>')
                    tag_.selfClosing = true;
                k++;
            }
            if (k >= n_)
                return n_ + 1;
            if (s_[k] == '>')
                return k + 1;
            size_t nameStart = k;
            do
                k++;
            while (k < n_ && !space(s_[k]) && s_[k] != '/' && s_[k] != '>' && s_[k] != '=');
            size_t nameEnd = k;
            while (k < n_ && space(s_[k]))
                k++;
            Buf value;
            if (k < n_ && s_[k] == '=')
            {
                k++;
                while (k < n_ && space(s_[k]))
                    k++;
                if (k < n_ && (s_[k] == '"' || s_[k] == '\''))
                {
                    char q = s_[k++];
                    size_t v = k;
                    while (k < n_ && s_[k] != q)
                        k++;
                    value_(value, v, k);
                    if (k < n_)
                        k++;
                }
                else
                {
                    size_t v = k;
                    while (k < n_ && !space(s_[k]) && s_[k] != '>')
                        k++;
                    value_(value, v, k);
                }
            }
            //  The first of two attributes with one name wins.
            char name[64];
            size_t m = 0;
            for (size_t x = nameStart; x < nameEnd && m + 1 < sizeof(name); x++)
                name[m++] = s_[x];
            name[m] = 0;
            if (!dupe(name))
            {
                tag_.attrs.append(name, m + 1);
                tag_.attrs.append(value.data ? value.data : (const uint8_t *)"", value.len);
                tag_.attrs.push(0);
                tag_.nAttrs++;
            }
        }
    }

    bool dupe(const char *name) const
    {
        const char *a = tag_.attrs.data ? (const char *)tag_.attrs.data : "";
        for (int i = 0; i < tag_.nAttrs; i++)
        {
            if (ieq(a, name))
                return true;
            a += strlen(a) + 1;
            a += strlen(a) + 1;
        }
        return false;
    }

    void value_(Buf &out, size_t from, size_t to)
    {
        for (size_t i = from; i < to;)
        {
            if (s_[i] == '&')
            {
                if (!reference(s_, to, i, true, out))
                    out.push((uint8_t)s_[i++]);
                continue;
            }
            if (s_[i] == '\r')
            {
                out.push('\n');
                if (i + 1 < to && s_[i + 1] == '\n')
                    i++;
                i++;
                continue;
            }
            if (s_[i])
                out.push((uint8_t)s_[i]);
            i++;
        }
    }

    //  Raw text or RCDATA up to "</name", which then closes the element.
    size_t raw(size_t k, Builder::Raw kind, const char *name)
    {
        size_t len = strlen(name), stop = n_;
        if (kind != Builder::PLAINTEXT)
            for (size_t x = k; x + 2 + len <= n_; x++)
                if (s_[x] == '<' && s_[x + 1] == '/' && ieqn(s_ + x + 2, name, len) &&
                    (x + 2 + len == n_ || space(s_[x + 2 + len]) || s_[x + 2 + len] == '>' || s_[x + 2 + len] == '/'))
                {
                    stop = x;
                    break;
                }
        if (kind == Builder::RCDATA)
        {
            text_.clear();
            size_t i = k;
            if (skipLF_)
            {
                skipLF_ = false;
                if (i < stop && s_[i] == '\r')
                    i++;
                if (i < stop && s_[i] == '\n')
                    i++;
            }
            while (i < stop)
            {
                if (s_[i] == '&' && reference(s_, stop, i, false, text_))
                    continue;
                if (s_[i] == '\r')
                {
                    text_.push('\n');
                    if (i + 1 < stop && s_[i + 1] == '\n')
                        i++;
                }
                else if (s_[i])
                    text_.push((uint8_t)s_[i]);
                i++;
            }
            b_.text((const char *)text_.data, text_.len);
        }
        else
            emit(k, stop);
        if (stop >= n_)
            return n_;
        size_t e = stop + 2;
        tagName(e);
        while (e < n_ && s_[e] != '>')
            e++;
        b_.end(tag_);
        return e < n_ ? e + 1 : n_;
    }
};

} // namespace

JSValue parseHtml(JSContext *ctx, const char *src, size_t n, bool fragment, const char *context)
{
    Builder b(ctx, fragment);
    Tokenizer t(src, n, b);
    Builder::Raw initial = Builder::NONE;
    if (fragment && context)
    {
        if (!strcmp(context, "script") || !strcmp(context, "style") || !strcmp(context, "xmp") ||
            !strcmp(context, "iframe") || !strcmp(context, "noembed") || !strcmp(context, "noframes"))
            initial = Builder::RAWTEXT;
        else if (!strcmp(context, "textarea") || !strcmp(context, "title"))
            initial = Builder::RCDATA;
    }
    //  A fragment read as raw text never meets its end tag: all of it is text.
    t.run(initial, initial != Builder::NONE ? "\x01" : nullptr);
    return b.finish();
}

} // namespace web
