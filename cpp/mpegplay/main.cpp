//
//  mpegplay --- an MPEG-1 video player for r2, with pl_mpeg doing the decoding.
//
//      mpegplay.elf --host 0xC12340 /mnt/tar/video/clip.mpg
//
//  It has no screen of its own: memento's Video window starts it with the
//  address of a VidHostBlock (vidhost.hpp), and the player decodes each
//  picture, turns it into the screen's colours with an ordered dither (the 16
//  EGA ones, or Memento's 256), and puts it in the block on time.  The window
//  copies it to the screen.  Or, when the window lends it the whole screen
//  (F, graphics kernel), the player draws on the framebuffer itself in true
//  colour: see "The whole screen" below.
//
//  Sound: the MP2 audio of a program stream is decoded by mp2.hpp --- pl_mpeg's
//  decoder taken back to integers, as r2 does not keep a task's SSE or x87
//  registers across a task switch and this program is built with
//  -mgeneral-regs-only --- and played through the HD Audio driver (syscall
//  0x3f, r2/audio.hpp).  Then the sound is the clock: a picture is shown when
//  what has been played reaches its time.  Without a sound device, or a film
//  without audio, the pictures keep time by the tick counter as before.
//
//  Both kinds of MPEG-1 file play: a program stream (.mpg, from
//  `ffmpeg -f mpeg`) and a bare video stream (.m1v, `-f mpeg1video`).  See
//  README.md for the ffmpeg line that makes one.
//
//  And a stream: with --stream in place of the file, the window fetches an
//  HLS playlist's MPEG-TS segments and hands the bytes over in a ring in the
//  block; the transport stream is demuxed here (MPEG-1 video, MP2 audio).
//

#include <r2/audio.hpp>
#include <r2/fs.hpp>
#include <r2/heap.hpp>
#include <r2/process.hpp>
#include <r2/syscall.hpp>
#include <r2/time.hpp>

#include "vidhost.hpp"

//  The heap: the three pictures pl_mpeg keeps plus its two 128 KiB buffers
//  come to about 600 KiB at 320x240.  A megabyte in the image, and more from
//  the user heap for a larger film.
R2_HEAP_ARENA_GROWING(1024 * 1024)

static void outOfMemory();

static void *plmAlloc(size_t n)
{
    void *p = r2::heap::allocate(n);
    if (!p)
        outOfMemory();
    return p;
}

static void *plmRealloc(void *p, size_t n)
{
    void *q = r2::heap::reallocate(p, n);
    if (!q)
        outOfMemory();
    return q;
}

#define PLM_MALLOC(sz) plmAlloc(sz)
#define PLM_FREE(p) r2::heap::deallocate(p)
#define PLM_REALLOC(p, sz) plmRealloc(p, sz)
#define PLM_NO_STDIO
#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg_r2.h"
#include "mp2.hpp"

namespace {

VidHostBlock *blk = nullptr;

// ── The host ────────────────────────────────────────────────────────────────

//  The block is on the user heap: the 4 MiB from 0xC00000, or the extension
//  the kernel adds past them when they are full (from 0xA000000).
constexpr uint64_t HeapStart = 0xC00000;

bool parseHex(r2::string_view s, uint64_t &out)
{
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s = s.substr(2);
    if (s.empty() || s.size() > 16)
        return false;
    uint64_t v = 0;
    for (size_t i = 0; i < s.size(); i++)
    {
        const char c = s[i];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0)
            return false;
        v = v * 16 + (uint64_t)d;
    }
    out = v;
    return true;
}

VidHostBlock *hostFromArgs()
{
    if (r2::arg_count() < 4 || r2::arg(1) != r2::string_view("--host"))
        return nullptr;
    uint64_t addr = 0;
    if (!parseHex(r2::arg(2), addr) || addr < HeapStart || (addr & 7))
        return nullptr;
    VidHostBlock *b = (VidHostBlock *)addr;
    if (b->magic != VidHostBlock::Magic || b->version != VidHostBlock::Version)
        return nullptr;
    return b;
}

void say(const char *text)
{
    size_t i = 0;
    for (; text[i] && i < sizeof(blk->message) - 1; i++)
        blk->message[i] = text[i];
    blk->message[i] = 0;
}

[[noreturn]] void leave(int code)
{
    r2::audio::close();
    if (blk)
        blk->exited = 1;
    r2::exit(code);
}

[[noreturn]] void fail(const char *why)
{
    say(why);
    blk->state = VidHostBlock::FAILED;
    //  Stay until the window has shown it and let go, so that the message is
    //  read before the block goes.
    uint32_t heard = blk->hostBeat;
    uint64_t lastHeard = r2::ticks();
    while (!blk->quit && r2::ticks() - lastHeard < 10000)
    {
        blk->playerBeat = blk->playerBeat + 1;
        if (blk->hostBeat != heard)
        {
            heard = blk->hostBeat;
            lastHeard = r2::ticks();
        }
        r2::sleep(50);
    }
    leave(1);
}

// ── The file ────────────────────────────────────────────────────────────────
//
// pl_mpeg reads through three callbacks, which here go to syscall 0x39: a
// chunk of the file at an offset.  Nothing but the 128 KiB buffer is held.

struct Source
{
    const char *path;
    uint64_t pos;
};

Source source;

void loadCallback(plm_buffer_t *b, void *user)
{
    Source *src = (Source *)user;
    //  Only when the buffer says so: while the decoder looks ahead for the
    //  next picture it turns this off, and rewinds to where it was after.
    if (b->discard_read_bytes)
        plm_buffer_discard_read_bytes(b);
    size_t room = b->capacity - b->length;
    if (room < 4096)
    {
        //  A look-ahead that filled the buffer: make room rather than report
        //  the end of the file.
        size_t cap = b->capacity * 2;
        b->bytes = (uint8_t *)PLM_REALLOC(b->bytes, cap);
        b->capacity = cap;
        room = cap - b->length;
    }
    int64_t got = r2::fs::read_at(src->path, r2::byte_span(b->bytes + b->length, room), src->pos);
    if (got <= 0)
    {
        b->has_ended = TRUE;
        return;
    }
    b->length += (size_t)got;
    src->pos += (uint64_t)got;
}

//  The file's size, from its directory's listing (there is no stat call).
//  Straight from syscall 0x2D rather than r2::fs::size_of(), which builds a
//  vector of strings on the way --- through library code that is not built
//  without SSE (see the Makefile).
uint32_t fileSize(const char *path)
{
    struct Entry
    {
        uint8_t name[32];
        uint8_t nameLen;
        uint8_t isDir;
        uint32_t size;
    } __attribute__((packed));
    static Entry entries[64];
    static char dir[128];

    size_t slash = 0, len = 0;
    for (; path[len]; len++) // not strlen(): libc++r2's is vectorised
        if (path[len] == '/')
            slash = len;
    if (len >= sizeof(dir))
        return 0;
    memcpy(dir, path, slash ? slash : 1);
    dir[slash ? slash : 1] = 0;
    const char *name = path + slash + 1;
    size_t nameLen = len - slash - 1;

    int64_t n = r2::raw_syscall(r2::Sys::ListDirPath, (int64_t)dir, (int64_t)entries);
    for (int64_t i = 0; i >= 0 && i < n && i < 64; i++)
    {
        const Entry &e = entries[i];
        if (e.isDir || e.nameLen != nameLen)
            continue;
        size_t k = 0;
        //  Names match without regard to case, as the filesystems do.
        while (k < nameLen && (e.name[k] | 0x20) == ((uint8_t)name[k] | 0x20))
            k++;
        if (k == nameLen)
            return e.size;
    }
    return 0;
}

