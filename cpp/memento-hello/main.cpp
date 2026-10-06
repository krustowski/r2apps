#include <r2/gfx.hpp>
#include <r2/fs.hpp>
#include <r2/heap.hpp>
#include <r2/process.hpp>
#include <r2/time.hpp>

#include "ui/platform/impl/UIImpl.h"
#include "ui/platform/PlatformWindow.h"
#include "ui/platform/PlatformKey.h"
#include "ui/platform/PlatformDrawingContext.h"
#include "ui/platform/PlatformBitmap.h"
#include "ui/platform/PlatformColor.h"
#include "ui/platform/PlatformFont.h"
#include "ui/platform/impl/r2/R2_WindowImpl.h"
#include "ui/platform/impl/r2/R2_UIRootImpl.h"

using namespace Memento;

#ifdef MEMENTO_BACKEND_R2
// The kernel calls that libc++r2 wraps — the mode switch, the clock, the
// task table, launching another ELF — are made through the library. What is
// left in this block is what only c/libcr2 has: its TCP/IP stack, and the
// handful of filesystem and sysinfo wrappers the windows below were written
// against.
extern "C"
{
    // ScNetStatus (0x38)
    struct NetStatus_T
    {
        unsigned char mac[6];
        unsigned char ip[4];
        unsigned char drv_active;
        unsigned char n_ports;
        unsigned short ports[16];
    } __attribute__((packed));
    long get_net_status(NetStatus_T *ns);

    // ScListMounts (0x2C)
    struct MountInfo_T
    {
        unsigned char path[32];
        unsigned char path_len;
        unsigned char fs_type;
    } __attribute__((packed));
    long list_mounts(MountInfo_T *buf);

    // ScListDirPath (0x2D)
    struct VfsDirEntry_T
    {
        unsigned char name[32];
        unsigned char name_len;
        unsigned char is_dir;
        unsigned int size;
    } __attribute__((packed));
    long list_dir_path(const unsigned char *path, VfsDirEntry_T *buf);

    // ScReadFile (0x20)
    long read_file(const unsigned char *name, unsigned char *buf);

    // ScChdir (0x2E)
    long chdir(const unsigned char *path);

    // ScRTC (0x02) — real-time clock
    struct RTC_raw
    {
        unsigned char seconds, minutes, hours, day, month;
        unsigned short year;
    } __attribute__((packed));
    long read_rtc(RTC_raw *rtc);

    // ScSysInfo (0x01) — system config read/write
    struct SysInfo_T
    {
        unsigned char system_name[32];
        unsigned char system_user[32];
        unsigned char system_path[32];
        unsigned char system_version[8];
        unsigned int system_path_cluster;
        unsigned int system_uptime;
        unsigned char ip_addr[4];
    } __attribute__((packed));
    long read_sysinfo(SysInfo_T *sysinfo);

// TCP networking (from libcr2/net.h) — types forward-declared to avoid pulling
// in the full libcr2 headers which redefine uint8_t etc.
#define CHAT_PORT_C 9000
#define MAX_SOCKETS_C 8

    struct TcpSocket_C
    {
        unsigned int id;
        int state; // SocketState enum: 0=CLOSED..3=ESTABLISHED..4=FIN_WAIT
        unsigned short local_port;
        unsigned short remote_port;
        unsigned char local_ip[4];
        unsigned char remote_ip[4];
        unsigned char rx_buffer[1024];
        unsigned char tx_buffer[1024];
        unsigned int rx_len;
        unsigned int tx_len;
        unsigned char used;
        unsigned int seq_num;
        unsigned int ack_num;
    } __attribute__((packed));

    struct Ipv4Header_C
    {
        unsigned char version;
        unsigned char dscp_ecn;
        unsigned short total_length;
        unsigned short identification;
        unsigned short flags_fragment_offset;
        unsigned char ttl;
        unsigned char protocol;
        unsigned short header_checksum;
        unsigned char source_addr[4];
        unsigned char destination_addr[4];
    } __attribute__((packed));

    struct TcpHeader_C
    {
        unsigned short source_port;
        unsigned short dest_port;
        unsigned int seq_num;
        unsigned int ack_num;
        unsigned short data_offset_reserved_flags;
        unsigned short window_size;
        unsigned short checksum;
        unsigned short urgent_pointer;
    } __attribute__((packed));

    struct NetDriver_C
    {
        int (*recv)(unsigned char *buf, unsigned int maxlen);
        void (*send_ip)(const unsigned char *ip_pkt, unsigned int len);
    };
    extern NetDriver_C net_drv;

    int net_driver_select(const unsigned char *name);
    int net_driver_bind_port(const unsigned char *name, unsigned short port);
    void net_get_local_ip(unsigned char ip[4]);
    void net_get_local_mac(unsigned char mac[6]);
    long send_eth_frame(const unsigned char *frame, unsigned int len);
    TcpSocket_C *tcp_connect(TcpSocket_C sockets[MAX_SOCKETS_C],
                             const unsigned char remote_ip[4], unsigned short remote_port,
                             unsigned short local_port, const unsigned char local_ip[4]);
    void on_tcp_packet(const unsigned char src_ip[4], const unsigned char dst_ip[4],
                       TcpHeader_C *tcp_header, const unsigned char *payload, unsigned int len,
                       TcpSocket_C sockets[MAX_SOCKETS_C]);
    unsigned short parse_ipv4_packet(const unsigned char *packet, Ipv4Header_C *header);
    unsigned short parse_tcp_packet(const unsigned char *packet, TcpHeader_C *header);
    // send_tcp_packet: retransmit SYN after ARP cache is populated
    void send_tcp_packet_c(TcpSocket_C *sock, const unsigned char *data,
                           unsigned int len, unsigned char flags) __asm__("send_tcp_packet");
    // read/write/close aliased to avoid clashing with any POSIX declarations
    unsigned int chat_sock_read(TcpSocket_C *sock, unsigned char *buf, unsigned int maxlen) __asm__("read");
    unsigned int chat_sock_write(TcpSocket_C *sock, const unsigned char *buf, unsigned int len) __asm__("write");
    void chat_sock_close(TcpSocket_C *sock) __asm__("close");
    // net_recv_nb: non-blocking receive — returns 0 immediately if no frame queued
    int net_recv_nb(unsigned char *buf, unsigned int maxlen);
    void net_arp_set(const unsigned char ip[4], const unsigned char mac[6]);
}
#endif

