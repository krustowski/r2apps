#pragma once
//
//  An MP4 of H.264 video made into an Animation: what Telegram makes of a
//  GIF.  Its apps turn one into a silent MP4 before it is sent, and the GIFs
//  its search finds (Giphy, Tenor) are MP4s already, so almost every "GIF"
//  in a chat is one of these.
//
//  The file is read for its first video track: the decoder's parameter sets
//  from the avcC box, then where each sample is and how long it is shown,
//  from the sample tables (stsz, stsc, stco/co64, stts).  Samples go to
//  h264bsd (web/h264.h) a NAL unit at a time, each picture it gives back is
//  made RGB and handed to an AnimationBuilder: the same fitting, dithering
//  and memory budget as a GIF has.
//
//  h264bsd decodes the Baseline profile, which is what the GIF sites serve.
//  A file in the Main or High profile is refused at start(), before anything
//  is decoded, and the caller shows what it has instead.
//
//  Decoding takes a while --- a frame is a few milliseconds on real hardware
//  and many more under an emulator --- so it goes a step at a time: start(),
//  then step() from an idle loop until it says it is done, then finish().
//
#include "image.h"

struct web_h264_pic; // web/h264.h

namespace web {

class Mp4Animation
{
public:
    Mp4Animation() {}
    ~Mp4Animation() { cancel(); }
    Mp4Animation(const Mp4Animation &) = delete;
    Mp4Animation &operator=(const Mp4Animation &) = delete;

    //  Takes the file (`file` is left empty, its contents are kept here while
    //  decoding) and reads its tables.  nullptr when decoding has begun,
    //  otherwise why it cannot be, and nothing is kept.
    const char *start(Buf &file, int maxW, int maxH, int colours, uint32_t bg, size_t budget);

    //  Decodes for about `ms` milliseconds; true while there is more to do.
    bool step(uint64_t ms);

    //  After the last step: the animation into `out` (what was there is
    //  released) and nullptr, or why there is none.  Everything else is let go.
    const char *finish(Animation &out);

    //  Stops and lets everything go.
    void cancel();

    bool busy() const { return decoder != nullptr; }

    //  The first video track's size and profile, from the header alone:
    //  false when it is not an MP4 of H.264.  For the tests.
    static bool probe(const uint8_t *data, size_t len, int &w, int &h, int &profile);

private:
    struct Sample
    {
        uint32_t offset, size;
        uint32_t ms; // how long its picture is shown
    };

    Buf file{true};
    Sample *samples = nullptr;
    int nSamples = 0, next = 0, shown = 0;
    int nalLen = 4;
    const uint8_t *paramSets = nullptr; // the avcC box's SPS and PPS
    size_t paramLen = 0;
    void *decoder = nullptr;
    Animation anim;
    AnimationBuilder *builder = nullptr;
    uint8_t *rgba = nullptr;
    int rgbaW = 0, rgbaH = 0;
    bool failed = false;

    static void onPicture(void *ctx, const struct web_h264_pic *pic);
    void picture(const struct web_h264_pic *pic);
    void feedSample(const uint8_t *p, size_t n);
};

} // namespace web