void seekCallback(plm_buffer_t *, size_t offset, void *user) { ((Source *)user)->pos = offset; }

size_t tellCallback(plm_buffer_t *, void *user) { return (size_t)((Source *)user)->pos; }

// ── Program streams ─────────────────────────────────────────────────────────
//
// A .mpg interleaves video with audio (and whatever else) in packets.  The
// demuxer reads them from the file and the video ones go into a buffer of
// their own, which the decoder reads --- what pl_mpeg's plm_t does, without
// the audio.

plm_demux_t *demux = nullptr;
plm_buffer_t *videoBuffer = nullptr;
plm_buffer_t *audioBuffer = nullptr; // the first MP2 stream, when there is one
int64_t firstVideoPts = PLM_PACKET_INVALID_TS, firstAudioPts = PLM_PACKET_INVALID_TS;

//  A packet to the buffer of its kind; the first time stamp of each is kept,
//  to start the sound and the pictures together.
bool route(plm_packet_t *packet)
{
    if (packet->type == PLM_DEMUX_PACKET_VIDEO_1)
    {
        if (firstVideoPts == PLM_PACKET_INVALID_TS)
            firstVideoPts = packet->pts;
        plm_buffer_write(videoBuffer, packet->data, packet->length);
        return true;
    }
    if (packet->type == PLM_DEMUX_PACKET_AUDIO_1 && audioBuffer)
    {
        if (firstAudioPts == PLM_PACKET_INVALID_TS)
            firstAudioPts = packet->pts;
        plm_buffer_write(audioBuffer, packet->data, packet->length);
        return true;
    }
    return false;
}

void demuxEnded()
{
    if (!plm_demux_has_ended(demux))
        return;
    plm_buffer_signal_end(videoBuffer);
    if (audioBuffer)
        plm_buffer_signal_end(audioBuffer);
}

//  Each decoder pulls packets until one of its own kind has come; the ones
//  of the other kind met on the way go to the other buffer.
void videoPacketCallback(plm_buffer_t *, void *)
{
    plm_packet_t *packet;
    while ((packet = plm_demux_decode(demux)))
        if (route(packet) && packet->type == PLM_DEMUX_PACKET_VIDEO_1)
            return;
    demuxEnded();
}

void audioPacketCallback(plm_buffer_t *, void *)
{
    plm_packet_t *packet;
    while ((packet = plm_demux_decode(demux)))
        if (route(packet) && packet->type == PLM_DEMUX_PACKET_AUDIO_1)
            return;
    demuxEnded();
}

bool hostStillThere();

// ── A transport stream (HLS) ────────────────────────────────────────────────
//
// 188-byte packets from the window's ring.  The PAT names the programme's
// PMT, the PMT names its streams, and the payload of each PES packet of the
// video and the audio stream goes into the buffer the decoder reads --- the
// same buffers, with the same first time stamps, as a program stream's.

enum
{
    TS_VIDEO = 1,
    TS_AUDIO = 2
};
int tsPmtPid = -1, tsVideoPid = -1, tsAudioPid = -1;
bool tsHasPmt = false;
int tsVideoType = 0, tsAudioType = 0; // stream_type from the PMT
uint8_t tsPacket[188];
size_t tsFill = 0;
//  MPEG-1 or MPEG-2 video: the PMT does not say (ffmpeg calls MPEG-1 0x02
//  too), the bitstream does --- MPEG-2 has a sequence extension (00 00 01 B5)
//  before its first picture (00 00 01 00).  Looked for until that picture.
bool tsVideoChecked = false, tsMpeg2 = false;
uint32_t tsScan = 0xFFFFFFFF;

//  Whatever of the ring there is, up to n bytes; does not wait.
size_t ringRead(uint8_t *dst, size_t n)
{
    uint64_t head = blk->streamHead, tail = blk->streamTail;
    __asm__ volatile("" ::: "memory");
    size_t avail = (size_t)(head - tail);
    if (n > avail)
        n = avail;
    const uint8_t *ring = (const uint8_t *)blk->streamRing;
    for (size_t k = 0; k < n;)
    {
        size_t at = (size_t)((tail + k) % blk->streamSize);
        size_t chunk = blk->streamSize - at;
        if (chunk > n - k)
            chunk = n - k;
        memcpy(dst + k, ring + at, chunk);
        k += chunk;
    }
    __asm__ volatile("" ::: "memory");
    blk->streamTail = tail + n;
    return n;
}

//  Time spent waiting for the window's bytes, in all: a picture's decode
//  time is told without it.
uint64_t tsWaitedMs = 0;

//  The next whole packet, waiting for the window as long as it takes (and
//  beating meanwhile); false at the end of the stream.
bool tsNextPacket()
{
    uint64_t waitFrom = 0;
    for (;;)
    {
        tsFill += ringRead(tsPacket + tsFill, sizeof(tsPacket) - tsFill);
        //  Lost sync: drop bytes up to the next 0x47.
        size_t skip = 0;
        while (skip < tsFill && tsPacket[skip] != 0x47)
            skip++;
        if (skip)
        {
            memmove(tsPacket, tsPacket + skip, tsFill - skip);
            tsFill -= skip;
            continue;
        }
        if (tsFill == sizeof(tsPacket))
        {
            tsFill = 0;
            if (waitFrom)
            {
                uint64_t w = r2::ticks() - waitFrom;
                tsWaitedMs += w;
                if (w > blk->waitMaxMs)
                    blk->waitMaxMs = (uint32_t)w;
                if (w > 50)
                    blk->waits = blk->waits + 1;
            }
            return true;
        }
        if (!waitFrom)
            waitFrom = r2::ticks();
        if (blk->streamEnded && blk->streamHead == blk->streamTail)
            return false;
        if (!hostStillThere())
            leave(0);
        r2::sleep(5);
    }
}

//  A PSI section's start in a packet's payload, past the pointer field.
const uint8_t *tsSection(const uint8_t *p, const uint8_t *end)
{
    if (p >= end)
        return nullptr;
    p += 1 + p[0];
    return p + 3 <= end ? p : nullptr;
}

void tsParsePat(const uint8_t *p, const uint8_t *end)
{
    p = tsSection(p, end);
    if (!p || p[0] != 0x00)
        return;
    int len = ((p[1] & 0x0F) << 8) | p[2];
    const uint8_t *e = p + 3 + len - 4; // less the CRC
    if (e > end)
        e = end;
    for (const uint8_t *q = p + 8; q + 4 <= e; q += 4)
    {
        int program = (q[0] << 8) | q[1];
        if (program != 0)
        {
            tsPmtPid = ((q[2] & 0x1F) << 8) | q[3];
            return;
        }
    }
}

