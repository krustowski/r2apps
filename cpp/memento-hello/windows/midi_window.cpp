//
// Window — MIDI player for the .MID files in /mnt/fat/SOUND
//
// The kernel will play a Standard MIDI File for you (syscall 0x1b), and that
// is exactly what this window does not do. That call parses the file and then
// sits in the kernel spinning on the PIT until the last note is over: the
// calling task is blocked for the length of the song, nothing else it owns
// gets a look in, and there is no way to ask it to stop — syscall 0x1f only
// silences the speaker, and you cannot call it from a task that is already
// inside 0x1b. Playing a song from here that way would freeze the whole
// desktop until it finished.
//
// So the sequencer lives here instead. The file is read once, parsed into
// track cursors, and advanced a step at a time from the window's idle loop:
// each step says which note should be sounding and until when, the speaker is
// programmed through the port syscalls, and between steps the loop returns
// and the rest of the desktop carries on. Stop is then just a flag.
//
// The parsing follows the kernel's own (src/audio/smf.rs) so that the same
// files sound the same: format 0, 1 and 2, running status, tempo changes,
// PPQN and SMPTE division, and a monophonic reduction to the highest note
// held, because a PC speaker has one voice. Channel 10 is dropped — drum
// notes carry no pitch worth hearing.
//

class MidiWindow
{
public:
    static void onEvent(void *instance, struct PlatformWindowInterfaceInputEvent *data)
    {
        reinterpret_cast<MidiWindow *>(instance)->onEvent_(data);
    }
    void SetWindow(PlatformWindow *w) { wnd = w; }

    ~MidiWindow() { silence(); }

private:
    PlatformWindow *wnd = nullptr;
    PlatformColor *dark = nullptr;
    PlatformColor *light = nullptr;
    PlatformFont *font = nullptr;

    // Layout, in the window's own client coordinates.
    static const int HEAD_Y = 3;
    static const int ROW_Y = 15;
    static const int ROW_H = 10;
    static const int MAX_ROWS = 8;
    static const int NAME_X = 6, NAME_W = 120;
    static const int SIZE_X = 130, SIZE_W = 50;
    static const int STATUS_Y = 100;
    static const int BTN_Y = 114, BTN_H = 11;
    static const int PLAY_X = 6, PLAY_W = 40;
    static const int STOP_X = 52, STOP_W = 40;
    static const int BACK_X = 140, BACK_W = 40;


    VfsDirEntry_T entries[64];
    int nFiles = 0;
    int sel = 0;
    bool listingStale = true;

    // ------------------------------------------------------------------
    // The file, and the cursors into it
    // ------------------------------------------------------------------
    static const int MAX_MIDI = 8192;
    static const int MAX_TRACKS = 16;

    unsigned char data[MAX_MIDI];
    unsigned int dataLen = 0;

    struct Track
    {
        unsigned int pos;
        unsigned int end;
        unsigned int nextTick; // absolute tick of this track's pending event
        unsigned char status;  // running status, 0 for none
        bool done;
        unsigned long long held[2]; // one bit per note this track holds
    };

    Track tracks[MAX_TRACKS];
    int trackCount = 0;
    unsigned short format = 0;
    unsigned short division = 0;
    int activePattern = 0; // format 2 plays its tracks one after another
    unsigned int tempoUs = 500000;
    unsigned int tick = 0;
    unsigned long long elapsedUs = 0;
    unsigned long long remainder = 0;
    unsigned char heldCount[128] = {};
    unsigned char current = 0; // the note being voiced, 0 for a rest

    // ------------------------------------------------------------------
    // Playback state
    // ------------------------------------------------------------------
    bool playing = false;
    unsigned long startMs = 0;
    unsigned long long stepEndUs = 0;
    unsigned char sounding = 0;
    char nowPlaying[33] = {};
    char message[40] = {};

