#include "css.h"

namespace web {

// ─── Small things ────────────────────────────────────────────────────────────

uint32_t cssHash(const char *s, size_t n, bool fold)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++)
    {
        h ^= (uint8_t)(fold ? lower(s[i]) : s[i]);
        h *= 16777619u;
    }
    return h ? h : 1; // 0 means "any" in a compound
}

static const uint8_t kPalette[16][3] = {
    {0x00, 0x00, 0x00}, {0x00, 0x00, 0xAA}, {0x00, 0xAA, 0x00}, {0x00, 0xAA, 0xAA},
    {0xAA, 0x00, 0x00}, {0xAA, 0x00, 0xAA}, {0xAA, 0x55, 0x00}, {0xAA, 0xAA, 0xAA},
    {0x55, 0x55, 0x55}, {0x55, 0x55, 0xFF}, {0x55, 0xFF, 0x55}, {0x55, 0xFF, 0xFF},
    {0xFF, 0x55, 0x55}, {0xFF, 0x55, 0xFF}, {0xFF, 0xFF, 0x55}, {0xFF, 0xFF, 0xFF},
};

uint8_t cssNearest(uint8_t r, uint8_t g, uint8_t b)
{
    int best = 0;
    long bestD = 0x7FFFFFFF;
    for (int i = 0; i < 16; i++)
    {
        long dr = (long)r - kPalette[i][0], dg = (long)g - kPalette[i][1], db = (long)b - kPalette[i][2];
        //  Green counts most and blue least, as the eye weighs them.
        long d = 3 * dr * dr + 6 * dg * dg + dg * 0 + db * db;
        if (d < bestD)
        {
            bestD = d;
            best = i;
        }
    }
    return (uint8_t)best;
}

int cssLuma(uint8_t i)
{
    return (kPalette[i & 15][0] * 30 + kPalette[i & 15][1] * 59 + kPalette[i & 15][2] * 11) / 100;
}

