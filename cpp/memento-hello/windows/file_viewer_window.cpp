//
// Window — File Viewer  (ScReadFile 0x20 + ScChdir 0x2E)
// Displays the text content of a file selected in MountWindow.
// read_file only searches one directory level, so we chdir to the parent
// dir first, then call read_file with just the filename component.
//

class FileViewerWindow
{
public:
    // 32 KB of visible content + 512-byte pad for last FAT sector overflow
    static const int MAX_FILE = 32768;
    static const int BUF_SIZE = MAX_FILE + 512;
    static const int MAX_LINES = 256;
    static const int VIS = 10;

    FileViewerWindow(const char *path, unsigned int size)
    {
        int pi = 0;
        while (path[pi] && pi < 127)
        {
            filePath[pi] = path[pi];
            pi++;
        }
        filePath[pi] = 0;

        nLines = 0;
        scrollTop = 0;
        fileBytes = 0;

        if (size == 0)
        {
            setMsg("(empty file)");
        }
        else if (size > (unsigned int)MAX_FILE)
        {
            setMsg("(file too large to display)");
        }
        else
        {
#ifdef MEMENTO_BACKEND_R2
            long ret = 0;

            // ISO9660: kernel read_file accepts the full absolute path directly
            // (try_iso9660_absolute strips the mount prefix and walks the directory).
            // FAT12: fat83() is a single-component converter, so we must chdir to
            // the parent directory and pass only the bare filename to read_file.
            {
                const char *iso_pfx = "/mnt/iso";
                int j = 0;
                while (iso_pfx[j] && path[j] == iso_pfx[j])
                    j++;
                bool is_iso = (!iso_pfx[j] && (path[j] == '/' || path[j] == 0));

                if (is_iso)
                {
                    ret = read_file((const unsigned char *)path, (unsigned char *)content);
                }
                else
                {
                    int last = 0;
                    for (int i = 0; path[i]; i++)
                        if (path[i] == '/')
                            last = i;
                    char parentBuf[128];
                    int pl = (last == 0) ? 1 : last;
                    for (int i = 0; i < pl; i++)
                        parentBuf[i] = path[i];
                    parentBuf[pl] = 0;
                    chdir((const unsigned char *)parentBuf);
                    ret = read_file((const unsigned char *)(path + last + 1), (unsigned char *)content);
                }
            }

            // libcr2 read_file: returns 1 on success, 0 on failure
            if (ret != 0)
            {
                fileBytes = size;
                // r2 stores every file it writes as a whole 512-byte sector,
                // zero-padded (Turbo C++ saves that way): the padding is not
                // part of the text, and would show as a line of dots.
                while (fileBytes > 0 && content[fileBytes - 1] == 0)
                    fileBytes--;
            }
            else
            {
                setMsg("(read error)");
            }
#else
            setMsg("(read_file not available on this platform)");
#endif
        }
        content[fileBytes] = 0;
        buildLines();
    }

    static void onEvent(void *instance, struct PlatformWindowInterfaceInputEvent *data)
    {
        reinterpret_cast<FileViewerWindow *>(instance)->onEvent_(data);
    }
    void SetWindow(PlatformWindow *w) { wnd = w; }

private:
    PlatformWindow *wnd = nullptr;
    PlatformColor *dark = nullptr;
    PlatformColor *light = nullptr;
    PlatformFont *font = nullptr;

    char filePath[128];
    unsigned int fileBytes = 0;
    int scrollTop = 0;
    int nLines = 0;

    // Static: only one FileViewerWindow open at a time; keeps heap object small.
    static char content[BUF_SIZE + 1];
    static int lineStart[MAX_LINES];
    static int lineLen[MAX_LINES];

    void setMsg(const char *msg)
    {
        int i = 0;
        while (msg[i] && i < MAX_FILE)
        {
            content[i] = msg[i];
            i++;
        }
        fileBytes = i;
    }

    void buildLines()
    {
        nLines = 0;
        int ls = 0, i = 0;
        while (i <= (int)fileBytes && nLines < MAX_LINES)
        {
            if (i == (int)fileBytes || content[i] == '\n')
            {
                int len = i - ls;
                while (len > 0 && content[ls + len - 1] == '\r')
                    len--;
                lineStart[nLines] = ls;
                lineLen[nLines] = len;
                nLines++;
                ls = i + 1;
            }
            i++;
        }
        if (nLines == 0)
        {
            lineStart[0] = 0;
            lineLen[0] = 0;
            nLines = 1;
        }
    }

    void clampScroll()
    {
        int maxTop = nLines - VIS;
        if (maxTop < 0)
            maxTop = 0;
        if (scrollTop > maxTop)
            scrollTop = maxTop;
        if (scrollTop < 0)
            scrollTop = 0;
    }

