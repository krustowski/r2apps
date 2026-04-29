#include "ui/platform/impl/UIImpl.h"
#include "ui/platform/PlatformWindow.h"
#include "ui/platform/PlatformKey.h"
#include "ui/platform/PlatformDrawingContext.h"
#include "ui/platform/PlatformBitmap.h"
#include "ui/platform/PlatformColor.h"
#include "ui/platform/PlatformFont.h"

using namespace Memento;

#ifdef MEMENTO_BACKEND_R2
extern "C" {
    long set_video_mode(unsigned char mode);

    // ScListTasks (0x2F) — forward-declare to avoid FBInfo_T conflict with R2_LL.h
    struct TaskInfo_T {
        unsigned char id, mode, status, _pad;
        unsigned char name[16];
    } __attribute__((packed));
    long list_tasks(TaskInfo_T* buf, unsigned char max);

    // ScNetStatus (0x38)
    struct NetStatus_T {
        unsigned char  mac[6];
        unsigned char  ip[4];
        unsigned char  drv_active;
        unsigned char  n_ports;
        unsigned short ports[16];
    } __attribute__((packed));
    long get_net_status(NetStatus_T* ns);

    // ScListMounts (0x2C)
    struct MountInfo_T {
        unsigned char path[32];
        unsigned char path_len;
        unsigned char fs_type;
    } __attribute__((packed));
    long list_mounts(MountInfo_T* buf);

    // ScListDirPath (0x2D)
    struct VfsDirEntry_T {
        unsigned char name[32];
        unsigned char name_len;
        unsigned char is_dir;
        unsigned int  size;
    } __attribute__((packed));
    long list_dir_path(const unsigned char* path, VfsDirEntry_T* buf);

    // Heap checkpoint/restore for the Desktop navigation loop
    unsigned long r2_heap_checkpoint();
    void          r2_heap_restore(unsigned long cp);
}
#endif

// 
// Window 1 — Hello
// 

class HelloWindow {
public:
    bool wantsNext = false;

    static void onEvent(void* instance, struct PlatformWindowInterfaceInputEvent* data) {
        reinterpret_cast<HelloWindow*>(instance)->onEvent_(data);
    }

    void SetWindow(PlatformWindow* w) { wnd = w; }

private:
    PlatformWindow* wnd  = nullptr;
    PlatformColor*  bg   = nullptr;
    PlatformColor*  fg   = nullptr;
    PlatformFont*   font = nullptr;

    void onEvent_(struct PlatformWindowInterfaceInputEvent* data) {
        if (data->type == PlatformWindowInputEventType::OnPaint) {
            OnPaint(data->Data.OnPaint.ctx, data->Data.OnPaint.target);
        } else if (data->type == PlatformWindowInputEventType::OnKeyEvent) {
            auto* key = data->Data.OnKeyEvent.key;
            if (key->isEscape) wnd->Close();
            if (key->isEnter)  { wantsNext = true; wnd->Close(); }
        }
    }

    void OnPaint(PlatformDrawingContext* dc, PlatformBitmap* target) {
        if (!bg)   bg   = dc->CreateColor(0xFF1A1A2E, nullptr, nullptr);
        if (!fg)   fg   = dc->CreateColor(0xFFE0E0FF, nullptr, nullptr);
        if (!font) font = dc->CreateFont(16, nullptr, false, false, false, nullptr, nullptr);
        if (!bg || !fg || !font) return;

        Coord w = target->GetWidth();
        Coord h = target->GetHeight();

        target->FillRect(0, 0, w, h, bg, false);

        PlatformDrawTextOptions opts{};
        opts.font            = font;
        opts.foreground      = fg;
        opts.horizontalAlign = PlatformAlign::Middle;
        opts.verticalAlign   = PlatformAlign::Middle;

        target->DrawText(0, h / 4,      w, 32, "Hello r2!",       &opts, false);
        target->DrawText(0, h / 2 - 2,  w, 18, "Enter  -  login", &opts, false);
        target->DrawText(0, h / 2 + 18, w, 18, "ESC  -  quit",    &opts, false);
    }
};

// 
// Window 2 — Login dialog (username + password)
// 

class LoginWindow {
public:
    static void onEvent(void* instance, struct PlatformWindowInterfaceInputEvent* data) {
        reinterpret_cast<LoginWindow*>(instance)->onEvent_(data);
    }

    void SetWindow(PlatformWindow* w) { wnd = w; }

    bool wantsDesktop = false;

private:
    PlatformWindow* wnd   = nullptr;
    PlatformColor*  dark  = nullptr;
    PlatformColor*  light = nullptr;
    PlatformFont*   font  = nullptr;
    int             focus = 0;  // 0=login field, 1=password field, 2=OK, 3=Cancel

    static const int MAX_LEN = 63;
    char loginBuf[64] = {};
    char passBuf[64]  = {};
    int  loginLen     = 0;
    int  passLen      = 0;

    void onEvent_(struct PlatformWindowInterfaceInputEvent* data) {
        if (data->type == PlatformWindowInputEventType::OnPaint) {
            OnPaint(data->Data.OnPaint.ctx, data->Data.OnPaint.target);
            return;
        }
        if (data->type != PlatformWindowInputEventType::OnKeyEvent) return;
        auto* key = data->Data.OnKeyEvent.key;
        if (!key->isKeyDown) return;

        if (key->isEscape) { wnd->Close(); return; }

        // Text input — explicit branches to avoid reference-to-ternary aliasing issues
        if (key->isChar) {
            if (focus == 0 && loginLen < MAX_LEN) {
                loginBuf[loginLen++] = (char)key->theChar;
                loginBuf[loginLen]   = 0;
                wnd->Repaint();
            } else if (focus == 1 && passLen < MAX_LEN) {
                passBuf[passLen++] = (char)key->theChar;
                passBuf[passLen]   = 0;
                wnd->Repaint();
            }
            return;
        }
        if (key->isBackspace) {
            if      (focus == 0 && loginLen > 0) { loginBuf[--loginLen] = 0; wnd->Repaint(); }
            else if (focus == 1 && passLen  > 0) { passBuf[--passLen]   = 0; wnd->Repaint(); }
            return;
        }

        // Navigation (Tab removed — it generates isTab before isKeyDown is checked)
        if (key->isArrowLeft || key->isArrowRight) {
            if (focus >= 2) { focus = (focus == 2) ? 3 : 2; wnd->Repaint(); }
            return;
        }
        if (key->isArrowDown) {
            focus = (focus + 1) % 4; wnd->Repaint(); return;
        }
        if (key->isArrowUp) {
            focus = (focus + 3) % 4; wnd->Repaint(); return;
        }
        if (key->isEnter) {
            if      (focus == 0) { focus = 1; wnd->Repaint(); }
            else if (focus == 1) { focus = 2; wnd->Repaint(); }
            else if (focus == 2) { wantsDesktop = true; wnd->Close(); }  // OK
            else                 { wnd->Close(); }                         // Cancel
        }
    }

