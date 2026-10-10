#pragma once

#include "wbase.h"
#include "css.h"

namespace web {

//
//  A page, as this browser understands one.
//
//  HTML is parsed into styled text items and a retained element tree. The
//  original Layout flows items into character cells; PixelLayout flows the
//  tree into block, inline and flex boxes using measured font pixels. Both
//  produce lines/runs and share links, images, forms and their live state.
//  The CSS cascade retains independent cell approximations and box lengths.
//
//  Text is stored already converted to the font's code page (CP437), so the
//  renderer copies bytes and never decodes anything.
//
//  Form controls are part of the page: each one sits in the text as a run of
//  placeholder cells, which the window draws from the control's live state,
//  and has an entry in the link table so that Tab and the mouse reach it.
//

enum Style : uint8_t
{
    ST_BOLD = 1,
    ST_ITALIC = 2,
    ST_LINK = 4,
    ST_CTRL = 8,   // a form control's cells
    ST_FAINT = 16, // list markers, image placeholders
    ST_BIG = 32,   // set on runs of a big line by the layout
    ST_UNDER = 64, // text-decoration: underline
    ST_BUTTON_EDGE = 128, // synthesized brackets, shown only in text layout
};

struct Item
{
    enum Kind : uint8_t
    {
        TEXT,
        BLOCK, // a new block starts: margin, indent, heading, marker, alignment
        BR,
        HR,
        IMG, // a picture; the `off` items after it are its alt text
    };
    uint8_t kind;
    uint8_t style;
    uint8_t heading; // BLOCK: 1..6, 0 for none
    uint8_t pre;     // BLOCK: whitespace is kept and lines are not wrapped
    uint8_t margin;  // BLOCK: blank lines wanted above
    uint8_t indent;  // BLOCK: in cells
    uint8_t align;   // BLOCK: 0 left, 1 centre, 2 right
    uint8_t fg, bg;  // TEXT: palette index + 1, 0 for the default
    uint8_t pad;
    uint32_t fgRgb = 0, bgRgb = 0;
    uint16_t img;    // IMG: index into the image table + 1
    int32_t link;    // TEXT, IMG: index into the link table, or -1
    uint32_t off;    // TEXT: the text; BLOCK: the list marker, if any; IMG: items of alt text
    uint32_t len;
    uint32_t box; // owning element in the retained box tree
};

//  One line of the laid-out page.  Big lines are headings drawn at twice the
//  size; they take two rows and have half as many columns.  A picture is a
//  line of its own, as many rows tall as it needs, with no runs.
struct Line
{
    uint32_t firstRun;
    uint16_t nRuns;
    uint8_t big;
    uint8_t hr;
    int32_t row;
    int32_t link;   // a picture's link, or -1
    uint16_t img;   // index into the image table + 1; 0 for a line of text
    uint16_t rows;  // a picture's height in rows
    uint16_t imgX;  // where the picture starts, in pixels from the left
    uint16_t imgW, imgH; // the size it is drawn at, in pixels

