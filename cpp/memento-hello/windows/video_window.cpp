//
// Window — Video: MPEG-1 films from /mnt/tar/video
//
// The decoding is not in this process.  It is cpp/mpegplay (mpegplay.elf in
// /mnt/tar/bin), pl_mpeg without its floating point, started with the address
// of a VidHostBlock this window allocates on the user heap.  The player puts
// each picture in the block, already in the 16 EGA colours, and this window
// copies it into its own bitmap --- the r2 backend's bitmaps are one palette
// index a pixel, so that is a row-by-row copy, not 76 800 FillRects.  The
// same arrangement as the Editor window and Turbo C++; the block's layout and
// the rules for reading it are in cpp/mpegplay/vidhost.hpp.
//
// Pictures are shown a window pixel to a picture pixel: 320x240 fills the
// picture area, and a smaller film sits in the middle of it.
//
// Keys:  Up/Down, Enter  pick and play       Space  pause
//        R  from the start                   Esc / Backspace  back to the list
//        U  type (or Ctrl+V paste) the address of a stream
//        Up/Down while playing  volume, in tens of percent (0--200)
//        F  the whole screen, in true colour (graphics kernel); F or Esc back
//
// The whole screen: Memento's own frames are palette indices at half the
// screen's resolution (a 1920x1080 screen is a 960x540 desktop), which is
// what a film in the window is dithered to.  With F the window holds
// Memento's screen (R2_HoldScreen) and the player draws on the framebuffer
// itself, in 32-bit colour at the screen's full resolution --- the film as it
// was made.  Leaving, the window waits for the player to say it has stopped
// drawing before Memento has the screen again; see vidhost.hpp.  Losing the
// focus (Alt+Tab, a click on another window) leaves as well, since keys stop
// coming here.
//
// Sound: the player decodes a film's MP2 audio and plays it through the HD
// Audio driver, and keeps the pictures to it; the status line says "sound"
// and the volume (or that there is no sound device).  The volume is the
// player's: the kernel has none, so it scales the samples before queueing
// them --- a change is heard once the 350 ms already queued have played.
// It stays for the next film.
//
// Streams: HLS.  An .m3u8 in the directory, a .url file holding an address,
// or an address typed with U.  This window fetches the playlist --- a master
// playlist's richest variant up to 6 Mbit/s (the leanest with 16 colours);
// a live one again every target
// duration, from three segments before its end --- and the MPEG-TS segments,
// with the browser's loader (HTTP and HTTPS), or from the disk for a playlist
// that is a file, and hands the bytes to the player (mpegplay.elf --stream)
// through a ring on the user heap.  Only MPEG-1 video with MP2 audio plays:
//     ffmpeg ... -c:v mpeg1video -c:a mp2 -f hls -hls_time 4 live.m3u8
//

#include "../../mpegplay/vidhost.hpp"
#include "ui/platform/impl/r2/R2_BitmapImpl.h"

#define VIDEO_DIR "/mnt/tar/video"

class VideoWindow
{
public:
    static void onEvent(void *instance, struct PlatformWindowInterfaceInputEvent *data)
    {
        reinterpret_cast<VideoWindow *>(instance)->onEvent_(data);
    }

    void SetWindow(PlatformWindow *w) { wnd = w; }

    ~VideoWindow()
    {
        if (loader)
        {
            loader->cancel();
            delete loader;
        }
        if (!blk)
            return;
        //  Taken down with the player still running: it is told to go, and
        //  given a moment to let go of the block before the block goes.
        blk->quit = 1;
        for (int i = 0; i < 100 && !blk->exited && playerAlive(); i++)
        {
            blk->hostBeat = blk->hostBeat + 1;
            r2::sleep(20);
        }
        if (blk->exited || !playerAlive())
            release();
        else
            forget();
        fsLetGo();
    }

    //  The picture area is 320x240 pixels, which at the window's two pixels a
    //  unit is 160x120 units; a status line goes under it.
    static const int PIC_W = VidHostBlock::MaxW / 2, PIC_H = VidHostBlock::MaxH / 2;
    static const int W = PIC_W + 4, H = PIC_H + 16;

private:
    PlatformWindow *wnd = nullptr;
    PlatformColor *dark = nullptr, *light = nullptr, *black = nullptr;
    PlatformFont *font = nullptr;

    // ── The list ──────────────────────────────────────────────────────────────
    static const int ROW_H = 10, MAX_ROWS = 11;
    VfsDirEntry_T entries[64];
    int nFiles = 0, sel = 0, top = 0;
    bool listed = false;

    // ── The player ────────────────────────────────────────────────────────────
    VidHostBlock *blk = nullptr;
    uint8_t pid = 0;
    uint64_t startedAt = 0, lastCheck = 0, stopAsked = 0;
    uint32_t shownFrame = 0;
    //  The whole screen: held from F until the player has let go of it.
    bool fsHeld = false;
    uint64_t fsLeaving = 0; // when leaving was asked, 0 while not
    char title[40] = {};
    char error[80] = {};

    // ── A stream ──────────────────────────────────────────────────────────────
    static const uint32_t STREAM_RING = 1024 * 1024;
    //  The richest HLS variant taken, bits a second: 6 Mbit/s is 3 MiB in a
    //  4-second segment, under the loader's 4 MiB.
    static const long MAX_VARIANT_BW = 6000000;
    static const int MAX_QUEUE = 8;
    static const int URL_CAP = 512;
    bool typing = false;             // U: an address being typed
    char typed[URL_CAP] = {};
    bool streaming = false;
    uint8_t *ring = nullptr;
    char playlist[URL_CAP] = {};     // the media playlist: an address, or a path
    bool local = false;              // a path: segments come from the disk
    web::Loader *loader = nullptr;
    enum Fetch
    {
        F_NONE,
        F_PLAYLIST,
        F_SEGMENT
    } fetching = F_NONE;
    char queue[MAX_QUEUE][URL_CAP];
    int nQueue = 0;
    int64_t nextSeq = -1;
    uint32_t targetMs = 6000;
    bool endList = false;
    int hops = 0;                    // master playlist to variant
    int failures = 0;
    uint64_t nextPlaylistAt = 0;