    void DrawInputField(PlatformBitmap* target, Coord bx, Coord by, Coord bw, Coord bh,
                        const char* buf, int len, bool focused, bool isPassword) {
        char display[66] = {};
        int i = 0;

        for (; i < len; i++) display[i] = isPassword ? '*' : buf[i];

        if (focused) display[i++] = '_';
        display[i] = 0;
        PlatformDrawTextOptions opts{};
        opts.font            = font;
        opts.horizontalAlign = PlatformAlign::Begin;
        opts.verticalAlign   = PlatformAlign::Middle;

        if (focused) {
            target->FillRect(bx, by, bw, bh, dark, false);
            opts.foreground = light;
        } else {
            target->FillRect(bx, by, bw, bh, dark, false);
            target->FillRect(bx + 1, by + 1, bw - 2, bh - 2, light, false);
            opts.foreground = dark;
        }

        target->DrawText(bx + 3, by, bw - 6, bh, (const mchar*)display, &opts, false);
    }

    void DrawButton(PlatformBitmap* target, Coord bx, Coord by, Coord bw, Coord bh,
                    const mchar* label, bool focused) {
        PlatformDrawTextOptions opts{};
        opts.font            = font;
        opts.horizontalAlign = PlatformAlign::Middle;
        opts.verticalAlign   = PlatformAlign::Middle;

        if (focused) {
            target->FillRect(bx, by, bw, bh, dark, false);
            opts.foreground = light;
        } else {
            target->FillRect(bx, by, bw, bh, dark, false);
            target->FillRect(bx + 1, by + 1, bw - 2, bh - 2, light, false);
            opts.foreground = dark;
        }

        target->DrawText(bx, by, bw, bh, label, &opts, false);
    }

    void OnPaint(PlatformDrawingContext* dc, PlatformBitmap* target) {
        if (!dark)  dark  = dc->CreateColor(0xFF0A0A20, nullptr, nullptr);
        if (!light) light = dc->CreateColor(0xFFE0E0FF, nullptr, nullptr);
        if (!font)  font  = dc->CreateFont(12, nullptr, false, false, false, nullptr, nullptr);
        if (!dark || !light || !font) return;

        Coord W = target->GetWidth();
        Coord H = target->GetHeight();

        target->FillRect(0, 0, W, H, dark, false);

        // Taskbar
        target->FillRect(0, H - 14, W, 1,  dark,  false);
        target->FillRect(0, H - 13, W, 13, light, false);

        // Dialog: outer border + light interior
        // outer x=40..280, y=52..148  inner x=42..278, y=54..146
        target->FillRect(40, 52, 240, 96, dark,  false);
        target->FillRect(42, 54, 236, 92, light, false);

        // Title bar separator at y=68, close button
        target->FillRect(42, 68, 236, 1,  dark, false);
        target->FillRect(266, 57, 10, 8,  dark, false);

        PlatformDrawTextOptions opts{};
        opts.font            = font;
        opts.foreground      = dark;
        opts.horizontalAlign = PlatformAlign::Middle;
        opts.verticalAlign   = PlatformAlign::Middle;

        target->DrawText(42,  54, 220, 14, "Login",         &opts, false);
        target->DrawText(0, H - 13, W, 13, "Login  -  r2", &opts, false);

        // Row labels (left-aligned)
        opts.horizontalAlign = PlatformAlign::Begin;
        target->DrawText(50, 74, 58, 14, "Login:",    &opts, false);
        target->DrawText(50, 93, 58, 14, "Password:", &opts, false);

        // Input fields
        DrawInputField(target, 110, 74, 160, 14, loginBuf, loginLen, focus == 0, false);
        DrawInputField(target, 110, 93, 160, 14, passBuf,  passLen,  focus == 1, true);

        // Buttons
        DrawButton(target, 100, 118, 44, 18, "OK",     focus == 2);
        DrawButton(target, 160, 118, 60, 18, "Cancel", focus == 3);
    }
};

// 
// Window 4 — Task Manager  (live data via ScListTasks 0x2F)
// 

class TasksWindow {
public:
    static void onEvent(void* instance, struct PlatformWindowInterfaceInputEvent* data) {
        reinterpret_cast<TasksWindow*>(instance)->onEvent_(data);
    }
    void SetWindow(PlatformWindow* w) { wnd = w; }

private:
    PlatformWindow* wnd    = nullptr;
    PlatformColor*  dark   = nullptr;
    PlatformColor*  light  = nullptr;
    PlatformFont*   font   = nullptr;
    int             sel    = 0;
    int             nLive  = 0;  // last known task count; key handlers use it

    static const char* statusStr(unsigned char s) {
        if (s == 0) return "Ready";
        if (s == 1) return "Running";
        if (s == 2) return "Idle";
        if (s == 3) return "Blocked";
        if (s == 4) return "Crashed";
        if (s == 5) return "Dead";
        return "?";
    }
    static const char* modeStr(unsigned char m) { return m ? "User" : "Kernel"; }

    static void pidStr(unsigned char n, char* out) {
        if (n >= 100) { out[0]='0'+n/100; out[1]='0'+(n/10)%10; out[2]='0'+n%10; out[3]=0; }
        else if (n >= 10) { out[0]='0'+n/10; out[1]='0'+n%10; out[2]=0; }
        else { out[0]='0'+n; out[1]=0; }
    }