    // ------------------------------------------------------------------
    // The speaker
    //
    // Port 0x61 is written blind: syscall 0x31 comes back 0xffffffff on this
    // kernel, so the read-modify-write the kernel does for itself is not
    // available here. Bits 2 and 3 are parity and channel check enables and
    // are nothing this program should be turning on, so writing 3 and 0 is
    // both what every speaker driver of the era did and the right answer.
    // ------------------------------------------------------------------
    static void outb(unsigned short port, unsigned char value)
    {
        unsigned short p = port;
        unsigned int v = value;
        r2::raw_syscall(r2::Sys::WritePort, (long)&p, (long)&v);
    }

    static void speakerOff() { outb(0x61, 0x00); }

    static void speakerOn(unsigned int freq)
    {
        if (freq < 20)
        {
            speakerOff();
            return;
        }
        unsigned int divisor = 1193180u / freq;
        if (divisor == 0 || divisor > 0xFFFF)
        {
            speakerOff();
            return;
        }
        outb(0x43, 0xB6); // channel 2, lo/hi byte, square wave
        outb(0x42, (unsigned char)(divisor & 0xFF));
        outb(0x42, (unsigned char)((divisor >> 8) & 0xFF));
        outb(0x61, 0x03); // gate the timer through to the speaker
    }

    // Equal temperament, A4 = 440 Hz, rounded — the same table the kernel
    // plays from, so a file sounds the same either way.
    static unsigned short noteFreq(unsigned char note)
    {
        static const unsigned short table[128] = {
            8, 9, 9, 10, 10, 11, 12, 13, 14, 15, 16, 17,
            18, 19, 21, 22, 23, 25, 26, 28, 29, 31, 33, 35,
            37, 39, 41, 44, 46, 49, 52, 55, 58, 62, 65, 69,
            73, 78, 82, 87, 92, 98, 104, 110, 117, 123, 131, 139,
            147, 156, 165, 175, 185, 196, 208, 220, 233, 247, 262, 277,
            294, 311, 330, 349, 370, 392, 415, 440, 466, 494, 523, 554,
            587, 622, 659, 698, 740, 784, 831, 880, 932, 988, 1047, 1109,
            1175, 1245, 1319, 1397, 1480, 1568, 1661, 1760, 1865, 1976, 2093, 2217,
            2349, 2489, 2637, 2794, 2960, 3136, 3322, 3520, 3729, 3951, 4186, 4435,
            4699, 4978, 5274, 5588, 5920, 6272, 6645, 7040, 7458, 7902, 8372, 8869,
            9397, 9956, 10548, 11175, 11840, 12544, 13290, 14080};
        return table[note & 0x7F];
    }

    void silence()
    {
        speakerOff();
        sounding = 0;
    }

    // ------------------------------------------------------------------
    // Reading the directory and the file
    // ------------------------------------------------------------------
    static bool isMidiName(const VfsDirEntry_T &e)
    {
        if (e.is_dir)
            return false;
        int n = e.name_len < 32 ? e.name_len : 32;
        if (n < 5)
            return false;
        const unsigned char *s = e.name + n - 4;
        if (s[0] != '.')
            return false;
        char a = (char)(s[1] | 0x20), b = (char)(s[2] | 0x20), c = (char)(s[3] | 0x20);
        return a == 'm' && b == 'i' && c == 'd';
    }

    void refreshListing()
    {
        int raw = (int)list_dir_path((const unsigned char *)"/mnt/fat/SOUND", entries);
        if (raw < 0)
            raw = 0;
        nFiles = 0;
        for (int i = 0; i < raw && nFiles < 64; i++)
        {
            if (!isMidiName(entries[i]))
                continue;
            if (nFiles != i)
                entries[nFiles] = entries[i];
            nFiles++;
        }
        if (sel > nFiles)
            sel = nFiles;
        listingStale = false;
    }

    static void copyName(const VfsDirEntry_T &e, char *out, int outSize)
    {
        int n = e.name_len < 32 ? e.name_len : 32;
        if (n > outSize - 1)
            n = outSize - 1;
        int at = 0;
        for (int i = 0; i < n; i++)
        {
            unsigned char c = e.name[i];
            out[at++] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
        }
        out[at] = 0;
    }