    int32_t pixelHeight = 0; // pixel layout: row and run.x are pixels
    int height() const { return pixelHeight ? pixelHeight : img ? rows : big ? 2 : 1; }
};

//  An <img>.  Its size is 0 until the window has the picture and says how
//  big it is (Document::setImageSize); until then its alt text stands in.
struct Image
{
    uint32_t src;   // the src attribute (string pool)
    uint16_t w, h;  // in pixels
};

struct Run
{
    uint32_t off;
    uint16_t len;
    uint16_t col; // in the line's own cells (big cells on a big line)
    uint8_t style;
    uint8_t fg, bg;
    uint8_t pad;
    uint32_t fgRgb = 0, bgRgb = 0;
    int32_t link;
    int32_t x = 0; // absolute pixel x, used by pixel layout
};

// A retained element, independent of the text layout. Index 0 is the page.
struct Box
{
    uint32_t parent = 0, firstChild = 0, lastChild = 0, next = 0, endBox = 0;
    uint32_t firstItem = 0, endItem = 0, uid = 0, id = 0, tag = 0;
    uint32_t firstLine = 0, endLine = 0;
    CssStyle style;
    bool block = false, big = false, atomic = false, anonymous = false, definiteHeight = false;
    int32_t link = -1;
    int x = 0, y = 0, w = 0, h = 0, contentW = 0, contentH = 0;
    int clientW = 0, clientH = 0, scrollW = 0, scrollH = 0;
    int margin[4] = {}, padding[4] = {}, border[4] = {};
};

//  A form control.  Strings are offsets into the document's string pool; the
//  text a user types is kept in `edit`.
struct Control
{
    enum Type : uint8_t
    {
        TEXT,
        PASSWORD,
        TEXTAREA,
        CHECKBOX,
        RADIO,
        SELECT,
        SUBMIT,
        RESET,
        BUTTON, // does nothing without a script
        IMAGE,  // a submit button drawn as a picture
        HIDDEN,
    };
    uint8_t type;
    uint8_t checked, initialChecked;
    uint8_t width;      // cells
    int16_t form;       // -1: outside any form
    int16_t selected, initialSelected;
    uint16_t nOptions;
    uint32_t name;      // raw bytes, as the page had them
    uint32_t value;     // the initial value, or a button's value
    uint32_t id, onclick; // DOM identity and an inline button handler
    uint32_t options;   // index of the first (value, label) pair in optionOffs_
    uint32_t textOff;   // where its placeholder cells are in the text
    char *edit;         // the current text of a text control
    uint32_t editLen, editCap;

    bool isText() const { return type == TEXT || type == PASSWORD || type == TEXTAREA; }
    bool isButton() const { return type == SUBMIT || type == RESET || type == BUTTON || type == IMAGE; }
};

//  For a script engine's own HTML parser (r2web): a page's bytes as UTF-8,
//  read in `charset` (sniffed from the page when empty) the way loadHtml
//  reads them; and the character reference at s[i] (the '&'), which moves i
//  past it, or 0 when there is none.
bool pageToUtf8(const uint8_t *src, size_t n, const char *charset, Buf &out);
uint32_t characterReference(const uint8_t *s, size_t n, size_t &i);

//  A style sheet fetched separately, handed to loadHtml.
struct StyleSheetText
{
    const uint8_t *data;
    size_t len;
};

class Document
{
public:
    Document();
    ~Document();

    void clear();

    //  src is the raw body; charset is what the HTTP header said, if anything.
    //  `sheets` are the page's linked style sheets, fetched after the page
    //  itself; with `css` false only the built-in defaults apply.
    void loadHtml(const uint8_t *src, size_t n, const char *charset, const StyleSheetText *sheets = nullptr,
                  int nSheets = 0, bool css = true);
    void loadText(const uint8_t *src, size_t n, const char *charset);

    //  A page made up here: errors, the start page.  Takes simple HTML.
    void loadMessage(const char *html) { loadHtml((const uint8_t *)html, strlen(html), "utf-8"); }

    void layout(int cols);
    void layoutPixels(int width, int cellWidth, int lineHeight, int viewportHeight = 0);
    bool pixelLayout() const { return pixels_; }
    size_t boxCount() const { return boxes_.len / sizeof(Box); }
    const Box &box(size_t i) const { return ((const Box *)boxes_.data)[i]; }
    const Box *boxForNode(uint32_t uid) const;
    const Box *boxForId(const char *id) const;
    int pixelLinkAt(int x, int y) const;
    bool pixelLinkPoint(int link, int &x, int &y) const;

    int layoutCols() const { return cols_; }
    int rows() const { return rows_; }
    const char *title() const { return title_; }
    bool outOfMemory() const { return oom_; }

    size_t lineCount() const { return lines_.len / sizeof(Line); }
    const Line &line(size_t i) const { return ((const Line *)lines_.data)[i]; }
    const Run &run(size_t i) const { return ((const Run *)runs_.data)[i]; }
    const char *text(uint32_t off) const { return (const char *)text_.data + off; }