    void onEvent_(struct PlatformWindowInterfaceInputEvent* data) {
        if (data->type == PlatformWindowInputEventType::OnPaint) {
            OnPaint(data->Data.OnPaint.ctx, data->Data.OnPaint.target);
            return;
        }

        if (data->type != PlatformWindowInputEventType::OnKeyEvent) return;

        auto* key = data->Data.OnKeyEvent.key;

        if (!key->isKeyDown) return;
        if (key->isEscape) { wnd->Close(); return; }
        if (key->isArrowUp)   { if (sel > 0)      { sel--; wnd->Repaint(); } return; }
        if (key->isArrowDown) { if (sel < nLive)   { sel++; wnd->Repaint(); } return; }
        if (key->isEnter && sel == nLive) { wnd->Close(); }
    }

    void OnPaint(PlatformDrawingContext* dc, PlatformBitmap* target) {
        if (!target) return;
        if (!dark)  dark  = dc->CreateColor(0xFF0A0A20, nullptr, nullptr);
        if (!light) light = dc->CreateColor(0xFFE0E0FF, nullptr, nullptr);
        if (!font)  font  = dc->CreateFont(12, nullptr, false, false, false, nullptr, nullptr);
        if (!dark || !light || !font) return;

        // Fetch live task list (max 10, 20 bytes each = 200 bytes on stack)
        TaskInfo_T buf[10];
        int n = (int)list_tasks(buf, 10);
        if (n < 0) n = 0;
        nLive = n;
        if (sel > nLive) sel = nLive;

        Coord W = target->GetWidth();
        Coord H = target->GetHeight();

        target->FillRect(0, 0, W, H, dark, false);
        target->FillRect(0, H - 14, W,  1, dark,  false);
        target->FillRect(0, H - 13, W, 13, light, false);

        // Window chrome — tall enough for 10 rows + header + back button
        // Outer y=8..182, inner y=10..180
        target->FillRect(5,  8,  310, 175, dark,  false);
        target->FillRect(7,  10, 306, 171, light, false);
        target->FillRect(7,  24, 306,   1, dark,  false);  // title separator

        PlatformDrawTextOptions opts{};
        opts.font            = font;
        opts.foreground      = dark;
        opts.horizontalAlign = PlatformAlign::Middle;
        opts.verticalAlign   = PlatformAlign::Middle;

        target->DrawText(7,  10, 280, 14, "Task Manager",   &opts, false);
        target->DrawText(0, H - 13, W, 13, "Tasks  -  r2", &opts, false);

        // Column headers  (x: PID=10 Name=44 Mode=198 Status=248)
        opts.horizontalAlign = PlatformAlign::Begin;
        target->DrawText(10,  26, 30,  12, "PID",    &opts, false);
        target->DrawText(44,  26, 150, 12, "Name",   &opts, false);
        target->DrawText(198, 26, 46,  12, "Mode",   &opts, false);
        target->DrawText(248, 26, 58,  12, "Status", &opts, false);
        target->FillRect(7, 38, 306, 1, dark, false);

        // Task rows — 12 px each, starting at y=40
        for (int i = 0; i < n; i++) {
            Coord ry = 40 + i * 12;
            if (sel == i) {
                target->FillRect(8, ry, 304, 11, dark, false);
                opts.foreground = light;
            } else {
                opts.foreground = dark;
            }
            char pidbuf[4];  pidStr(buf[i].id, pidbuf);
            char namebuf[17];
            for (int j = 0; j < 16; j++) namebuf[j] = (char)buf[i].name[j];

            namebuf[16] = 0;

            opts.horizontalAlign = PlatformAlign::Begin;
            target->DrawText(10,  ry, 30,  11, (const mchar*)pidbuf,              &opts, false);
            target->DrawText(44,  ry, 150, 11, (const mchar*)namebuf,             &opts, false);
            target->DrawText(198, ry, 46,  11, (const mchar*)modeStr(buf[i].mode),&opts, false);
            target->DrawText(248, ry, 58,  11, (const mchar*)statusStr(buf[i].status), &opts, false);
        }

        // Separator + Back button
        target->FillRect(7, 163, 306, 1, dark, false);
        bool backFocused = (sel == nLive);
        opts.horizontalAlign = PlatformAlign::Middle;
        opts.verticalAlign   = PlatformAlign::Middle;

        if (backFocused) {
            target->FillRect(120, 166, 80, 13, dark, false);
            opts.foreground = light;
        } else {
            target->FillRect(120, 166, 80, 13, dark, false);
            target->FillRect(121, 167, 78, 11, light, false);
            opts.foreground = dark;
        }

        target->DrawText(120, 166, 80, 13, "Back", &opts, false);
    }
};

// 
// Window — Network Status  (ScNetStatus 0x38)
// Shows IP, MAC, driver state and bound TCP port registry.
// 

class NetWindow {
public:
    static void onEvent(void* instance, struct PlatformWindowInterfaceInputEvent* data) {
        reinterpret_cast<NetWindow*>(instance)->onEvent_(data);
    }
    void SetWindow(PlatformWindow* w) { wnd = w; }

private:
    PlatformWindow* wnd   = nullptr;
    PlatformColor*  dark  = nullptr;
    PlatformColor*  light = nullptr;
    PlatformFont*   font  = nullptr;

    static char hexNibble(unsigned char n) { return n < 10 ? '0'+n : 'a'+(n-10); }

    static void byteToStr(unsigned char b, char* out) {
        if (b >= 100) { out[0]='0'+b/100; out[1]='0'+(b/10)%10; out[2]='0'+b%10; out[3]=0; }
        else if (b >= 10) { out[0]='0'+b/10; out[1]='0'+b%10; out[2]=0; }
        else { out[0]='0'+b; out[1]=0; }
    }

    static void u16ToStr(unsigned short n, char* out) {
        if (!n) { out[0]='0'; out[1]=0; return; }
        char t[6]; int i=0;
        while(n) { t[i++]='0'+n%10; n/=10; }
        for(int j=0;j<i;j++) out[j]=t[i-1-j]; out[i]=0;
    }