    void setMessage(const char *text)
    {
        int i = 0;
        for (; text[i] && i < (int)sizeof(message) - 1; i++)
            message[i] = text[i];
        message[i] = 0;
    }

    // read_file takes a single path component and searches the current
    // directory, so the directory is changed first — the same dance the file
    // viewer does. The size comes from the directory entry, and a file that
    // will not fit is refused rather than read over the end of the buffer.
    bool loadFile(int index)
    {
        if (index < 0 || index >= nFiles)
            return false;
        if (entries[index].size == 0 || entries[index].size > (unsigned int)MAX_MIDI)
        {
            setMessage("file too large");
            return false;
        }

        char name[33];
        copyName(entries[index], name, sizeof(name));

        chdir((const unsigned char *)"/mnt/fat/SOUND");
        memset(data, 0, sizeof(data));
        long ok = read_file((const unsigned char *)name, data);
        // The working directory belongs to the system, not to this window:
        // leave it where it was or the shell comes back from the desktop
        // sitting in a directory it never asked for.
        chdir((const unsigned char *)"/");
        if (ok == 0)
        {
            setMessage("read error");
            return false;
        }
        dataLen = entries[index].size;
        return true;
    }

    // ------------------------------------------------------------------
    // Standard MIDI File parsing
    // ------------------------------------------------------------------
    unsigned int be32(unsigned int pos) const
    {
        if (pos + 4 > dataLen)
            return 0;
        return ((unsigned int)data[pos] << 24) | ((unsigned int)data[pos + 1] << 16) |
               ((unsigned int)data[pos + 2] << 8) | (unsigned int)data[pos + 3];
    }

    unsigned short be16(unsigned int pos) const
    {
        if (pos + 2 > dataLen)
            return 0;
        return (unsigned short)(((unsigned int)data[pos] << 8) | (unsigned int)data[pos + 1]);
    }

    // A variable-length quantity: seven bits a byte, top bit set to continue,
    // four bytes at the most.
    bool readVarlen(unsigned int pos, unsigned int end, unsigned int *value, unsigned int *used) const
    {
        unsigned int v = 0;
        for (unsigned int i = 0; i < 4; i++)
        {
            unsigned int p = pos + i;
            if (p >= end || p >= dataLen)
                return false;
            unsigned char b = data[p];
            v = (v << 7) | (unsigned int)(b & 0x7F);
            if ((b & 0x80) == 0)
            {
                *value = v;
                *used = i + 1;
                return true;
            }
        }
        return false;
    }

    static bool divisionValid(unsigned short d)
    {
        if ((d & 0x8000) == 0)
            return d != 0;
        int fps = -(int)(signed char)(d >> 8);
        unsigned short perFrame = d & 0xFF;
        return (fps == 24 || fps == 25 || fps == 29 || fps == 30) && perFrame != 0;
    }

    bool parse()
    {
        trackCount = 0;
        if (dataLen < 14)
            return false;
        if (data[0] != 'M' || data[1] != 'T' || data[2] != 'h' || data[3] != 'd')
            return false;
        unsigned int headerLen = be32(4);
        if (headerLen < 6)
            return false;
        format = be16(8);
        unsigned int declared = be16(10);
        division = be16(12);
        if (format > 2 || declared == 0 || !divisionValid(division))
            return false;

        unsigned int pos = 8 + headerLen;
        while (trackCount < (int)declared && trackCount < MAX_TRACKS)
        {
            if (pos + 8 > dataLen)
                break;
            unsigned int len = be32(pos + 4);
            unsigned int start = pos + 8;
            // A file truncated by the read buffer is still played as far as
            // the data goes.
            unsigned long long endL = (unsigned long long)start + len;
            unsigned int end = endL > dataLen ? dataLen : (unsigned int)endL;
            if (data[pos] == 'M' && data[pos + 1] == 'T' && data[pos + 2] == 'r' && data[pos + 3] == 'k')
            {
                tracks[trackCount].pos = start;
                tracks[trackCount].end = end;
                tracks[trackCount].nextTick = 0;
                tracks[trackCount].status = 0;
                tracks[trackCount].done = false;
                tracks[trackCount].held[0] = 0;
                tracks[trackCount].held[1] = 0;
                trackCount++;
            }
            unsigned long long nextPos = (unsigned long long)start + len;
            if (nextPos >= dataLen)
                break;
            pos = (unsigned int)nextPos;
        }
        if (trackCount == 0)
            return false;

        activePattern = 0;
        tempoUs = 500000;
        tick = 0;
        elapsedUs = 0;
        remainder = 0;
        current = 0;
        memset(heldCount, 0, sizeof(heldCount));
        for (int i = 0; i < trackCount; i++)
            readDelta(i);
        return true;
    }