    void onEvent_(struct PlatformWindowInterfaceInputEvent *data)
    {
        if (data->type == PlatformWindowInputEventType::OnPaint)
        {
            OnPaint(data->Data.OnPaint.ctx, data->Data.OnPaint.target);
            return;
        }
        if (data->type == PlatformWindowInputEventType::OnMouseClick)
        {
            if (data->Data.OnMouseClick.state == PlatformWindowButtonState::Pressed)
                wnd->Close();
            return;
        }
        if (data->type != PlatformWindowInputEventType::OnKeyEvent)
            return;
        auto *key = data->Data.OnKeyEvent.key;
        if (!key->isKeyDown)
            return;
        if (key->isEscape || key->isEnter)
        {
            wnd->Close();
            return;
        }
        // F4 edits the file in Turbo C++.  The viewer goes: what it shows
        // would be out of date by the time the editor gives the screen back.
        if (key->isF && key->f == 4)
        {
            if (openInEditor(filePath))
                wnd->Close();
            return;
        }
        if (key->isArrowUp)
        {
            scrollTop--;
            clampScroll();
            wnd->Repaint();
            return;
        }
        if (key->isArrowDown)
        {
            scrollTop++;
            clampScroll();
            wnd->Repaint();
            return;
        }
        if (key->isPageUp)
        {
            scrollTop -= VIS;
            clampScroll();
            wnd->Repaint();
            return;
        }
        if (key->isPageDown)
        {
            scrollTop += VIS;
            clampScroll();
            wnd->Repaint();
            return;
        }
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

        Coord W = target->GetWidth(), H = target->GetHeight();

        // Client area only — the frame, the title and the taskbar entry are
        // the root's.
        target->FillRect(0, 0, W, H, light, false);
        target->FillRect(2, 13, W - 4, 1, dark, false);

        PlatformDrawTextOptions opts{};
        opts.font = font;
        opts.foreground = dark;
        opts.horizontalAlign = PlatformAlign::Middle;
        opts.verticalAlign = PlatformAlign::Middle;

        // Path on left, line/total on right of the subheader row
        opts.horizontalAlign = PlatformAlign::Begin;
        target->DrawText(3, 2, W - 60, 10, (const mchar *)filePath, &opts, false);

        if (nLines > VIS)
        {
            // "line / total" indicator, e.g. "12/47"
            char sbuf[16];
            int sn = 0;
            auto writeInt = [&](int v)
            {
                if (v == 0)
                {
                    sbuf[sn++] = '0';
                    return;
                }
                char tmp[6];
                int ti = 0;
                while (v > 0)
                {
                    tmp[ti++] = '0' + v % 10;
                    v /= 10;
                }
                for (int j = ti - 1; j >= 0; j--)
                    sbuf[sn++] = tmp[j];
            };
            writeInt(scrollTop + 1);
            sbuf[sn++] = '/';
            writeInt(nLines);
            sbuf[sn] = 0;
            opts.horizontalAlign = PlatformAlign::End;
            target->DrawText(3, 2, W - 6, 10, (const mchar *)sbuf, &opts, false);
        }

        opts.horizontalAlign = PlatformAlign::Begin;
        for (int row = 0; row < VIS; row++)
        {
            int li = scrollTop + row;
            if (li >= nLines)
                break;
            Coord ry = 16 + row * 10;
            char lineBuf[128];
            int ll = lineLen[li] < 127 ? lineLen[li] : 127;
            for (int j = 0; j < ll; j++)
            {
                char c = content[lineStart[li] + j];
                lineBuf[j] = (c >= 0x20 && c < 0x7F) ? c : '.';
            }
            lineBuf[ll] = 0;
            target->DrawText(3, ry, W - 6, 9, (const mchar *)lineBuf, &opts, false);
        }

        // Back button
        int backX = (F_COORD(W) - 80) / 2;
        int backY = F_COORD(H) - 13;
        target->FillRect(2, backY - 3, W - 4, 1, dark, false);
        target->FillRect(backX, backY, 80, 11, dark, false);
        target->FillRect(backX + 1, backY + 1, 78, 9, light, false);
        opts.foreground = dark;
        opts.horizontalAlign = PlatformAlign::Middle;
        target->DrawText(backX, backY, 80, 11, "Back", &opts, false);
        opts.horizontalAlign = PlatformAlign::Begin;
        target->DrawText(4, backY, 60, 11, "F4 Edit", &opts, false);
    }
};

char FileViewerWindow::content[FileViewerWindow::BUF_SIZE + 1];
int FileViewerWindow::lineStart[FileViewerWindow::MAX_LINES];
int FileViewerWindow::lineLen[FileViewerWindow::MAX_LINES];