    //  The diagnostics line (I while playing): what each stage did over the
    //  last two seconds, for finding which one a stuttering film waits on.
    bool info = false;
    //  Percent, for every film this window plays; above 100 is louder than
    //  decoded, and clips where the film is loud already.
    int volume = 100;
    static const int VOLUME_MAX = 200, VOLUME_STEP = 10;
    char infoLine[96] = {};
    uint64_t infoAt = 0;
    uint64_t segmentFrom = 0; // when the segment being fetched was asked for
    uint32_t segmentKBs = 0;  // how fast the last one came, KiB a second
    web::Buf pending{true};          // a segment on its way into the ring
    size_t pendingOff = 0;

    //  Appends as much of s as fits.
    static void cat(char *d, size_t cap, const char *s)
    {
        size_t at = strlen(d);
        while (*s && at + 1 < cap)
            d[at++] = *s++;
        d[at] = 0;
    }

    static bool isVideo(const VfsDirEntry_T &e)
    {
        if (e.is_dir)
            return false;
        int n = e.name_len < 32 ? e.name_len : 32;
        auto ends = [&](const char *ext) {
            int l = (int)strlen(ext);
            if (n <= l)
                return false;
            for (int i = 0; i < l; i++)
                if ((e.name[n - l + i] | 0x20) != ext[i])
                    return false;
            return true;
        };
        return ends(".mpg") || ends(".mpeg") || ends(".m1v") || ends(".m3u8") || ends(".url");
    }

    static bool endsWith(const char *s, const char *ext)
    {
        size_t n = strlen(s), l = strlen(ext);
        if (n < l)
            return false;
        for (size_t i = 0; i < l; i++)
            if ((s[n - l + i] | 0x20) != ext[i])
                return false;
        return true;
    }

    void refreshListing()
    {
        int raw = (int)list_dir_path((const unsigned char *)VIDEO_DIR, entries);
        if (raw < 0 || raw > 64)
            raw = 0;
        nFiles = 0;
        for (int i = 0; i < raw; i++)
            if (isVideo(entries[i]))
                entries[nFiles++] = entries[i];
        if (sel >= nFiles)
            sel = nFiles ? nFiles - 1 : 0;
        listed = true;
    }

    static void nameOf(const VfsDirEntry_T &e, char *out, int cap)
    {
        int n = e.name_len < 32 ? e.name_len : 32;
        if (n > cap - 1)
            n = cap - 1;
        for (int i = 0; i < n; i++)
            out[i] = (char)e.name[i];
        out[n] = 0;
    }

    // ── Starting and stopping ─────────────────────────────────────────────────

    void play(int index)
    {
        if (index < 0 || index >= nFiles || blk)
            return;
        error[0] = 0;
        nameOf(entries[index], title, sizeof(title));
        if (endsWith(title, ".m3u8") || endsWith(title, ".url"))
        {
            char path[URL_CAP] = VIDEO_DIR "/";
            cat(path, sizeof(path), title);
            if (endsWith(title, ".m3u8"))
            {
                startStream(path);
                return;
            }
            //  A .url file: the address on its first line.
            char url[URL_CAP] = {};
            int64_t got = r2::fs::read_at(path, r2::byte_span((uint8_t *)url, sizeof(url) - 1), 0);
            if (got <= 0)
            {
                strcpy(error, "Cannot read that file.");
                return;
            }
            url[got] = 0;
            for (char *p = url; *p; p++)
                if (*p == '\r' || *p == '\n' || *p == ' ')
                {
                    *p = 0;
                    break;
                }
            startStream(url);
            return;
        }

        //  "mpegplay.elf --host 0xC1A2B0 /mnt/tar/video/NAME.MPG"
        char args[128] = "mpegplay.elf --host 0x";
        size_t at = strlen(args);
        if (at + 8 + 1 + sizeof(VIDEO_DIR) + strlen(title) + 1 > sizeof(args) || strchr(title, ' '))
        {
            strcpy(error, "That file name cannot be passed to the player.");
            return;
        }
        //  The kernel heap hands blocks back zeroed.
        blk = (VidHostBlock *)r2::heap::kernel_allocate(sizeof(VidHostBlock));
        if (!blk)
        {
            strcpy(error, "No memory for the picture (it takes 1.2 MiB).");
            return;
        }
        blk->magic = VidHostBlock::Magic;
        blk->version = VidHostBlock::Version;
        blk->reading = VidHostBlock::NONE;
        blk->hostBeat = 1;
        blk->colours = (uint16_t)MementoR2Impl::R2_Palette::Count();
        blk->volume = (uint8_t)volume;

        uint64_t addr = (uint64_t)(uintptr_t)blk;
        for (int shift = 28; shift >= 0; shift -= 4)
            args[at++] = "0123456789ABCDEF"[(addr >> shift) & 15];
        args[at] = 0;
        cat(args, sizeof(args), " " VIDEO_DIR "/");
        cat(args, sizeof(args), title);

        r2::optional<uint8_t> id = r2::spawn("mpegplay.elf", args);
        if (!id)
        {
            strcpy(error, "Could not start mpegplay.elf (is it in /mnt/tar/bin?)");
            release();
            return;
        }
        pid = *id;
        startedAt = r2::ticks();
        shownFrame = 0;
        stopAsked = 0;
        wnd->SetImmediateMode(true);
        wnd->Repaint();
    }