    // "a.b.c.d\0" — caller supplies buf[16]
    static void ipToStr(const unsigned char ip[4], char* buf) {
        int pos = 0;
        for (int i = 0; i < 4; i++) {
            char tmp[4]; byteToStr(ip[i], tmp);
            for (int j = 0; tmp[j]; j++) buf[pos++] = tmp[j];
            if (i < 3) buf[pos++] = '.';
        }
        buf[pos] = 0;
    }

    // "aa:bb:cc:dd:ee:ff\0" — caller supplies buf[18]
    static void macToStr(const unsigned char mac[6], char* buf) {
        for (int i = 0; i < 6; i++) {
            buf[i*3]   = hexNibble(mac[i] >> 4);
            buf[i*3+1] = hexNibble(mac[i] & 0xF);
            buf[i*3+2] = (i < 5) ? ':' : 0;
        }
        buf[17] = 0;
    }

    void onEvent_(struct PlatformWindowInterfaceInputEvent* data) {
        if (data->type == PlatformWindowInputEventType::OnPaint) {
            OnPaint(data->Data.OnPaint.ctx, data->Data.OnPaint.target);
            return;
        }
        if (data->type != PlatformWindowInputEventType::OnKeyEvent) return;
        auto* key = data->Data.OnKeyEvent.key;
        if (!key->isKeyDown) return;
        if (key->isEscape || key->isEnter) wnd->Close();
    }

    void OnPaint(PlatformDrawingContext* dc, PlatformBitmap* target) {
        if (!target) return;
        if (!dark)  dark  = dc->CreateColor(0xFF0A0A20, nullptr, nullptr);
        if (!light) light = dc->CreateColor(0xFFE0E0FF, nullptr, nullptr);
        if (!font)  font  = dc->CreateFont(12, nullptr, false, false, false, nullptr, nullptr);
        if (!dark || !light || !font) return;

        NetStatus_T ns{};
        get_net_status(&ns);

        Coord W = target->GetWidth(), H = target->GetHeight();
        target->FillRect(0,   0,   W,   H,  dark,  false);
        target->FillRect(0, H-14,  W,   1,  dark,  false);
        target->FillRect(0, H-13,  W,  13,  light, false);
        target->FillRect(5,   8, 310, 175,  dark,  false);
        target->FillRect(7,  10, 306, 171,  light, false);
        target->FillRect(7,  24, 306,   1,  dark,  false);  // title sep

        PlatformDrawTextOptions opts{};
        opts.font            = font;
        opts.foreground      = dark;
        opts.horizontalAlign = PlatformAlign::Middle;
        opts.verticalAlign   = PlatformAlign::Middle;
        target->DrawText(7,    10, 280, 14, "Network",          &opts, false);
        target->DrawText(0,  H-13,   W, 13, "Net  -  r2",      &opts, false);

        // Section: Interface
        opts.horizontalAlign = PlatformAlign::Begin;

        // IP row
        char ipbuf[16]; ipToStr(ns.ip, ipbuf);
        target->DrawText(10, 28, 36, 12, "IP:",  &opts, false);
        target->DrawText(50, 28, 250, 12, (const mchar*)ipbuf, &opts, false);

        // MAC row
        char macbuf[18]; macToStr(ns.mac, macbuf);
        target->DrawText(10, 42, 36, 12, "MAC:", &opts, false);
        target->DrawText(50, 42, 250, 12, (const mchar*)macbuf, &opts, false);

        // Driver row
        target->DrawText(10, 56, 60, 12, "Driver:", &opts, false);
        target->DrawText(76, 56, 220, 12,
            ns.drv_active ? "Active" : "Inactive", &opts, false);

        // Separator before port table
        target->FillRect(7, 72, 306, 1, dark, false);

        // Ports section header
        target->DrawText(10, 75, 70, 12, "TCP ports:", &opts, false);

        if (ns.n_ports == 0) {
            target->DrawText(90, 75, 210, 12, "(none registered)", &opts, false);
        } else {
            // Render ports as a space-separated run, wrapping every 8 per row
            static const int COLS = 8;
            for (int i = 0; i < ns.n_ports && i < 16; i++) {
                int row = i / COLS, col = i % COLS;
                Coord ry = 75 + row * 13;
                Coord rx = (col == 0) ? 90 : 90 + col * 36;
                if (col == 0 && row > 0) {
                    // new row label blank
                    rx = 10;
                    // shift the column positions on rows > 0
                    rx = 10 + (i % COLS) * 36;
                }
                char pbuf[6]; u16ToStr(ns.ports[i], pbuf);
                target->DrawText(rx, ry, 35, 12, (const mchar*)pbuf, &opts, false);
            }
        }

        // Back button
        target->FillRect(7, 158, 306, 1, dark, false);
        target->FillRect(120, 161, 80, 13, dark, false);
        target->FillRect(121, 162, 78, 11, light, false);
        opts.foreground      = dark;
        opts.horizontalAlign = PlatformAlign::Middle;
        opts.verticalAlign   = PlatformAlign::Middle;
        target->DrawText(120, 161, 80, 13, "Back", &opts, false);
    }
};

// 
// Window — File Browser  (ScListMounts 0x2C + ScListDirPath 0x2D)
// Top level shows mount points; Enter drills into one; Backspace/[..] returns.
// 

class MountWindow {
public:
    MountWindow() { currentPath[0] = 0; mountRoot[0] = 0; }

    static void onEvent(void* instance, struct PlatformWindowInterfaceInputEvent* data) {
        reinterpret_cast<MountWindow*>(instance)->onEvent_(data);
    }
    void SetWindow(PlatformWindow* w) { wnd = w; }

private:
    PlatformWindow* wnd        = nullptr;
    PlatformColor*  dark       = nullptr;
    PlatformColor*  light      = nullptr;
    PlatformFont*   font       = nullptr;
    int             sel        = 0;
    int             scrollTop  = 0;
    bool            atMounts   = true;   // true = mount list, false = dir listing
    char            currentPath[128];
    char            mountRoot[33];       // path of the mount we entered