void tsParsePmt(const uint8_t *p, const uint8_t *end)
{
    p = tsSection(p, end);
    if (!p || p[0] != 0x02)
        return;
    int len = ((p[1] & 0x0F) << 8) | p[2];
    const uint8_t *e = p + 3 + len - 4;
    if (e > end)
        e = end;
    int infoLen = ((p[10] & 0x0F) << 8) | p[11];
    for (const uint8_t *q = p + 12 + infoLen; q + 5 <= e;)
    {
        int type = q[0];
        int pid = ((q[1] & 0x1F) << 8) | q[2];
        int esLen = ((q[3] & 0x0F) << 8) | q[4];
        if (tsVideoPid < 0 && (type == 0x01 || type == 0x02 || type == 0x1B || type == 0x24))
            tsVideoPid = pid, tsVideoType = type;
        else if (tsAudioPid < 0 && (type == 0x03 || type == 0x04 || type == 0x0F || type == 0x11))
            tsAudioPid = pid, tsAudioType = type;
        q += 5 + esLen;
    }
    tsHasPmt = true;
}

//  One packet's payload to where it belongs; returns which elementary stream
//  got bytes (0 for none).
int tsProcess()
{
    const uint8_t *pkt = tsPacket;
    bool start = pkt[1] & 0x40;
    int pid = ((pkt[1] & 0x1F) << 8) | pkt[2];
    int afc = (pkt[3] >> 4) & 3;
    const uint8_t *p = pkt + 4, *end = pkt + 188;
    if (afc & 2)
        p += 1 + p[0];
    if (!(afc & 1) || p >= end)
        return 0;
    if (pid == 0)
    {
        if (start)
            tsParsePat(p, end);
        return 0;
    }
    if (pid == tsPmtPid)
    {
        if (start)
            tsParsePmt(p, end);
        return 0;
    }
    int kind = pid == tsVideoPid ? TS_VIDEO : pid == tsAudioPid && audioBuffer ? TS_AUDIO : 0;
    if (!kind)
        return 0;
    if (start)
    {
        //  A PES header: 00 00 01, the stream id, the length, two flag bytes
        //  and the header's own length; a PTS when the flags say so.
        if (end - p < 9 || p[0] || p[1] || p[2] != 1)
            return 0;
        int flags = p[7], hlen = p[8];
        if (flags & 0x80 && end - p >= 14)
        {
            int64_t pts = ((int64_t)(p[9] & 0x0E) << 29) | ((int64_t)p[10] << 22) | ((int64_t)(p[11] & 0xFE) << 14) |
                          ((int64_t)p[12] << 7) | (p[13] >> 1);
            int64_t &first = kind == TS_VIDEO ? firstVideoPts : firstAudioPts;
            if (first == PLM_PACKET_INVALID_TS)
                first = pts;
        }
        p += 9 + hlen;
        if (p >= end)
            return 0;
    }
    if (kind == TS_VIDEO && !tsVideoChecked)
        for (const uint8_t *q = p; q < end && !tsVideoChecked; q++)
        {
            tsScan = (tsScan << 8) | *q;
            if (tsScan == 0x000001B5)
                tsMpeg2 = true;
            else if (tsScan == 0x00000100)
                tsVideoChecked = true;
        }
    plm_buffer_write(kind == TS_VIDEO ? videoBuffer : audioBuffer, (uint8_t *)p, (size_t)(end - p));
    return kind;
}

//  Packets until the stream `want` has had a few KiB; at the end of the
//  stream both buffers are told so.  A few KiB and not one packet's 184
//  bytes: a decoder asks for more once per look at its buffer, and an MP2
//  frame (about 400 bytes) would otherwise take three asks, of which it
//  makes one before giving up for the moment.
void tsPump(int want)
{
    size_t got = 0;
    while (tsNextPacket())
        if (tsProcess() == want && (got += 184) >= 4096)
            return;
    if (got)
        return; // the end, but what came is to be read first
    plm_buffer_signal_end(videoBuffer);
    if (audioBuffer)
        plm_buffer_signal_end(audioBuffer);
}

void tsVideoCallback(plm_buffer_t *, void *) { tsPump(TS_VIDEO); }
void tsAudioCallback(plm_buffer_t *, void *) { tsPump(TS_AUDIO); }

//  The PAT and the PMT, before anything can be decoded; the streams' kinds
//  checked against what plays here.
void tsStart()
{
    uint64_t since = r2::ticks();
    while (!tsHasPmt)
    {
        if (!tsNextPacket())
            fail("The stream ended before it said what is in it.");
        tsProcess();
        if (r2::ticks() - since > 30000)
            fail("No programme table in the stream after 30 s.");
    }
    if (tsVideoPid < 0)
        fail("The stream has no video.");
    if (tsVideoType == 0x1B || tsVideoType == 0x24)
        fail("The stream is H.264/H.265: only MPEG-1 video plays (-c:v mpeg1video).");
    if (tsAudioType == 0x0F || tsAudioType == 0x11)
        tsAudioPid = -1; // AAC: the pictures play, without sound
}

// ── Sound ───────────────────────────────────────────────────────────────────
//
// MP2 frames are decoded and queued until about LEAD_MS is waiting in the
// kernel's ring (0.68 s at 48 kHz); what has been played is what was written
// less what is still queued, and that is the clock the pictures keep to.

constexpr uint32_t LEAD_MS = 350;
//  A picture this far behind (at least three frames) is skipped, not shown.
constexpr int64_t LATE_MS = 100;

struct Sound
{
    Mp2Decoder *mp2 = nullptr;
    bool on = false;      // the stream is open and the film has audio left
    int rate = 0;
    uint64_t written = 0; // bytes
    int64_t offsetMs = 0; // where the sound starts, on the pictures' clock
};

Sound sound;

