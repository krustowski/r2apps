#pragma once

#include "wbase.h"

namespace web {

//
//  A page, as this browser understands one.
//
//  The HTML is parsed once into a flat stream of items --- runs of text with a
//  style, and the block boundaries between them --- and that stream is laid
//  out into lines for a given width, as often as the width changes.  While it
//  parses, the parser keeps the stack of open elements, so that a subset of
//  CSS (css.h) can decide what is shown and how: the tags give each element
//  its defaults, through a small built-in style sheet, and the page's own
//  style sheets and style attributes override them.  Everything is laid out
//  in character cells, because the only font on the machine is fixed-width.
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
};

struct Item
{
    enum Kind : uint8_t
    {
        TEXT,
        BLOCK, // a new block starts: margin, indent, heading, marker, alignment
        BR,
        HR,
    };
    uint8_t kind;
    uint8_t style;
    uint8_t heading; // BLOCK: 1..6, 0 for none
    uint8_t pre;     // BLOCK: whitespace is kept and lines are not wrapped
    uint8_t margin;  // BLOCK: blank lines wanted above
    uint8_t indent;  // BLOCK: in cells
    uint8_t align;   // BLOCK: 0 left, 1 centre, 2 right
    uint8_t fg, bg;  // TEXT: palette index + 1, 0 for the default
    uint8_t pad[3];
    int32_t link;    // TEXT: index into the link table, or -1
    uint32_t off;    // TEXT: the text; BLOCK: the list marker, if any
    uint32_t len;
};

//  One line of the laid-out page.  Big lines are headings drawn at twice the
//  size; they take two rows and have half as many columns.
struct Line
{
    uint32_t firstRun;
    uint16_t nRuns;
    uint8_t big;
    uint8_t hr;
    int32_t row;
};

struct Run
{
    uint32_t off;
    uint16_t len;
    uint16_t col; // in the line's own cells (big cells on a big line)
    uint8_t style;
    uint8_t fg, bg;
    uint8_t pad;
    int32_t link;
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
    uint32_t options;   // index of the first (value, label) pair in optionOffs_
    uint32_t textOff;   // where its placeholder cells are in the text
    char *edit;         // the current text of a text control
    uint32_t editLen, editCap;

    bool isText() const { return type == TEXT || type == PASSWORD || type == TEXTAREA; }
    bool isButton() const { return type == SUBMIT || type == RESET || type == BUTTON || type == IMAGE; }
};

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

    //  Where link i first appears, as a row; -1 if it is not laid out.
    int linkRow(int i) const;

    //  The next run at or after (line, run) whose text contains needle,
    //  case-insensitively.  Returns false when there is none.
    bool find(const char *needle, size_t &lineIdx, size_t &runIdx) const;

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
    void controlInsert(int c, char ch);
    void controlBackspace(int c);

    //  Checkbox, radio (clears its group), select (next option).
    void controlActivate(int c, bool backwards = false);
    void resetForm(int f);

    //  The form's data as application/x-www-form-urlencoded, as submitted
    //  by `submitter` (a button, or -1 for an implicit submission).
    bool formData(int form, int submitter, Buf &out) const;

private:
    Buf text_{true};
    Buf items_{true};
    Buf links_{true};
    Buf linkOffs_{true};
    Buf lines_{true};
    Buf runs_{true};
    Buf strings_{true};
    Buf controls_{true};
    Buf forms_{true};
    Buf optionOffs_{true};
    Buf sheetOffs_{true};
    char title_[96] = {};
    int cols_ = 0;
    int rows_ = 0;
    bool oom_ = false;

    uint32_t addString(const char *s, size_t n);

    friend class HtmlParser;
    friend class Layout;
};

//  Unicode code point to the font's byte; 0 drops the character.  Used by the
//  address bar too, which shows what the page's title says.
uint8_t glyphFor(uint32_t cp);

} // namespace web
