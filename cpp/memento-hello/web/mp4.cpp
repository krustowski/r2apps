#include "mp4.h"
#include "h264.h"

namespace web {

namespace {

uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

constexpr uint32_t fourcc(const char (&s)[5])
{
    return (uint32_t)(uint8_t)s[0] << 24 | (uint32_t)(uint8_t)s[1] << 16 | (uint32_t)(uint8_t)s[2] << 8 |
           (uint8_t)s[3];
}

//  A box: its type and what is inside it.
struct Box
{
    uint32_t type = 0;
    const uint8_t *p = nullptr, *e = nullptr;
    explicit operator bool() const { return p != nullptr; }
    size_t len() const { return (size_t)(e - p); }
};

//  The box at `at` among those in [at, end), moving `at` past it; an empty
//  Box at the end or where the boxes stop making sense.
Box nextBox(const uint8_t *&at, const uint8_t *end)
{
    Box b;
    if (!at || end - at < 8)
        return b;
    uint64_t size = be32(at);
    size_t head = 8;
    if (size == 1)
    {
        if (end - at < 16)
            return b;
        size = be64(at + 8);
        head = 16;
    }
    else if (size == 0)
        size = (uint64_t)(end - at); // to the end of what holds it
    if (size < head || size > (uint64_t)(end - at))
        return b;
    b.type = be32(at + 4);
    b.p = at + head;
    b.e = at + size;
    at = b.e;
    return b;
}

//  The first box of `type` directly inside [p, e).
Box child(const uint8_t *p, const uint8_t *e, uint32_t type)
{
    for (Box b = nextBox(p, e); b; b = nextBox(p, e))
        if (b.type == type)
            return b;
    return Box{};
}

Box child(const Box &in, uint32_t type) { return in ? child(in.p, in.e, type) : Box{}; }

//  What the first video track says: where its tables are.
struct Track
{
    Box stbl, avcC;
    uint32_t timescale = 0;
    int w = 0, h = 0;
    uint32_t codec = 0;
};

//  The first track whose handler is "vide".  Why not, or nullptr.
const char *findVideo(const uint8_t *data, size_t len, Track &t)
{
    Box moov = child(data, data + len, fourcc("moov"));
    if (!moov)
        return child(data, data + len, fourcc("ftyp")) ? "an MP4 without its index (moov)" : "not an MP4";
    const uint8_t *at = moov.p;
    for (Box trak = nextBox(at, moov.e); trak; trak = nextBox(at, moov.e))
    {
        if (trak.type != fourcc("trak"))
            continue;
        Box mdia = child(trak, fourcc("mdia"));
        Box hdlr = child(mdia, fourcc("hdlr"));
        if (!hdlr || hdlr.len() < 12 || be32(hdlr.p + 8) != fourcc("vide"))
            continue;
        Box mdhd = child(mdia, fourcc("mdhd"));
        if (mdhd && mdhd.len() >= 24)
            t.timescale = be32(mdhd.p + (mdhd.p[0] == 1 ? 20 : 12));
        t.stbl = child(child(mdia, fourcc("minf")), fourcc("stbl"));
        Box stsd = child(t.stbl, fourcc("stsd"));
        //  Its first sample entry, a VisualSampleEntry: 78 bytes of fields,
        //  the width and height among them, then boxes of its own.
        if (!stsd || stsd.len() < 8 + 8 + 78)
            return "an MP4 with a broken video track";
        const uint8_t *e0 = stsd.p + 8;
        Box entry = nextBox(e0, stsd.e);
        if (!entry || entry.len() < 78)
            return "an MP4 with a broken video track";
        t.codec = entry.type;
        t.w = be16(entry.p + 24);
        t.h = be16(entry.p + 26);
        t.avcC = child(entry.p + 78, entry.e, fourcc("avcC"));
        if ((t.codec != fourcc("avc1") && t.codec != fourcc("avc3")) || !t.avcC)
            return t.codec == fourcc("hvc1") || t.codec == fourcc("hev1") ? "H.265 video, past this decoder"
                                                                          : "video that is not H.264";
        if (!t.stbl || !t.timescale)
            return "an MP4 with a broken video track";
        return nullptr;
    }
    return "an MP4 with no video in it";
}

//  The picture sizes this decodes: its reference pictures are kept whole,
//  up to 16 of them, and 640x640 is a little over half a megabyte each.
const long MAX_PIXELS = 640L * 640;
const int MAX_SAMPLES = 4096;

} // namespace

bool Mp4Animation::probe(const uint8_t *data, size_t len, int &w, int &h, int &profile)
{
    Track t;
    if (findVideo(data, len, t) || t.avcC.len() < 4)
        return false;
    w = t.w;
    h = t.h;
    profile = t.avcC.p[1];
    return true;
}

const char *Mp4Animation::start(Buf &src, int maxW, int maxH, int colours, uint32_t bg, size_t budget)
{
    cancel();
    file.swap(src);
    const uint8_t *data = file.data;
    size_t len = file.len;
    Track t;
    const char *why = data ? findVideo(data, len, t) : "not an MP4";
    auto fail = [&](const char *w) {
        cancel();
        return w;
    };
    if (why)
        return fail(why);

    //  avcC: version, profile, its compatibility flags, level, the size of
    //  the lengths in front of NAL units, then the parameter sets.
    const uint8_t *c = t.avcC.p;
    if (t.avcC.len() < 7 || c[0] != 1)
        return fail("an MP4 with broken H.264 settings");
    int profile = c[1], compat = c[2];
    //  Baseline, or another profile that says it keeps to Baseline's tools
    //  (constraint_set0_flag).
    if (profile != 66 && !(compat & 0x80))
        return fail(profile == 77   ? "H.264 in the Main profile: this decodes Baseline only"
                    : profile >= 100 ? "H.264 in the High profile: this decodes Baseline only"
                                     : "H.264 in a profile this does not decode");
    if ((long)t.w * t.h > MAX_PIXELS || t.w <= 0 || t.h <= 0)
        return fail(t.w > 0 && t.h > 0 ? "a video too big to animate" : "an MP4 with a broken video track");
    nalLen = (c[4] & 3) + 1;
    paramSets = c + 5;
    paramLen = (size_t)(t.avcC.e - paramSets);

    //  The sample tables.
    Box stsz = child(t.stbl, fourcc("stsz")), stsc = child(t.stbl, fourcc("stsc"));
    Box stco = child(t.stbl, fourcc("stco")), co64 = child(t.stbl, fourcc("co64"));
    Box stts = child(t.stbl, fourcc("stts"));
    if (!stco)
        stco = co64;
    if (!stsz || stsz.len() < 12 || !stsc || stsc.len() < 8 || !stco || stco.len() < 8 || !stts || stts.len() < 8)
        return fail("an MP4 with broken sample tables");
    uint32_t fixedSize = be32(stsz.p + 4), count = be32(stsz.p + 8);
    if (!count)
        return fail("a fragmented MP4, which this does not read");
    if (count > (uint32_t)MAX_SAMPLES)
        count = MAX_SAMPLES; // the first few minutes are plenty for a GIF
    if (!fixedSize && stsz.len() < 12 + 4 * (size_t)count)
        return fail("an MP4 with broken sample tables");
    samples = (Sample *)big_alloc(sizeof(Sample) * count);
    if (!samples)
        return fail("out of memory");
    nSamples = (int)count;

    //  Sizes, then offsets: chunk by chunk, stsc saying how many samples
    //  each has, the samples of a chunk one after another from its offset.
    for (int i = 0; i < nSamples; i++)
        samples[i].size = fixedSize ? fixedSize : be32(stsz.p + 12 + 4 * i);
    bool wide = stco.type == fourcc("co64");
    uint32_t nChunks = be32(stco.p + 4), nRuns = be32(stsc.p + 4);
    if (stco.len() < 8 + (wide ? 8 : 4) * (size_t)nChunks || stsc.len() < 8 + 12 * (size_t)nRuns)
        return fail("an MP4 with broken sample tables");
    int s = 0;
    for (uint32_t r = 0; r < nRuns && s < nSamples; r++)
    {
        const uint8_t *run = stsc.p + 8 + 12 * r;
        uint32_t first = be32(run), per = be32(run + 4);
        uint32_t last = r + 1 < nRuns ? be32(run + 12) - 1 : nChunks; // chunks are counted from 1
        for (uint32_t ch = first; ch >= 1 && ch <= last && ch <= nChunks && s < nSamples; ch++)
        {
            uint64_t off = wide ? be64(stco.p + 8 + 8 * (ch - 1)) : be32(stco.p + 8 + 4 * (ch - 1));
            for (uint32_t k = 0; k < per && s < nSamples; k++, s++)
            {
                if (off + samples[s].size > len)
                    return fail("an MP4 cut short");
                samples[s].offset = (uint32_t)off;
                off += samples[s].size;
            }
        }
    }
    if (s < nSamples)
        nSamples = s; // tables that do not add up: what they do say

    //  How long each is shown: stts in the track's time scale, made
    //  milliseconds from the running total so that rounding does not add up.
    uint32_t nTimes = be32(stts.p + 4);
    if (stts.len() < 8 + 8 * (size_t)nTimes)
        return fail("an MP4 with broken sample tables");
    uint64_t ticks = 0, lastMs = 0;
    s = 0;
    for (uint32_t r = 0; r < nTimes && s < nSamples; r++)
    {
        uint32_t n = be32(stts.p + 8 + 8 * r), d = be32(stts.p + 12 + 8 * r);
        for (uint32_t k = 0; k < n && s < nSamples; k++, s++)
        {
            ticks += d;
            uint64_t ms = ticks * 1000 / t.timescale;
            samples[s].ms = (uint32_t)(ms - lastMs);
            lastMs = ms;
        }
    }
    for (; s < nSamples; s++)
        samples[s].ms = s ? samples[s - 1].ms : 100;

    decoder = web_h264_open();
    builder = new AnimationBuilder(anim, maxW, maxH, colours, bg, budget);
    if (!decoder || !builder)
        return fail("out of memory");
    next = shown = 0;
    failed = false;

    //  The parameter sets first: numOfSequenceParameterSets (low five bits),
    //  each a 16-bit length and the NAL unit, then the picture ones likewise.
    const uint8_t *p = paramSets, *e = paramSets + paramLen;
    for (int kind = 0; kind < 2 && p < e; kind++)
    {
        int n = kind ? *p++ : *p++ & 31;
        for (int i = 0; i < n && e - p >= 2; i++)
        {
            size_t l = be16(p);
            p += 2;
            if ((size_t)(e - p) < l)
                break;
            if (l && web_h264_nal(decoder, (unsigned char *)p, (unsigned)l, onPicture, this) < 0)
                return fail("out of memory");
            p += l;
        }
    }
    return nullptr;
}

void Mp4Animation::onPicture(void *ctx, const struct web_h264_pic *pic)
{
    ((Mp4Animation *)ctx)->picture(pic);
}

//  A decoded picture: made RGB (BT.601, as H.264 is unless it says
//  otherwise) and given to the builder with its sample's time.
void Mp4Animation::picture(const struct web_h264_pic *pic)
{
    if (failed || pic->w <= 0 || pic->h <= 0)
        return;
    if (!rgba || rgbaW != pic->w || rgbaH != pic->h)
    {
        if (rgba)
            big_free(rgba);
        rgba = (uint8_t *)big_alloc((size_t)pic->w * pic->h * 4);
        rgbaW = pic->w;
        rgbaH = pic->h;
        if (!rgba)
        {
            failed = true;
            return;
        }
    }
    auto clamp = [](int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); };
    for (int y = 0; y < pic->h; y++)
    {
        const uint8_t *py = pic->y + (size_t)y * pic->stride;
        const uint8_t *pu = pic->cb + (size_t)(y / 2) * pic->cstride;
        const uint8_t *pv = pic->cr + (size_t)(y / 2) * pic->cstride;
        uint8_t *o = rgba + (size_t)y * pic->w * 4;
        for (int x = 0; x < pic->w; x++, o += 4)
        {
            int Y = py[x], D = pu[x / 2] - 128, E = pv[x / 2] - 128;
            if (pic->fullRange)
            {
                o[0] = clamp(Y + ((359 * E + 128) >> 8));
                o[1] = clamp(Y - ((88 * D + 183 * E + 128) >> 8));
                o[2] = clamp(Y + ((454 * D + 128) >> 8));
            }
            else
            {
                int C = 298 * (Y - 16) + 128;
                o[0] = clamp((C + 409 * E) >> 8);
                o[1] = clamp((C - 100 * D - 208 * E) >> 8);
                o[2] = clamp((C + 516 * D) >> 8);
            }
            o[3] = 255;
        }
    }
    int ms = samples[shown < nSamples ? shown : nSamples - 1].ms;
    shown++;
    if (!builder->add(rgba, pic->w, pic->h, ms))
        failed = true;
}