    MountInfo_T   mounts[8];
    int           nMounts  = 0;
    VfsDirEntry_T entries[32];
    int           nEntries = 0;

    static const int VIS = 10;

    static bool streq(const char* a, const char* b) {
        while (*a && *b && *a == *b) { a++; b++; }
        return *a == 0 && *b == 0;
    }
    int plen() { int i = 0; while (currentPath[i]) i++; return i; }

    static const char* fsType(unsigned char t) {
        if (t == 1) return "rootfs";
        if (t == 2) return "fat12";
        if (t == 3) return "iso9660";
        return "none";
    }

    // Enter a mount — copy its null-terminated path into currentPath & mountRoot
    void enterMount(int mi) {
        if (mi < 0 || mi >= nMounts) return;
        int nl = mounts[mi].path_len < 32 ? mounts[mi].path_len : 32;
        for (int i = 0; i < nl; i++) currentPath[i] = mountRoot[i] = (char)mounts[mi].path[i];
        currentPath[nl] = mountRoot[nl] = 0;
        if (nl == 0) { currentPath[0] = mountRoot[0] = '/'; currentPath[1] = mountRoot[1] = 0; }
        atMounts = false; sel = 0; scrollTop = 0;
    }

    // Go up: return to mount list if at mount root, else strip last path component
    void goUp() {
        if (streq(currentPath, mountRoot)) {
            atMounts = true; sel = 0; scrollTop = 0; return;
        }
        int len = plen(), i = len - 1;
        while (i > 0 && currentPath[i] != '/') i--;
        if (i == 0) currentPath[1] = 0; else currentPath[i] = 0;
        sel = 0; scrollTop = 0;
    }

    // Navigate into a subdirectory entry
    void goInto(int ei) {
        if (ei < 0 || ei >= nEntries || !entries[ei].is_dir) return;
        int cl = plen();
        int nl = entries[ei].name_len < 32 ? entries[ei].name_len : 32;
        if (cl + 1 + nl >= 127) return;
        if (cl == 1) {
            for (int i = 0; i < nl; i++) currentPath[1+i] = (char)entries[ei].name[i];
            currentPath[1+nl] = 0;
        } else {
            currentPath[cl] = '/';
            for (int i = 0; i < nl; i++) currentPath[cl+1+i] = (char)entries[ei].name[i];
            currentPath[cl+1+nl] = 0;
        }
        sel = 0; scrollTop = 0;
    }

    static void u32str(unsigned int n, char* out) {
        if (!n) { out[0] = '0'; out[1] = 0; return; }
        char t[10]; int i = 0;
        while (n) { t[i++] = '0' + n % 10; n /= 10; }
        for (int j = 0; j < i; j++) out[j] = t[i-1-j];
        out[i] = 0;
    }

    void onEvent_(struct PlatformWindowInterfaceInputEvent* data) {
        if (data->type == PlatformWindowInputEventType::OnPaint) {
            OnPaint(data->Data.OnPaint.ctx, data->Data.OnPaint.target);
            return;
        }
        if (data->type != PlatformWindowInputEventType::OnKeyEvent) return;
        auto* key = data->Data.OnKeyEvent.key;
        if (!key->isKeyDown) return;
        if (key->isEscape) { wnd->Close(); return; }

        if (atMounts) {
            // listItems = nMounts; Back at sel==nMounts
            if (key->isArrowUp) {
                if (sel > 0) { sel--; if (sel < scrollTop) scrollTop = sel; }
                wnd->Repaint(); return;
            }
            if (key->isArrowDown) {
                if (sel < nMounts) { sel++; } // nMounts = Back index
                wnd->Repaint(); return;
            }
            if (key->isEnter) {
                if (sel == nMounts) { wnd->Close(); return; }
                enterMount(sel);
                wnd->Repaint();
            }
        } else {
            // listItems = 1 + nEntries ([..] + entries); Back at sel==1+nEntries
            int listItems = 1 + nEntries;
            if (key->isBackspace) { goUp(); wnd->Repaint(); return; }
            if (key->isArrowUp) {
                if (sel > 0) { sel--; if (sel < scrollTop) scrollTop = sel; }
                wnd->Repaint(); return;
            }
            if (key->isArrowDown) {
                if (sel < listItems) {
                    sel++;
                    if (sel < listItems && sel >= scrollTop + VIS) scrollTop = sel - VIS + 1;
                }
                wnd->Repaint(); return;
            }
            if (key->isEnter) {
                if (sel == listItems) { wnd->Close(); return; }
                if (sel == 0) goUp();                    // [..]
                else goInto(sel - 1);
                wnd->Repaint();
            }
        }
    }

    void drawChrome(PlatformBitmap* target, const mchar* title, const mchar* pathLine,
                    PlatformDrawTextOptions& opts, Coord W, Coord H) {
        target->FillRect(0,    0,   W,   H,  dark,  false);
        target->FillRect(0,  H-14,  W,   1,  dark,  false);
        target->FillRect(0,  H-13,  W,  13,  light, false);
        target->FillRect(5,   8,  310, 175,  dark,  false);
        target->FillRect(7,  10,  306, 171,  light, false);
        target->FillRect(7,  24,  306,   1,  dark,  false);
        target->FillRect(7,  37,  306,   1,  dark,  false);
        opts.horizontalAlign = PlatformAlign::Middle;
        opts.foreground      = dark;
        target->DrawText(7,    10, 280, 14, title,         &opts, false);
        target->DrawText(0,  H-13,   W, 13, "Files  -  r2", &opts, false);
        opts.horizontalAlign = PlatformAlign::Begin;
        target->DrawText(10,  25, 290, 12, pathLine, &opts, false);
    }

    void drawBack(PlatformBitmap* target, bool focused, PlatformDrawTextOptions& opts) {
        target->FillRect(7, 158, 306, 1, dark, false);
        if (focused) {
            target->FillRect(120, 161, 80, 13, dark, false);
            opts.foreground = light;
        } else {
            target->FillRect(120, 161, 80, 13, dark, false);
            target->FillRect(121, 162, 78, 11, light, false);
            opts.foreground = dark;
        }
        opts.horizontalAlign = PlatformAlign::Middle;
        opts.verticalAlign   = PlatformAlign::Middle;
        target->DrawText(120, 161, 80, 13, "Back", &opts, false);
    }