//
//  The desktop no longer closes to make room for the program it launches:
//  windows are opened over it and it stays where it is. It calls this, which
//  main() defines once every window class is in scope.
//
enum AppKind
{
    APP_CLOCK = 0,
    APP_SHELL,
    APP_NET,
    APP_MOUNT,
    APP_TASKS,
    APP_CHAT,
    APP_CALC,
    APP_IRC,
    APP_MIDI,
    APP_WEB,
    APP_EDITOR,
    APP_SNAKE,
    APP_MINES,
    APP_TELEGRAM,
    APP_VIDEO,
    APP_SPOTIFY,
};
static void openApp(int kind);
static bool openBrowserWindow(const char *url = nullptr);
static void openFileViewer(const char *path, unsigned int size);

//
//  Programs that want the whole screen --- the shell, Turbo C++ --- cannot run
//  in a window: they program the VGA themselves.  A window asks for one here;
//  the desktop closes, main() hands the screen over and waits for the program
//  to end, and the desktop comes back with the other windows where they were.
//  `args` is the whole command line, argv[0] included; the kernel splits it
//  at spaces.  Returns false when the request does not fit.
//
static bool launchProgram(const char *program, const char *args);
//  Why the last program could not be started, for the desktop to say.
static char g_launchError[64];

//  Esc on the desktop: the session ends and the login dialog comes back
//  (rather than Memento ending).  Set by the desktop before it closes.
static bool g_logout = false;

//  The Editor in a window of its own (windows/editor_window.cpp).
static bool openEditorWindow(const char *path);

//  A DOS program in the theM emulator, in a window of its own
//  (windows/them_window.cpp).  False when the window could not be made.
static bool openThemWindow(const char *path);

//  Draws the desktop again, wallpaper and all (after the file manager has
//  set a new wallpaper).  Nothing when there is no desktop.
static void repaintDesktop();

//  The clipboard: one line of text that every window in the process shares.
//  Ctrl+C in a window puts something here, Ctrl+V types it back in.  It is
//  plain bytes in the font's code page; nothing survives the process.
static char g_clipboard[1200];