    static bool trackHolds(const Track &t, unsigned char note)
    {
        return (t.held[note >> 6] & (1ull << (note & 63))) != 0;
    }

    static void trackSetHeld(Track &t, unsigned char note, bool on)
    {
        unsigned long long mask = 1ull << (note & 63);
        if (on)
            t.held[note >> 6] |= mask;
        else
            t.held[note >> 6] &= ~mask;
    }

    void readDelta(int i)
    {
        Track &t = tracks[i];
        if (t.done)
            return;
        unsigned int delta = 0, used = 0;
        if (!readVarlen(t.pos, t.end, &delta, &used))
        {
            finishTrack(i);
            return;
        }
        t.pos += used;
        t.nextTick += delta;
    }

    // A truncated or sloppy track must not leave a note stuck on.
    void finishTrack(int i)
    {
        tracks[i].done = true;
        for (unsigned char note = 1; note < 128; note++)
            if (trackHolds(tracks[i], note))
                noteOff(i, note);
    }

    unsigned char highestHeld() const
    {
        for (int n = 127; n >= 1; n--)
            if (heldCount[n] > 0)
                return (unsigned char)n;
        return 0;
    }

    void noteOn(int track, unsigned char note)
    {
        // Note 0 is the rest sentinel, and inaudible anyway.
        if (note == 0 || note > 127 || trackHolds(tracks[track], note))
            return;
        trackSetHeld(tracks[track], note, true);
        if (heldCount[note] < 255)
            heldCount[note]++;
        if (note > current)
            current = note;
    }

    void noteOff(int track, unsigned char note)
    {
        if (note == 0 || note > 127 || !trackHolds(tracks[track], note))
            return;
        trackSetHeld(tracks[track], note, false);
        if (heldCount[note] > 0)
            heldCount[note]--;
        if (heldCount[note] == 0 && current == note)
            current = highestHeld();
    }