    void OnPaint(PlatformDrawingContext* dc, PlatformBitmap* target) {
        if (!target) return;
        if (!dark)  dark  = dc->CreateColor(0xFF0A0A20, nullptr, nullptr);
        if (!light) light = dc->CreateColor(0xFFE0E0FF, nullptr, nullptr);
        if (!font)  font  = dc->CreateFont(12, nullptr, false, false, false, nullptr, nullptr);
        if (!dark || !light || !font) return;

        Coord W = target->GetWidth(), H = target->GetHeight();
        PlatformDrawTextOptions opts{};
        opts.font          = font;
        opts.verticalAlign = PlatformAlign::Middle;

        if (atMounts) {
            // --- Mount list view ---
            int raw = (int)list_mounts(mounts);
            nMounts = raw > 0 ? raw : 0;
            if (sel > nMounts) sel = nMounts;

            drawChrome(target, "Files", "Mount Points", opts, W, H);

            if (nMounts == 0) {
                opts.horizontalAlign = PlatformAlign::Middle;
                opts.foreground      = dark;
                target->DrawText(8, 38, 304, 11, "(no mounts)", &opts, false);
            } else {
                for (int row = 0; row < VIS && row < nMounts; row++) {
                    Coord ry = 38 + row * 12;
                    bool isSel = (sel == row);
                    if (isSel) { target->FillRect(8, ry, 304, 11, dark, false); opts.foreground = light; }
                    else        { opts.foreground = dark; }
                    // Mount path (null-terminate)
                    char pb[33]; int pl = mounts[row].path_len < 32 ? mounts[row].path_len : 32;
                    for (int j = 0; j < pl; j++) pb[j] = (char)mounts[row].path[j]; pb[pl] = 0;
                    if (pl == 0) { pb[0] = '/'; pb[1] = 0; }
                    opts.horizontalAlign = PlatformAlign::Begin;
                    target->DrawText(9,   ry, 180, 11, (const mchar*)pb,              &opts, false);
                    target->DrawText(210, ry,  90, 11, (const mchar*)fsType(mounts[row].fs_type), &opts, false);
                }
            }
            drawBack(target, sel == nMounts, opts);

        } else {
            // --- Directory listing view ---
            int raw = (int)list_dir_path((const unsigned char*)currentPath, entries);
            if (raw < 0) raw = 0;
            nEntries = 0;
            for (int i = 0; i < raw; i++) {
                unsigned char nl = entries[i].name_len;
                if (nl == 1 && entries[i].name[0] == '.') continue;
                if (nl == 2 && entries[i].name[0] == '.' && entries[i].name[1] == '.') continue;
                if (nEntries != i) entries[nEntries] = entries[i];
                nEntries++;
            }
            int listItems = 1 + nEntries;  // [..] + entries; Back at sel==listItems
            if (sel > listItems) sel = listItems;

            drawChrome(target, "Files", (const mchar*)currentPath, opts, W, H);

            for (int row = 0; row < VIS; row++) {
                int vi = scrollTop + row;
                if (vi >= listItems) break;
                Coord ry = 38 + row * 12;
                bool isSel = (sel == vi);
                if (isSel) { target->FillRect(8, ry, 304, 11, dark, false); opts.foreground = light; }
                else        { opts.foreground = dark; }
                opts.horizontalAlign = PlatformAlign::Begin;
                if (vi == 0) {
                    target->DrawText(9, ry, 290, 11, "[..]", &opts, false);
                } else {
                    int ei = vi - 1;
                    char nb[33]; int nl = entries[ei].name_len < 32 ? entries[ei].name_len : 32;
                    for (int j = 0; j < nl; j++) nb[j] = (char)entries[ei].name[j]; nb[nl] = 0;
                    if (entries[ei].is_dir) {
                        target->DrawText(9,  ry,   8, 11, "/",              &opts, false);
                        target->DrawText(17, ry, 190, 11, (const mchar*)nb, &opts, false);
                    } else {
                        target->DrawText(17, ry, 175, 11, (const mchar*)nb, &opts, false);
                        char sb[12]; u32str(entries[ei].size, sb);
                        target->DrawText(222, ry, 78,  11, (const mchar*)sb, &opts, false);
                    }
                }
            }
            drawBack(target, sel == listItems, opts);
        }
    }
};

// 
// Window 5 — Desktop launcher
// 

class DesktopWindow {
public:
    static void onEvent(void* instance, struct PlatformWindowInterfaceInputEvent* data) {
        reinterpret_cast<DesktopWindow*>(instance)->onEvent_(data);
    }

    void SetWindow(PlatformWindow* w) { wnd = w; }
    bool wantsTasks = false;
    bool wantsMount = false;
    bool wantsNet   = false;

private:
    PlatformWindow* wnd   = nullptr;
    PlatformColor*  dark  = nullptr;
    PlatformColor*  light = nullptr;
    PlatformFont*   font  = nullptr;
    int             sel   = 0;  // 0=Clock, 1=Shell, 2=Net, 3=Mount, 4=Tasks

    // Icon layout: 5×32px icons, 20px gaps, centred in 296px inner width
    //   left_pad = (296 - 5*32 - 4*20) / 2 = 28
    static const int IX0 = 40, IX1 = 92, IX2 = 144, IX3 = 196, IX4 = 248;
    static const int IY  = 55;   // icon top y
    static const int LY  = 91;   // label top y  (IY + 32 + 4)

    // Draw a dark 36×36 selection outline (2 px extension on each side)
    void DrawSel(PlatformBitmap* t, Coord ix, Coord iy) {
        t->FillRect(ix - 2, iy - 2, 36, 36, dark, false);
    }