    //  The first line at or below a row, for drawing and hit testing.
    size_t lineAtRow(int row) const;

    //  Links and controls share one table, in the order they appear.
    int linkCount() const { return (int)(linkOffs_.len / sizeof(uint32_t)); }
    const char *linkHref(int i) const;
    int linkControl(int i) const; // the control behind link i, or -1
    const char *linkHandler(int i) const;
    const char *linkId(int i) const;

    //  Where link i first appears, as a row; -1 if it is not laid out.
    int linkRow(int i) const;

    //  The next run at or after (line, run) whose text contains needle,
    //  case-insensitively.  Returns false when there is none.
    bool find(const char *needle, size_t &lineIdx, size_t &runIdx) const;

    //  The pictures, in the order they appear.  A picture with a size is laid
    //  out as one: the window sets the size when it has the picture, and the
    //  pixels a cell and a row take so that it knows how many rows that is
    //  and how wide the page is.  Either one lays the page out again.
    int imageCount() const { return (int)(images_.len / sizeof(Image)); }
    const char *imageSrc(int i) const { return str(((const Image *)images_.data)[i].src); }
    void setImageSize(int i, int w, int h);
    void imageSize(int i, int &w, int &h) const { const Image &im=((const Image *)images_.data)[i];w=im.w;h=im.h; }
    void setCellPixels(int cellW, int rowH);

    //  The <link rel=stylesheet> hrefs the page named, in order.
    int stylesheetCount() const { return (int)(sheetOffs_.len / sizeof(uint32_t)); }
    const char *stylesheetHref(int i) const { return str(((const uint32_t *)sheetOffs_.data)[i]); }

    // ── Forms ────────────────────────────────────────────────────────────────
    int controlCount() const { return (int)(controls_.len / sizeof(Control)); }
    Control &control(int i) { return ((Control *)controls_.data)[i]; }
    const Control &control(int i) const { return ((const Control *)controls_.data)[i]; }
    const char *str(uint32_t off) const { return (const char *)strings_.data + off; }
    const char *formAction(int f) const;
    bool formPost(int f) const;

    //  What a control shows in its cells, in the font's code page, exactly
    //  `width` cells long.  False for buttons, which show their label text.
    bool controlCells(int c, char *out, size_t cap) const;

    //  Editing: the current text of a text control, and changing it.
    const char *controlText(int c) const;
    void controlSetText(int c, const char *text);
    void controlInsert(int c, char ch);
    void controlBackspace(int c);

    //  Checkbox, radio (clears its group), select (next option).
    void controlActivate(int c, bool backwards = false);
    void resetForm(int f);

    //  The form's data as application/x-www-form-urlencoded, as submitted
    //  by `submitter` (a button, or -1 for an implicit submission).
    bool formData(int form, int submitter, Buf &out) const;

private:
    Buf boxes_{true};
    bool pixels_ = false;
    Buf text_{true};
    Buf items_{true};
    Buf links_{true};
    Buf linkOffs_{true};
    Buf linkEvents_{true}; // pairs of string offsets: onclick, id
    Buf lines_{true};
    Buf runs_{true};
    Buf strings_{true};
    Buf controls_{true};
    Buf forms_{true};
    Buf optionOffs_{true};
    Buf sheetOffs_{true};
    Buf images_{true};
    int cellPx_ = 0, rowPx_ = 0;
    char title_[96] = {};
    int cols_ = 0;
    int rows_ = 0;
    bool oom_ = false;

    uint32_t addString(const char *s, size_t n);

    friend class HtmlParser;
    friend class Layout;
    friend class PixelLayout;
};

//  Unicode code point to the font's byte; 0 drops the character.  Used by the
//  address bar too, which shows what the page's title says.
uint8_t glyphFor(uint32_t cp);

} // namespace web