    void processEvent(int i)
    {
        Track &t = tracks[i];
        unsigned int end = t.end;
        unsigned int p = t.pos;

        if (p >= end || p >= dataLen)
        {
            finishTrack(i);
            return;
        }

        unsigned char first = data[p];
        unsigned char status;
        if (first >= 0x80)
        {
            p++;
            status = first;
        }
        else if (t.status != 0)
        {
            status = t.status;
        }
        else
        {
            // A data byte with no running status: the track is corrupt.
            finishTrack(i);
            return;
        }

        bool finished = false;
        bool ok = false;

        if (status == 0xFF)
        {
            // Meta event: type, length, payload.
            unsigned int len = 0, used = 0;
            if (p < end && readVarlen(p + 1, end, &len, &used))
            {
                unsigned char type = data[p];
                p += 1 + used;
                unsigned int payloadEnd = p + len;
                if (type == 0x2F)
                {
                    finished = true;
                }
                else if (type == 0x51 && len == 3 && payloadEnd <= end && payloadEnd <= dataLen)
                {
                    unsigned int tempo = ((unsigned int)data[p] << 16) |
                                         ((unsigned int)data[p + 1] << 8) |
                                         (unsigned int)data[p + 2];
                    if (tempo != 0)
                        tempoUs = tempo;
                }
                p = payloadEnd;
                ok = true;
            }
        }
        else if (status == 0xF0 || status == 0xF7)
        {
            // SysEx, or an escape: a length-prefixed blob that cancels
            // running status.
            unsigned int len = 0, used = 0;
            if (readVarlen(p, end, &len, &used))
            {
                p += used + len;
                t.status = 0;
                ok = true;
            }
        }
        else if (status >= 0x80 && status <= 0xEF)
        {
            t.status = status;
            unsigned char kind = status & 0xF0;
            unsigned char channel = status & 0x0F;
            unsigned int dataBytes = (kind == 0xC0 || kind == 0xD0) ? 1u : 2u;
            if (p < end && p < dataLen && (dataBytes == 1 || (p + 1 < end && p + 1 < dataLen)))
            {
                unsigned char d1 = data[p];
                unsigned char d2 = dataBytes == 2 ? data[p + 1] : 0;
                p += dataBytes;
                if (channel != 9) // channel 10 is percussion
                {
                    if (kind == 0x90 && d2 > 0)
                        noteOn(i, d1);
                    else if (kind == 0x90 || kind == 0x80)
                        noteOff(i, d1);
                }
                ok = true;
            }
        }
        // System common and real-time bytes never appear in an SMF; anything
        // else leaves ok false and ends the track.

        t.pos = p;

        if (!ok || p >= end || finished)
        {
            finishTrack(i);
            return;
        }
        readDelta(i);
    }

    // Which track's pending event comes first. For format 2 the tracks are
    // independent patterns and are played one after another.
    int nextTrack()
    {
        if (format == 2)
        {
            for (int i = activePattern; i < trackCount; i++)
            {
                if (tracks[i].done)
                    continue;
                if (i != activePattern)
                {
                    activePattern = i;
                    tick = 0;
                    tempoUs = 500000;
                }
                return i;
            }
            return -1;
        }

        int best = -1;
        for (int i = 0; i < trackCount; i++)
        {
            if (tracks[i].done)
                continue;
            if (best < 0 || tracks[i].nextTick < tracks[best].nextTick)
                best = i;
        }
        return best;
    }

    // Ticks per microsecond is numerator/denominator; the denominator is
    // fixed for the whole file, which lets the remainder carry across a tempo
    // change without rescaling and keeps a long song from drifting.
    unsigned long long timeDenominator() const
    {
        if ((division & 0x8000) == 0)
            return division;
        unsigned long long fps = (unsigned long long)(-(int)(signed char)(division >> 8));
        return fps * (unsigned long long)(division & 0xFF);
    }

    unsigned long long timeNumerator() const
    {
        return (division & 0x8000) == 0 ? (unsigned long long)tempoUs : 1000000ull;
    }

    void advance(unsigned long long ticks)
    {
        unsigned long long denom = timeDenominator();
        if (denom == 0)
            denom = 1;
        unsigned long long total = ticks * timeNumerator() + remainder;
        elapsedUs += total / denom;
        remainder = total % denom;
    }

    // The next monophonic segment: the note to sound, and the song time it
    // lasts until. False when the song is over.
    bool nextStep(unsigned char *note, unsigned long long *endUs)
    {
        for (int guard = 0; guard < 4096; guard++)
        {
            int i = nextTrack();
            if (i < 0)
                return false;
            unsigned int eventTick = tracks[i].nextTick;
            if (eventTick > tick)
            {
                advance((unsigned long long)(eventTick - tick));
                tick = eventTick;
                *note = current;
                *endUs = elapsedUs;
                return true;
            }
            processEvent(i);
        }
        // Thousands of events at the same tick is a file doing something this
        // player has no answer for; stopping beats spinning.
        return false;
    }

