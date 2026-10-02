#pragma once
//
//  A PNG from a picture of palette indices: what a screenshot is here, one
//  byte a pixel in the screen's colours.  Colour type 3 (a palette), four
//  bits a pixel when there are 16 colours and eight when there are more.
//
//  Kept simple and still small for a screen: every row is filtered "Up" (a
//  pixel the same as the one above it becomes 0), and the result deflated
//  with the fixed Huffman codes and runs of one byte --- which is what a
//  screen of flat windows and unchanged rows turns into.  No search for
//  longer matches, so a photograph would come out large; a screenshot does
//  not.
//
#include "wbase.h"

namespace web {

//  `palette` is `colours` RGB triplets.  False when out of memory.
bool encodePng(const uint8_t *pixels, int width, int height, const uint8_t *palette, int colours, Buf &out);

} // namespace web