//  And an image: the last PrintScreen, as the screen had it --- palette
//  indices and the palette they index.  Whichever of the two was put on the
//  clipboard last is what it holds.
static uint8_t *g_clipImage = nullptr;
static int g_clipImageW = 0, g_clipImageH = 0, g_clipColours = 16;
static uint8_t g_clipPalette[768];
static bool g_clipIsImage = false;

//  Or a GIF, as Telegram knows it: the file_id its Bot API gave for an
//  animation.  The Telegram window copies one from a message and pastes it
//  back in to send it again --- by that id, so nothing is uploaded.  Pasted
//  anywhere else it is the text put beside it.
static char g_clipGif[128];
static bool g_clipIsGif = false;

static bool clipboardSetImage(const uint8_t *pixels, int w, int h)
{
    size_t n = (size_t)w * (size_t)h;
    if (g_clipImage)
        r2::heap::kernel_deallocate(g_clipImage);
    g_clipImage = (uint8_t *)r2::heap::kernel_allocate(n);
    g_clipIsImage = false;
    g_clipIsGif = false;
    if (!g_clipImage)
        return false;
    memcpy(g_clipImage, pixels, n);
    g_clipImageW = w;
    g_clipImageH = h;
    g_clipColours = (int)MementoR2Impl::R2_Palette::Count();
    memcpy(g_clipPalette, MementoR2Impl::R2_Palette::Table(), 768);
    g_clipIsImage = true;
    return true;
}

static bool clipboardHasImage() { return g_clipIsImage && g_clipImage; }

//  PrintScreen, from the window manager (R2_UIRootImpl::PrintScreen).
static bool onPrintScreen(const uint8 *pixels, int32 width, int32 height)
{
    return clipboardSetImage(pixels, width, height);
}

