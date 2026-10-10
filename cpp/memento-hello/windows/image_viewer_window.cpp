// Images selected in Files: decoded once, then fitted to the client area at
// each paint. Each window owns its pixels, so several previews can stay open.
#include "../web/image.h"

class ImageViewerWindow
{
public:
    static const int W = 290, H = 150;

    static bool accepts(const char *path)
    {
        const char *ext = nullptr;
        for (const char *p = path; *p; p++)
            if (*p == '/')
                ext = nullptr;
            else if (*p == '.')
                ext = p;
        return ext && (web::ieq(ext, ".png") || web::ieq(ext, ".jpg") || web::ieq(ext, ".jpeg") ||
                       web::ieq(ext, ".jpe") || web::ieq(ext, ".gif") || web::ieq(ext, ".bmp"));
    }

    explicit ImageViewerWindow(const char *path)
    {
        web::scopy(filePath, path, sizeof(filePath));
        r2::string_view sv(path, strlen(path));
        auto size = r2::fs::size_of(sv);
        if (!size)
        {
            error = "Cannot read the file";
            return;
        }
        if (!*size)
        {
            error = "Empty image file";
            return;
        }
        // The decoder takes an int length. Keep the compressed input bounded
        // too, before asking the kernel heap for it.
        if (*size > 32u * 1024 * 1024)
        {
            error = "Image file too large";
            return;
        }
        web::Buf file{true};
        if (!file.reserve(*size))
        {
            error = "Out of memory";
            return;
        }
        while (file.len < *size)
        {
            int64_t got = r2::fs::read_at(sv, r2::byte_span(file.data + file.len, *size - file.len), file.len);
            if (got <= 0 || (uint64_t)got > *size - file.len)
            {
                error = "Cannot read the whole file";
                return;
            }
            file.len += (size_t)got;
        }
        if (!web::pictureSize(file.data, file.len, sourceW, sourceH))
        {
            error = "Unsupported or damaged image (PNG, JPEG, GIF, BMP)";
            return;
        }
        int32 sw = 640, sh = 400;
        MementoR2Impl::R2_Vga640x400::ScreenSize(sw, sh);
        // Retain enough detail for a maximized preview. decodePicture keeps
        // small images at their original size and enforces its pixel limit.
        error = web::decodePicture(file.data, file.len, sw, sh, (int)MementoR2Impl::R2_Palette::Count(),
                                   0x000000, picture);
    }

    ~ImageViewerWindow() { picture.release(); }

    static void onEvent(void *instance, struct PlatformWindowInterfaceInputEvent *data)
    {
        reinterpret_cast<ImageViewerWindow *>(instance)->onEvent_(data);
    }
    void SetWindow(PlatformWindow *w) { wnd = w; }

private:
    PlatformWindow *wnd = nullptr;
    PlatformColor *dark = nullptr, *light = nullptr, *black = nullptr;
    PlatformFont *font = nullptr;
    char filePath[256] = {};
    const char *error = nullptr;
    web::Picture picture;
    int sourceW = 0, sourceH = 0;
    int clientW = W, clientH = H;

    void onEvent_(struct PlatformWindowInterfaceInputEvent *data)
    {
        if (data->type == PlatformWindowInputEventType::OnPaint)
            OnPaint(data->Data.OnPaint.ctx, data->Data.OnPaint.target);
        else if (data->type == PlatformWindowInputEventType::OnKeyEvent)
        {
            auto *key = data->Data.OnKeyEvent.key;
            if (key->isKeyDown && (key->isEscape || key->isEnter))
                wnd->Close();
        }
        else if (data->type == PlatformWindowInputEventType::OnMouseClick &&
                 data->Data.OnMouseClick.state == PlatformWindowButtonState::Pressed)
        {
            int x = (int)COORD_VAL(data->Data.OnMouseClick.mouseX);
            int y = (int)COORD_VAL(data->Data.OnMouseClick.mouseY);
            int bx = (clientW - 80) / 2, by = clientH - 13;
            if (x >= bx && x < bx + 80 && y >= by && y < by + 11)
                wnd->Close();
        }
    }