    // ------------------------------------------------------------------
    // Transport
    // ------------------------------------------------------------------
    void startPlaying(int index)
    {
        stopPlaying();
        if (!loadFile(index))
            return;
        if (!parse())
        {
            setMessage("not a MIDI file");
            return;
        }
        copyName(entries[index], nowPlaying, sizeof(nowPlaying));
        setMessage("");
        playing = true;
        startMs = (unsigned long)r2::ticks();
        stepEndUs = 0;
        sounding = 0;
        // The idle loop is what advances the song; without it the window only
        // hears from the root when something happens to it.
        wnd->SetImmediateMode(true);
        wnd->Repaint();
    }

    void stopPlaying()
    {
        if (playing)
        {
            playing = false;
            wnd->SetImmediateMode(false);
        }
        silence();
        nowPlaying[0] = 0;
    }

    void tickPlayback()
    {
        if (!playing)
            return;

        unsigned long now = (unsigned long)r2::ticks();
        unsigned long long songUs = (unsigned long long)(now - startMs) * 1000ull;

        // Catch up: a step can be shorter than the loop's own period, so more
        // than one may be due.
        int steps = 0;
        while (songUs >= stepEndUs && steps < 64)
        {
            unsigned char note = 0;
            unsigned long long endUs = 0;
            if (!nextStep(&note, &endUs))
            {
                stopPlaying();
                setMessage("finished");
                wnd->Repaint();
                return;
            }
            stepEndUs = endUs;
            if (note != sounding)
            {
                sounding = note;
                if (note == 0)
                    speakerOff();
                else
                    speakerOn(noteFreq(note));
            }
            steps++;
        }
    }

    // ------------------------------------------------------------------
    // Events
    // ------------------------------------------------------------------
    void onEvent_(struct PlatformWindowInterfaceInputEvent *data_)
    {
        if (data_->type == PlatformWindowInputEventType::OnPaint)
        {
            OnPaint(data_->Data.OnPaint.ctx, data_->Data.OnPaint.target);
            return;
        }
        if (data_->type == PlatformWindowInputEventType::OnImmediateModeIdleLoop)
        {
            tickPlayback();
            return;
        }
        if (data_->type == PlatformWindowInputEventType::OnMouseClick)
        {
            if (data_->Data.OnMouseClick.state != PlatformWindowButtonState::Pressed)
                return;
            Coord mx = data_->Data.OnMouseClick.mouseX;
            Coord my = data_->Data.OnMouseClick.mouseY;
            if (my >= BTN_Y && my < BTN_Y + BTN_H)
            {
                if (mx >= PLAY_X && mx < PLAY_X + PLAY_W)
                    startPlaying(sel);
                else if (mx >= STOP_X && mx < STOP_X + STOP_W)
                {
                    stopPlaying();
                    setMessage("stopped");
                    wnd->Repaint();
                }
                else if (mx >= BACK_X && mx < BACK_X + BACK_W)
                    wnd->Close();
                return;
            }
            for (int i = 0; i < nFiles && i < MAX_ROWS; i++)
            {
                if (my >= ROW_Y + i * ROW_H && my < ROW_Y + i * ROW_H + (ROW_H - 1))
                {
                    sel = i;
                    wnd->Repaint();
                    break;
                }
            }
            return;
        }
        if (data_->type != PlatformWindowInputEventType::OnKeyEvent)
            return;

        auto *key = data_->Data.OnKeyEvent.key;
        if (!key->isKeyDown)
            return;
        if (key->isEscape)
        {
            wnd->Close();
            return;
        }
        if (key->isArrowUp && sel > 0)
        {
            sel--;
            wnd->Repaint();
            return;
        }
        if (key->isArrowDown && sel + 1 < nFiles)
        {
            sel++;
            wnd->Repaint();
            return;
        }
        if (key->isEnter)
        {
            startPlaying(sel);
            return;
        }
        if (key->isChar && (key->theChar == 's' || key->theChar == 'S'))
        {
            stopPlaying();
            setMessage("stopped");
            wnd->Repaint();
        }
    }

    static void sizeStr(unsigned int n, char *out)
    {
        char tmp[12];
        int i = 0;
        if (n == 0)
            tmp[i++] = '0';
        while (n && i < 11)
        {
            tmp[i++] = (char)('0' + n % 10);
            n /= 10;
        }
        int at = 0;
        while (i--)
            out[at++] = tmp[i];
        out[at] = 0;
    }

