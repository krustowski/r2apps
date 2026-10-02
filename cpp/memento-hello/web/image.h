#pragma once
//
//  Pictures: a PNG, JPEG, GIF or BMP turned into what the r2 backend of
//  Memento draws, one palette index a pixel.  The browser shows them on pages
//  and the Telegram window in chats.
//
//  Decoding is stb_image (stb_image.c).  The picture is then made to fit a
//  box --- scaled down, never up, keeping its shape, each pixel the average
//  of the ones it stands for --- drawn over a background where it is
//  transparent, and put into the screen's colours with a 4x4 ordered dither:
//  the 16 EGA colours, or with 256 the 6x6x6 cube the palette has at 16..231
//  (cpp/mpegplay does the same for films).
//
#include "wbase.h"

namespace web {

struct Picture
{
    int w = 0, h = 0;
    uint8_t *px = nullptr; // w * h palette indices, row by row
    void release();
};

//  Decodes `data` into `out`, at most maxW x maxH.  `colours` is 16 or 256,
//  `bg` the colour under transparent pixels as 0xRRGGBB.  nullptr when it
//  worked, otherwise why not, in a few words.  `out` is released first.
const char *decodePicture(const uint8_t *data, size_t len, int maxW, int maxH, int colours, uint32_t bg,
                          Picture &out);

//  Only the size, from the header: false when it is not a picture this reads.
bool pictureSize(const uint8_t *data, size_t len, int &w, int &h);

//  An animated GIF: its frames, each a Picture's worth of palette indices,
//  and how long each is shown.  Where the file has more frames than `budget`
//  bytes hold, every other one is left out (and the time it was shown given
//  to the one before), as often as it takes: the whole loop at a lower frame
//  rate, not the first second of it.
struct Animation
{
    int w = 0, h = 0, frames = 0;
    uint8_t *px = nullptr;       // frames * w * h palette indices, a frame after another
    uint16_t *delay = nullptr;   // ms each is shown
    uint32_t length = 0;         // ms all of them are, once round
    const uint8_t *frame(int i) const { return px + (size_t)i * w * h; }
    //  The frame to show `ms` into the loop (any number: it goes round).
    int frameAt(uint64_t ms) const;
    void release();
};

bool isGif(const uint8_t *data, size_t len);

//  An Animation put together a frame at a time from RGBA, as a decoder hands
//  them over: each made to fit maxW x maxH (the first one's shape sets the
//  size) and put into the screen's colours, within `budget` bytes as above.
//  decodeAnimation is one of these round stb_image; web/mp4.h another round
//  an H.264 decoder.
class AnimationBuilder
{
public:
    AnimationBuilder(Animation &out, int maxW, int maxH, int colours, uint32_t bg, size_t budget);
    //  The next frame, to be shown for `ms`.  False when it could not be
    //  taken and no more will be (why() says why).
    bool add(const uint8_t *rgba, int w, int h, int ms);
    //  After the last: nullptr when there is at least a frame, else why not
    //  (and the animation is released).
    const char *finish();
    const char *why() const { return why_; }

private:
    Animation &a;
    int maxW, maxH, colours;
    uint32_t bg;
    size_t budget;
    int cap = 0;    // frames px has room for, an even number
    int stride = 1; // every stride-th frame given is kept...
    long seen = 0;  // ...counting from the first, of these
    const char *why_ = nullptr;
};

//  As decodePicture, for every frame of a GIF.  One frame is an animation
//  too: it just does not move.
const char *decodeAnimation(const uint8_t *data, size_t len, int maxW, int maxH, int colours, uint32_t bg,
                            size_t budget, Animation &out);

} // namespace web
