// Spotify's model, HTTPS and audio run in go/spotify as spotify.elf. This
// window owns only the host block and presentation, as the shell/theM hosts do.
#include "../../../go/spotify/host.h"

static SpotifyHostBlock *g_spotifyBlocks[4] = {};
static uint32_t spotifyLoad(const uint32_t *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static void spotifyStore(uint32_t *p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
static void spotifyKeepAlive()
{
    for (SpotifyHostBlock *b : g_spotifyBlocks)
        if (b) spotifyStore(&b->hostBeat, spotifyLoad(&b->hostBeat) + 1);
}

class SpotifyWindow
{
public:
    static const int W = 292, H = 211;
    SpotifyWindow()
    {
        int slot = -1;
        for (int i = 0; i < 4; i++) if (!g_spotifyBlocks[i]) { slot = i; break; }
        if (slot < 0) { strcpy(error, "Too many Spotify windows."); return; }
        blk = (SpotifyHostBlock *)r2::heap::kernel_allocate(sizeof(SpotifyHostBlock));
        if (!blk) { strcpy(error, "No memory for the Spotify host block."); return; }
        memset(blk, 0, sizeof(*blk));
        blk->magic = SPOTIFY_MAGIC; blk->version = SPOTIFY_VERSION;
        blk->reading = SPOTIFY_NONE; blk->hostBeat = 1;
        g_spotifyBlocks[slot] = blk;
        char args[64] = "spotify.elf --host 0x";
        size_t at = strlen(args);
        uintptr_t addr = (uintptr_t)blk;
        for (int shift = 28; shift >= 0; shift -= 4) args[at++] = "0123456789abcdef"[(addr >> shift) & 15];
        args[at] = 0;
        auto id = r2::spawn("spotify.elf", args);
        if (!id) { strcpy(error, "Install spotify.elf in /mnt/tar/bin or /mnt/iso/bin."); release(); return; }
        pid = *id; startedAt = r2::ticks();
        strcpy(snapshot.status, "Starting the Go Spotify prototype...");
        for (int i = 0; i < SPOTIFY_VISIBLE; i++) snapshot.playlists[i].index = snapshot.tracks[i].index = 0xffffffffu;
    }
    ~SpotifyWindow()
    {
        if (!blk) return;
        spotifyStore(&blk->quit, 1);
        for (int i = 0; i < 50 && !spotifyLoad(&blk->exited) && alive(); i++) {
            spotifyStore(&blk->hostBeat, spotifyLoad(&blk->hostBeat) + 1);
            r2::sleep(20);
        }
        if (spotifyLoad(&blk->exited) || !alive()) release();
        else {
            // Never free memory still used by the child. Stop heartbeats so
            // it exits by timeout; the parent's process owns the leftover block.
            forget();
        }
    }
    bool failed() const { return !blk; }
    const char *why() const { return error; }
    void SetWindow(PlatformWindow *w) { wnd = w; wnd->SetImmediateMode(true); }
    static void onEvent(void *p, PlatformWindowInterfaceInputEvent *e) { ((SpotifyWindow *)p)->event(e); }

private:
    PlatformWindow *wnd = nullptr;
    SpotifyHostBlock *blk = nullptr;
    SpotifySnapshot snapshot{};
    uint8_t pid = 0;
    uint64_t startedAt = 0, lastCheck = 0;
    uint32_t shownFrame = 0;
    char error[80] = {};
    bool playlistFocus = true;
    PlatformColor *bg = nullptr, *fg = nullptr, *green = nullptr, *selected = nullptr, *dim = nullptr;
    PlatformFont *font = nullptr;

    void forget()
    {
        for (SpotifyHostBlock *&b : g_spotifyBlocks) if (b == blk) b = nullptr;
        blk = nullptr;
    }
    void release() { auto *b = blk; forget(); r2::heap::kernel_deallocate(b); }
    bool alive()
    {
        auto tasks = r2::tasks();
        if (tasks.empty()) return true;
        for (size_t i = 0; i < tasks.size(); i++)
            if (tasks[i].id == pid && tasks[i].status < 4 &&
                (tasks[i].name[0] | 32) == 's' && (tasks[i].name[1] | 32) == 'p') return true;
        return false;
    }
    void send(uint32_t op, uint32_t value = 0)
    {
        if (!blk || spotifyLoad(&blk->exited)) return;
        uint32_t h = spotifyLoad(&blk->head);
        if (h - spotifyLoad(&blk->tail) >= SPOTIFY_QUEUE) return;
        blk->commands[h % SPOTIFY_QUEUE] = {op, value};
        spotifyStore(&blk->head, h + 1);
    }
    void idle()
    {
        if (!blk) return;
        spotifyStore(&blk->hostBeat, spotifyLoad(&blk->hostBeat) + 1);
        uint32_t frame = spotifyLoad(&blk->frame);
        if (frame != shownFrame) {
            uint32_t f = spotifyLoad(&blk->front);
            if (f < 2) {
                spotifyStore(&blk->reading, f);
                if (spotifyLoad(&blk->front) == f) { snapshot = blk->snapshots[f]; shownFrame = frame; wnd->Repaint(); }
                spotifyStore(&blk->reading, SPOTIFY_NONE);
            }
        }
        uint64_t now = r2::ticks();
        if (now - startedAt < 2000 || now - lastCheck < 1000) return;
        lastCheck = now;
        if (spotifyLoad(&blk->exited) || !alive()) {
            strcpy(snapshot.status, "Go client ended. Close and reopen to restart.");
            wnd->SetImmediateMode(false); wnd->Repaint();
        }
    }
    void key(PlatformKey *k)
    {
        if (!k->isKeyDown) return;
        if (k->isEscape) { wnd->Close(); return; }
        if (k->isTab) { playlistFocus = !playlistFocus; wnd->Repaint(); return; }
        if (k->isEnter) {
            if (playlistFocus) { send(SpotifyOpenPlaylist, snapshot.playlistSelected); playlistFocus = false; wnd->Repaint(); }
            else send(SpotifyPlayPause);
            return;
        }
        if (k->isChar && k->theChar == ' ') { send(SpotifyPlayPause); return; }
        if (k->isChar) {
            switch (k->theChar >= 'A' && k->theChar <= 'Z' ? k->theChar + ('a' - 'A') : k->theChar) { case 'r': send(SpotifyRefresh); return; case 'n': send(SpotifyNext); return;
                case '-': case '_': send(SpotifyVolumeDown); return;
                case '+': case '=': send(SpotifyVolumeUp); return;
                case 'm': send(SpotifyMute); return; case 'g': send(SpotifyCollect); return; case 'p': send(SpotifyPrevious); return; case 's': send(SpotifyStop); return; }
        }
        if (k->isArrowLeft || k->isArrowRight || k->isPageUp || k->isPageDown) {
            send(playlistFocus ? SpotifyPlaylistPage : SpotifyTrackPage, (k->isArrowLeft || k->isPageUp) ? 0xffffffffu : 1); return;
        }
        if (k->isArrowUp || k->isArrowDown) {
            uint32_t sel = playlistFocus ? snapshot.playlistSelected : snapshot.trackSelected;
            uint32_t off = playlistFocus ? snapshot.playlistOffset : snapshot.trackOffset;
            uint32_t count = playlistFocus ? snapshot.playlistCount : snapshot.trackCount;
            if (k->isArrowUp && sel > off) sel--;
            else if (k->isArrowDown && sel + 1 < count && sel + 1 < off + SPOTIFY_VISIBLE) sel++;
            send(playlistFocus ? SpotifySelectPlaylist : SpotifySelectTrack, sel);
        }
    }
    static const int RowTop = 30, RowHeight = 10;
    void click(int x, int y)
    {
        if (y >= RowTop && y < RowTop + SPOTIFY_VISIBLE * RowHeight) {
            int row = (y - RowTop) / RowHeight;
            if (x >= 4 && x < 106 && snapshot.playlists[row].index != 0xffffffffu) {
                playlistFocus = false; send(SpotifyOpenPlaylist, snapshot.playlists[row].index);
            } else if (x >= 110 && x < W - 4 && snapshot.tracks[row].index != 0xffffffffu) {
                playlistFocus = false; send(SpotifySelectTrack, snapshot.tracks[row].index);
            }
            wnd->Repaint(); return;
        }
        if (y >= 18 && y < 28) {
            if (x >= 82 && x < 106) send(SpotifyPlaylistPage, x < 94 ? 0xffffffffu : 1);
            if (x >= 264 && x < 288) send(SpotifyTrackPage, x < 276 ? 0xffffffffu : 1);
        }
        if (y >= 153 && y < 166) {
            if (x >= 4 && x < 46) send(SpotifyVolumeDown);
            else if (x >= 48 && x < 90) send(SpotifyVolumeUp);
            else if (x >= 94 && x < 158) send(SpotifyMute);
        }
        if (y >= 137 && y < 150) {
            if (x >= 4 && x < 47) send(SpotifyPrevious);
            else if (x >= 49 && x < 120) send(SpotifyPlayPause);
            else if (x >= 122 && x < 165) send(SpotifyNext);
            else if (x >= 167 && x < 208) send(SpotifyStop);
            else if (x >= 210 && x < 288) send(SpotifyRefresh);
        }
    }
    void event(PlatformWindowInterfaceInputEvent *e)
    {
        switch (e->type) {
        case PlatformWindowInputEventType::OnImmediateModeIdleLoop: idle(); break;
        case PlatformWindowInputEventType::OnKeyEvent: key(e->Data.OnKeyEvent.key); break;
        case PlatformWindowInputEventType::OnMouseClick:
            if (e->Data.OnMouseClick.state == PlatformWindowButtonState::Pressed)
                click((int)COORD_VAL(e->Data.OnMouseClick.mouseX), (int)COORD_VAL(e->Data.OnMouseClick.mouseY));
            break;
        case PlatformWindowInputEventType::OnPaint: paint(e->Data.OnPaint.ctx, e->Data.OnPaint.target); break;
        default: break;
        }
    }
    void text(PlatformBitmap *t, int x, int y, int width, const char *s, PlatformColor *color)
    {
        PlatformDrawTextOptions o{}; o.font = font; o.foreground = color;
        o.horizontalAlign = PlatformAlign::Begin; o.verticalAlign = PlatformAlign::Middle;
        t->DrawText(x, y, width, 9, s, &o, false);
    }
    void button(PlatformBitmap *t, int x, int width, const char *label, int y = 137)
    {
        t->FillRect(x, y, width, 13, selected, false);
        text(t, x + 3, y + 2, width - 6, label, fg);
    }
    static void appendTime(char *out, uint32_t ms, size_t cap)
    {
        uint32_t seconds = ms / 1000;
        web::scatInt(out, seconds / 60, cap);
        web::scat(out, seconds % 60 < 10 ? ":0" : ":", cap);
        web::scatInt(out, seconds % 60, cap);
    }
    void paint(PlatformDrawingContext *dc, PlatformBitmap *t)
    {
        if (!t) return;
        if (!font) {
            font = dc->CreateFont(6, nullptr, false, false, false, nullptr, nullptr);
            bg = dc->CreateColor(0xff111111, nullptr, nullptr);
            fg = dc->CreateColor(0xffeeeeee, nullptr, nullptr);
            green = dc->CreateColor(0xff00aa55, nullptr, nullptr);
            selected = dc->CreateColor(0xff333333, nullptr, nullptr);
            dim = dc->CreateColor(0xffaaaaaa, nullptr, nullptr);
        }
        if (!font || !bg || !fg || !selected || !green || !dim) return;
        t->FillRect(0, 0, t->GetWidth(), t->GetHeight(), bg, false);
        text(t, 4, 3, 66, "Spotify / Go", green);
        text(t, 76, 3, W - 80, snapshot.nowPlaying[0] ? snapshot.nowPlaying : "Spotify playlists + native audio", fg);
        text(t, 4, 18, 78, playlistFocus ? "> Playlists (Enter)" : "Playlists (Enter)", fg);
        text(t, 110, 18, 150, playlistFocus ? "Songs" : "> Songs", fg);
        text(t, 82, 18, 24, "<  >", green); text(t, 264, 18, 24, "<  >", green);
        for (int i = 0; i < SPOTIFY_VISIBLE; i++) {
            int y = RowTop + i * RowHeight;
            if (snapshot.playlists[i].index != 0xffffffffu) {
                if (snapshot.playlists[i].index == snapshot.playlistSelected) t->FillRect(4, y, 102, RowHeight, selected, false);
                text(t, 6, y, 98, snapshot.playlists[i].text, fg);
            }
            if (snapshot.tracks[i].index != 0xffffffffu) {
                if (snapshot.tracks[i].index == snapshot.trackSelected) t->FillRect(110, y, W - 114, RowHeight, selected, false);
                text(t, 112, y, W - 118, snapshot.tracks[i].text, fg);
            }
        }
        button(t, 4, 43, "Prev (P)"); button(t, 49, 71, snapshot.playing && !snapshot.paused ? "Pause (Space)" : "Play (Space)");
        button(t, 122, 43, "Next (N)"); button(t, 167, 41, "Stop (S)"); button(t, 210, 78, "Refresh (R)");
        button(t, 4, 42, "Vol- (-)", 153); button(t, 48, 42, "Vol+ (+)", 153);
        button(t, 94, 64, snapshot.muted ? "Unmute (M)" : "Mute (M)", 153);
        char volume[32] = "Volume: ";
        web::scatInt(volume, snapshot.volume, sizeof(volume));
        web::scat(volume, snapshot.muted ? "% (muted)" : "%", sizeof(volume));
        text(t, 164, 155, W - 168, volume, fg);
        char elapsed[48] = {};
        appendTime(elapsed, snapshot.positionMS, sizeof(elapsed));
        web::scat(elapsed, " / ", sizeof(elapsed));
        if (snapshot.durationMS) appendTime(elapsed, snapshot.durationMS, sizeof(elapsed));
        else web::scat(elapsed, "--:--", sizeof(elapsed));
        text(t, 4, 168, W - 8, elapsed, fg);
        t->FillRect(4, 180, W - 8, 2, selected, false);
        if (snapshot.durationMS) {
            uint64_t px = (uint64_t)snapshot.positionMS * (W - 8) / snapshot.durationMS;
            if (px > W - 8) px = W - 8;
            t->FillRect(4, 180, (int)px, 2, green, false);
        }
        char stats[96] = "Go: ";
        web::scatInt(stats, snapshot.goroutines, sizeof(stats));
        web::scat(stats, " goroutines | heap ", sizeof(stats));
        web::scatInt(stats, snapshot.heapBytes / 1024, sizeof(stats));
        web::scat(stats, " KiB | stack peak ", sizeof(stats));
        web::scatInt(stats, snapshot.stackBytes, sizeof(stats));
        web::scat(stats, " B", sizeof(stats));
        text(t, 4, 184, W - 8, stats, dim);
        text(t, 4, 197, W - 8, snapshot.status, fg);
    }
};