    void drawButton(PlatformBitmap *target, int bx, const mchar *label, int bw,
                    PlatformDrawTextOptions &opts, bool filled)
    {
        target->FillRect(bx, BTN_Y, bw, BTN_H, dark, false);
        if (!filled)
            target->FillRect(bx + 1, BTN_Y + 1, bw - 2, BTN_H - 2, light, false);
        opts.foreground = filled ? light : dark;
        opts.horizontalAlign = PlatformAlign::Middle;
        target->DrawText(bx, BTN_Y, bw, BTN_H, label, &opts, false);
    }

    void OnPaint(PlatformDrawingContext *dc, PlatformBitmap *target)
    {
        if (!target)
            return;
        if (!dark)
            dark = dc->CreateColor(0xFF0000AA, nullptr, nullptr);
        if (!light)
            light = dc->CreateColor(0xFFE0E0FF, nullptr, nullptr);
        if (!font)
            font = dc->CreateFont(6, nullptr, false, false, false, nullptr, nullptr);
        if (!dark || !light || !font)
            return;

        // The floppy is read when the window opens, not on every paint.
        if (listingStale)
            refreshListing();

        Coord W = target->GetWidth();
        Coord H = target->GetHeight();
        target->FillRect(0, 0, W, H, light, false);

        PlatformDrawTextOptions opts{};
        opts.font = font;
        opts.foreground = dark;
        opts.horizontalAlign = PlatformAlign::Begin;
        opts.verticalAlign = PlatformAlign::Middle;

        target->DrawText(NAME_X, HEAD_Y, NAME_W, 10, "/mnt/fat/SOUND", &opts, false);
        target->DrawText(SIZE_X, HEAD_Y, SIZE_W, 10, "Bytes", &opts, false);
        target->FillRect(2, ROW_Y - 2, W - 4, 1, dark, false);

        if (nFiles == 0)
        {
            target->DrawText(NAME_X, ROW_Y, NAME_W, 10, "(no .MID files)", &opts, false);
        }
        for (int i = 0; i < nFiles && i < MAX_ROWS; i++)
        {
            Coord ry = ROW_Y + i * ROW_H;
            if (sel == i)
            {
                target->FillRect(2, ry, W - 4, ROW_H - 1, dark, false);
                opts.foreground = light;
            }
            else
            {
                opts.foreground = dark;
            }
            char name[33];
            copyName(entries[i], name, sizeof(name));
            char sizebuf[12];
            sizeStr(entries[i].size, sizebuf);
            target->DrawText(NAME_X, ry, NAME_W, ROW_H - 1, (const mchar *)name, &opts, false);
            target->DrawText(SIZE_X, ry, SIZE_W, ROW_H - 1, (const mchar *)sizebuf, &opts, false);
        }

        // Status line: what is sounding, or why nothing is.
        opts.foreground = dark;
        opts.horizontalAlign = PlatformAlign::Begin;
        target->FillRect(2, STATUS_Y - 3, W - 4, 1, dark, false);
        if (playing)
        {
            char line[48];
            int at = 0;
            const char *prefix = "Playing: ";
            for (int i = 0; prefix[i]; i++)
                line[at++] = prefix[i];
            for (int i = 0; nowPlaying[i] && at < 46; i++)
                line[at++] = nowPlaying[i];
            line[at] = 0;
            target->DrawText(NAME_X, STATUS_Y, W - NAME_X * 2, 10, (const mchar *)line, &opts, false);
        }
        else
        {
            target->DrawText(NAME_X, STATUS_Y, W - NAME_X * 2, 10,
                             message[0] ? (const mchar *)message : "Stopped", &opts, false);
        }

        drawButton(target, PLAY_X, "Play", PLAY_W, opts, playing);
        drawButton(target, STOP_X, "Stop", STOP_W, opts, false);
        drawButton(target, BACK_X, "Back", BACK_W, opts, false);
    }
};