//  A sample: NAL units, each behind its length.
void Mp4Animation::feedSample(const uint8_t *p, size_t n)
{
    const uint8_t *e = p + n;
    while (!failed && (size_t)(e - p) > (size_t)nalLen)
    {
        size_t l = 0;
        for (int i = 0; i < nalLen; i++)
            l = l << 8 | *p++;
        if (!l || l > (size_t)(e - p))
            return;
        if (web_h264_nal(decoder, (unsigned char *)p, (unsigned)l, onPicture, this) < 0)
            failed = true;
        p += l;
    }
}

bool Mp4Animation::step(uint64_t ms)
{
    if (!decoder)
        return false;
    uint64_t from = now_ms();
    while (next < nSamples && !failed)
    {
        feedSample(file.data + samples[next].offset, samples[next].size);
        next++;
        if (now_ms() - from >= ms)
            return true;
    }
    if (!failed)
        web_h264_flush(decoder, onPicture, this);
    //  The file and the decoder are done with; the frames are kept for finish().
    web_h264_close(decoder);
    decoder = nullptr;
    return false;
}

const char *Mp4Animation::finish(Animation &out)
{
    const char *why = builder ? builder->finish() : "not decoded";
    if (!why)
    {
        out.release();
        out = anim;             // the frames change owners...
        anim = Animation{};     // ...and are not released with the rest
    }
    cancel();
    return why;
}

void Mp4Animation::cancel()
{
    if (decoder)
        web_h264_close(decoder);
    decoder = nullptr;
    delete builder;
    builder = nullptr;
    anim.release();
    if (samples)
        big_free(samples);
    samples = nullptr;
    nSamples = next = shown = 0;
    if (rgba)
        big_free(rgba);
    rgba = nullptr;
    rgbaW = rgbaH = 0;
    file.release();
    failed = false;
}

} // namespace web