static void clipboardSet(const char *s)
{
    g_clipIsImage = false;
    g_clipIsGif = false;
    int n = 0;
    while (s[n] && n < (int)sizeof(g_clipboard) - 1)
    {
        //  One line: a newline or a tab would mean something else where it
        //  is pasted (Enter sends, Tab moves on).
        char c = s[n];
        g_clipboard[n++] = (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
    }
    g_clipboard[n] = 0;
}

static const char *clipboardGet() { return g_clipboard; }

static void clipboardSetGif(const char *fileId, const char *text)
{
    clipboardSet(text);
    int n = 0;
    while (fileId[n] && n < (int)sizeof(g_clipGif) - 1)
    {
        g_clipGif[n] = fileId[n];
        n++;
    }
    g_clipGif[n] = 0;
    g_clipIsGif = n != 0;
}

//  The file_id of the GIF on the clipboard, or nullptr.
static const char *clipboardGif() { return g_clipIsGif ? g_clipGif : nullptr; }

//  Turbo C++ 23 (~/vxn/tcpp/r2), which r2_main's build_iso puts in /mnt/iso/bin
//  and the kernel finds there from any working directory.  With a path it
//  opens that file.  It runs in a window beside the others, never over the
//  whole screen: when the window cannot be made, g_launchError says why and
//  the caller shows it.
static bool openInEditor(const char *path)
{
    g_launchError[0] = 0;
    if (openEditorWindow(path))
        return !g_launchError[0];
    if (!g_launchError[0])
        strcpy(g_launchError, "Editor: no memory left for its window.");
    return false;
}

#include "windows/hello_window.cpp"
#include "windows/wallpaper.cpp"
#include "windows/login_window.cpp"
#include "windows/tasks_window.cpp"
#include "windows/net_window.cpp"
#include "windows/mount_window.cpp"
#include "windows/file_viewer_window.cpp"
#include "windows/chat_window.cpp"
#include "windows/calculator_window.cpp"
#include "windows/clock_window.cpp"
#include "windows/irc_window.cpp"
#include "windows/midi_window.cpp"
#include "windows/browser_window.cpp"
#include "windows/editor_window.cpp"
#include "windows/shell_window.cpp"
#include "windows/snake_window.cpp"
#include "windows/mines_window.cpp"
#include "windows/telegram_window.cpp"
#include "windows/video_window.cpp"
#include "windows/them_window.cpp"
#include "windows/spotify_window.cpp"
#include "windows/desktop_window.cpp"

//  Y to the taskbar's "save it?" after PrintScreen: the clipboard's picture as
//  a PNG in the RAM disk, /mnt/tmp, or on the floppy when that will not take
//  it; SCR00001.PNG and on, whichever number is free.  Says where it went.
static char g_saveMessage[64];

static bool saveScreenshotIn(const char *dir, const web::Buf &png)
{
    char path[40];
    for (int n = 1; n < 100000; n++)
    {
        web::scopy(path, dir, sizeof(path));
        web::scat(path, "/SCR", sizeof(path));
        char num[8];
        for (int i = 4, v = n; i >= 0; i--, v /= 10)
            num[i] = (char)('0' + v % 10);
        num[5] = 0;
        web::scat(path, num, sizeof(path));
        web::scat(path, ".PNG", sizeof(path));
        if (r2::fs::exists(r2::string_view(path, strlen(path))))
            continue;
        int64_t put = r2::fs::write_at(r2::string_view(path, strlen(path)),
                                       r2::const_byte_span(png.data, png.len), 0);
        if (put == (int64_t)png.len)
        {
            web::scopy(g_saveMessage, "Saved as ", sizeof(g_saveMessage));
            web::scat(g_saveMessage, path, sizeof(g_saveMessage));
            return true;
        }
        //  A short write: the disk is full.  What there is of it goes.
        if (put > 0)
            (void)r2::fs::remove(r2::string_view(path, strlen(path)));
        return false;
    }
    return false;
}

static const mchar *onSaveScreenshot()
{
    web::Buf png{true};
    if (!g_clipImage)
        web::scopy(g_saveMessage, "Nothing to save", sizeof(g_saveMessage));
    else if (!web::encodePng(g_clipImage, g_clipImageW, g_clipImageH, g_clipPalette, g_clipColours, png))
        web::scopy(g_saveMessage, "No memory to save the screenshot", sizeof(g_saveMessage));
    else if (!saveScreenshotIn("/mnt/tmp", png) && !saveScreenshotIn("/mnt/fat", png))
        web::scopy(g_saveMessage, "Could not save it: /mnt/tmp and the floppy are full", sizeof(g_saveMessage));
    return (const mchar *)g_saveMessage;
}

//
//  Opening and closing windows.
//
//  Each floating window owns an application object, and the platform window
//  carries a delete callback that takes it down with it: the root deletes a
//  window once it has closed, and that is where the object goes too. Nothing
//  is left for main() to reclaim.
//
static UIRootImpl *g_root = nullptr;
static UIRootImpl::PlatformWindowOptions g_appOpts{};

static PlatformWindow *g_desktopWnd = nullptr;
static void repaintDesktop()
{
    if (g_desktopWnd)
        g_desktopWnd->Repaint();
}
static struct
{
    bool pending;
    char program[16];
    char args[128];
} g_launch;
//  Booted with r2's graphics kernel, Memento draws on GRUB's framebuffer in
//  256 colours, and there is no text screen for a full-screen program to use:
//  the kernel's console is not shown on the framebuffer.
static bool onFramebuffer()
{
#ifdef MEMENTO_BACKEND_R2
    return MementoR2Impl::R2_Vga640x400::FramebufferAvailable();
#else
    return false;
#endif
}

static bool launchProgram(const char *program, const char *args)
{
    if (onFramebuffer())
    {
        strcpy(g_launchError, "Full-screen programs need the text-mode kernel.");
        return false;
    }
    if (!g_desktopWnd || strlen(program) >= sizeof(g_launch.program) || strlen(args) >= sizeof(g_launch.args))
        return false;
    strcpy(g_launch.program, program);
    strcpy(g_launch.args, args);
    g_launch.pending = true;
    g_desktopWnd->Close(); // ends the main loop; main() takes it from there
    return true;
}

static void deleteClock(void *p) { delete (ClockWindow *)p; }
static void deleteNet(void *p) { delete (NetWindow *)p; }
static void deleteMount(void *p) { delete (MountWindow *)p; }
static void deleteTasks(void *p) { delete (TasksWindow *)p; }
static void deleteChat(void *p) { delete (ChatWindow *)p; }
static void deleteCalc(void *p) { delete (CalculatorWindow *)p; }
static void deleteIRC(void *p) { delete (IRCWindow *)p; }
static void deleteMidi(void *p) { delete (MidiWindow *)p; }
static void deleteWeb(void *p) { delete (BrowserWindow *)p; }
static void deleteTelegram(void *p) { delete (TelegramWindow *)p; }
static void deleteVideo(void *p) { delete (VideoWindow *)p; }
static void deleteEditor(void *p) { delete (EditorWindow *)p; }

static bool openEditorWindow(const char *path)
{
    if (!g_root)
        return false;
    EditorWindow *w = new EditorWindow(path);
    if (!w)
        return false;
    if (w->failed())
    {
        strcpy(g_launchError, "Editor: ");
        size_t at = strlen(g_launchError);
        for (const char *e = w->why(); *e && at + 1 < sizeof(g_launchError); e++)
            g_launchError[at++] = *e;
        g_launchError[at] = 0;
        delete w;
        return true; // said why; the full screen would fail the same way
    }
    PlatformWindow *wnd =
        g_root->CreateWindow("Editor", EditorWindow::W, EditorWindow::H, EditorWindow::onEvent, w, &g_appOpts, deleteEditor, w);
    if (!wnd)
    {
        delete w; // tells the editor to go
        strcpy(g_launchError, "Editor: no memory left for its window.");
        return false;
    }
    w->SetWindow(wnd);
    wnd->SetVisible(true);
    return true;
}
static void deleteThem(void *p) { delete (ThemWindow *)p; }

static bool openThemWindow(const char *path)
{
    if (!g_root)
        return false;
    ThemWindow *w = new ThemWindow(path);
    if (!w)
        return false;
    PlatformWindow *wnd =
        g_root->CreateWindow(w->Title(), ThemWindow::W, ThemWindow::H, ThemWindow::onEvent, w, &g_appOpts, deleteThem, w);
    if (!wnd)
    {
        delete w;
        return false;
    }
    w->SetWindow(wnd);
    wnd->SetVisible(true);
    return true;
}

static void deleteViewer(void *p) { delete (FileViewerWindow *)p; }

//  The userland shell in a window of its own (windows/shell_window.cpp).
//  When it cannot be opened, g_launchError says why.
static void deleteSpotify(void *p) { delete (SpotifyWindow *)p; }
static void deleteShell(void *p) { delete (ShellWindow *)p; }
static void deleteSnake(void *p) { delete (SnakeWindow *)p; }
static void deleteMines(void *p) { delete (MinesWindow *)p; }
static bool openShellWindow()
{
    if (!g_root)
        return false;
    g_launchError[0] = 0;
    ShellWindow *w = new ShellWindow();
    if (!w)
    {
        strcpy(g_launchError, "Shell: no memory left for it.");
        return false;
    }
    if (w->failed())
    {
        strcpy(g_launchError, "Shell: ");
        size_t at = strlen(g_launchError);
        for (const char *e = w->why(); *e && at + 1 < sizeof(g_launchError); e++)
            g_launchError[at++] = *e;
        g_launchError[at] = 0;
        delete w;
        return false;
    }
    PlatformWindow *wnd =
        g_root->CreateWindow("Shell", ShellWindow::W, ShellWindow::H, ShellWindow::onEvent, w, &g_appOpts, deleteShell, w);
    if (!wnd)
    {
        delete w; // tells the shell to go
        strcpy(g_launchError, "Shell: no memory left for its window.");
        return false;
    }
    w->SetWindow(wnd);
    wnd->SetVisible(true);
    return true;
}

//
//  Window sizes, in the coordinates the windows are written in. The root adds
//  the frame and the title bar around them and finds them a place to sit.
//
static bool openBrowserWindow(const char *url)
{
    if (!g_root) return false;
    BrowserWindow *w = new BrowserWindow(url);
    if (!w || w->failed()) {
        web::scopy(g_launchError, w ? w->why() : "Web: no memory for its window.", sizeof(g_launchError));
        delete w; return false;
    }
    PlatformWindow *wnd = g_root->CreateWindow("Web", 310, 176,
        BrowserWindow::onEvent, w, &g_appOpts, deleteWeb, w);
    if (!wnd) {
        delete w; strcpy(g_launchError, "Web: no room for another window."); return false;
    }
    w->SetWindow(wnd); wnd->SetVisible(true); return true;
}

static void openApp(int kind)
{
    if (!g_root)
        return;

    PlatformWindow *wnd = nullptr;

    switch (kind)
    {
    case APP_SPOTIFY:
    {
        SpotifyWindow *w = new SpotifyWindow();
        if (!w || w->failed()) {
            web::scopy(g_launchError, w ? w->why() : "Spotify: no memory for its window.", sizeof(g_launchError));
            delete w;
            return;
        }
        wnd = g_root->CreateWindow("Spotify / Go", SpotifyWindow::W, SpotifyWindow::H,
            SpotifyWindow::onEvent, w, &g_appOpts, deleteSpotify, w);
        if (wnd) w->SetWindow(wnd);
        else { delete w; strcpy(g_launchError, "Spotify: no memory for its window."); }
        break;
    }
    case APP_SHELL:
        openShellWindow();
        return;
    case APP_CLOCK:
    {
        ClockWindow *w = new ClockWindow();
        wnd = g_root->CreateWindow("Clock", 108, 96, ClockWindow::onEvent, w, &g_appOpts, deleteClock, w);
        if (wnd)
            w->SetWindow(wnd);
        else
            delete w;
        break;
    }
    case APP_NET:
    {
        NetWindow *w = new NetWindow();
        wnd = g_root->CreateWindow("Network", NetWindow::W, NetWindow::H, NetWindow::onEvent, w, &g_appOpts, deleteNet, w);
        if (wnd)
            w->SetWindow(wnd);
        else
            delete w;
        break;
    }
    case APP_MOUNT:
    {
        MountWindow *w = new MountWindow();
        wnd = g_root->CreateWindow("Files", 290, 150, MountWindow::onEvent, w, &g_appOpts, deleteMount, w);
        if (wnd)
            w->SetWindow(wnd);
        else
            delete w;
        break;
    }
    case APP_TASKS:
    {
        TasksWindow *w = new TasksWindow();
        wnd = g_root->CreateWindow("Tasks", 290, 150, TasksWindow::onEvent, w, &g_appOpts, deleteTasks, w);
        if (wnd)
            w->SetWindow(wnd);
        else
            delete w;
        break;
    }
    case APP_CHAT:
    {
        ChatWindow *w = new ChatWindow();
        wnd = g_root->CreateWindow("Chat", 290, 150, ChatWindow::onEvent, w, &g_appOpts, deleteChat, w);
        if (wnd)
            w->SetWindow(wnd);
        else
            delete w;
        break;
    }
    case APP_CALC:
    {
        CalculatorWindow *w = new CalculatorWindow();
        wnd = g_root->CreateWindow("Calculator", 160, 104, CalculatorWindow::onEvent, w, &g_appOpts, deleteCalc, w);
        if (wnd)
            w->SetWindow(wnd);
        else
            delete w;
        break;
    }
    case APP_IRC:
    {
        IRCWindow *w = new IRCWindow();
        wnd = g_root->CreateWindow("IRC", 290, 150, IRCWindow::onEvent, w, &g_appOpts, deleteIRC, w);
        if (wnd)
            w->SetWindow(wnd);
        else
            delete w;
        break;
    }
    case APP_MIDI:
    {
        MidiWindow *w = new MidiWindow();
        wnd = g_root->CreateWindow("Music", 190, 130, MidiWindow::onEvent, w, &g_appOpts, deleteMidi, w);
        if (wnd)
            w->SetWindow(wnd);
        else
            delete w;
        break;
    }
    case APP_WEB:
        openBrowserWindow();
        return;
    case APP_EDITOR:
        openInEditor(nullptr);
        return;
    case APP_SNAKE:
    {
        SnakeWindow *w = new SnakeWindow();
        wnd = g_root->CreateWindow("Snake", SnakeWindow::W, SnakeWindow::H, SnakeWindow::onEvent, w, &g_appOpts, deleteSnake, w);
        if (wnd)
            w->SetWindow(wnd);
        else
            delete w;
        break;
    }
    case APP_MINES:
    {
        MinesWindow *w = new MinesWindow();
        wnd = g_root->CreateWindow("Minesweeper", MinesWindow::W, MinesWindow::H, MinesWindow::onEvent, w, &g_appOpts,
                                   deleteMines, w);
        if (wnd)
            w->SetWindow(wnd);
        else
            delete w;
        break;
    }
    case APP_VIDEO:
    {
        VideoWindow *w = new VideoWindow();
        wnd = g_root->CreateWindow("Video", VideoWindow::W, VideoWindow::H, VideoWindow::onEvent, w, &g_appOpts,
                                   deleteVideo, w);
        if (wnd)
            w->SetWindow(wnd);
        else
            delete w;
        break;
    }
    case APP_TELEGRAM:
    {
        TelegramWindow *w = new TelegramWindow();
        wnd = g_root->CreateWindow("Telegram", TelegramWindow::W, TelegramWindow::H, TelegramWindow::onEvent, w,
                                   &g_appOpts, deleteTelegram, w);
        if (wnd)
            w->SetWindow(wnd);
        else
            delete w;
        break;
    }
    default:
        return;
    }

    if (wnd)
        wnd->SetVisible(true);
}

//
//  The file browser asks for a viewer on the file it is sitting on. It opens
//  as a window of its own, over the browser.
//
static void openFileViewer(const char *path, unsigned int size)
{
    if (!g_root)
        return;
    FileViewerWindow *w = new FileViewerWindow(path, size);
    PlatformWindow *wnd = g_root->CreateWindow("File", 290, 150, FileViewerWindow::onEvent, w, &g_appOpts, deleteViewer, w);
    if (!wnd)
    {
        delete w;
        return;
    }
    w->SetWindow(wnd);
    wnd->SetVisible(true);
}

//
//  Hands the screen to a program and waits for it to end.  The kernel starts it
//  in the background and returns its task id; the wait watches the task table
//  for that id (and, for a kernel that reports none, for the name), a few
//  misses in a row counting as gone, since the table cannot always be read.
//
static void runForeground(const char *program, const char *args)
{
#ifdef MEMENTO_BACKEND_R2
    g_launchError[0] = 0;
    r2::gfx::restore_text_mode();
    r2::optional<uint8_t> pid = r2::spawn(program, args);
    if (!pid)
    {
        strcpy(g_launchError, "Could not start ");
        size_t at = strlen(g_launchError);
        for (size_t i = 0; program[i] && at + 1 < sizeof(g_launchError); i++)
            g_launchError[at++] = program[i];
        g_launchError[at] = 0;
        return;
    }

    //  The name as the task table has it: "TCPP    .ELF", first eight upper.
    char name8[9] = {};
    for (int i = 0; i < 8 && program[i] && program[i] != '.'; i++)
        name8[i] = (char)(program[i] >= 'a' && program[i] <= 'z' ? program[i] - 32 : program[i]);
    size_t nl = strlen(name8);

    int misses = 0;
    while (true)
    {
        r2::sleep(500);
        editorKeepAlive(); // the Editor windows' editors wait for us meanwhile
        spotifyKeepAlive();
        browserKeepAlive();
        shellKeepAlive();  // and so do the Shell windows' shells
        r2::vector<r2::TaskInfo> tasks = r2::tasks();
        if (tasks.empty())
            continue; // try_lock failed, keep waiting
        bool alive = false;
        for (size_t i = 0; i < tasks.size() && !alive; i++)
        {
            const r2::TaskInfo &t = tasks[i];
            if (t.status >= 4)
                continue; // crashed or dead
            if (*pid && t.id == *pid)
                alive = !memcmp(t.name, name8, nl);
            else if (!*pid)
                alive = !memcmp(t.name, name8, nl);
        }
        if (alive)
        {
            misses = 0;
            continue;
        }
        if (++misses >= 3)
            break; // three misses in a row: it has ended
    }
#else
    (void)program;
    (void)args;
#endif
}

//
// Entry point
//

#include "netmux.h"

extern "C" int main()
{
    //  Chat and IRC (c/libcr2) and the Web window (web/net_r2.cpp) each run a
    //  TCP/IP stack of their own on the one frame queue this process has:
    //  netmux sorts it between them.
    netmux_install_cr2();

    UIRootImpl *root = new UIRootImpl();
    if (!root || root->HasError())
        return 1;
    g_root = root;
    MementoR2Impl::PrintScreenHook = onPrintScreen;
    MementoR2Impl::SaveScreenshotHook = onSaveScreenshot;

    // The screen is 640x400. At 192 DPI a window measures half that in the
    // coordinates these windows are written in, so the layouts are unchanged
    // and everything — including the text — is drawn at twice the resolution.
    UIRootImpl::PlatformWindowOptions fullScreen{};
    fullScreen.initialVisible = true;
    fullScreen.useCustomDPI = true;
    fullScreen.customDPI = 192;
    fullScreen.undecorated = true;   // no frame: these cover the whole area
    fullScreen.hideOnTaskbar = true; // and they are not something to switch to

    g_appOpts = fullScreen;
    g_appOpts.undecorated = false;   // the root frames the floating windows
    g_appOpts.hideOnTaskbar = false; // and lists them along the bottom

    //  The landing screen and the login cover the whole screen, taskbar and
    //  all: 320x200 of these units on the VGA, and on the graphics kernel's
    //  framebuffer whatever it is (two pixels to a unit at this DPI).
    int32 screenPxW = 640, screenPxH = 400;
    MementoR2Impl::R2_Vga640x400::ScreenSize(screenPxW, screenPxH);
    const Coord fullW = Coord(screenPxW / 2.0), fullH = Coord(screenPxH / 2.0);

    // --- Landing screen ---
    HelloWindow *hw = new HelloWindow();
    PlatformWindow *wnd = root->CreateWindow(
        "Hello r2", fullW, fullH, HelloWindow::onEvent, hw, &fullScreen, nullptr, nullptr);
    if (!wnd)
        return 1;
    hw->SetWindow(wnd);
    wnd->SetVisible(true);
    root->EnterMainLoop();

    bool wantsLogin = hw->wantsNext;
    delete wnd;
    delete hw;
    resetWallpaperCache(); // its colours belonged to that window's context

    // --- Login, and the desktop sessions after it ---
    //
    // Esc on the desktop logs out: every window closes and the login dialog
    // comes back.  Leaving the login dialog (Esc, Cancel, its close box)
    // restarts the machine; on a kernel too old to do that, Memento ends as it
    // always did.
    while (wantsLogin)
    {
        bool wantsDesktop = false;
        {
            LoginWindow *lw = new LoginWindow();
            PlatformWindow *wnd2 = root->CreateWindow(
                "Login", fullW, fullH, LoginWindow::onEvent, lw, &fullScreen, nullptr, nullptr);
            if (wnd2)
            {
                lw->SetWindow(wnd2);
                wnd2->SetVisible(true);
                root->EnterMainLoop();
                wantsDesktop = lw->wantsDesktop;
                delete wnd2;
            }
            delete lw;
            resetWallpaperCache();
        }
        if (!wantsDesktop)
        {
            r2::reboot();
            break; // still here: a kernel without the call
        }

        // --- Desktop session ---
        //
        // One loop from here on: the desktop is the bottom window and stays open,
        // everything launched from it is a window over it, and the loop only ends
        // when the desktop itself closes. The shell is the exception — it wants
        // the screen in text mode — so it takes the desktop down, runs, and the
        // desktop comes back.
        while (wantsDesktop)
        {
            wantsDesktop = false;

            DesktopWindow *desk = new DesktopWindow();
            PlatformWindow *wnd3 = root->CreateWindow(
                "Desktop", 0, 0, DesktopWindow::onEvent, desk, &fullScreen, nullptr, nullptr);
            if (!wnd3)
            {
                delete desk;
                break;
            }
            desk->SetWindow(wnd3);
            wnd3->SetVisible(true);
            g_desktopWnd = wnd3;
            root->EnterMainLoop();

            g_desktopWnd = nullptr;
            delete wnd3;
            delete desk;
            resetWallpaperCache();

            if (g_launch.pending)
            {
                g_launch.pending = false;
                runForeground(g_launch.program, g_launch.args);
                wantsDesktop = true;
            }
        }

        if (!g_logout)
            break;
        g_logout = false;
    }

    g_root = nullptr;
    delete root; // on the framebuffer, that leaves the screen black

#ifdef MEMENTO_BACKEND_R2
    if (!onFramebuffer())
        r2::gfx::restore_text_mode();
#endif
    return 0;
}