    void DrawClock(PlatformBitmap* t, Coord ix, Coord iy, bool s) {
        if (s) DrawSel(t, ix, iy);
        t->FillRect(ix,    iy,    32, 32, dark,  false);   // background
        t->FillRect(ix+3,  iy+3,  26, 26, light, false);   // clock face
        // Corner roundoff (remove light corners → circle approximation)
        t->FillRect(ix+3,  iy+3,  3, 3, dark, false);
        t->FillRect(ix+26, iy+3,  3, 3, dark, false);
        t->FillRect(ix+3,  iy+26, 3, 3, dark, false);
        t->FillRect(ix+26, iy+26, 3, 3, dark, false);
        // Tick marks at 12 / 3 / 6 / 9
        t->FillRect(ix+13, iy+4,  6, 2, dark, false);
        t->FillRect(ix+26, iy+13, 2, 6, dark, false);
        t->FillRect(ix+13, iy+26, 6, 2, dark, false);
        t->FillRect(ix+4,  iy+13, 2, 6, dark, false);
        // Hour hand (up) and minute hand (right), pivot dot
        t->FillRect(ix+15, iy+8,  2, 9, dark, false);
        t->FillRect(ix+16, iy+15, 8, 2, dark, false);
        t->FillRect(ix+15, iy+15, 2, 2, dark, false);
    }

    void DrawShell(PlatformBitmap* t, Coord ix, Coord iy, bool s) {
        if (s) DrawSel(t, ix, iy);
        t->FillRect(ix,   iy,   32, 32, dark,  false);     // terminal bg
        t->FillRect(ix+2, iy+2, 28,  6, light, false);     // title bar strip
        // Three window-control dots in the title bar
        t->FillRect(ix+4,  iy+3, 4, 4, dark, false);
        t->FillRect(ix+10, iy+3, 4, 4, dark, false);
        t->FillRect(ix+16, iy+3, 4, 4, dark, false);
        // ">_" prompt text
        PlatformDrawTextOptions opts{};
        opts.font            = font;
        opts.foreground      = light;
        opts.horizontalAlign = PlatformAlign::Begin;
        opts.verticalAlign   = PlatformAlign::Begin;
        t->DrawText(ix+4, iy+10, 24, 16, ">_", &opts, false);
    }

    void DrawNet(PlatformBitmap* t, Coord ix, Coord iy, bool s) {
        if (s) DrawSel(t, ix, iy);
        t->FillRect(ix,    iy,    32, 32, dark,  false);
        // Parabolic dish (opens right; each step's right edge = next step's left
        // edge so consecutive steps share a pixel column and stay connected):
        t->FillRect(ix+11, iy+3,  4,  2, light, false);  // top arm
        t->FillRect(ix+8,  iy+5,  4,  2, light, false);  // curve step
        t->FillRect(ix+5,  iy+7,  4,  2, light, false);  // curve step
        t->FillRect(ix+3,  iy+9,  3,  2, light, false);  // curve step
        t->FillRect(ix+2,  iy+11, 3,  5, light, false);  // apex (flat centre)
        t->FillRect(ix+3,  iy+16, 3,  2, light, false);  // curve step
        t->FillRect(ix+5,  iy+18, 4,  2, light, false);  // curve step
        t->FillRect(ix+8,  iy+20, 4,  2, light, false);  // curve step
        t->FillRect(ix+11, iy+22, 4,  2, light, false);  // bottom arm
        // Triangular base (narrow at top where dish meets mount, wide at foot):
        t->FillRect(ix+9,  iy+24, 6,  2, light, false);
        t->FillRect(ix+7,  iy+26, 8,  2, light, false);
        t->FillRect(ix+5,  iy+28, 12, 2, light, false);
        // Signal glyphs: "<" incoming (from outer, pointing toward dish),
        // ">" outgoing (from dish, pointing away); staggered in two columns.
        PlatformDrawTextOptions sopts{};
        sopts.font            = font;
        sopts.foreground      = light;
        sopts.horizontalAlign = PlatformAlign::Begin;
        sopts.verticalAlign   = PlatformAlign::Begin;
        t->DrawText(ix+17, iy+3,  12, 12, "<<", &sopts, false);  // incoming, upper
        t->DrawText(ix+17, iy+11, 12, 12, ">>", &sopts, false);  // outgoing, lower
    }

    void DrawMount(PlatformBitmap* t, Coord ix, Coord iy, bool s) {
        if (s) DrawSel(t, ix, iy);
        t->FillRect(ix,   iy,    32, 32, dark,  false);
        // Three stacked volume bars representing mount points
        t->FillRect(ix+3, iy+5,  26,  6, light, false);
        t->FillRect(ix+3, iy+14, 26,  6, light, false);
        t->FillRect(ix+3, iy+23, 26,  6, light, false);
        // Small label dot on the left of each bar
        t->FillRect(ix+5, iy+7,  4,   2, dark,  false);
        t->FillRect(ix+5, iy+16, 4,   2, dark,  false);
        t->FillRect(ix+5, iy+25, 4,   2, dark,  false);
    }

    void DrawTasks(PlatformBitmap* t, Coord ix, Coord iy, bool s) {
        if (s) DrawSel(t, ix, iy);
        t->FillRect(ix,    iy,    32, 32, dark,  false);
        // Four activity bars at varying heights (task manager graph)
        t->FillRect(ix+3,  iy+20, 4,  9, light, false);
        t->FillRect(ix+10, iy+12, 4, 17, light, false);
        t->FillRect(ix+17, iy+16, 4, 13, light, false);
        t->FillRect(ix+24, iy+8,  4, 21, light, false);
        // Baseline
        t->FillRect(ix+3,  iy+28, 26,  2, light, false);
    }

    void onEvent_(struct PlatformWindowInterfaceInputEvent* data) {
        if (data->type == PlatformWindowInputEventType::OnPaint) {
            OnPaint(data->Data.OnPaint.ctx, data->Data.OnPaint.target);
            return;
        }
        if (data->type != PlatformWindowInputEventType::OnKeyEvent) return;
        auto* key = data->Data.OnKeyEvent.key;
        if (!key->isKeyDown) return;
        if (key->isEscape) { wnd->Close(); return; }
        if (key->isArrowLeft  || key->isArrowUp)   { sel = (sel + 4) % 5; wnd->Repaint(); return; }
        if (key->isArrowRight || key->isArrowDown) { sel = (sel + 1) % 5; wnd->Repaint(); return; }
        if (key->isEnter) {
            if (sel == 2) wantsNet   = true;
            if (sel == 3) wantsMount = true;
            if (sel == 4) wantsTasks = true;
            wnd->Close();
        }
    }