    void stop()
    {
        if (!blk)
            return;
        blk->quit = 1;
        if (!stopAsked)
            stopAsked = r2::ticks();
    }

    //  The player is gone (or will not go): back to the list.
    void finished()
    {
        if (blk && blk->state == VidHostBlock::FAILED && !error[0])
        {
            //  What it said, kept for the list to show.
            size_t i = 0;
            for (; i < sizeof(blk->message) && blk->message[i] && i < sizeof(error) - 1; i++)
                error[i] = blk->message[i];
            error[i] = 0;
        }
        if (blk && (blk->exited || !playerAlive()))
            release();
        else
            forget();
        fsLetGo(); // the player is gone, or has been given up on
        wnd->SetImmediateMode(false);
        wnd->Repaint();
    }

    void forget()
    {
        blk = nullptr;
        ring = nullptr; // still the player's to read: left to it
        endStream();
    }

    void release()
    {
        VidHostBlock *b = blk;
        blk = nullptr;
        r2::heap::kernel_deallocate(b);
        if (ring)
            r2::heap::kernel_deallocate(ring);
        ring = nullptr;
        endStream();
    }

    // ── The whole screen ──────────────────────────────────────────────────────

    void fsEnter()
    {
        if (!blk || fsHeld || blk->colours != 256)
        {
            if (blk && blk->colours != 256)
                strcpy(error, "The whole screen needs the graphics kernel.");
            return;
        }
        if (!MementoR2Impl::R2_HoldScreen(true))
            return;
        fsHeld = true;
        fsLeaving = 0;
        error[0] = 0;
        blk->fullscreen = 1;
    }

    void fsLeave()
    {
        if (!fsHeld || fsLeaving)
            return;
        blk->fullscreen = 0;
        fsLeaving = r2::ticks();
    }

    //  Memento's screen again, whatever the player is doing.
    void fsLetGo()
    {
        if (!fsHeld)
            return;
        fsHeld = false;
        fsLeaving = 0;
        if (blk)
            blk->fullscreen = 0;
        MementoR2Impl::R2_HoldScreen(false);
        wnd->Repaint();
    }

    //  From the idle loop.
    void fsStep(uint64_t now)
    {
        if (!fsHeld)
            return;
        if (blk->fsRefused)
        {
            //  It could not: nothing of it is on the screen.
            strcpy(error, "No whole screen: it needs a 32-bit framebuffer and 4 bytes of memory a pixel.");
            blk->fsRefused = 0;
            fsLetGo();
            return;
        }
        if (!fsLeaving && !MementoR2Impl::R2_HolderFocused())
            fsLeave();
        //  Back once the player has stopped drawing; one that does not say so
        //  within two seconds is not waited for.
        if (fsLeaving && (!blk->fsShowing || now - fsLeaving > 2000))
            fsLetGo();
    }

    // ── A stream ──────────────────────────────────────────────────────────────

    void endStream()
    {
        streaming = false;
        if (loader)
            loader->cancel();
        fetching = F_NONE;
        nQueue = 0;
        pending.release();
    }

    //  `src`: an http(s) address, or the path of a playlist on a disk.
    void startStream(const char *src)
    {
        if (blk)
            return;
        error[0] = 0;
        local = src[0] == '/';
        web::Url u;
        if (!local && !web::urlFromInput(src, u))
        {
            strcpy(error, "That is not an address (http:// or https://).");
            return;
        }
        if (!local)
            web::urlFormat(u, playlist, sizeof(playlist));
        else
            web::scopy(playlist, src, sizeof(playlist));
        const char *base = strrchr(src, '/');
        web::scopy(title, base && base[1] ? base + 1 : src, sizeof(title));

        blk = (VidHostBlock *)r2::heap::kernel_allocate(sizeof(VidHostBlock));
        ring = blk ? (uint8_t *)r2::heap::kernel_allocate(STREAM_RING) : nullptr;
        if (!blk || !ring)
        {
            strcpy(error, "No memory for a stream (it takes 2.2 MiB).");
            if (blk)
                r2::heap::kernel_deallocate(blk);
            blk = nullptr;
            ring = nullptr;
            return;
        }
        if (!loader && !local)
            loader = new web::Loader(web::r2Net(), web::gatherEntropy, web::currentTime);
        blk->magic = VidHostBlock::Magic;
        blk->version = VidHostBlock::Version;
        blk->reading = VidHostBlock::NONE;
        blk->hostBeat = 1;
        blk->colours = (uint16_t)MementoR2Impl::R2_Palette::Count();
        blk->volume = (uint8_t)volume;
        blk->streamRing = (uint64_t)(uintptr_t)ring;
        blk->streamSize = STREAM_RING;

        streaming = true;
        nQueue = 0;
        nextSeq = -1;
        endList = false;
        hops = 0;
        failures = 0;
        fetching = F_NONE;
        pending.release();
        fetchPlaylist();

        char args[64] = "mpegplay.elf --host 0x";
        size_t at = strlen(args);
        uint64_t addr = (uint64_t)(uintptr_t)blk;
        for (int shift = 28; shift >= 0; shift -= 4)
            args[at++] = "0123456789ABCDEF"[(addr >> shift) & 15];
        args[at] = 0;
        cat(args, sizeof(args), " --stream");
        r2::optional<uint8_t> id = r2::spawn("mpegplay.elf", args);
        if (!id)
        {
            strcpy(error, "Could not start mpegplay.elf (is it in /mnt/tar/bin?)");
            release();
            return;
        }
        pid = *id;
        startedAt = r2::ticks();
        shownFrame = 0;
        stopAsked = 0;
        wnd->SetImmediateMode(true);
        wnd->Repaint();
    }

