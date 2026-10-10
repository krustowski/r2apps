#pragma once

#include "wbase.h"

namespace web {

//
//  The part of CSS this browser can act on.
//
//  The cascade keeps typography, RGB and EGA colours, the earlier text layout's
//  margins/indents rounded to rows/cells, and BoxStyle's independent pixel
//  lengths and flex properties. Unsupported fonts, positioning and animation
//  declarations are still dropped.
//
//  Selectors: type, universal, #id, .class, [attr], [attr=value], :root,
//  :link, :not() of one simple selector, descendant and child combinators.
//  A selector with anything else in it (sibling combinators, structural or
//  dynamic pseudo-classes, pseudo-elements) is dropped whole: no element is
//  hovered or focused here, and a partial match would be a wrong one.
//  @media is evaluated for a screen 600 pixels wide.
//

//  An element as selectors see it.  Names, ids and classes are hashed.
struct CssElement
{
    static const int MAX_CLASSES = 12;
    static const int MAX_ATTRS = 6;
    uint32_t tag;
    uint32_t id;
    uint32_t cls[MAX_CLASSES];
    uint8_t nCls;
    uint32_t attrName[MAX_ATTRS];
    uint32_t attrValue[MAX_ATTRS];
    uint8_t nAttr;
};

// Lengths retain pixels or percentages until the containing box is known.
// Negative pixel margins are supported; the reserved range encodes percentages.
struct BoxStyle
{
    static constexpr int16_t Auto = -32768;
    int16_t width = Auto, height = Auto, minWidth = 0, minHeight = 0;
    int16_t maxWidth = Auto, maxHeight = Auto;
    int16_t margin[4] = {}, padding[4] = {}, border[4] = {}; // top/right/bottom/left
    int16_t gap[2] = {}, basis = Auto, grow = 0, shrink = 100;
    uint32_t borderRgb = 0;
    int16_t radius = 0;
    uint8_t borderColor = 0, sizing = 0, direction = 0, wrap = 0, justify = 0, align = 3;
    int8_t self = -1;
};

//  What the cascade decided for an element.  -1 (or 0 for colours) is "not
//  said", so the element's own defaults and its parent's values apply.
struct CssStyle
{
    enum Display : int8_t
    {
        D_UNSET = 0,
        D_NONE,
        D_BLOCK,
        D_INLINE,
        D_FLEX,
        D_INLINE_BLOCK,
    };
    int8_t display = D_UNSET;
    BoxStyle box;
    int16_t fontSize = 0; // pixel mode chooses the normal or enlarged system font
    int8_t hidden = 0; // visibility: hidden
    int8_t bold = -1, italic = -1, underline = -1;
    uint8_t fg = 0, bg = 0; // palette index + 1 for text layout
    uint32_t fgRgb = 0, bgRgb = 0; // opaque ARGB for pixel painting; 0 is unspecified/transparent
    int8_t align = -1;      // 0 left, 1 center, 2 right
    int8_t pre = -1;        // white-space: pre and friends
    int8_t listNone = -1;
    int8_t upper = -1;
    int8_t marginTop = -1, marginBottom = -1; // in lines, 0..2
    int8_t indent = -1;                       // margin/padding-left, in cells

    //  Copies what children inherit; the rest starts over.
    void inheritFrom(const CssStyle &p)
    {
        *this = CssStyle{};
        bold = p.bold;
        italic = p.italic;
        underline = p.underline;
        fg = p.fg;
        fgRgb = p.fgRgb;
        fontSize = p.fontSize;
        align = p.align;
        pre = p.pre;
        listNone = p.listNone;
        upper = p.upper;
        hidden = p.hidden;
    }
};

uint32_t cssHash(const char *s, size_t n, bool fold);

class Css
{
public:
    Css();
    ~Css();

    enum Origin
    {
        UA = 0,
        AUTHOR = 1,
    };

    //  A style sheet's text.  May be called again as <style> blocks turn up.
    void addSheet(const char *text, size_t n, Origin origin);

    //  The cascade for the element at the top of `chain` (chain[depth-1]);
    //  the others are its ancestors, outermost first.  `inlineStyle` is its
    //  style attribute, if any.
    void compute(const CssElement *const *chain, int depth, const char *inlineStyle, CssStyle &out) const;

    size_t ruleCount() const { return rules_.len / sizeof(Rule); }

private:
    struct Compound
    {
        uint32_t tag;   // 0: any
        uint32_t id;    // 0: any
        uint32_t cls[4];
        uint8_t nCls;
        uint8_t nAttr;
        uint8_t combinator; // to the next compound, leftwards: 1 descendant, 2 child
        uint8_t notKind;    // :not(): 0 none, 1 tag, 2 id, 3 class, 4 attribute
        uint32_t attrName[2];
        uint32_t attrValue[2]; // 0: presence only
        uint32_t notHash;
    };
    struct Rule
    {
        uint32_t firstCompound; // rightmost first
        uint32_t firstDecl;
        uint32_t specificity;
        uint32_t next; // the next rule in the same bucket, or ~0
        uint16_t nDecl;
        uint8_t nCompound;
        uint8_t origin;
    };
    struct Decl
    {
        uint8_t prop;
        uint8_t important;
        uint16_t reserved;
        int32_t value;
    };

    static const int BUCKETS = 256;
    uint32_t buckets_[BUCKETS];
    uint32_t universal_ = ~0u;
    Buf rules_{true};
    Buf compounds_{true};
    Buf decls_{true};

    void parseBlock(const char *s, size_t n, Origin origin, int depth);
    bool parseSelector(const char *s, size_t n, Buf &out, uint32_t &spec);
    void addRule(const Compound *cs, int nc, uint32_t spec, uint32_t firstDecl, uint16_t nDecl, Origin origin);
    bool matchCompound(const Compound &c, const CssElement &e) const;
    bool matchRule(const Rule &r, const CssElement *const *chain, int depth) const;

    friend void cssParseDeclarations(const char *s, size_t n, Buf &out);
    friend void cssApply(const void *decls, size_t count, bool important, CssStyle &st);
};

//  The EGA palette index nearest to a colour, and the brightness of an index
//  (for keeping text readable on its background).
uint8_t cssNearest(uint8_t r, uint8_t g, uint8_t b);
int cssLuma(uint8_t index);
uint32_t cssDarkRgb(uint32_t rgb); // invert lightness while retaining hue

} // namespace web
