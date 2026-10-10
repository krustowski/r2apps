#pragma once
//
//  htmlparse --- HTML read into the tree dom.js builds.
//
//  A tokenizer and a tree builder after the HTML standard's, simplified: the
//  implied html, head and body; paragraphs, list items, options and table
//  cells that end where browsers end them; implied tbody and tr; raw text
//  in script, style, textarea and title; SVG and MathML kept apart.  What
//  comes out is a flat list of operations, balanced, that dom.js turns into
//  nodes (build() there):
//
//      [1, tag, [name, value, ...], ns]   open an element (ns 0 HTML, 1 SVG, 2 MathML)
//      2                                  close it
//      "text"                             text in the open element
//      [8, text]                          a comment
//      [10, name]                         a doctype
//
//  The input is UTF-8.  A fragment (innerHTML) has no implied html, head or
//  body; `context` is the element it goes into, which decides whether it is
//  read as raw text.
//
#include "../memento-hello/web/wbase.h"
#include "quickjs.h"

namespace web {

JSValue parseHtml(JSContext *ctx, const char *src, size_t n, bool fragment, const char *context);

} // namespace web