    //  `ref` made whole against the playlist it came from.
    bool resolve(const char *ref, char *out, size_t cap)
    {
        if (local)
        {
            if (ref[0] == '/')
                web::scopy(out, ref, cap);
            else
            {
                web::scopy(out, playlist, cap);
                char *slash = strrchr(out, '/');
                if (slash)
                    slash[1] = 0;
                web::scat(out, ref, cap);
            }
            return true;
        }
        web::Url base, u;
        if (!web::urlFromInput(playlist, base) || !web::urlResolve(base, ref, u))
            return false;
        web::urlFormat(u, out, cap);
        return true;
    }

    //  A file from the disk into `pending`, as a download would have left it.
    bool readLocal(const char *path)
    {
        pending.release();
        pending.big = true;
        auto size = r2::fs::size_of(path);
        if (!size || !*size || !pending.reserve(*size))
            return false;
        int64_t got = r2::fs::read_at(path, r2::byte_span(pending.data, *size), 0);
        if (got <= 0)
            return false;
        pending.len = (size_t)got;
        pendingOff = 0;
        return true;
    }

    void fetchPlaylist()
    {
        if (local)
        {
            if (readLocal(playlist))
            {
                parsePlaylist((const char *)pending.cstr(), pending.len);
                pending.release();
            }
            else
                streamFailed("Cannot read the playlist.");
            return;
        }
        web::Url u;
        if (!web::urlFromInput(playlist, u))
            return streamFailed("The playlist's address is not one this can fetch.");
        fetching = F_PLAYLIST;
        loader->start(u, false);
    }

    void fetchSegment(const char *where)
    {
        if (local)
        {
            if (!readLocal(where))
                pending.release(); // a segment that is not there is skipped
            return;
        }
        web::Url u;
        if (!web::urlFromInput(where, u))
            return;
        fetching = F_SEGMENT;
        segmentFrom = r2::ticks();
        loader->start(u, false);
    }

    void streamFailed(const char *why)
    {
        web::scopy(error, why, sizeof(error));
        if (blk)
            blk->streamEnded = 1;
        endList = true;
        nQueue = 0;
    }

    static long number(const char *p)
    {
        long v = 0;
        while (*p >= '0' && *p <= '9')
            v = v * 10 + (*p++ - '0');
        return v;
    }

    //  A playlist: a master one names variants (which BANDWIDTH is taken:
    //  see below), a media one names segments.
    void parsePlaylist(const char *text, size_t len)
    {
        if (len < 7 || memcmp(text, "#EXTM3U", 7) != 0)
            return streamFailed("That is not an HLS playlist (#EXTM3U).");
        long bestBw = -1;
        char best[URL_CAP] = {};
        long pendingBw = -1;
        long mediaSeq = 0;
        int count = 0;
        //  The segments' names, for as long as the parse takes: on the heap,
        //  not in the image, which has no 32 KiB to spare.
        typedef char Uri[URL_CAP];
        Uri *uris = (Uri *)web::big_alloc(sizeof(Uri) * 64);
        if (!uris)
            return streamFailed("No memory to read the playlist.");
        bool isMaster = false;
        const char *p = text, *end = text + len;
        while (p < end)
        {
            const char *e = p;
            while (e < end && *e != '\n')
                e++;
            char line[URL_CAP];
            size_t n = (size_t)(e - p);
            while (n && (p[n - 1] == '\r' || p[n - 1] == ' '))
                n--;
            if (n >= sizeof(line))
                n = sizeof(line) - 1;
            memcpy(line, p, n);
            line[n] = 0;
            p = e + 1;
            if (!line[0])
                continue;
            if (web::istarts(line, "#EXT-X-STREAM-INF"))
            {
                isMaster = true;
                const char *bw = strstr(line, "BANDWIDTH=");
                pendingBw = bw ? number(bw + 10) : 0;
            }
            else if (web::istarts(line, "#EXT-X-MEDIA-SEQUENCE:"))
                mediaSeq = number(line + 22);
            else if (web::istarts(line, "#EXT-X-TARGETDURATION:"))
                targetMs = (uint32_t)number(line + 22) * 1000;
            else if (web::istarts(line, "#EXT-X-ENDLIST"))
                endList = true;
            else if (line[0] != '#')
            {
                if (pendingBw >= 0)
                {
                    //  With 256 colours (the graphics kernel, where the film
                    //  can have the whole screen) the richest variant a
                    //  segment of a few seconds of which the loader takes
                    //  (HttpResponse::MAX_BODY); with 16, where 320x240 is
                    //  all that is shown, the leanest.
                    bool better;
                    if (blk && blk->colours == 256)
                        better = bestBw < 0 || (pendingBw <= MAX_VARIANT_BW && (bestBw > MAX_VARIANT_BW || pendingBw > bestBw)) ||
                                 (bestBw > MAX_VARIANT_BW && pendingBw < bestBw);
                    else
                        better = bestBw < 0 || pendingBw < bestBw;
                    if (better)
                    {
                        bestBw = pendingBw;
                        web::scopy(best, line, sizeof(best));
                    }
                    pendingBw = -1;
                }
                else if (count < 64)
                    web::scopy(uris[count++], line, URL_CAP);
            }
        }
        if (isMaster)
        {
            web::big_free(uris);
            char next[URL_CAP];
            if (!best[0] || ++hops > 3 || !resolve(best, next, sizeof(next)))
                return streamFailed("The master playlist names no stream this can follow.");
            web::scopy(playlist, next, sizeof(playlist));
            endList = false;
            return fetchPlaylist();
        }
        if (targetMs < 1000)
            targetMs = 1000;
        //  Live: start three segments from the end, as players do.
        if (nextSeq < 0)
            nextSeq = endList ? mediaSeq : (count > 3 ? mediaSeq + count - 3 : mediaSeq);
        else if (!endList && nextSeq < mediaSeq && count > 1)
        {
            //  Fallen out of the live window: what is gone is gone, and the
            //  oldest one listed is the next to be deleted --- the one after.
            nextSeq = mediaSeq + 1;
        }
        bool added = false;
        for (int i = 0; i < count && nQueue < MAX_QUEUE; i++)
        {
            long seq = mediaSeq + i;
            if (seq < nextSeq)
                continue;
            if (resolve(uris[i], queue[nQueue], URL_CAP))
            {
                nQueue++;
                added = true;
            }
            nextSeq = seq + 1;
        }
        web::big_free(uris);
        failures = 0;
        nextPlaylistAt = r2::ticks() + (added ? targetMs : targetMs / 2);
    }