static bool isWs(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

static bool isIdent(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
           (unsigned char)c >= 0x80 || c == '\\';
}

//  Skips white space and comments.
static size_t skipWs(const char *s, size_t n, size_t i)
{
    for (;;)
    {
        while (i < n && isWs(s[i]))
            i++;
        if (i + 1 < n && s[i] == '/' && s[i + 1] == '*')
        {
            i += 2;
            while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/'))
                i++;
            i = i + 2 <= n ? i + 2 : n;
            continue;
        }
        return i;
    }
}

//  From an opening '{' (or anywhere inside a block), the index of the '}'
//  that closes it, stepping over strings, comments and nested blocks.
static size_t blockEnd(const char *s, size_t n, size_t i)
{
    int depth = 0;
    for (; i < n; i++)
    {
        char c = s[i];
        if (c == '"' || c == '\'')
        {
            for (i++; i < n && s[i] != c; i++)
                if (s[i] == '\\')
                    i++;
        }
        else if (c == '/' && i + 1 < n && s[i + 1] == '*')
        {
            for (i += 2; i + 1 < n && !(s[i] == '*' && s[i + 1] == '/'); i++)
                ;
            i++;
        }
        else if (c == '{')
            depth++;
        else if (c == '}')
        {
            if (--depth <= 0)
                return i;
        }
    }
    return n;
}

// ─── Values ──────────────────────────────────────────────────────────────────

enum Prop : uint8_t
{
    P_DISPLAY = 1,
    P_VISIBILITY,
    P_COLOR,
    P_BG,
    P_WEIGHT,
    P_STYLE,
    P_DECOR,
    P_ALIGN,
    P_WS,
    P_LIST,
    P_TRANSFORM,
    P_MT,
    P_MB,
    P_INDENT,
};

struct Named
{
    const char *name;
    uint32_t rgb;
};

static const Named kColors[] = {
    {"black", 0x000000},  {"white", 0xFFFFFF},   {"red", 0xFF0000},    {"green", 0x008000},
    {"blue", 0x0000FF},   {"navy", 0x000080},    {"gray", 0x808080},   {"grey", 0x808080},
    {"silver", 0xC0C0C0}, {"maroon", 0x800000},  {"purple", 0x800080}, {"teal", 0x008080},
    {"olive", 0x808000},  {"lime", 0x00FF00},    {"aqua", 0x00FFFF},   {"cyan", 0x00FFFF},
    {"fuchsia", 0xFF00FF}, {"magenta", 0xFF00FF}, {"yellow", 0xFFFF00}, {"orange", 0xFFA500},
    {"brown", 0xA52A2A},  {"darkgray", 0xA9A9A9}, {"darkgrey", 0xA9A9A9}, {"lightgray", 0xD3D3D3},
    {"lightgrey", 0xD3D3D3}, {"darkblue", 0x00008B}, {"darkred", 0x8B0000}, {"darkgreen", 0x006400},
    {"gold", 0xFFD700},   {"pink", 0xFFC0CB},    {"whitesmoke", 0xF5F5F5}, {"gainsboro", 0xDCDCDC},
    {"dimgray", 0x696969}, {"dimgrey", 0x696969}, {"crimson", 0xDC143C}, {"steelblue", 0x4682B4},
};

static int hexv(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    c = lower(c);
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

//  A number at s[i]; returns false when there is none.  Moves i past it.
static bool number(const char *s, size_t n, size_t &i, long &milli)
{
    size_t j = i;
    bool neg = false;
    if (j < n && (s[j] == '-' || s[j] == '+'))
        neg = s[j++] == '-';
    long whole = 0, frac = 0, scale = 1000;
    bool any = false;
    while (j < n && s[j] >= '0' && s[j] <= '9')
    {
        whole = whole * 10 + (s[j++] - '0');
        any = true;
        if (whole > 100000)
            whole = 100000;
    }
    if (j < n && s[j] == '.')
    {
        j++;
        while (j < n && s[j] >= '0' && s[j] <= '9')
        {
            scale /= 10;
            frac += (s[j++] - '0') * scale;
            any = true;
        }
    }
    if (!any)
        return false;
    milli = whole * 1000 + frac;
    if (neg)
        milli = -milli;
    i = j;
    return true;
}

//  A colour, as palette index + 1.  0: transparent.  -1: not a colour we can
//  know (a variable, currentColor, a keyword that defers to something else).
static int parseColor(const char *s, size_t n)
{
    size_t i = skipWs(s, n, 0);
    if (i >= n)
        return -1;
    if (s[i] == '#')
    {
        i++;
        int d[8];
        int k = 0;
        while (i < n && k < 8 && hexv(s[i]) >= 0)
            d[k++] = hexv(s[i++]);
        int r, g, b, a = 255;
        if (k == 3 || k == 4)
        {
            r = d[0] * 17;
            g = d[1] * 17;
            b = d[2] * 17;
            if (k == 4)
                a = d[3] * 17;
        }
        else if (k == 6 || k == 8)
        {
            r = d[0] * 16 + d[1];
            g = d[2] * 16 + d[3];
            b = d[4] * 16 + d[5];
            if (k == 8)
                a = d[6] * 16 + d[7];
        }
        else
            return -1;
        if (a < 64)
            return 0;
        return 1 + cssNearest((uint8_t)r, (uint8_t)g, (uint8_t)b);
    }
    if (istarts(s + i, "rgb"))
    {
        const char *p = (const char *)memchr(s + i, '(', n - i);
        if (!p)
            return -1;
        size_t j = (size_t)(p - s) + 1;
        long v[4] = {0, 0, 0, 1000};
        int k = 0;
        while (k < 4)
        {
            j = skipWs(s, n, j);
            if (j < n && (s[j] == ',' || s[j] == '/'))
            {
                j++;
                continue;
            }
            long m;
            if (!number(s, n, j, m))
                break;
            if (j < n && s[j] == '%')
            {
                j++;
                m = k < 3 ? m * 255 / 100 : m / 100;
            }
            v[k++] = m;
        }
        if (k < 3)
            return -1;
        if (v[3] < 250)
            return 0; // mostly transparent
        long c[3];
        for (int q = 0; q < 3; q++)
            c[q] = v[q] / 1000 < 0 ? 0 : (v[q] / 1000 > 255 ? 255 : v[q] / 1000);
        return 1 + cssNearest((uint8_t)c[0], (uint8_t)c[1], (uint8_t)c[2]);
    }
    size_t e = i;
    while (e < n && isIdent(s[e]))
        e++;
    char word[16];
    scopyn(word, s + i, e - i, sizeof(word));
    if (ieq(word, "transparent") || ieq(word, "none"))
        return 0;
    for (const Named &c : kColors)
        if (ieq(word, c.name))
            return 1 + cssNearest((uint8_t)(c.rgb >> 16), (uint8_t)(c.rgb >> 8), (uint8_t)c.rgb);
    return -1;
}

//  A length in CSS pixels (em as 16).  Returns false for anything relative to
//  something unknown here (%, vw, calc, auto).
static bool parseLength(const char *s, size_t n, size_t &i, long &px)
{
    i = skipWs(s, n, i);
    long m;
    if (!number(s, n, i, m))
        return false;
    size_t u = i;
    while (i < n && isIdent(s[i]))
        i++;
    char unit[8];
    scopyn(unit, s + u, i - u, sizeof(unit));
    if (!unit[0] || ieq(unit, "px"))
        px = m / 1000;
    else if (ieq(unit, "em") || ieq(unit, "rem") || ieq(unit, "ch"))
        px = m * 16 / 1000;
    else if (ieq(unit, "pt"))
        px = m * 4 / 3000;
    else
        return false;
    if (!unit[0] && m != 0)
        return false; // a bare non-zero number is not a length
    return true;
}

static int16_t pxToLines(long px) { return (int16_t)(px < 6 ? 0 : (px < 24 ? 1 : 2)); }
static int16_t pxToCells(long px) { return (int16_t)(px <= 0 ? 0 : (px / 10 > 8 ? 8 : px / 10)); }

static bool has(const char *v, size_t n, const char *word) { return ifind(v, n, word) != nullptr; }

//  One "name: value" into zero or more declarations.
static void declaration(const char *name, size_t nl, const char *v, size_t n, bool important, Buf &out)
{
    auto push = [&](uint8_t prop, int value) {
        uint8_t d[4];
        d[0] = prop;
        d[1] = important ? 1 : 0;
        d[2] = (uint8_t)(value & 0xFF);
        d[3] = (uint8_t)((value >> 8) & 0xFF);
        out.append(d, 4);
    };
    char p[24];
    scopyn(p, name, nl, sizeof(p));
    if (has(v, n, "var(") || has(v, n, "inherit") || has(v, n, "initial") || has(v, n, "unset") ||
        has(v, n, "revert"))
        return;

    if (ieq(p, "display"))
    {
        if (has(v, n, "none"))
            push(P_DISPLAY, CssStyle::D_NONE);
        else if (has(v, n, "inline"))
            push(P_DISPLAY, CssStyle::D_INLINE);
        else if (has(v, n, "block") || has(v, n, "flex") || has(v, n, "grid") || has(v, n, "list-item") ||
                 ieq(v, "table") || has(v, n, "flow-root"))
            push(P_DISPLAY, CssStyle::D_BLOCK);
    }
    else if (ieq(p, "visibility"))
        push(P_VISIBILITY, has(v, n, "hidden") || has(v, n, "collapse") ? 1 : 0);
    else if (ieq(p, "color"))
    {
        int c = parseColor(v, n);
        if (c > 0)
            push(P_COLOR, c);
    }
    else if (ieq(p, "background-color") || ieq(p, "background"))
    {
        //  The shorthand: whichever of its words is a colour.
        for (size_t i = 0; i < n;)
        {
            i = skipWs(v, n, i);
            size_t e = i;
            int depth = 0;
            while (e < n && (depth || !isWs(v[e])))
            {
                if (v[e] == '(')
                    depth++;
                else if (v[e] == ')')
                    depth--;
                e++;
            }
            if (e > i)
            {
                int c = parseColor(v + i, e - i);
                if (c >= 0)
                {
                    push(P_BG, c);
                    break;
                }
            }
            i = e;
        }
    }
    else if (ieq(p, "font-weight"))
    {
        long m = 0;
        size_t i = 0;
        bool num = number(v, n, i, m);
        push(P_WEIGHT, (num ? m >= 600000 : (has(v, n, "bold") || has(v, n, "bolder"))) ? 1 : 0);
    }
    else if (ieq(p, "font"))
    {
        if (has(v, n, "bold"))
            push(P_WEIGHT, 1);
        if (has(v, n, "italic"))
            push(P_STYLE, 1);
    }
    else if (ieq(p, "font-style"))
        push(P_STYLE, has(v, n, "italic") || has(v, n, "oblique") ? 1 : 0);
    else if (ieq(p, "text-decoration") || ieq(p, "text-decoration-line"))
    {
        if (has(v, n, "underline"))
            push(P_DECOR, 1);
        else if (has(v, n, "none"))
            push(P_DECOR, 0);
    }
    else if (ieq(p, "text-align"))
    {
        if (has(v, n, "center"))
            push(P_ALIGN, 1);
        else if (has(v, n, "right") || has(v, n, "end"))
            push(P_ALIGN, 2);
        else
            push(P_ALIGN, 0);
    }
    else if (ieq(p, "white-space") || ieq(p, "white-space-collapse"))
        push(P_WS, has(v, n, "pre") || has(v, n, "break-spaces") || has(v, n, "preserve") ? 1 : 0);
    else if (ieq(p, "list-style") || ieq(p, "list-style-type"))
        push(P_LIST, has(v, n, "none") ? 1 : 0);
    else if (ieq(p, "text-transform"))
        push(P_TRANSFORM, has(v, n, "uppercase") ? 1 : 0);
    else if (ieq(p, "margin-top") || ieq(p, "margin-bottom"))
    {
        size_t i = 0;
        long px;
        if (parseLength(v, n, i, px))
            push(p[7] == 't' ? P_MT : P_MB, pxToLines(px));
    }
    else if (ieq(p, "margin-left") || ieq(p, "padding-left") || ieq(p, "margin-inline-start") ||
             ieq(p, "padding-inline-start"))
    {
        size_t i = 0;
        long px;
        if (parseLength(v, n, i, px))
            push(P_INDENT, pxToCells(px));
    }
    else if (ieq(p, "margin") || ieq(p, "padding"))
    {
        //  1 to 4 values: top, right, bottom, left.
        long vals[4];
        bool ok[4] = {};
        int k = 0;
        size_t i = 0;
        while (k < 4)
        {
            i = skipWs(v, n, i);
            if (i >= n)
                break;
            size_t before = i;
            ok[k] = parseLength(v, n, i, vals[k]);
            if (i == before)
            {
                //  auto, a percentage: skip the word.
                while (i < n && !isWs(v[i]))
                    i++;
            }
            k++;
        }
        if (!k)
            return;
        int top = 0, bottom = k >= 3 ? 2 : 0, left = k == 4 ? 3 : (k >= 2 ? 1 : 0);
        if (p[0] == 'm')
        {
            if (ok[top])
                push(P_MT, pxToLines(vals[top]));
            if (ok[bottom])
                push(P_MB, pxToLines(vals[bottom]));
        }
        if (ok[left])
            push(P_INDENT, pxToCells(vals[left]));
    }
}

void cssParseDeclarations(const char *s, size_t n, Buf &out)
{
    size_t i = 0;
    while (i < n)
    {
        i = skipWs(s, n, i);
        size_t ns = i;
        while (i < n && s[i] != ':' && s[i] != ';' && s[i] != '{' && s[i] != '}')
            i++;
        if (i >= n || s[i] != ':')
        {
            //  Junk, or a nested rule: step past it.
            if (i < n && s[i] == '{')
                i = blockEnd(s, n, i);
            i++;
            continue;
        }
        size_t ne = i;
        while (ne > ns && isWs(s[ne - 1]))
            ne--;
        i++;
        size_t vs = i;
        int depth = 0;
        while (i < n && (depth || (s[i] != ';' && s[i] != '}')))
        {
            char c = s[i];
            if (c == '(')
                depth++;
            else if (c == ')')
                depth--;
            else if (c == '"' || c == '\'')
                for (i++; i < n && s[i] != c; i++)
                    ;
            i++;
        }
        size_t ve = i;
        while (ve > vs && isWs(s[ve - 1]))
            ve--;
        bool important = false;
        const char *bang = ifind(s + vs, ve - vs, "!important");
        if (bang)
        {
            important = true;
            ve = (size_t)(bang - s);
            while (ve > vs && isWs(s[ve - 1]))
                ve--;
        }
        if (ne > ns && ve > vs)
            declaration(s + ns, ne - ns, s + vs, ve - vs, important, out);
        i++;
    }
}

void cssApply(const void *decls, size_t count, bool important, CssStyle &st)
{
    const uint8_t *d = (const uint8_t *)decls;
    for (size_t k = 0; k < count; k++, d += 4)
    {
        if ((d[1] != 0) != important)
            continue;
        int v = (int16_t)(d[2] | (d[3] << 8));
        switch (d[0])
        {
        case P_DISPLAY:
            st.display = (int8_t)v;
            break;
        case P_VISIBILITY:
            st.hidden = (int8_t)v;
            break;
        case P_COLOR:
            st.fg = (uint8_t)v;
            break;
        case P_BG:
            st.bg = (uint8_t)v;
            break;
        case P_WEIGHT:
            st.bold = (int8_t)v;
            break;
        case P_STYLE:
            st.italic = (int8_t)v;
            break;
        case P_DECOR:
            st.underline = (int8_t)v;
            break;
        case P_ALIGN:
            st.align = (int8_t)v;
            break;
        case P_WS:
            st.pre = (int8_t)v;
            break;
        case P_LIST:
            st.listNone = (int8_t)v;
            break;
        case P_TRANSFORM:
            st.upper = (int8_t)v;
            break;
        case P_MT:
            st.marginTop = (int8_t)v;
            break;
        case P_MB:
            st.marginBottom = (int8_t)v;
            break;
        case P_INDENT:
            st.indent = (int8_t)v;
            break;
        }
    }
}

// ─── Media queries ───────────────────────────────────────────────────────────

//  The screen this browser pretends to be, for min-width and max-width.
static const long VIEWPORT_PX = 600;

static bool mediaQueryMatches(const char *s, size_t n)
{
    if (has(s, n, "print") || has(s, n, "speech") || has(s, n, "not "))
        return false;
    //  Every "(feature: value)" has to hold.
    for (size_t i = 0; i < n; i++)
    {
        if (s[i] != '(')
            continue;
        size_t e = i;
        while (e < n && s[e] != ')')
            e++;
        const char *f = s + i + 1;
        size_t fl = e - i - 1;
        const char *colon = (const char *)memchr(f, ':', fl);
        if (colon)
        {
            size_t vi = (size_t)(colon - f) + 1;
            long px = 0;
            bool len = parseLength(f, fl, vi, px);
            if (istarts(f, "min-width"))
            {
                if (!len || VIEWPORT_PX < px)
                    return false;
            }
            else if (istarts(f, "max-width"))
            {
                if (!len || VIEWPORT_PX > px)
                    return false;
            }
            else if (istarts(f, "prefers-color-scheme"))
            {
                if (!has(colon, fl - (size_t)(colon - f), "light"))
                    return false;
            }
            else if (istarts(f, "orientation"))
            {
                if (!has(colon, fl - (size_t)(colon - f), "landscape"))
                    return false;
            }
            else
                return false; // a feature not known here: better not
        }
        i = e;
    }
    return true;
}

static bool mediaMatches(const char *s, size_t n)
{
    //  A comma-separated list: any one query will do.
    size_t start = 0;
    for (size_t i = 0; i <= n; i++)
    {
        if (i == n || s[i] == ',')
        {
            size_t a = skipWs(s, i, start);
            if (a < i && mediaQueryMatches(s + a, i - a))
                return true;
            if (a >= i && i == n && start == 0)
                return true; // empty: all
            start = i + 1;
        }
    }
    return false;
}

// ─── Selectors ───────────────────────────────────────────────────────────────

static size_t ident(const char *s, size_t n, size_t i)
{
    while (i < n && isIdent(s[i]))
        i++;
    return i;
}

bool Css::parseSelector(const char *s, size_t n, Buf &out, uint32_t &spec)
{
    static const int MAX = 8;
    Compound cs[MAX];
    int nc = 0;
    spec = 0;
    uint8_t comb = 0;
    size_t i = skipWs(s, n, 0);
    while (i < n)
    {
        if (nc)
        {
            size_t j = i;
            bool ws = false;
            while (j < n && isWs(s[j]))
            {
                j++;
                ws = true;
            }
            if (j < n && s[j] == '>')
            {
                comb = 2;
                j = skipWs(s, n, j + 1);
            }
            else if (j < n && (s[j] == '+' || s[j] == '~'))
                return false;
            else if (ws)
                comb = 1;
            i = j;
            if (i >= n)
                break;
        }
        if (nc == MAX)
            return false;
        Compound c = {};
        bool any = false;
        if (s[i] == '*')
        {
            i++;
            any = true;
        }
        else if (isIdent(s[i]))
        {
            size_t e = ident(s, n, i);
            c.tag = cssHash(s + i, e - i, true);
            spec += 1;
            i = e;
            any = true;
        }
        while (i < n && !isWs(s[i]) && s[i] != '>' && s[i] != '+' && s[i] != '~')
        {
            char k = s[i];
            if (k == '#' || k == '.')
            {
                size_t e = ident(s, n, i + 1);
                if (e == i + 1)
                    return false;
                uint32_t h = cssHash(s + i + 1, e - i - 1, false);
                if (k == '#')
                {
                    c.id = h;
                    spec += 1 << 16;
                }
                else
                {
                    if (c.nCls == 4)
                        return false;
                    c.cls[c.nCls++] = h;
                    spec += 1 << 8;
                }
                i = e;
            }
            else if (k == '[')
            {
                size_t e = i;
                while (e < n && s[e] != ']')
                    e++;
                if (e >= n || c.nAttr == 2)
                    return false;
                size_t a = skipWs(s, e, i + 1);
                size_t ae = ident(s, e, a);
                size_t op = skipWs(s, e, ae);
                uint32_t val = 0;
                if (op < e)
                {
                    if (s[op] != '=')
                        return false; // ~= |= ^= $= *=
                    size_t vs = skipWs(s, e, op + 1);
                    size_t ve = e;
                    while (ve > vs && isWs(s[ve - 1]))
                        ve--;
                    if (ve > vs && (s[vs] == '"' || s[vs] == '\''))
                    {
                        vs++;
                        if (ve > vs && (s[ve - 1] == '"' || s[ve - 1] == '\''))
                            ve--;
                    }
                    if (ve > vs && s[ve - 1] == 'i' && ve - 1 > vs && isWs(s[ve - 2]))
                        return false; // case-insensitive flag
                    val = cssHash(s + vs, ve - vs, false);
                }
                c.attrName[c.nAttr] = cssHash(s + a, ae - a, true);
                c.attrValue[c.nAttr] = val;
                c.nAttr++;
                spec += 1 << 8;
                i = e + 1;
            }
            else if (k == ':')
            {
                if (i + 1 < n && s[i + 1] == ':')
                    return false; // a pseudo-element
                size_t e = ident(s, n, i + 1);
                char name[16];
                scopyn(name, s + i + 1, e - i - 1, sizeof(name));
                if (ieq(name, "root"))
                {
                    c.tag = cssHash("html", 4, true);
                    spec += 1 << 8;
                    i = e;
                }
                else if (ieq(name, "link") || ieq(name, "any-link"))
                {
                    if (c.nAttr == 2)
                        return false;
                    c.attrName[c.nAttr++] = cssHash("href", 4, true);
                    spec += 1 << 8;
                    i = e;
                }
                else if (ieq(name, "not") && e < n && s[e] == '(')
                {
                    size_t ce = e;
                    while (ce < n && s[ce] != ')')
                        ce++;
                    size_t a = skipWs(s, ce, e + 1);
                    size_t z = ce;
                    while (z > a && isWs(s[z - 1]))
                        z--;
                    if (z <= a || c.notKind)
                        return false;
                    if (s[a] == '.' || s[a] == '#')
                    {
                        if (ident(s, z, a + 1) != z)
                            return false;
                        c.notKind = s[a] == '.' ? 3 : 2;
                        c.notHash = cssHash(s + a + 1, z - a - 1, false);
                    }
                    else if (s[a] == '[')
                    {
                        size_t ae = ident(s, z, a + 1);
                        if (ae + 1 != z || s[ae] != ']')
                            return false;
                        c.notKind = 4;
                        c.notHash = cssHash(s + a + 1, ae - a - 1, true);
                    }
                    else if (ident(s, z, a) == z)
                    {
                        c.notKind = 1;
                        c.notHash = cssHash(s + a, z - a, true);
                    }
                    else
                        return false;
                    spec += 1 << 8;
                    i = ce + 1;
                }
                else
                    return false; // structural or dynamic: never true here
            }
            else
                return false;
            any = true;
        }
        if (!any)
            return false;
        if (nc)
            cs[nc - 1].combinator = comb;
        cs[nc++] = c;
        comb = 0;
    }
    if (!nc)
        return false;
    //  Stored rightmost first, each compound saying how it relates to the
    //  one after it (to its left in the source).
    for (int k = nc - 1; k >= 0; k--)
    {
        Compound c = cs[k];
        c.combinator = k > 0 ? cs[k - 1].combinator : 0;
        out.append(&c, sizeof(c));
    }
    return true;
}

bool Css::matchCompound(const Compound &c, const CssElement &e) const
{
    if (c.tag && c.tag != e.tag)
        return false;
    if (c.id && c.id != e.id)
        return false;
    for (int k = 0; k < c.nCls; k++)
    {
        bool found = false;
        for (int j = 0; j < e.nCls && !found; j++)
            found = e.cls[j] == c.cls[k];
        if (!found)
            return false;
    }
    for (int k = 0; k < c.nAttr; k++)
    {
        bool found = false;
        for (int j = 0; j < e.nAttr && !found; j++)
            found = e.attrName[j] == c.attrName[k] && (!c.attrValue[k] || c.attrValue[k] == e.attrValue[j]);
        if (!found)
            return false;
    }
    switch (c.notKind)
    {
    case 1:
        return e.tag != c.notHash;
    case 2:
        return e.id != c.notHash;
    case 3:
        for (int j = 0; j < e.nCls; j++)
            if (e.cls[j] == c.notHash)
                return false;
        return true;
    case 4:
        for (int j = 0; j < e.nAttr; j++)
            if (e.attrName[j] == c.notHash)
                return false;
        return true;
    }
    return true;
}

bool Css::matchRule(const Rule &r, const CssElement *const *chain, int depth) const
{
    const Compound *c = (const Compound *)compounds_.data + r.firstCompound;
    int e = depth - 1;
    if (!matchCompound(c[0], *chain[e]))
        return false;
    for (int k = 1; k < r.nCompound; k++)
    {
        if (c[k - 1].combinator == 2)
        {
            if (--e < 0 || !matchCompound(c[k], *chain[e]))
                return false;
        }
        else
        {
            for (--e; e >= 0 && !matchCompound(c[k], *chain[e]); e--)
                ;
            if (e < 0)
                return false;
        }
    }
    return true;
}

// ─── Rules ───────────────────────────────────────────────────────────────────

Css::Css()
{
    for (int i = 0; i < BUCKETS; i++)
        buckets_[i] = ~0u;
}

Css::~Css() {}

void Css::addRule(const Compound *cs, int nc, uint32_t spec, uint32_t firstDecl, uint16_t nDecl, Origin origin)
{
    Rule r = {};
    r.firstCompound = (uint32_t)(compounds_.len / sizeof(Compound));
    r.firstDecl = firstDecl;
    r.nDecl = nDecl;
    r.nCompound = (uint8_t)nc;
    r.specificity = spec;
    r.origin = (uint8_t)origin;
    if (!compounds_.append(cs, sizeof(Compound) * (size_t)nc))
        return;
    //  Filed under the rightmost compound's most selective part.
    const Compound &key = cs[0];
    uint32_t h = key.id ? key.id : (key.nCls ? key.cls[0] : key.tag);
    uint32_t idx = (uint32_t)ruleCount();
    uint32_t *head = h ? &buckets_[h % BUCKETS] : &universal_;
    r.next = *head;
    if (!rules_.append(&r, sizeof(r)))
        return;
    *head = idx;
}

void Css::parseBlock(const char *s, size_t n, Origin origin, int depth)
{
    size_t i = 0;
    Buf decl;
    Buf sel;
    while (i < n)
    {
        i = skipWs(s, n, i);
        if (i >= n)
            break;
        if (s[i] == '}' || s[i] == ';')
        {
            i++;
            continue;
        }
        if (s[i] == '<')
        {
            //  "<!--" and "-->" around a <style>'s contents, from long ago.
            i += 4;
            continue;
        }
        if (s[i] == '-' && i + 2 < n && s[i + 1] == '-' && s[i + 2] == '>')
        {
            i += 3;
            continue;
        }

        //  The prelude runs to the first '{' or ';' outside strings and parens.
        size_t ps = i;
        int paren = 0;
        while (i < n)
        {
            char c = s[i];
            if (c == '"' || c == '\'')
                for (i++; i < n && s[i] != c; i++)
                    ;
            else if (c == '(')
                paren++;
            else if (c == ')')
                paren--;
            else if (!paren && (c == '{' || c == ';'))
                break;
            i++;
        }
        if (i >= n)
            break;
        size_t pe = i;
        if (s[i] == ';')
        {
            i++; // @import, @charset, a stray declaration
            continue;
        }
        size_t be = blockEnd(s, n, i);
        const char *body = s + i + 1;
        size_t bl = be > i ? be - i - 1 : 0;
        i = be + 1;

        if (s[ps] == '@')
        {
            size_t e = ident(s, pe, ps + 1);
            char at[16];
            scopyn(at, s + ps + 1, e - ps - 1, sizeof(at));
            if (depth < 4 && ((ieq(at, "media") && mediaMatches(s + e, pe - e)) || ieq(at, "supports") ||
                              ieq(at, "layer") || ieq(at, "document") || ieq(at, "scope")))
                parseBlock(body, bl, origin, depth + 1);
            continue; // @font-face, @keyframes, @page, @container...
        }

        decl.clear();
        cssParseDeclarations(body, bl, decl);
        if (!decl.len)
            continue; // nothing here this browser can use
        uint32_t firstDecl = (uint32_t)(decls_.len / sizeof(Decl));
        if (!decls_.append(decl.data, decl.len))
            return;
        uint16_t nDecl = (uint16_t)(decl.len / sizeof(Decl));

        //  A selector list: each selector a rule of its own.
        size_t a = ps;
        paren = 0;
        for (size_t k = ps; k <= pe; k++)
        {
            if (k < pe && s[k] == '(')
                paren++;
            else if (k < pe && s[k] == ')')
                paren--;
            if (k == pe || (s[k] == ',' && !paren))
            {
                sel.clear();
                uint32_t spec;
                if (parseSelector(s + a, k - a, sel, spec))
                    addRule((const Compound *)sel.data, (int)(sel.len / sizeof(Compound)), spec, firstDecl, nDecl,
                            origin);
                a = k + 1;
            }
        }
    }
}

void Css::addSheet(const char *text, size_t n, Origin origin) { parseBlock(text, n, origin, 0); }

void Css::compute(const CssElement *const *chain, int depth, const char *inlineStyle, CssStyle &out) const
{
    const CssElement &e = *chain[depth - 1];

    //  Candidates: the buckets this element's id, classes and tag fall in,
    //  and the rules filed under nothing.
    static const int MAX_MATCH = 128;
    uint32_t matched[MAX_MATCH];
    int nm = 0;
    uint32_t keys[CssElement::MAX_CLASSES + 3];
    int nk = 0;
    if (e.id)
        keys[nk++] = e.id;
    for (int k = 0; k < e.nCls; k++)
        keys[nk++] = e.cls[k];
    keys[nk++] = e.tag;
    const Rule *rules = (const Rule *)rules_.data;

    auto collect = [&](uint32_t head) {
        for (uint32_t r = head; r != ~0u; r = rules[r].next)
        {
            bool dup = false;
            for (int k = 0; k < nm && !dup; k++)
                dup = matched[k] == r;
            if (dup || nm == MAX_MATCH || !matchRule(rules[r], chain, depth))
                continue;
            matched[nm++] = r;
        }
    };
    uint32_t seenBuckets[CssElement::MAX_CLASSES + 3];
    int ns = 0;
    for (int k = 0; k < nk; k++)
    {
        uint32_t b = keys[k] % BUCKETS;
        bool seen = false;
        for (int j = 0; j < ns && !seen; j++)
            seen = seenBuckets[j] == b;
        if (seen)
            continue;
        seenBuckets[ns++] = b;
        collect(buckets_[b]);
    }
    collect(universal_);

    //  Cascade order: origin, then specificity, then order of appearance
    //  (which is the rule index).
    for (int a = 1; a < nm; a++)
    {
        uint32_t r = matched[a];
        int b = a - 1;
        auto before = [&](uint32_t x, uint32_t y) {
            const Rule &rx = rules[x], &ry = rules[y];
            if (rx.origin != ry.origin)
                return rx.origin < ry.origin;
            if (rx.specificity != ry.specificity)
                return rx.specificity < ry.specificity;
            return x < y;
        };
        while (b >= 0 && before(r, matched[b]))
        {
            matched[b + 1] = matched[b];
            b--;
        }
        matched[b + 1] = r;
    }

    Buf inl;
    if (inlineStyle && inlineStyle[0])
        cssParseDeclarations(inlineStyle, strlen(inlineStyle), inl);
    const Decl *decls = (const Decl *)decls_.data;
    for (int pass = 0; pass < 2; pass++)
    {
        bool important = pass == 1;
        for (int k = 0; k < nm; k++)
            cssApply(decls + rules[matched[k]].firstDecl, rules[matched[k]].nDecl, important, out);
        cssApply(inl.data, inl.len / sizeof(Decl), important, out);
    }
}

} // namespace web