//  Keeps the ring about LEAD_MS full.  When the audio has run out and been
//  played, the sound is no longer the clock.
void feed()
{
    if (!sound.on)
        return;
    const uint64_t lead = (uint64_t)sound.rate * 4 * LEAD_MS / 1000;
    while (r2::audio::queued() < lead)
    {
        if (!sound.mp2->decode())
        {
            if (plm_buffer_has_ended(audioBuffer) && r2::audio::queued() == 0)
                sound.on = false;
            return;
        }
        //  At the window's volume (Up / Down there): percent, 100 as decoded,
        //  louder ones clipped rather than wrapped round.
        static int16_t scaled[Mp2Decoder::SAMPLES * 2];
        const int16_t *in = (const int16_t *)sound.mp2->out();
        const int vol = blk->volume;
        for (int i = 0; i < Mp2Decoder::SAMPLES * 2; i++)
        {
            int v = in[i] * vol / 100;
            scaled[i] = (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
        }
        const uint8_t *pcm = (const uint8_t *)scaled;
        size_t left = Mp2Decoder::SAMPLES * 4;
        while (left)
        {
            size_t took = r2::audio::write(pcm, left);
            if (!took)
            {
                r2::sleep(2);
                continue;
            }
            pcm += took;
            left -= took;
            sound.written += took;
        }
    }
}

//  Milliseconds of the film that have been heard.
int64_t soundMs()
{
    uint64_t q = r2::audio::queued();
    uint64_t played = sound.written > q ? sound.written - q : 0;
    return (int64_t)(played * 1000 / ((uint64_t)sound.rate * 4)) + sound.offsetMs;
}

//  Opens the stream for the film's audio, when it has any and there is a
//  device; says which in the block.
void startSound()
{
    sound.on = false;
    sound.written = 0;
    blk->sound = VidHostBlock::SOUND_NONE;
    if (!audioBuffer)
        return;
    if (!sound.mp2)
        sound.mp2 = new Mp2Decoder(audioBuffer);
    sound.rate = sound.mp2->rate();
    if (!sound.rate)
        return;
    if (!r2::audio::open((uint32_t)sound.rate))
    {
        blk->sound = VidHostBlock::SOUND_NO_DEVICE;
        return;
    }
    sound.offsetMs = firstAudioPts != PLM_PACKET_INVALID_TS && firstVideoPts != PLM_PACKET_INVALID_TS
                         ? (firstAudioPts - firstVideoPts) / 90
                         : 0;
    sound.on = true;
    blk->sound = VidHostBlock::SOUND_ON;
    feed();
}

// ── Colour ──────────────────────────────────────────────────────────────────
//
// Sixteen colours for a film.  Each pixel goes from YCbCr to RGB (pl_mpeg's
// own integer coefficients), has a 4x4 Bayer offset added --- the palette's
// steps are 85 apart, so the offsets span about that --- and is looked up in
// a table of the nearest palette colour for every RGB rounded to four bits a
// channel.  Blocks of dithered colour, but the shapes and the brightness come
// through, and there is no division or search per pixel.

const uint8_t kEga[16][3] = {
    {0, 0, 0},     {0, 0, 170},    {0, 170, 0},    {0, 170, 170},  {170, 0, 0},    {170, 0, 170},
    {170, 85, 0},  {170, 170, 170}, {85, 85, 85},  {85, 85, 255},  {85, 255, 85},  {85, 255, 255},
    {255, 85, 85}, {255, 85, 255}, {255, 255, 85}, {255, 255, 255},
};

uint8_t nearest[16 * 16 * 16];
//  8x8 Bayer offsets: 64 levels between two palette steps instead of 16, so
//  gradients come out smoother and the pattern is finer than the 4x4 one.
int bayer[8][8];

//  With 256 colours, the 6x6x6 cube at 16..231 (steps of 51) takes over from
//  the table: a channel's level is q6 of it once the (smaller) offset is on.
bool cube = false;
uint8_t q6[256];
int bayer6[8][8];
int bayerG[8][8]; // for the greys, ten apart

void buildColourTables()
{
    for (int r = 0; r < 16; r++)
        for (int g = 0; g < 16; g++)
            for (int b = 0; b < 16; b++)
            {
                int R = r * 17, G = g * 17, B = b * 17, best = 0, bestD = 1 << 30;
                for (int k = 0; k < 16; k++)
                {
                    int dr = R - kEga[k][0], dg = G - kEga[k][1], db = B - kEga[k][2];
                    //  The eye weighs green most and blue least.
                    int d = 3 * dr * dr + 4 * dg * dg + 2 * db * db;
                    if (d < bestD)
                        bestD = d, best = k;
                }
                nearest[(r << 8) | (g << 4) | b] = (uint8_t)best;
            }
    //  The 8x8 matrix from the 4x4 one: 4 * m4 plus the 2x2 pattern per quadrant.
    static const int m4[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    static const int m2[2][2] = {{0, 2}, {3, 1}};
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
        {
            int m = 4 * m4[y & 3][x & 3] + m2[y >> 2][x >> 2]; // 0..63
            bayer[y][x] = (2 * m + 1) * 85 / 128 - 42;
            bayer6[y][x] = (2 * m + 1) * 51 / 128 - 25;
            bayerG[y][x] = (2 * m + 1) * 10 / 128 - 5;
        }
    for (int v = 0; v < 256; v++)
        q6[v] = (uint8_t)((v * 5 + 127) / 255);
}

inline int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

//  The picture, scaled down to fit when it is larger than the block allows
//  (nearest neighbour, the same factor both ways), into buffer `out`.
//  Where each output column and row reads from, for the size last used:
//  the source pixel and the weight of the next one (0..255), so that a film
//  enlarged to a maximised window is interpolated, not blocks.
//  Up to a whole screen wide and tall, for the whole-screen picture below.
constexpr int SampleMax = 4096;
struct Sampler
{
    int outW = 0, outH = 0, srcW = 0, srcH = 0;
    uint16_t x0[SampleMax];
    uint8_t xw[SampleMax];
    uint16_t y0[SampleMax];
    uint8_t yw[SampleMax];
};
Sampler sampler;
Sampler chromaSampler; // the same over the half-size colour planes

void samplerFor(Sampler &s, int outW, int outH, int srcW, int srcH)
{
    if (s.outW == outW && s.outH == outH && s.srcW == srcW && s.srcH == srcH)
        return;
    s.outW = outW, s.outH = outH, s.srcW = srcW, s.srcH = srcH;
    //  Pixel centres: (o + 1/2) * src / out - 1/2, in 1/256ths.
    for (int o = 0; o < outW; o++)
    {
        int v = (int)(((2 * o + 1) * (int64_t)srcW * 256 / outW - 256) / 2);
        if (v < 0)
            v = 0;
        if (v > (srcW - 1) * 256)
            v = (srcW - 1) * 256;
        s.x0[o] = (uint16_t)(v >> 8);
        s.xw[o] = (uint8_t)(s.x0[o] + 1 < srcW ? v & 255 : 0);
    }
    for (int o = 0; o < outH; o++)
    {
        int v = (int)(((2 * o + 1) * (int64_t)srcH * 256 / outH - 256) / 2);
        if (v < 0)
            v = 0;
        if (v > (srcH - 1) * 256)
            v = (srcH - 1) * 256;
        s.y0[o] = (uint16_t)(v >> 8);
        s.yw[o] = (uint8_t)(s.y0[o] + 1 < srcH ? v & 255 : 0);
    }
}

//  The picture into palette indices at outW x outH.  Brightness and colour
//  are both interpolated between the four nearest samples of their planes:
//  colour from the nearest alone made its edges steps two source pixels wide,
//  which a film enlarged to a maximised window shows as blocks.  With 256
//  colours a pixel with hardly any colour goes to the 24 greys, ten apart, dithered by a tenth as
//  much as the cube's colours, whose steps are 51: a dark film is mostly such
//  pixels, and on the cube alone it was six levels of grey and loud dots.
//  "Hardly any" is a spread under 12 between the channels: at 24 a film's
//  colour grade --- a teal cast over a street, a warm one over a wall ---
//  was all turned grey, the average colour 4 levels off; at 12 it is 1.
void convert(const plm_frame_t *f, uint8_t *out, int outW, int outH)
{
    const int fw = (int)f->width, fh = (int)f->height;
    const int ys = (int)f->y.width, cs = (int)f->cr.width;
    samplerFor(sampler, outW, outH, fw, fh);
    samplerFor(chromaSampler, outW, outH, (fw + 1) >> 1, (fh + 1) >> 1);
    const Sampler &sm = sampler, &cs_ = chromaSampler;
    for (int oy = 0; oy < outH; oy++)
    {
        const int y = sm.y0[oy], wy = sm.yw[oy];
        const uint8_t *Y0 = f->y.data + y * ys;
        const uint8_t *Y1 = wy ? Y0 + ys : Y0;
        const int cy = cs_.y0[oy], cwy = cs_.yw[oy];
        const uint8_t *Cr0 = f->cr.data + cy * cs, *Cr1 = cwy ? Cr0 + cs : Cr0;
        const uint8_t *Cb0 = f->cb.data + cy * cs, *Cb1 = cwy ? Cb0 + cs : Cb0;
        const int *dither = cube ? bayer6[oy & 7] : bayer[oy & 7];
        const int *ditherG = bayerG[oy & 7];
        uint8_t *o = out + oy * outW;
        for (int ox = 0; ox < outW; ox++)
        {
            const int x = sm.x0[ox], wx = sm.xw[ox];
            int yv;
            if (wx | wy)
            {
                int top = Y0[x] * 256 + (Y0[x + (wx ? 1 : 0)] - Y0[x]) * wx;
                int bot = Y1[x] * 256 + (Y1[x + (wx ? 1 : 0)] - Y1[x]) * wx;
                yv = (top * 256 + (bot - top) * wy) >> 16;
            }
            else
                yv = Y0[x];
            const int cx = cs_.x0[ox], cwx = cs_.xw[ox], cx1 = cx + (cwx ? 1 : 0);
            int cr, cb;
            if (cwx | cwy)
            {
                int t = Cr0[cx] * 256 + (Cr0[cx1] - Cr0[cx]) * cwx, u = Cr1[cx] * 256 + (Cr1[cx1] - Cr1[cx]) * cwx;
                cr = ((t * 256 + (u - t) * cwy) >> 16) - 128;
                t = Cb0[cx] * 256 + (Cb0[cx1] - Cb0[cx]) * cwx, u = Cb1[cx] * 256 + (Cb1[cx1] - Cb1[cx]) * cwx;
                cb = ((t * 256 + (u - t) * cwy) >> 16) - 128;
            }
            else
                cr = Cr0[cx] - 128, cb = Cb0[cx] - 128;
            int luma = ((yv - 16) * 76309) >> 16;
            int r = luma + ((cr * 104597) >> 16);
            int g = luma - ((cb * 25674 + cr * 53278) >> 16);
            int b = luma + ((cb * 132201) >> 16);
            if (cube)
            {
                int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
                int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
                if (mx - mn < 12)
                {
                    int v = ((r * 77 + g * 150 + b * 29) >> 8) + ditherG[ox & 7];
                    o[ox] = v < 4 ? 16 : v > 247 ? 231 : (uint8_t)(232 + (v < 8 ? 0 : v > 238 ? 23 : (v - 3) / 10));
                    continue;
                }
                int d = dither[ox & 7];
                o[ox] = (uint8_t)(16 + 36 * q6[clamp255(r + d)] + 6 * q6[clamp255(g + d)] + q6[clamp255(b + d)]);
            }
            else
            {
                int d = dither[ox & 7];
                r = clamp255(r + d), g = clamp255(g + d), b = clamp255(b + d);
                o[ox] = nearest[((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)];
            }
        }
    }
}

// ── The whole screen ────────────────────────────────────────────────────────
//
// With F the window lends the player the framebuffer (vidhost.hpp): each
// picture then goes to the screen in true colour, 0x00RRGGBB, through syscall
// 0x17, with no palette and no dither --- what the film is, at up to the
// screen's own resolution rather than Memento's half of it.  0x17 takes a
// buffer the size of the screen as it is, or any other size and enlarges it
// to the screen (nearest pixel), so the picture is made in one of three ways:
//
//   NATIVE  a film between half the screen and the screen (720p on 1080p):
//           at its own size, one pixel for each of the film's, and the
//           kernel enlarges it.  Converting is then what pl_mpeg's own
//           plm_frame_to_rgb does --- a colour sample for each 2x2 block of
//           brightness --- and a few table lookups a pixel.
//   SCREEN  a film larger than the screen: scaled down to it here.
//   HALF    a film no larger than half the screen, or a SCREEN one the
//           machine cannot keep up with: to half the screen's resolution
//           each way, smoothly, and the kernel doubles it.
//
// Scaling here mixes four samples of each plane for every pixel, some 70
// instructions a pixel: at 1920x1080 that was 27 ms a picture on a fast
// laptop, more than the decoding --- which is why a film that fits is sent at
// its own size and left to the kernel to enlarge.  The buffer is the screen's
// shape in every case, with the film in the middle and black round it, as
// 0x17 stretches it to the whole screen.  Up to 8 MiB, from the user heap.

enum FsMode : uint8_t
{
    FS_NATIVE,
    FS_SCREEN,
    FS_HALF,
};

struct FullScreen
{
    uint32_t *px = nullptr; // fbW x fbH, the most any mode needs
    int fbW = 0, fbH = 0;
    FsMode mode = FS_NATIVE;
    bool slow = false;       // SCREEN did not keep up: HALF from now on
    int w = 0, h = 0;        // the buffer as sent
    int ox = 0, oy = 0, ow = 0, oh = 0; // the picture in it
    int shown = 0, late = 0; // at SCREEN, to tell whether it keeps up
} fs;

//  Brightness from Y, and 0..255 from what the colour adds to it, by table:
//  a third faster than the multiply and the three clamps (7 against 11.5 ms
//  for a 1080p picture on the laptop it was measured on).  lumT is offset by
//  ClOff so that brightness plus or minus any colour difference is an index.
constexpr int ClOff = 400;
int lumT[256];
uint8_t clT[1024];

void buildFsTables()
{
    for (int v = 0; v < 256; v++)
        lumT[v] = (((v - 16) * 76309) >> 16) + ClOff;
    for (int i = 0; i < 1024; i++)
        clT[i] = (uint8_t)clamp255(i - ClOff);
}

inline uint32_t rgb(int yv, int dr, int dg, int db)
{
    const int l = lumT[yv];
    return (uint32_t)clT[l + dr] << 16 | (uint32_t)clT[l - dg] << 8 | clT[l + db];
}

//  Where the picture goes in the buffer, and the rest black.
void fsLayout(int filmW, int filmH)
{
    //  The film's size on the screen, its shape kept.
    int fitW = fs.fbW, fitH = (int)((int64_t)filmH * fs.fbW / filmW);
    if (fitH > fs.fbH)
        fitH = fs.fbH, fitW = (int)((int64_t)filmW * fs.fbH / filmH);
    if (filmW > fitW)
        fs.mode = fs.slow ? FS_HALF : FS_SCREEN;
    else if (filmW * 2 <= fitW)
        fs.mode = FS_HALF;
    else
        fs.mode = FS_NATIVE;
    switch (fs.mode)
    {
    case FS_SCREEN:
        fs.w = fs.fbW, fs.h = fs.fbH, fs.ow = fitW, fs.oh = fitH;
        break;
    case FS_HALF:
        fs.w = fs.fbW / 2, fs.h = fs.fbH / 2, fs.ow = fitW / 2, fs.oh = fitH / 2;
        break;
    case FS_NATIVE:
        //  The screen's shape round the film.
        fs.ow = filmW, fs.oh = filmH;
        if ((int64_t)filmW * fs.fbH >= (int64_t)filmH * fs.fbW)
            fs.w = filmW, fs.h = (int)(((int64_t)filmW * fs.fbH + fs.fbW / 2) / fs.fbW);
        else
            fs.h = filmH, fs.w = (int)(((int64_t)filmH * fs.fbW + fs.fbH / 2) / fs.fbH);
        if (fs.h < filmH)
            fs.h = filmH;
        if (fs.w < filmW)
            fs.w = filmW;
        break;
    }
    fs.ox = (fs.w - fs.ow) / 2, fs.oy = (fs.h - fs.oh) / 2;
    memset(fs.px, 0, (size_t)fs.w * fs.h * 4);
    fs.shown = fs.late = 0;
    blk->fsScaled = fs.w != fs.fbW || fs.h != fs.fbH;
    blk->fsW = (uint16_t)fs.ow;
    blk->fsH = (uint16_t)fs.oh;
}

//  The framebuffer and the memory for it, the first time.
bool fsOpen(int filmW, int filmH)
{
    if (!fs.px)
    {
        static r2::FbInfo info;
        if (r2::raw_syscall(r2::Sys::GetFbInfo, (int64_t)&info, 0) != 0 || info.bpp != 32 || info.width < 64 ||
            info.height < 64 || info.width > SampleMax || info.height > SampleMax)
            return false;
        fs.px = (uint32_t *)r2::heap::kernel_allocate((size_t)info.width * info.height * 4);
        if (!fs.px)
            return false;
        fs.fbW = (int)info.width, fs.fbH = (int)info.height;
        buildFsTables();
    }
    fsLayout(filmW, filmH);
    return true;
}

//  NATIVE: one pixel for each of the film's.  Chroma is sited in the middle
//  of each 2x2 block of brightness in MPEG-1, so one colour for the four is
//  the film as coded, not a shortcut.
void fsNative(const plm_frame_t *f)
{
    const int fw = (int)f->width, fh = (int)f->height;
    const int ys = (int)f->y.width, cs = (int)f->cr.width;
    for (int y = 0; y < fh; y += 2)
    {
        const uint8_t *Y0 = f->y.data + y * ys, *Y1 = Y0 + ys;
        const uint8_t *Cr = f->cr.data + (y >> 1) * cs, *Cb = f->cb.data + (y >> 1) * cs;
        uint32_t *o0 = fs.px + (fs.oy + y) * fs.w + fs.ox, *o1 = o0 + fs.w;
        const bool two = y + 1 < fh;
        for (int x = 0; x < fw; x += 2)
        {
            const int cr = Cr[x >> 1] - 128, cb = Cb[x >> 1] - 128;
            const int dr = (cr * 104597) >> 16, dg = (cb * 25674 + cr * 53278) >> 16, db = (cb * 132201) >> 16;
            //  (The planes are whole macroblocks wide and tall, so x + 1 and
            //  the row under are there to read even at an odd edge.)
            const bool pair = x + 1 < fw;
            o0[x] = rgb(Y0[x], dr, dg, db);
            if (pair)
                o0[x + 1] = rgb(Y0[x + 1], dr, dg, db);
            if (two)
            {
                o1[x] = rgb(Y1[x], dr, dg, db);
                if (pair)
                    o1[x + 1] = rgb(Y1[x + 1], dr, dg, db);
            }
        }
    }
}

//  A plane's two source rows mixed for one output row, 8.8 fixed point.
uint16_t mixY[SampleMax], mixCb[SampleMax / 2 + 1], mixCr[SampleMax / 2 + 1];

//  SCREEN and HALF: scaled to fs.ow x fs.oh.  Separable: the two source rows
//  of each output row are mixed once, across the film's width, and each
//  output pixel then mixes two neighbours of that.
void fsScaled(const plm_frame_t *f)
{
    const int fw = (int)f->width, fh = (int)f->height;
    const int cw = (fw + 1) >> 1;
    const int ys = (int)f->y.width, cs = (int)f->cr.width;
    samplerFor(sampler, fs.ow, fs.oh, fw, fh);
    samplerFor(chromaSampler, fs.ow, fs.oh, cw, (fh + 1) >> 1);
    const Sampler &sm = sampler, &cm = chromaSampler;
    int lastY = -1, lastWy = -1, lastCy = -1, lastCwy = -1;
    for (int oy = 0; oy < fs.oh; oy++)
    {
        const int y = sm.y0[oy], wy = sm.yw[oy];
        if (y != lastY || wy != lastWy)
        {
            const uint8_t *a = f->y.data + y * ys, *b = wy ? a + ys : a;
            for (int x = 0; x < fw; x++)
                mixY[x] = (uint16_t)((a[x] << 8) + (b[x] - a[x]) * wy);
            mixY[fw] = mixY[fw - 1];
            lastY = y, lastWy = wy;
        }
        const int cy = cm.y0[oy], cwy = cm.yw[oy];
        if (cy != lastCy || cwy != lastCwy)
        {
            const uint8_t *a = f->cr.data + cy * cs, *b = cwy ? a + cs : a;
            const uint8_t *c = f->cb.data + cy * cs, *d = cwy ? c + cs : c;
            for (int x = 0; x < cw; x++)
            {
                mixCr[x] = (uint16_t)((a[x] << 8) + (b[x] - a[x]) * cwy);
                mixCb[x] = (uint16_t)((c[x] << 8) + (d[x] - c[x]) * cwy);
            }
            mixCr[cw] = mixCr[cw - 1], mixCb[cw] = mixCb[cw - 1];
            lastCy = cy, lastCwy = cwy;
        }
        uint32_t *o = fs.px + (fs.oy + oy) * fs.w + fs.ox;
        for (int ox = 0; ox < fs.ow; ox++)
        {
            const int x = sm.x0[ox], wx = sm.xw[ox];
            const int yv = ((int)mixY[x] * 256 + ((int)mixY[x + 1] - (int)mixY[x]) * wx) >> 16;
            const int cx = cm.x0[ox], cwx = cm.xw[ox];
            const int cr = (((int)mixCr[cx] * 256 + ((int)mixCr[cx + 1] - (int)mixCr[cx]) * cwx) >> 16) - 128;
            const int cb = (((int)mixCb[cx] * 256 + ((int)mixCb[cx + 1] - (int)mixCb[cx]) * cwx) >> 16) - 128;
            o[ox] = rgb(yv, (cr * 104597) >> 16, (cb * 25674 + cr * 53278) >> 16, (cb * 132201) >> 16);
        }
    }
}

void fsConvert(const plm_frame_t *f)
{
    if (fs.mode == FS_NATIVE)
        fsNative(f);
    else
        fsScaled(f);
}

//  To the screen: as it is, or for the kernel to enlarge.
void fsSend()
{
    const bool whole = fs.w == fs.fbW && fs.h == fs.fbH;
    r2::raw_syscall(r2::Sys::BlitBuffer, (int64_t)fs.px, whole ? 0 : ((int64_t)fs.w << 16) | fs.h);
}

//  Keeping count at SCREEN: more than one picture in ten late, over fifty,
//  and it goes to HALF for good.
void fsCount(bool wasLate, const plm_frame_t *f)
{
    if (fs.mode != FS_SCREEN)
        return;
    (wasLate ? fs.late : fs.shown)++;
    if (fs.shown + fs.late < 50)
        return;
    if (fs.late * 10 > fs.shown + fs.late)
    {
        fs.slow = true;
        fsLayout((int)f->width, (int)f->height);
    }
    fs.shown = fs.late = 0;
}

// ── Keeping in touch ────────────────────────────────────────────────────────

uint32_t heardBeat = 0;
uint64_t heardAt = 0;

//  False once the host has asked us to go, or has not been heard from for ten
//  seconds (it died, or took the window down without asking).
bool hostStillThere()
{
    blk->playerBeat = blk->playerBeat + 1;
    if (blk->quit)
        return false;
    uint64_t now = r2::ticks();
    if (blk->hostBeat != heardBeat)
    {
        heardBeat = blk->hostBeat;
        heardAt = now;
    }
    return now - heardAt < 10000;
}

// ── Between the window and the whole screen ─────────────────────────────────

uint8_t back = 0;              // the buffer of pixels[] the next picture goes in
int filmW = 0, filmH = 0;      // the film's own size
int ownW = 0, ownH = 0;        // the player's size for it in pixels[] (at most MaxW x MaxH)
const plm_frame_t *lastFrame = nullptr; // the last decoded: it stays until the next decode

//  The picture for the window: at its size, in its buffer, published.
void publish(const plm_frame_t *f)
{
    while (blk->reading == back && hostStillThere())
        r2::sleep(1);
    int tw = ownW, th = ownH;
    if (cube)
    {
        int ww = blk->wantW, wh = blk->wantH;
        if (ww >= 16 && wh >= 16 && ww <= VidHostBlock::OutMaxW && wh <= VidHostBlock::OutMaxH)
            tw = ww, th = wh;
    }
    convert(f, blk->pixels[back], tw, th);
    blk->bufW[back] = (uint16_t)tw;
    blk->bufH[back] = (uint16_t)th;
    __asm__ volatile("" ::: "memory"); // the pixels are all in before the flip says so
    blk->front = back;
    blk->frame = blk->frame + 1;
    back ^= 1;
}

//  What the window asks for, between pictures: the whole screen, or back.
//  Going there, the picture on hand goes out at once (a paused film is not
//  left on Memento's last frame); coming back, it goes to the window, whose
//  own copy was not kept up meanwhile.  fsShowing changes only here, never
//  while a picture is on its way to the screen.
void fsSync()
{
    bool want = blk->fullscreen && cube && !blk->fsRefused;
    if (want == (blk->fsShowing != 0))
        return;
    if (want)
    {
        if (!fsOpen(filmW, filmH))
        {
            blk->fsRefused = 1;
            return;
        }
        blk->fsShowing = 1;
        if (lastFrame)
        {
            fsConvert(lastFrame);
            fsSend();
        }
        return;
    }
    blk->fsShowing = 0;
    if (lastFrame)
        publish(lastFrame);
}

} // namespace

static void outOfMemory()
{
    if (blk)
        fail("Out of memory: make the video 320x240 or smaller.");
    leave(1);
}

int main()
{
    blk = hostFromArgs();
    if (!blk)
    {
        //  Nothing to draw into, and no screen of its own: say how it is used.
        static const char usage[] = "mpegplay: started by memento's Video window, as\n"
                                    "  mpegplay.elf --host <block> <file.mpg>\n";
        r2::raw_syscall(r2::Sys::PrintString, (int64_t)usage, 0);
        return 1;
    }
    heardBeat = blk->hostBeat;
    heardAt = r2::ticks();
    blk->reading = VidHostBlock::NONE;
    blk->state = VidHostBlock::LOADING;

    r2::string_view pathArg = r2::arg(3);
    plm_video_t *video;
    bool program = false;
    if (pathArg == r2::string_view("--stream"))
    {
        if (!blk->streamRing || !blk->streamSize)
            fail("No stream to read.");
        tsStart();
        videoBuffer = plm_buffer_create_with_capacity(PLM_BUFFER_DEFAULT_SIZE);
        plm_buffer_set_load_callback(videoBuffer, tsVideoCallback, nullptr);
        if (tsAudioPid >= 0)
        {
            audioBuffer = plm_buffer_create_with_capacity(PLM_BUFFER_DEFAULT_SIZE);
            plm_buffer_set_load_callback(audioBuffer, tsAudioCallback, nullptr);
        }
        video = plm_video_create_with_buffer(videoBuffer, TRUE);
        blk->durationMs = 0;
        goto haveVideo;
    }
    {
    static char path[128];
    if (pathArg.size() >= sizeof(path))
        fail("That path is too long.");
    memcpy(path, pathArg.data(), pathArg.size());
    path[pathArg.size()] = 0;
    source.path = path;

    //  What kind of file: a pack header starts a program stream, a sequence
    //  header a bare video stream.
    static uint8_t head[8192];
    int64_t got = r2::fs::read_at(path, r2::byte_span(head, sizeof(head)), 0);
    if (got < 12)
        fail("Cannot read the file.");
    program = head[0] == 0 && head[1] == 0 && head[2] == 1 && head[3] == 0xBA;
    bool elementary = head[0] == 0 && head[1] == 0 && head[2] == 1 && head[3] == 0xB3;
    if (!program && !elementary)
        fail("Not an MPEG-1 file (see README: ffmpeg -f mpeg).");
    //  MPEG-2 shares the start codes, and pl_mpeg would turn it into noise
    //  rather than refuse it.  Its pack header begins with the bits 01 where
    //  MPEG-1's begins 0010, and its video has a sequence extension (B5).
    bool mpeg2 = program && (head[4] & 0xC0) == 0x40;
    for (int64_t i = 0; !mpeg2 && i + 3 < got; i++)
        mpeg2 = head[i] == 0 && head[i + 1] == 0 && head[i + 2] == 1 && head[i + 3] == 0xB5;
    if (mpeg2)
        fail("This is MPEG-2; only MPEG-1 plays (-c:v mpeg1video).");

    plm_buffer_t *file = plm_buffer_create_with_callbacks(loadCallback, seekCallback, tellCallback, fileSize(path), &source);
    file->discard_read_bytes = TRUE; // as a file buffer of pl_mpeg's own does

    if (program)
    {
        demux = plm_demux_create(file, TRUE);
        if (!plm_demux_has_headers(demux) || plm_demux_get_num_video_streams(demux) < 1)
            fail("The file has no MPEG-1 video in it.");
        int64_t duration = plm_demux_get_duration(demux, PLM_DEMUX_PACKET_VIDEO_1);
        blk->durationMs = duration > 0 ? (uint32_t)(duration / 90) : 0;
        videoBuffer = plm_buffer_create_with_capacity(PLM_BUFFER_DEFAULT_SIZE);
        plm_buffer_set_load_callback(videoBuffer, videoPacketCallback, nullptr);
        if (plm_demux_get_num_audio_streams(demux) > 0)
        {
            audioBuffer = plm_buffer_create_with_capacity(PLM_BUFFER_DEFAULT_SIZE);
            plm_buffer_set_load_callback(audioBuffer, audioPacketCallback, nullptr);
        }
        video = plm_video_create_with_buffer(videoBuffer, TRUE);
    }
    else
        video = plm_video_create_with_buffer(file, TRUE);
    }

haveVideo:
    if (!plm_video_has_header(video))
        fail("No picture header: is it MPEG-1 (not MPEG-2 or 4)?");
    if (tsMpeg2)
        fail("The stream is MPEG-2 video: only MPEG-1 plays (-c:v mpeg1video).");

    int w = plm_video_get_width(video), h = plm_video_get_height(video);
    filmW = w, filmH = h;
    int outW = w, outH = h;
    if (outW > VidHostBlock::MaxW || outH > VidHostBlock::MaxH)
    {
        //  One factor for both, so the picture keeps its shape.
        if (w * VidHostBlock::MaxH > h * VidHostBlock::MaxW)
            outW = VidHostBlock::MaxW, outH = h * VidHostBlock::MaxW / w;
        else
            outH = VidHostBlock::MaxH, outW = w * VidHostBlock::MaxH / h;
    }
    int rate = plm_video_get_framerate_milli(video);
    if (rate <= 0)
        rate = 25000;
    blk->width = (uint16_t)outW;
    blk->height = (uint16_t)outH;
    ownW = outW, ownH = outH;
    blk->rateMilli = (uint32_t)rate;
    buildColourTables();
    cube = blk->colours == 256;

    //  Pictures are shown on the clock, not as fast as they decode: the n-th
    //  is due n / rate after the start.  One that is ready too late is decoded
    //  (the next ones are built on it) but not shown, unless nothing has been
    //  for a quarter of a second.
    startSound();
    uint64_t start = r2::ticks();
    uint64_t pausedAt = 0;
    uint64_t lastShown = 0;
    uint32_t restartSeen = blk->restart;
    int64_t n = 0;
    blk->state = VidHostBlock::PLAYING;

    while (hostStillThere())
    {
        if (blk->restart != restartSeen && !demux && !program && blk->streamRing)
            restartSeen = blk->restart; // a stream cannot go back to its start
        if (blk->restart != restartSeen)
        {
            restartSeen = blk->restart;
            if (demux)
                plm_demux_rewind(demux);
            plm_video_rewind(video);
            if (audioBuffer)
            {
                plm_buffer_rewind(audioBuffer);
                sound.mp2->rewind();
                firstVideoPts = firstAudioPts = PLM_PACKET_INVALID_TS;
            }
            startSound(); // a new stream: what was queued goes
            n = 0;
            start = r2::ticks();
            pausedAt = 0;
            blk->dropped = 0;
            blk->state = blk->paused ? VidHostBlock::PAUSED : VidHostBlock::PLAYING;
        }
        fsSync();
        if (blk->state == VidHostBlock::ENDED)
        {
            r2::sleep(50);
            continue;
        }
        if (blk->paused)
        {
            if (!pausedAt)
            {
                pausedAt = r2::ticks();
                if (sound.on)
                    r2::audio::pause(); // the sound's clock stands still too
            }
            blk->state = VidHostBlock::PAUSED;
            r2::sleep(20);
            continue;
        }
        if (pausedAt)
        {
            start += r2::ticks() - pausedAt; // the clock stood still meanwhile
            pausedAt = 0;
            if (sound.on)
                r2::audio::resume();
            blk->state = VidHostBlock::PLAYING;
        }
        feed();

        uint64_t decodeFrom = r2::ticks(), waitedBefore = tsWaitedMs;
        plm_frame_t *frame = plm_video_decode(video);
        {
            uint64_t spent = r2::ticks() - decodeFrom - (tsWaitedMs - waitedBefore);
            if (spent > blk->decodeMaxMs && spent < 100000)
                blk->decodeMaxMs = (uint32_t)spent;
        }
        if (!frame)
        {
            blk->state = VidHostBlock::ENDED;
            continue;
        }
        lastFrame = frame;
        uint64_t due = start + (uint64_t)(n * 1000000 / rate);
        n++;
        uint64_t now = r2::ticks();
        const int64_t frameMs = 1000000 / rate;
        //  How far behind a picture may still be shown.  An I-picture takes
        //  several times as long to decode as the P-pictures around it, and
        //  with a slack of one frame each one came out late and was skipped
        //  --- a hitch every GOP.  Up to a tenth of a second behind the sound
        //  cannot be seen; the cheap pictures after it catch up.
        const int64_t slack = frameMs * 3 > LATE_MS ? frameMs * 3 : LATE_MS;
        bool late = sound.on ? soundMs() > frame->time + slack : now > due + slack;
        if (late)
        {
            int64_t behind = sound.on ? soundMs() - frame->time : (int64_t)(now - due);
            if (behind > (int64_t)blk->lateMaxMs)
                blk->lateMaxMs = (uint32_t)behind;
        }
        const bool full = blk->fsShowing;
        if (full)
            fsCount(late, frame); // may go to half size, before this one is made
        if (late && now - lastShown < 250 && lastShown)
        {
            blk->dropped = blk->dropped + 1;
            continue;
        }

        //  Made now, shown when it is due: on the whole screen, or into the
        //  buffer the window is not reading, at the size the window shows it
        //  (with 256 colours; otherwise, and until it says, the player's own).
        int tw = outW, th = outH;
        if (full)
            fsConvert(frame);
        else
        {
            while (blk->reading == back && hostStillThere())
                r2::sleep(1);
            if (cube)
            {
                int ww = blk->wantW, wh = blk->wantH;
                if (ww >= 16 && wh >= 16 && ww <= VidHostBlock::OutMaxW && wh <= VidHostBlock::OutMaxH)
                    tw = ww, th = wh;
            }
            convert(frame, blk->pixels[back], tw, th);
            blk->bufW[back] = (uint16_t)tw;
            blk->bufH[back] = (uint16_t)th;
        }

        if (sound.on)
        {
            //  On the sound's clock, and fed while waiting for it.
            while (sound.on && soundMs() < frame->time && !blk->paused && hostStillThere())
            {
                feed();
                r2::sleep(2);
            }
            if (!sound.on) // the audio ran out: the ticks take over from here
                start = r2::ticks() - (uint64_t)(n * 1000000 / rate);
        }
        else
        {
            now = r2::ticks();
            if (due > now)
                r2::sleep(due - now);
        }
        if (full)
            fsSend();
        else
        {
            __asm__ volatile("" ::: "memory"); // the pixels are all in before the flip says so
            blk->front = back;
            back ^= 1;
        }
        blk->positionMs = (uint32_t)frame->time;
        blk->frame = blk->frame + 1;
        lastShown = r2::ticks();
    }
    leave(0);
}