    //  From the idle loop: the fetches, and the ring kept fed.
    void pumpStream()
    {
        if (!streaming || !blk || !ring)
            return;
        if (pending.len)
        {
            uint64_t head = blk->streamHead, tail = blk->streamTail;
            size_t room = STREAM_RING - (size_t)(head - tail);
            size_t n = pending.len - pendingOff;
            if (n > room)
                n = room;
            for (size_t k = 0; k < n;)
            {
                size_t at = (size_t)((head + k) % STREAM_RING);
                size_t chunk = STREAM_RING - at;
                if (chunk > n - k)
                    chunk = n - k;
                memcpy(ring + at, pending.data + pendingOff + k, chunk);
                k += chunk;
            }
            __asm__ volatile("" ::: "memory");
            blk->streamHead = head + n;
            pendingOff += n;
            if (pendingOff >= pending.len)
                pending.release();
            return;
        }
        if (fetching != F_NONE)
        {
            loader->step();
            if (loader->busy())
                return;
            Fetch was = fetching;
            fetching = F_NONE;
            web::HttpResponse &r = loader->response();
            bool ok = loader->phase() == web::Loader::DONE && r.status == 200 && r.body.len;
            if (was == F_PLAYLIST)
            {
                if (ok)
                    parsePlaylist((const char *)r.body.cstr(), r.body.len);
                else if (++failures >= 3)
                    streamFailed(loader->phase() == web::Loader::FAILED ? loader->error() : "The playlist could not be fetched.");
                else
                    nextPlaylistAt = r2::ticks() + 3000;
            }
            else if (ok)
            {
                //  The segment's bytes, taken over from the response.
                uint64_t took = r2::ticks() - segmentFrom;
                segmentKBs = (uint32_t)(r.body.len / 1024 * 1000 / (took ? took : 1));
                pending.release();
                pending.data = r.body.data;
                pending.len = r.body.len;
                pending.cap = r.body.cap;
                pending.big = r.body.big;
                r.body.data = nullptr;
                r.body.len = r.body.cap = 0;
                pendingOff = 0;
            }
            r.body.release();
            return;
        }
        if (nQueue)
        {
            char where[URL_CAP];
            web::scopy(where, queue[0], sizeof(where));
            for (int i = 1; i < nQueue; i++)
                memcpy(queue[i - 1], queue[i], URL_CAP);
            nQueue--;
            fetchSegment(where);
            return;
        }
        if (!endList)
        {
            if (r2::ticks() >= nextPlaylistAt)
                fetchPlaylist();
            return;
        }
        blk->streamEnded = 1;
    }

    //  The player's task is still in the table.  A table that cannot be read
    //  just now counts as alive.
    bool playerAlive()
    {
        r2::vector<r2::TaskInfo> tasks = r2::tasks();
        if (tasks.empty())
            return true;
        for (size_t i = 0; i < tasks.size(); i++)
            if (tasks[i].id == pid && tasks[i].status < 4 && tasks[i].name[0] == 'M' && tasks[i].name[1] == 'P')
                return true;
        return false;
    }

    void onIdle()
    {
        if (!blk)
            return;
        blk->hostBeat = blk->hostBeat + 1;
        pumpStream();
        uint64_t now = r2::ticks();
        fsStep(now);
        //  (On the whole screen the pictures do not come here.)
        if (blk->frame != shownFrame && !fsHeld)
        {
            shownFrame = blk->frame;
            wnd->Repaint();
        }
        if (info && now - infoAt >= 2000)
            takeInfo(now);
        //  Asked to stop: the player leaves within a frame or two.  One that
        //  has not after three seconds is left to finish on its own.
        if (stopAsked && (blk->exited || now - stopAsked > 3000))
        {
            finished();
            return;
        }
        //  Once a second: has it gone?  It says so when it leaves properly; a
        //  crash shows only in the task table.
        if (now - lastCheck < 1000 || now - startedAt < 2000)
            return;
        lastCheck = now;
        if (blk->exited || !playerAlive())
            finished();
        else if (blk->state == VidHostBlock::FAILED && !stopAsked)
        {
            //  It waits for this before it goes, so that its reason is read:
            //  finished() takes it from the block once it has.
            stop();
        }
        else if (blk->state != VidHostBlock::PLAYING)
            wnd->Repaint(); // the status line: paused, ended
    }