    void paintPicture(PlatformBitmap *target)
    {
        auto *bm = static_cast<MementoR2Impl::R2_BitmapImpl *>(target);
        uint8 *px = bm->GetPixels();
        int bw = bm->GetRealWidth().intValue(), bh = bm->GetRealHeight().intValue();
        if (!px || !picture.px)
            return;
        // The surface is allocated in steps of 150 pixels. Use the window's
        // DPI, rather than bw / clientW, to locate the visible client area.
        int dpi = wnd->GetEffectiveDPI();
        if (dpi <= 0)
            dpi = 192;
        int x = 2 * dpi / 96, y = 16 * dpi / 96;
        int aw = (clientW - 4) * dpi / 96, ah = (clientH - 32) * dpi / 96;
        if (aw > bw - x)
            aw = bw - x;
        if (ah > bh - y)
            ah = bh - y;
        if (aw <= 0 || ah <= 0)
            return;
        int ow = picture.w, oh = picture.h;
        if (ow > aw)
        {
            oh = (int)((int64_t)oh * aw / ow);
            ow = aw;
        }
        if (oh > ah)
        {
            ow = (int)((int64_t)ow * ah / oh);
            oh = ah;
        }
        if (ow < 1)
            ow = 1;
        if (oh < 1)
            oh = 1;
        x += (aw - ow) / 2;
        y += (ah - oh) / 2;
        for (int row = 0; row < oh; row++)
        {
            const uint8_t *src = picture.px + (size_t)((int64_t)row * picture.h / oh) * picture.w;
            uint8 *dst = px + (size_t)(y + row) * bw + x;
            if (ow == picture.w)
                memcpy(dst, src, (size_t)ow);
            else
                for (int col = 0; col < ow; col++)
                    dst[col] = src[(int64_t)col * picture.w / ow];
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
        if (!black)
            black = dc->CreateColor(0xFF000000, nullptr, nullptr);
        if (!font)
            font = dc->CreateFont(6, nullptr, false, false, false, nullptr, nullptr);
        if (!dark || !light || !black || !font)
            return;
        Coord w = target->GetWidth(), h = target->GetHeight();
        clientW = (int)COORD_VAL(w);
        clientH = (int)COORD_VAL(h);
        target->FillRect(0, 0, w, h, light, false);
        target->FillRect(2, 13, w - 4, 1, dark, false);
        target->FillRect(2, 16, w - 4, h - 32, black, false);

        PlatformDrawTextOptions opts{};
        opts.font = font;
        opts.foreground = dark;
        opts.horizontalAlign = PlatformAlign::Begin;
        opts.verticalAlign = PlatformAlign::Middle;
        target->DrawText(3, 2, w - 6, 10, (const mchar *)filePath, &opts, false);
        if (error)
        {
            opts.foreground = light;
            opts.horizontalAlign = PlatformAlign::Middle;
            target->DrawText(4, 16, w - 8, h - 32, (const mchar *)error, &opts, false);
        }
        else
            paintPicture(target);

        int bx = (clientW - 80) / 2, by = clientH - 13;
        target->FillRect(2, by - 3, w - 4, 1, dark, false);
        target->FillRect(bx, by, 80, 11, dark, false);
        target->FillRect(bx + 1, by + 1, 78, 9, light, false);
        opts.foreground = dark;
        opts.horizontalAlign = PlatformAlign::Middle;
        target->DrawText(bx, by, 80, 11, "Back", &opts, false);
        if (picture.px)
        {
            char dimensions[32];
            snprintf(dimensions, sizeof(dimensions), "%dx%d", sourceW, sourceH);
            opts.horizontalAlign = PlatformAlign::Begin;
            target->DrawText(4, by, bx - 6, 11, (const mchar *)dimensions, &opts, false);
        }
    }
};