    void OnPaint(PlatformDrawingContext* dc, PlatformBitmap* target) {
        if (!target) return;
        if (!dark)  dark  = dc->CreateColor(0xFF0A0A20, nullptr, nullptr);
        if (!light) light = dc->CreateColor(0xFFE0E0FF, nullptr, nullptr);
        if (!font)  font  = dc->CreateFont(12, nullptr, false, false, false, nullptr, nullptr);
        if (!dark || !light || !font) return;

        Coord W = target->GetWidth();
        Coord H = target->GetHeight();

        target->FillRect(0, 0, W, H, dark, false);

        // Taskbar
        target->FillRect(0, H - 14, W,  1, dark,  false);
        target->FillRect(0, H - 13, W, 13, light, false);

        // Dialog: outer border, light interior, title-bar separator
        target->FillRect(10, 22, 300, 120, dark,  false);
        target->FillRect(12, 24, 296, 116, light, false);
        target->FillRect(12, 38, 296,   1, dark,  false);

        PlatformDrawTextOptions opts{};
        opts.font            = font;
        opts.foreground      = dark;
        opts.horizontalAlign = PlatformAlign::Middle;
        opts.verticalAlign   = PlatformAlign::Middle;

        target->DrawText(12,     24, 270,  14, "Desktop",        &opts, false);
        target->DrawText(0,  H - 13,   W,  13, "Desktop  -  r2", &opts, false);

        // Icons
        DrawClock (target, IX0, IY, sel == 0);
        DrawShell (target, IX1, IY, sel == 1);
        DrawNet   (target, IX2, IY, sel == 2);
        DrawMount (target, IX3, IY, sel == 3);
        DrawTasks (target, IX4, IY, sel == 4);

        // Labels — 48px wide centred on icon centre
        target->DrawText(IX0 - 8, LY, 48, 16, "Clock", &opts, false);
        target->DrawText(IX1 - 8, LY, 48, 16, "Shell", &opts, false);
        target->DrawText(IX2 - 8, LY, 48, 16, "Net",   &opts, false);
        target->DrawText(IX3 - 8, LY, 48, 16, "Mount", &opts, false);
        target->DrawText(IX4 - 8, LY, 48, 16, "Tasks", &opts, false);
    }
};

// 
// Entry point — window objects are heap-allocated to keep the stack lean
// 

extern "C" int main() {
    UIRootImpl* root = new UIRootImpl();
    if (!root || root->HasError()) return 1;

    UIRootImpl::PlatformWindowOptions opts{};
    opts.initialVisible = true;
    opts.useCustomDPI   = true;
    opts.customDPI      = 96;

    // --- Window 1 ---
    HelloWindow* hw = new HelloWindow();
    PlatformWindow* wnd = root->CreateWindow(
        "Hello r2", 0, 0,
        HelloWindow::onEvent, hw,
        &opts, nullptr, nullptr);
    if (!wnd) return 1;
    hw->SetWindow(wnd);
    wnd->SetVisible(true);
    root->EnterMainLoop();

    // --- Window 2: Login ---
    if (hw->wantsNext) {
        LoginWindow* lw = new LoginWindow();
        PlatformWindow* wnd2 = root->CreateWindow(
            "Login", 0, 0,
            LoginWindow::onEvent, lw,
            &opts, nullptr, nullptr);
        if (wnd2) {
            lw->SetWindow(wnd2);
            wnd2->SetVisible(true);
            root->EnterMainLoop();
        }

        // --- Windows 3+: Desktop ↔ Tasks loop ---
        bool showDesktop = lw->wantsDesktop;
#ifdef MEMENTO_BACKEND_R2
        unsigned long heapMark = r2_heap_checkpoint();
#endif
        while (showDesktop) {
            showDesktop = false;
#ifdef MEMENTO_BACKEND_R2
            r2_heap_restore(heapMark);  // reclaim previous iteration's windows
#endif
            DesktopWindow* desk = new DesktopWindow();
            PlatformWindow* wnd3 = root->CreateWindow(
                "Desktop", 0, 0,
                DesktopWindow::onEvent, desk,
                &opts, nullptr, nullptr);
            if (!wnd3) break;
            desk->SetWindow(wnd3);
            wnd3->SetVisible(true);
            root->EnterMainLoop();

            if (desk->wantsNet) {
                NetWindow* nw = new NetWindow();
                PlatformWindow* wndN = root->CreateWindow(
                    "Network", 0, 0,
                    NetWindow::onEvent, nw,
                    &opts, nullptr, nullptr);
                if (wndN) {
                    nw->SetWindow(wndN);
                    wndN->SetVisible(true);
                    root->EnterMainLoop();
                }
                showDesktop = true;
            } else if (desk->wantsTasks) {
                TasksWindow* tw = new TasksWindow();
                PlatformWindow* wnd4 = root->CreateWindow(
                    "Tasks", 0, 0,
                    TasksWindow::onEvent, tw,
                    &opts, nullptr, nullptr);
                if (wnd4) {
                    tw->SetWindow(wnd4);
                    wnd4->SetVisible(true);
                    root->EnterMainLoop();
                }
                showDesktop = true;
            } else if (desk->wantsMount) {
                MountWindow* mw = new MountWindow();
                PlatformWindow* wnd5 = root->CreateWindow(
                    "Files", 0, 0,
                    MountWindow::onEvent, mw,
                    &opts, nullptr, nullptr);
                if (wnd5) {
                    mw->SetWindow(wnd5);
                    wnd5->SetVisible(true);
                    root->EnterMainLoop();
                }
                showDesktop = true;
            }
            // ESC on desktop → showDesktop stays false → exit loop
        }
    }

#ifdef MEMENTO_BACKEND_R2
    set_video_mode(0x03);
#endif
    return 0;
}