    //  The diagnostics line, from the counts of the last two seconds:
    //    rep   repaints of the screen, average/longest ms
    //    pass  the loop's longest pass, ms: the network waits that long
    //    buf   stream bytes waiting for the player, KiB; dl the last segment's speed
    //    wait  the player's longest wait for stream bytes (ms), and how many
    //          waits over 50 ms since the start
    //    dec   its longest decode of a picture, ms; behind: how far behind the
    //          furthest skipped picture was, ms
    void takeInfo(uint64_t now)
    {
        MementoR2Impl::R2_LoopStats ls = MementoR2Impl::R2_TakeLoopStats();
        long secs = (long)((now - infoAt) / 1000);
        infoAt = now;
        char *o = infoLine;
        size_t cap = sizeof(infoLine);
        web::scopy(o, "rep ", cap);
        web::scatInt(o, secs > 0 ? (long)ls.repaints / secs : (long)ls.repaints, cap);
        web::scat(o, "/s ", cap);
        web::scatInt(o, ls.repaints ? (long)(ls.repaintMsSum / ls.repaints) : 0, cap);
        web::scat(o, "/", cap);
        web::scatInt(o, (long)ls.repaintMsMax, cap);
        web::scat(o, "ms pass ", cap);
        web::scatInt(o, (long)ls.passMsMax, cap);
        if (streaming)
        {
            web::scat(o, " buf ", cap);
            web::scatInt(o, (long)((blk->streamHead - blk->streamTail) / 1024), cap);
            web::scat(o, "K dl ", cap);
            web::scatInt(o, (long)segmentKBs, cap);
            web::scat(o, "K/s wait ", cap);
            web::scatInt(o, (long)blk->waitMaxMs, cap);
            web::scat(o, "/", cap);
            web::scatInt(o, (long)blk->waits, cap);
        }
        web::scat(o, " dec ", cap);
        web::scatInt(o, (long)blk->decodeMaxMs, cap);
        web::scat(o, " behind ", cap);
        web::scatInt(o, (long)blk->lateMaxMs, cap);
        if (blk->fsW)
        {
            //  The last whole screen: the size the picture was made at, and
            //  "up" when the kernel enlarged it to the screen.
            web::scat(o, " fs ", cap);
            web::scatInt(o, (long)blk->fsW, cap);
            web::scat(o, "x", cap);
            web::scatInt(o, (long)blk->fsH, cap);
            if (blk->fsScaled)
                web::scat(o, " up", cap);
        }
        blk->waitMaxMs = blk->decodeMaxMs = blk->lateMaxMs = 0;
        wnd->Repaint();
    }

    // ── Keys ──────────────────────────────────────────────────────────────────

    void onKey(PlatformKey *key)
    {
        if (!key->isKeyDown)
            return;
        if (blk)
        {
            if (fsHeld && (key->isEscape || (key->isChar && (key->theChar == 'f' || key->theChar == 'F'))))
                fsLeave();
            else if (key->isChar && (key->theChar == 'f' || key->theChar == 'F'))
                fsEnter();
            else if (key->isEscape || key->isBackspace)
                stop();
            else if (key->isSpace)
                blk->paused = !blk->paused;
            else if (key->isArrowUp || key->isArrowDown)
            {
                volume += key->isArrowUp ? VOLUME_STEP : -VOLUME_STEP;
                volume = volume < 0 ? 0 : volume > VOLUME_MAX ? VOLUME_MAX : volume;
                blk->volume = (uint8_t)volume;
            }
            else if (key->isChar && (key->theChar == 'r' || key->theChar == 'R'))
            {
                blk->restart = blk->restart + 1;
                blk->paused = 0;
            }
            else if (key->isChar && (key->theChar == 'i' || key->theChar == 'I'))
            {
                info = !info;
                infoLine[0] = 0;
                if (info)
                {
                    //  From now: the counts so far are thrown away.
                    infoAt = r2::ticks();
                    MementoR2Impl::R2_TakeLoopStats();
                    blk->waitMaxMs = blk->decodeMaxMs = blk->lateMaxMs = 0;
                }
            }
            wnd->Repaint();
            return;
        }
        if (typing)
        {
            size_t n = strlen(typed);
            if (key->isEscape)
                typing = false;
            else if (key->isEnter)
            {
                typing = false;
                startStream(typed);
            }
            else if (key->isBackspace)
            {
                if (n)
                    typed[n - 1] = 0;
            }
            else if ((key->isLeftControl || key->isRightControl) && key->isChar &&
                     (key->theChar == 'v' || key->theChar == 'V'))
            {
                for (const char *p = clipboardGet(); *p && n + 1 < sizeof(typed); p++)
                    if (*p > ' ' && (unsigned char)*p < 0x7F)
                        typed[n++] = *p;
                typed[n] = 0;
            }
            else if (key->isChar && key->theChar > ' ' && (unsigned char)key->theChar < 0x7F && n + 1 < sizeof(typed))
            {
                typed[n] = (char)key->theChar;
                typed[n + 1] = 0;
            }
            wnd->Repaint();
            return;
        }
        if (key->isChar && (key->theChar == 'u' || key->theChar == 'U'))
        {
            typing = true;
            typed[0] = 0;
            error[0] = 0;
            wnd->Repaint();
            return;
        }
        if (key->isEscape)
        {
            wnd->Close();
            return;
        }
        if (key->isArrowUp && sel > 0)
            sel--;
        else if (key->isArrowDown && sel + 1 < nFiles)
            sel++;
        else if (key->isEnter)
        {
            play(sel);
            return;
        }
        else
            return;
        if (sel < top)
            top = sel;
        if (sel >= top + MAX_ROWS)
            top = sel - MAX_ROWS + 1;
        wnd->Repaint();
    }

    void onClick(double x, double y)
    {
        (void)x;
        if (blk)
            return;
        int row = (int)((y - 14) / ROW_H);
        if (row < 0 || top + row >= nFiles)
            return;
        if (top + row == sel)
            play(sel);
        else
        {
            sel = top + row;
            wnd->Repaint();
        }
    }

    void onEvent_(struct PlatformWindowInterfaceInputEvent *data)
    {
        switch (data->type)
        {
        case PlatformWindowInputEventType::OnImmediateModeIdleLoop:
            onIdle();
            return;
        case PlatformWindowInputEventType::OnPaint:
            OnPaint(data->Data.OnPaint.ctx, data->Data.OnPaint.target);
            return;
        case PlatformWindowInputEventType::OnKeyEvent:
            onKey(data->Data.OnKeyEvent.key);
            return;
        case PlatformWindowInputEventType::OnMouseClick:
            if (data->Data.OnMouseClick.state == PlatformWindowButtonState::Pressed)
            {
                Coord mx = data->Data.OnMouseClick.mouseX, my = data->Data.OnMouseClick.mouseY;
                onClick(COORD_VAL(mx), COORD_VAL(my));
            }
            return;
        default:
            return;
        }
    }

    // ── Drawing ───────────────────────────────────────────────────────────────

    static void clock(char *out, uint32_t ms)
    {
        uint32_t s = ms / 1000;
        char tmp[16];
        int n = 0;
        uint32_t m = s / 60;
        do
            tmp[n++] = (char)('0' + m % 10);
        while ((m /= 10) && n < 6);
        int at = 0;
        while (n)
            out[at++] = tmp[--n];
        out[at++] = ':';
        out[at++] = (char)('0' + (s % 60) / 10);
        out[at++] = (char)('0' + s % 10);
        out[at] = 0;
    }

    //  The newest picture, straight into the window's pixels.  See vidhost.hpp
    //  for why `reading` is set and front checked again.
    //  Source column of each output column, for blit().
    static const int MAX_OUT_W = 2048;
    uint16_t srcCol[MAX_OUT_W];
    int lastSrcW = 0, lastOutW = 0; // what srcCol was worked out for

    void blit(PlatformBitmap *target, double unitsW, double unitsH)
    {
        auto *bm = static_cast<MementoR2Impl::R2_BitmapImpl *>(target);
        uint8 *px = bm->GetPixels();
        int bw = bm->GetRealWidth().intValue(), bh = bm->GetRealHeight().intValue();
        if (!px || unitsW <= 0)
            return;
        //  Pixels a unit, from the DPI: 2 at this DPI.  Not bw / unitsW ---
        //  Memento allocates bitmaps in steps of 150 pixels, so the bitmap is
        //  often wider than the window, and the picture would move off-centre.
        int scale = wnd->GetEffectiveDPI() > 0 ? (wnd->GetEffectiveDPI() + 48) / 96 : 2;
        int w = blk->width, h = blk->height;
        if (w <= 0 || h <= 0 || w > VidHostBlock::MaxW || h > VidHostBlock::MaxH)
            return;
        //  The picture fills the window, less the margin and the status line,
        //  as large as it goes with its shape kept: a maximised window shows
        //  it across the screen.  Nearest pixel, the same source column for
        //  every row, and a row that repeats the one above is copied from it.
        int aw = (int)(unitsW - 4) * scale, ah = (int)(unitsH - 16) * scale;
        if (aw < 16 || ah < 16)
            return;
        int ow = aw, oh = (int)((int64_t)h * aw / w);
        if (oh > ah)
            oh = ah, ow = (int)((int64_t)w * ah / h);
        int x0 = 2 * scale + (aw - ow) / 2, y0 = 2 * scale + (ah - oh) / 2;
        if (ow <= 0 || oh <= 0 || x0 < 0 || y0 < 0 || x0 + ow > bw || y0 + oh > bh || ow > MAX_OUT_W)
            return;

        //  The player makes the picture at this size (with 256 colours), so
        //  it is dithered where it is shown and not enlarged here: asked for
        //  every time, since the window may have been resized.  Beyond what
        //  the buffers hold it is asked for smaller, and enlarged here.
        int wantW = ow, wantH = oh;
        if (wantW > VidHostBlock::OutMaxW)
            wantW = VidHostBlock::OutMaxW, wantH = (int)((int64_t)oh * wantW / ow);
        if (wantH > VidHostBlock::OutMaxH)
            wantH = VidHostBlock::OutMaxH, wantW = (int)((int64_t)ow * wantH / oh);
        blk->wantW = (uint16_t)wantW;
        blk->wantH = (uint16_t)wantH;

        for (int tries = 0; tries < 4; tries++)
        {
            uint8_t f = blk->front;
            blk->reading = f;
            __asm__ volatile("" ::: "memory");
            if (blk->front != f)
                continue;
            //  The size of what is in this buffer: the last size asked for,
            //  or an earlier one while the player catches up.
            w = blk->bufW[f & 1], h = blk->bufH[f & 1];
            if (w <= 0 || h <= 0)
                w = blk->width, h = blk->height;
            if (w <= 0 || h <= 0 || w > VidHostBlock::OutMaxW || h > VidHostBlock::OutMaxH)
                break;
            if (w != lastSrcW || ow != lastOutW)
            {
                for (int x = 0; x < ow; x++)
                    srcCol[x] = (uint16_t)((int64_t)x * w / ow);
                lastSrcW = w, lastOutW = ow;
            }
            const uint8_t *src = blk->pixels[f & 1];
            int lastSrc = -1;
            for (int y = 0; y < oh; y++)
            {
                uint8_t *out = px + (y0 + y) * bw + x0;
                int sy = (int)((int64_t)y * h / oh);
                if (sy == lastSrc)
                {
                    memcpy(out, out - bw, (size_t)ow);
                    continue;
                }
                lastSrc = sy;
                const uint8_t *row = src + sy * w;
                if (ow == w)
                    memcpy(out, row, (size_t)w);
                else
                    for (int x = 0; x < ow; x++)
                        out[x] = row[srcCol[x]];
            }
            break;
        }
        blk->reading = VidHostBlock::NONE;
    }

    void OnPaint(PlatformDrawingContext *dc, PlatformBitmap *target)
    {
        if (!target)
            return;
        if (!dark)
            dark = dc->CreateColor(0xFF0000AA, nullptr, nullptr);
        if (!light)
            light = dc->CreateColor(0xFFE0E0FF, nullptr, nullptr);
        if (!black)
            black = dc->CreateColor(0xFF000000, nullptr, nullptr);
        if (!font)
            font = dc->CreateFont(6, nullptr, false, false, false, nullptr, nullptr);
        if (!dark || !light || !black || !font)
            return;

        Coord Wc = target->GetWidth(), Hc = target->GetHeight();
        double Wd = COORD_VAL(Wc), Hd = COORD_VAL(Hc);
        PlatformDrawTextOptions o{};
        o.font = font;
        o.foreground = dark;
        o.horizontalAlign = PlatformAlign::Begin;
        o.verticalAlign = PlatformAlign::Middle;

        if (blk)
        {
            target->FillRect(0, 0, Wc, Hc, black, false);
            if (blk->frame)
                blit(target, Wd, Hd);
            //  The status line: name, time, and what is going on.
            target->FillRect(0, Coord(Hd - 12), Wc, 12, light, false);
            char line[96];
            strcpy(line, title);
            cat(line, sizeof(line), "  ");
            char t[16];
            clock(t, blk->positionMs);
            cat(line, sizeof(line), t);
            if (blk->durationMs)
            {
                cat(line, sizeof(line), " / ");
                clock(t, blk->durationMs);
                cat(line, sizeof(line), t);
            }
            static const char *const states[] = {"  loading", "", "  paused", "  (end: R again)", "  failed"};
            if (streaming && blk->state == VidHostBlock::ENDED)
                cat(line, sizeof(line), "  (end)"); // a stream has no start to go back to
            else if (blk->state < 5)
                cat(line, sizeof(line), states[blk->state]);
            if (blk->dropped)
            {
                //  Pictures decoded too late to show: the machine is not
                //  keeping up with this film's size or rate.
                char d[16];
                int n = 0;
                uint32_t v = blk->dropped;
                do
                    d[n++] = (char)('0' + v % 10);
                while ((v /= 10) && n < 10);
                cat(line, sizeof(line), "  ");
                size_t at = strlen(line);
                while (n && at + 1 < sizeof(line))
                    line[at++] = d[--n];
                line[at] = 0;
                cat(line, sizeof(line), " late");
            }
            if (blk->sound == VidHostBlock::SOUND_ON)
            {
                cat(line, sizeof(line), "  sound ");
                char v[8];
                int n = 0, x = volume;
                do
                    v[n++] = (char)('0' + x % 10);
                while ((x /= 10) && n < 4);
                size_t at = strlen(line);
                while (n && at + 2 < sizeof(line))
                    line[at++] = v[--n];
                line[at++] = '%';
                line[at] = 0;
            }
            else if (blk->sound == VidHostBlock::SOUND_NO_DEVICE)
                cat(line, sizeof(line), "  (no sound device)");
            if (stopAsked)
                cat(line, sizeof(line), "  stopping");
            else if (error[0])
            {
                cat(line, sizeof(line), "  ");
                cat(line, sizeof(line), error);
            }
            target->DrawText(3, Coord(Hd - 11), Coord(Wd - 6), 10, (const mchar *)line, &o, false);
            if (info)
            {
                //  Over the bottom of the picture: it is for a moment.
                target->FillRect(0, Coord(Hd - 24), Wc, 12, light, false);
                target->DrawText(3, Coord(Hd - 23), Coord(Wd - 6), 10,
                                 (const mchar *)(infoLine[0] ? infoLine : "measuring..."), &o, false);
            }
            return;
        }

        if (!listed)
            refreshListing();
        target->FillRect(0, 0, Wc, Hc, light, false);
        target->DrawText(4, 1, Coord(Wd - 8), 10, VIDEO_DIR, &o, false);
        target->FillRect(2, 12, Coord(Wd - 4), 1, dark, false);
        if (!nFiles)
            target->DrawText(4, 15, Coord(Wd - 8), 10, "(no .mpg or .m3u8 files: see cpp/mpegplay/README)", &o, false);
        for (int i = 0; i < MAX_ROWS && top + i < nFiles; i++)
        {
            double ry = 14 + i * ROW_H;
            bool on = top + i == sel;
            if (on)
                target->FillRect(2, Coord(ry), Coord(Wd - 4), ROW_H - 1, dark, false);
            o.foreground = on ? light : dark;
            char name[33];
            nameOf(entries[top + i], name, sizeof(name));
            target->DrawText(4, Coord(ry), Coord(Wd - 8), ROW_H - 1, (const mchar *)name, &o, false);
        }
        o.foreground = dark;
        target->FillRect(2, Coord(Hd - 13), Coord(Wd - 4), 1, dark, false);
        if (typing)
        {
            //  The address being typed, its end in view.
            char shown[80] = "Stream: ";
            size_t n = strlen(typed), room = sizeof(shown) - 10;
            web::scat(shown, n > room ? typed + n - room : typed, sizeof(shown));
            web::scat(shown, "_", sizeof(shown));
            target->DrawText(4, Coord(Hd - 12), Coord(Wd - 8), 11, (const mchar *)shown, &o, false);
            return;
        }
        target->DrawText(4, Coord(Hd - 12), Coord(Wd - 8), 11,
                         error[0] ? error : "Enter plays; U: a stream's address; Esc closes", &o, false);
    }
};
