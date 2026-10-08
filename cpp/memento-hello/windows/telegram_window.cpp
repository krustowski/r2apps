//
// Window — Telegram, as a bot
//
// Telegram has two doors.  The one its own apps use, MTProto, is a protocol
// of its own: its own key exchange, its own encryption, a login by phone
// number and SMS code.  The other is the Bot API, which is plain HTTPS and
// JSON on api.telegram.org --- and that the browser's engine (web/) already
// speaks.  So this window is a bot: ask @BotFather for one, type the token it
// gives you here, and anyone who writes to the bot shows up as a chat on the
// left.  What you type is sent back as the bot.
//
// The traffic is one request at a time on one web::Loader, the browser's:
//
//   getMe                   once, to check the token and learn the bot's name
//   getUpdates?timeout=20   a long poll: the server holds it until something
//                           arrives or 20 s pass (the loader gives up on a
//                           connection that says nothing for 30)
//   sendMessage             what you typed.  A poll in progress is dropped
//                           for it; the updates it would have brought come
//                           again, because the offset only moves past what
//                           has been read.
//
//   getFile, then the file  a photo someone sent: the size of it nearest 320
//                           pixels wide, drawn under its message in the
//                           screen's colours (web/image.h).  The last few
//                           photos are kept; older ones go back to [photo].
//                           A GIF the same way, every frame of it, moving.
//                           Telegram keeps a GIF as one only when it was sent
//                           as a file; the usual way it becomes a silent MP4
//                           of H.264.  For those Telegram's still comes
//                           first, then the MP4, which is decoded a few
//                           milliseconds at a time (web/mp4.h) and moves in
//                           place of the still.  Baseline profile only, which
//                           is what Giphy and Tenor serve: in Main or High the
//                           still stays, and the status line says why.  At
//                           most three GIFs move at once; older ones go still.
//
//   sendAnimation           a GIF pasted in (Copy GIF on a message, then
//                           Ctrl+V), sent again by its file_id: nothing is
//                           uploaded, and an MP4 one goes as well as any.
//
//   setMessageReaction      a reaction chosen from a message's menu.  A bot
//                           may put one standard emoji on a message; it is
//                           not told of its own, so the window keeps it.
//
// The poll names what it wants (allowed_updates): messages, channel posts
// and reactions --- the last only when asked for, and in groups only where
// the bot is an administrator.  Emoji are shown by a few letters (<3, +1):
// the font has none.
//
// Code: the "code" and "pre" entities of a message come in as marks inside
// its text (PRE_ON, PRE_OFF, CODE); a block is drawn in a box with its line
// breaks kept, inline code on a grey ground.  Going out, `inline` and
// ```block``` in what is typed become entities the same way Telegram's own
// apps make them.
//
// Each request is a new TLS handshake (Connection: close, as everything the
// loader does), which the long poll keeps down to one per 20 s when nothing
// is happening.
//
// The token is read from /mnt/tar/opt/memento/telegram.txt, a line of text
// shipped on the boot medium (put it in r2_main's iso/opt/memento/ before
// make build_iso).  Without one there, the window asks for it; a token typed
// in is kept in /mnt/fat/TELEGRAM.CFG, since the archive cannot be written,
// and used whenever the archive has none.
//
// Replies: Reply on a message's menu puts it over the input box, and what is
// sent next answers it (reply_parameters).  A reply that comes in has the
// start of what it answers on a faint line over it.
//
// Keys:  Enter sends    Shift+Enter  a line break (for ```code```)
//        Ctrl+Space, Alt+Space, right click  the menu on a message: 1-9 a
//                reaction, 0 none, R reply, C copy the text, G copy the
//                GIF; PgUp/PgDn move it to an older / newer message; Esc or
//                the same key closes it
//        Esc     not reply after all (then: close)
//        Tab / Shift+Tab  next / previous chat
//        Up/Down, PgUp/PgDn  scroll     Ctrl+T  change the token
//        Ctrl+V  paste (a PrintScreen or a copied GIF too: it is attached,
//                and Enter sends it with what is typed as its caption;
//                Backspace on an empty line takes it off again)
//                                       Ctrl+C  copy what is typed, or with
//                                               nothing typed the chat's
//                                               newest message
//        Esc  close
//
// Web and Telegram share the process's one web::r2Net() stack; the Chat and
// IRC windows run libcr2's, and netmux sorts the frames between the two.
//

#include "../web/doc.h"
#include "../web/image.h"
#include "../web/loader.h"
#include "../web/mp4.h"
#include "../web/net_r2.h"
#include "../web/png.h"
#include "ui/platform/impl/r2/R2_BitmapImpl.h"

//  Just enough JSON to read what the Bot API answers: a cursor over the text
//  that can skip a value, find a member of an object, walk an array and take
//  a string out already turned into the font's code page.  Nothing is built.
namespace tgjson {

static const char *ws(const char *p, const char *e)
{
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
        p++;
    return p;
}

//  Past the value at p; nullptr when it is not one.
static const char *skip(const char *p, const char *e)
{
    p = ws(p, e);
    if (p >= e)
        return nullptr;
    if (*p == '"')
    {
        for (p++; p < e; p++)
        {
            if (*p == '\\')
                p++;
            else if (*p == '"')
                return p + 1;
        }
        return nullptr;
    }
    if (*p == '{' || *p == '[')
    {
        char close = *p == '{' ? '}' : ']';
        p = ws(p + 1, e);
        if (p < e && *p == close)
            return p + 1;
        for (;;)
        {
            if (close == '}')
            {
                p = skip(p, e); // the key
                if (!p)
                    return nullptr;
                p = ws(p, e);
                if (p >= e || *p != ':')
                    return nullptr;
                p++;
            }
            p = skip(p, e);
            if (!p)
                return nullptr;
            p = ws(p, e);
            if (p < e && *p == ',')
            {
                p++;
                continue;
            }
            if (p < e && *p == close)
                return p + 1;
            return nullptr;
        }
    }
    //  A number, true, false or null.
    const char *s = p;
    while (p < e && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t')
        p++;
    return p > s ? p : nullptr;
}

//  The value of member `key` of the object at p, or nullptr.
static const char *member(const char *p, const char *e, const char *key)
{
    if (!p)
        return nullptr;
    p = ws(p, e);
    if (p >= e || *p != '{')
        return nullptr;
    p = ws(p + 1, e);
    size_t kl = strlen(key);
    while (p < e && *p == '"')
    {
        const char *k = p + 1;
        const char *ke = skip(p, e);
        if (!ke)
            return nullptr;
        p = ws(ke, e);
        if (p >= e || *p != ':')
            return nullptr;
        const char *v = ws(p + 1, e);
        if ((size_t)(ke - 1 - k) == kl && !memcmp(k, key, kl))
            return v;
        p = skip(v, e);
        if (!p)
            return nullptr;
        p = ws(p, e);
        if (p < e && *p == ',')
            p = ws(p + 1, e);
        else
            break;
    }
    return nullptr;
}

//  The first element of the array at p (nullptr when empty), and the one
//  after the element at p.
static const char *first(const char *p, const char *e)
{
    if (!p)
        return nullptr;
    p = ws(p, e);
    if (p >= e || *p != '[')
        return nullptr;
    p = ws(p + 1, e);
    return p < e && *p != ']' ? p : nullptr;
}

static const char *next(const char *p, const char *e)
{
    p = skip(p, e);
    if (!p)
        return nullptr;
    p = ws(p, e);
    if (p >= e || *p != ',')
        return nullptr;
    return ws(p + 1, e);
}

//  A number, or true/false, as its text.
static bool raw(const char *p, const char *e, char *out, size_t cap)
{
    if (!p || *p == '"' || *p == '{' || *p == '[')
        return false;
    const char *q = skip(p, e);
    if (!q)
        return false;
    size_t n = (size_t)(q - p) < cap - 1 ? (size_t)(q - p) : cap - 1;
    memcpy(out, p, n);
    out[n] = 0;
    return true;
}

static int hex(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

//  The character at p inside a string, unescaped and decoded from UTF-8;
//  p is moved past it.
static uint32_t nextCp(const char *&p, const char *e)
{
    uint32_t cp;
    if (*p == '\\' && p + 1 < e)
    {
        char c = p[1];
        p += 2;
        if (c == 'u' && p + 4 <= e)
        {
            cp = 0;
            for (int i = 0; i < 4; i++)
                cp = cp * 16 + (uint32_t)(hex(p[i]) < 0 ? 0 : hex(p[i]));
            p += 4;
            //  A surrogate pair is one character (an emoji, mostly).
            if (cp >= 0xD800 && cp < 0xDC00 && p + 6 <= e && p[0] == '\\' && p[1] == 'u')
            {
                uint32_t lo = 0;
                for (int i = 0; i < 4; i++)
                    lo = lo * 16 + (uint32_t)(hex(p[2 + i]) < 0 ? 0 : hex(p[2 + i]));
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                p += 6;
            }
        }
        else
            cp = c == 'n' ? '\n' : c == 't' ? ' ' : c == 'r' ? ' ' : c == 'b' || c == 'f' ? ' ' : (uint8_t)c;
        return cp;
    }
    uint8_t b = (uint8_t)*p++;
    int more = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : b >= 0xC0 ? 1 : 0;
    cp = more == 3 ? b & 7 : more == 2 ? b & 15 : more == 1 ? b & 31 : b;
    for (; more && p < e && ((uint8_t)*p & 0xC0) == 0x80; more--)
        cp = (cp << 6) | ((uint8_t)*p++ & 0x3F);
    return cp;
}

//  A byte to put into a string at a character position, as Telegram counts
//  them: in UTF-16 code units.  Sorted by `at`.
struct Mark
{
    long at;
    char byte;
};

//  The string at p, unescaped and decoded from UTF-8, one glyph per character
//  (web::glyphFor: what the font has no glyph for comes out as '?').  Line
//  breaks become spaces unless `lines`; `marks` go in where they say.
static bool str(const char *p, const char *e, char *out, size_t cap, bool lines = false, const Mark *marks = nullptr,
                int nMarks = 0)
{
    size_t at = 0;
    long u16 = 0;
    int mk = 0;
    out[0] = 0;
    if (!p || *p != '"')
        return false;
    for (p++; p < e && *p != '"';)
    {
        for (; mk < nMarks && marks[mk].at <= u16; mk++)
            if (at + 1 < cap)
                out[at++] = marks[mk].byte;
        uint32_t cp = nextCp(p, e);
        u16 += cp >= 0x10000 ? 2 : 1;
        uint8_t g = cp == '\n' ? (lines ? '\n' : ' ') : web::glyphFor(cp);
        if (g && at + 1 < cap)
            out[at++] = (char)g;
    }
    //  Whatever closes at the very end.  When the text was cut short, the
    //  last byte is given up for it rather than leave a block open.
    for (; mk < nMarks; mk++)
    {
        if (at + 1 >= cap && at)
            at--;
        if (at + 1 < cap)
            out[at++] = marks[mk].byte;
    }
    out[at] = 0;
    return true;
}

//  The code points of the string at p, up to cap of them; how many.
static int cps(const char *p, const char *e, uint32_t *out, int cap)
{
    int n = 0;
    if (!p || *p != '"')
        return 0;
    for (p++; p < e && *p != '"' && n < cap;)
        out[n++] = nextCp(p, e);
    return n;
}

} // namespace tgjson

class TelegramWindow
{
public:
    TelegramWindow() : loader(web::r2Net(), web::gatherEntropy, web::currentTime) {}

    static void onEvent(void *instance, struct PlatformWindowInterfaceInputEvent *data)
    {
        reinterpret_cast<TelegramWindow *>(instance)->onEvent_(data);
    }

    void SetWindow(PlatformWindow *w)
    {
        wnd = w;
        loadToken();
        if (token[0])
            startChatting();
    }

    ~TelegramWindow()
    {
        loader.cancel();
        for (Photo &p : photos)
            p.release();
    }

    static const int W = 300, H = 170;

private:
    static constexpr const char *SHIPPED_TOKEN = "/mnt/tar/opt/memento/telegram.txt";
    static constexpr const char *TYPED_TOKEN = "/mnt/fat/TELEGRAM.CFG";
    static const int TOKEN_CAP = 64;
    static const int IN_CAP = 200;
    static const int TEXT_CAP = 480;
    static const int MAX_MSGS = 96;
    static const int MAX_CHATS = 12;
    static const int MAX_OUT = 4;
    static const int CHATS_W = 72;

    PlatformWindow *wnd = nullptr;
    web::Loader loader;

    char token[TOKEN_CAP] = {};
    bool setup = true; // asking for the token
    char botName[40] = {};
    long long offset = 0;

    enum Request
    {
        RQ_NONE,
        RQ_GETME,
        RQ_POLL,
        RQ_SEND,
        RQ_FILEINFO, // getFile: where a photo is
        RQ_FILE,     // the photo itself
        RQ_SENDPHOTO, // a screenshot going out
        RQ_REACT,     // setMessageReaction
    } request = RQ_NONE;

    //  Formatting rides along in Msg::text as bytes glyphFor never gives:
    //  a code block between PRE_ON and PRE_OFF, drawn in a box with its line
    //  breaks kept; inline code between two CODEs.  '\n' is a line break.
    //  A reply starts with QUOTE and the message it answers, "Name: the
    //  start of it", up to a line break: drawn faint, behind a rule.
    static const char PRE_ON = '\x01', PRE_OFF = '\x02', CODE = '\x03', QUOTE = '\x04';

    //  The reactions offered, 1-9 in the picker.  Bots may use only the
    //  standard set, one at a time on a message.  The font has no emoji, so
    //  each has a few letters to be shown by.
    struct Reaction
    {
        uint32_t cp;       // its first code point, to know it again
        const char *utf8;  // as sent
        const char *label; // as shown
    };
    static constexpr Reaction REACTIONS[] = {
        {0x2764, "\xE2\x9D\xA4", "<3"},       {0x1F44D, "\xF0\x9F\x91\x8D", "+1"},
        {0x1F44E, "\xF0\x9F\x91\x8E", "-1"}, {0x1F525, "\xF0\x9F\x94\xA5", "fire"},
        {0x1F601, "\xF0\x9F\x98\x81", ":D"}, {0x1F914, "\xF0\x9F\xA4\x94", "hmm"},
        {0x1F622, "\xF0\x9F\x98\xA2", ":'("}, {0x1F389, "\xF0\x9F\x8E\x89", "yay"},
        {0x1F44C, "\xF0\x9F\x91\x8C", "ok"},
    };
    static const int N_REACTIONS = (int)(sizeof(REACTIONS) / sizeof(REACTIONS[0]));
    static const int OTHER_REACTION = N_REACTIONS; // anything else, shown as "*"
    static const int MAX_REACTS = 4;               // kinds of reaction kept per message

    //  Reactions on their way out: a message and what to set on it (-1: none).
    struct React
    {
        int chat;
        long long msgId;
        int8_t r;
    };
    static const int MAX_REACT_OUT = 4;
    React reactOut[MAX_REACT_OUT];
    int nReactOut = 0;

    //  The context menu on a message (Ctrl+Space, Alt+Space, right click):
    //  the message, by its number, -1 when the menu is closed; the item
    //  under the bar; its geometry as last painted, kept for the mouse.
    int menuSeq = -1;
    int menuSel = 0;
    bool menuMoved = false; // scroll its message into view at the next paint
    double menuL = 0, menuT = 0, menuW = 0, menuAnchorY = -1;
    enum
    {
        MI_REMOVE = N_REACTIONS, // the reactions come first, then these
        MI_SEP,
        MI_REPLY,
        MI_COPY,
        MI_COPY_GIF,
        MI_COUNT,
    };
    static constexpr const char *REACTION_NAMES[] = {"heart", "thumbs up", "thumbs down", "fire", "grin",
                                                     "thinking", "crying", "party", "OK"};
    //  The message each row of the message area showed, as last painted.
    static const int ROWS_CAP = 128;
    int rowSeq[ROWS_CAP];

    //  A screenshot pasted in (PNG), waiting for Enter; and one on its way,
    //  as the multipart body of sendPhoto.  One of each at a time.
    static constexpr const char *BOUNDARY = "r2memento-5c1e7f3a9b";
    web::Buf attachment{true};
    int attachW = 0, attachH = 0;
    web::Buf photoBody{true};
    bool photoQueued = false;
    int photoChat = -1;
    char photoCaption[IN_CAP + 1] = {};
    //  Or a GIF pasted in, by the file_id Telegram gave it: sent again with
    //  sendAnimation, nothing uploaded.
    static const int FILE_ID_CAP = 128;
    char attachGif[FILE_ID_CAP] = {};

    //  The message what is typed answers (Reply in the menu): its chat and
    //  Telegram's number for it, 0 when not answering anything.
    int replyChat = -1;
    long long replyId = 0;
    int replySeq = -1;
    uint64_t retryAt = 0;
    bool immediate = false;

    struct Chat
    {
        char id[24]; // as the API writes it; groups are negative
        char name[28];
        int unread;
    };
    Chat chats[MAX_CHATS];
    int nChats = 0;
    int cur = -1; // the chat on the right

    struct Msg
    {
        int chat;
        bool mine;
        int8_t photo;   // index into photos, or -1
        long long id;   // Telegram's message_id in its chat; 0 for one never sent
        int8_t myReact; // the bot's reaction, index into REACTIONS, or -1
        struct
        {
            int8_t r; // index into REACTIONS (or OTHER_REACTION), -1 unused
            uint8_t n;
        } reacts[MAX_REACTS]; // everyone else's
        char text[TEXT_CAP];  // "Name: what they said", in the font's code page
    };

    //  Photos: those still to fetch, oldest first, and the decoded ones,
    //  each kept for the message (by its number, msgTotal at the time) it
    //  belongs to.  `path` is filled in by getFile.
    static const int MAX_WANTED = 8, MAX_PHOTOS = 8;
    static const int PHOTO_MAX_H = 200; // pixels
    enum : uint8_t
    {
        W_STILL, // a photo, or the still Telegram has of a GIF
        W_GIF,   // a GIF file: every frame
        W_MP4,   // a GIF as Telegram keeps most of them: an H.264 MP4
    };
    struct Wanted
    {
        int seq;
        uint8_t kind;
        char fileId[FILE_ID_CAP];
        char path[160];
    };
    Wanted wanted[MAX_WANTED];
    int nWanted = 0;
    struct Photo
    {
        int seq = -1;        // -1: a free slot
        web::Picture pic;    // a photo, or a GIF's still: kept under its
        web::Animation anim; // frames, for when there are too many to keep
        int shown = -1;      // the frame of anim last painted, -1 when not on screen
        bool any() const { return pic.px || anim.px; }
        int w() const { return anim.px ? anim.w : pic.w; }
        int h() const { return anim.px ? anim.h : pic.h; }
        void release()
        {
            pic.release();
            anim.release();
            shown = -1;
        }
    };
    Photo photos[MAX_PHOTOS];
    //  The frames of all the GIFs on show, decoded, at most: every other one
    //  is left out of a GIF that would take more (web/image.h).
    static const size_t GIF_BUDGET = 2 * 1024 * 1024;
    //  A GIF (or MP4) file bigger than this is shown by Telegram's still.
    static const long GIF_MAX_BYTES = 3 * 1024 * 1024;
    //  GIFs that move at once, at most: the frames of older ones go, and
    //  they are shown still.  With GIF_BUDGET each, 6 MiB of a user heap
    //  every process shares.
    static const int MAX_ANIMATED = 3;
    //  The MP4 being decoded (a few milliseconds of it at every idle pass),
    //  and the message it is for.
    web::Mp4Animation mp4;
    int mp4Seq = -1;
    //  The GIFs (animations) of the last few messages, by file_id, for
    //  Copy GIF.
    static const int MAX_GIF_REFS = 16;
    struct GifRef
    {
        int seq = -1;
        char id[FILE_ID_CAP];
    };
    GifRef gifRefs[MAX_GIF_REFS];
    int nextGifRef = 0;
    double pxPerUnit = 2; // screen pixels a unit, as last painted
    double msgW = 200;    // the width of the messages, in units, as last painted
    double msgX = 0, msgTop = 0; // and where they start
    Msg msgs[MAX_MSGS];
    int msgTotal = 0; // ever added; the last MAX_MSGS of them are kept

    struct Out
    {
        int chat;
        long long replyTo;      // message_id answered, 0 for none
        char gif[FILE_ID_CAP];  // a GIF to send with text as its caption, "" for text
        char text[IN_CAP + 1];
    };
    Out outbox[MAX_OUT];
    int nOut = 0;

    char input[IN_CAP + 1] = {};
    int inputLen = 0;
    int scroll = 0; // wrapped lines up from the bottom
    char status[96] = {};

    // Paint resources and the geometry hit testing needs.
    PlatformColor *cBg = nullptr, *cText = nullptr, *cMine = nullptr, *cPane = nullptr, *cSel = nullptr,
                  *cSelText = nullptr, *cRule = nullptr, *cFaint = nullptr, *cCode = nullptr, *cPick = nullptr;
    PlatformFont *font = nullptr;
    double cw = 3, ch = 6, rowH = 7;
    int visibleRows = 1;

    // ── Token ─────────────────────────────────────────────────────────────────

    void loadToken()
    {
        if (!loadToken(SHIPPED_TOKEN))
            loadToken(TYPED_TOKEN);
    }

    bool loadToken(const char *path)
    {
        auto text = r2::fs::read_text(path, 512);
        if (!text)
            return false;
        const char *p = text->c_str();
        int n = 0;
        while (*p == ' ' || *p == '\t')
            p++;
        //  The file is written a whole sector at a time: the token ends at
        //  the first thing that cannot be in one.
        while (n < TOKEN_CAP - 1 && tokenChar(p[n]))
            n++;
        memcpy(token, p, (size_t)n);
        token[n] = 0;
        if (!validToken(token))
            token[0] = 0;
        return token[0] != 0;
    }

    static bool tokenChar(char c)
    {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == ':' || c == '_' ||
               c == '-';
    }

    //  "123456789:AAH...": the bot's number, a colon and a secret.
    static bool validToken(const char *t)
    {
        int digits = 0;
        while (t[digits] >= '0' && t[digits] <= '9')
            digits++;
        if (!digits || t[digits] != ':' || strlen(t + digits + 1) < 20)
            return false;
        for (const char *p = t; *p; p++)
            if (!tokenChar(*p))
                return false;
        return true;
    }

    void saveToken()
    {
        char buf[TOKEN_CAP + 2];
        web::scopy(buf, token, sizeof(buf));
        web::scat(buf, "\n", sizeof(buf));
        (void)r2::fs::write_text(TYPED_TOKEN, r2::string_view(buf, strlen(buf)));
    }

    void startChatting()
    {
        setup = false;
        botName[0] = 0;
        retryAt = 0;
        web::scopy(status, "Checking the token...", sizeof(status));
        idle(true);
        wnd->Repaint();
    }

    void backToSetup(const char *why)
    {
        loader.cancel();
        request = RQ_NONE;
        setup = true;
        idle(false);
        web::scopy(status, why, sizeof(status));
        web::scopy(input, token, sizeof(input));
        inputLen = (int)strlen(input);
        wnd->Repaint();
    }

    // ── Chats and messages ────────────────────────────────────────────────────

    int chatFor(const char *id, const char *name)
    {
        for (int i = 0; i < nChats; i++)
            if (!strcmp(chats[i].id, id))
            {
                if (name[0])
                    web::scopy(chats[i].name, name, sizeof(chats[i].name));
                return i;
            }
        //  Full: the last chat on the list makes room.  Its messages stay in
        //  the ring but belong to nobody and are no longer shown.
        int at;
        if (nChats < MAX_CHATS)
            at = nChats++;
        else
        {
            at = MAX_CHATS - 1;
            for (Msg &m : msgs)
                if (m.chat == at)
                    m.chat = -1;
        }
        web::scopy(chats[at].id, id, sizeof(chats[at].id));
        web::scopy(chats[at].name, name[0] ? name : id, sizeof(chats[at].name));
        chats[at].unread = 0;
        if (cur < 0)
            cur = at;
        return at;
    }

    void addMsg(int chat, bool mine, const char *who, const char *text, long long id = 0, const char *quote = "")
    {
        Msg &m = msgs[msgTotal % MAX_MSGS];
        if (msgTotal >= MAX_MSGS && m.photo >= 0)
        {
            photos[m.photo].release();
            photos[m.photo].seq = -1;
        }
        if (msgTotal >= MAX_MSGS && menuSeq == msgTotal - MAX_MSGS)
            menuSeq = -1; // the one the menu is on leaves the ring
        if (msgTotal >= MAX_MSGS && replySeq == msgTotal - MAX_MSGS)
            replySeq = -1; // still answered, but no longer shown above the input
        m.photo = -1;
        m.chat = chat;
        m.mine = mine;
        m.id = id;
        m.myReact = -1;
        for (auto &r : m.reacts)
            r.r = -1;
        m.text[0] = 0;
        if (quote[0])
        {
            char q[2] = {QUOTE, 0};
            web::scopy(m.text, q, sizeof(m.text));
            web::scat(m.text, quote, sizeof(m.text));
            web::scat(m.text, "\n", sizeof(m.text));
        }
        web::scat(m.text, who, sizeof(m.text));
        web::scat(m.text, ": ", sizeof(m.text));
        web::scat(m.text, text, sizeof(m.text));
        msgTotal++;
        if (chat == cur)
            scroll = 0;
        else if (chat >= 0 && !mine)
            chats[chat].unread++;
        // Someone else's message while the window is minimised or behind
        // others: its title bar and taskbar button go red until it is looked
        // at, as IRC's do. (The root ignores this for the window in front.)
        // What we sent, or failed to, is no news.
        if (!mine)
            wnd->SetAttention(true);
    }

    //  One Message object: a chat to put it in, who wrote it and what.
    void takeMessage(const char *m, const char *e)
    {
        using namespace tgjson;
        const char *chat = member(m, e, "chat");
        char id[24];
        if (!raw(member(chat, e, "id"), e, id, sizeof(id)))
            return;
        char name[28] = {}, last[28] = {};
        if (!str(member(chat, e, "title"), e, name, sizeof(name)))
        {
            str(member(chat, e, "first_name"), e, name, sizeof(name));
            if (str(member(chat, e, "last_name"), e, last, sizeof(last)) && last[0])
            {
                web::scat(name, " ", sizeof(name));
                web::scat(name, last, sizeof(name));
            }
        }
        const char *from = member(m, e, "from");
        char who[28] = {}, isBot[8] = {};
        str(member(from, e, "first_name"), e, who, sizeof(who));
        raw(member(from, e, "is_bot"), e, isBot, sizeof(isBot));
        bool mine = !strcmp(isBot, "true") && botName[0] && usernameIs(from, e);
        if (!who[0])
            web::scopy(who, name[0] ? name : "?", sizeof(who));

        char text[TEXT_CAP];
        tgjson::Mark marks[16];
        int nMarks = codeMarks(member(m, e, "entities"), e, marks, 16);
        bool plain = str(member(m, e, "text"), e, text, sizeof(text), true, marks, nMarks);
        const char *mediaLabel = wantMedia(m, e);
        if (!plain)
        {
            //  Not text: say what it was, and its caption if it has one.
            static const char *const kinds[][2] = {
                {"photo", "[photo]"},       {"sticker", "[sticker]"}, {"voice", "[voice message]"},
                {"video", "[video]"},       {"video_note", "[video]"}, {"animation", "[GIF]"},
                {"document", "[file]"},     {"audio", "[audio]"},     {"location", "[location]"},
                {"contact", "[contact]"},   {"poll", "[poll]"},       {"new_chat_members", "[joined]"},
                {"left_chat_member", "[left]"},
            };
            web::scopy(text, "[something this client cannot show]", sizeof(text));
            for (auto &k : kinds)
                if (member(m, e, k[0]))
                {
                    web::scopy(text, k[1], sizeof(text));
                    break;
                }
            if (mediaLabel)
                web::scopy(text, mediaLabel, sizeof(text));
            char caption[TEXT_CAP];
            nMarks = codeMarks(member(m, e, "caption_entities"), e, marks, 16);
            if (str(member(m, e, "caption"), e, caption, sizeof(caption), true, marks, nMarks) && caption[0])
            {
                web::scat(text, " ", sizeof(text));
                web::scat(text, caption, sizeof(text));
            }
        }
        char quote[64];
        quoteOf(member(m, e, "reply_to_message"), e, quote, sizeof(quote));
        addMsg(chatFor(id, name), mine, mine ? myName() : who, text, number(member(m, e, "message_id"), e), quote);
    }

    //  The message a reply answers, as the line over it: "Name: the start of
    //  it" ("" when it answers nothing).
    void quoteOf(const char *r, const char *e, char *out, size_t cap)
    {
        using namespace tgjson;
        out[0] = 0;
        if (!r)
            return;
        const char *from = member(r, e, "from");
        char who[28] = {}, isBot[8] = {}, what[64] = {};
        str(member(from, e, "first_name"), e, who, sizeof(who));
        raw(member(from, e, "is_bot"), e, isBot, sizeof(isBot));
        if (!strcmp(isBot, "true") && botName[0] && usernameIs(from, e))
            web::scopy(who, myName(), sizeof(who));
        if (!who[0])
            str(member(member(r, e, "chat"), e, "title"), e, who, sizeof(who));
        if (!str(member(r, e, "text"), e, what, sizeof(what)) && !str(member(r, e, "caption"), e, what, sizeof(what)))
            web::scopy(what, member(r, e, "animation") ? "[GIF]" : member(r, e, "photo") ? "[photo]" : "[...]",
                       sizeof(what));
        web::scopy(out, who[0] ? who : "?", cap);
        web::scat(out, ": ", cap);
        web::scat(out, what, cap);
    }

    static long long number(const char *p, const char *e)
    {
        char t[24];
        if (!tgjson::raw(p, e, t, sizeof(t)))
            return 0;
        long long v = 0;
        bool neg = t[0] == '-';
        for (const char *q = t + neg; *q >= '0' && *q <= '9'; q++)
            v = v * 10 + (*q - '0');
        return neg ? -v : v;
    }

    //  The code and pre entities of a text, as the marks that go into it:
    //  sorted by position, what closes before what opens at the same place.
    static int codeMarks(const char *ents, const char *e, tgjson::Mark *marks, int cap)
    {
        using namespace tgjson;
        int n = 0;
        for (const char *en = first(ents, e); en && n + 2 <= cap; en = next(en, e))
        {
            char type[8];
            if (!str(member(en, e, "type"), e, type, sizeof(type)))
                continue;
            bool pre = !strcmp(type, "pre");
            if (!pre && strcmp(type, "code"))
                continue;
            long off = (long)number(member(en, e, "offset"), e), len = (long)number(member(en, e, "length"), e);
            if (off < 0 || len <= 0)
                continue;
            marks[n++] = {off, pre ? PRE_ON : CODE};
            marks[n++] = {off + len, pre ? PRE_OFF : CODE};
        }
        //  Insertion sort; an opening mark (even index) goes after a closing
        //  one at the same position.
        auto opens = [](const Mark &m, int i) { return m.byte == PRE_ON || (m.byte == CODE && i % 2 == 0); };
        int order[16];
        for (int i = 0; i < n; i++)
            order[i] = i;
        for (int i = 1; i < n; i++)
            for (int j = i; j > 0; j--)
            {
                const Mark &a = marks[order[j - 1]], &b = marks[order[j]];
                bool swap = a.at > b.at || (a.at == b.at && opens(a, order[j - 1]) && !opens(b, order[j]));
                if (!swap)
                    break;
                int t = order[j];
                order[j] = order[j - 1];
                order[j - 1] = t;
            }
        Mark sorted[16];
        for (int i = 0; i < n; i++)
            sorted[i] = marks[order[i]];
        for (int i = 0; i < n; i++)
            marks[i] = sorted[i];
        return n;
    }

    //  A picture in message m, to be fetched for the message about to be
    //  added.  What to call it instead of what its kind says, or nullptr.
    //
    //  A photo: the size nearest 320 pixels wide that is not wider (the
    //  smallest when all are).  A GIF: the file, when Telegram kept it as
    //  one.  Mostly it did not: a GIF sent from its apps, or by a bot from an
    //  address, becomes a silent H.264 MP4 (an "animation" whose mime_type
    //  is video/mp4), which is past what this machine decodes, and shows as
    //  the still Telegram made of it.  A GIF sent as a file is a document.
    const char *wantMedia(const char *m, const char *e)
    {
        using namespace tgjson;
        const char *anim = member(m, e, "animation");
        const char *doc = anim ? anim : member(m, e, "document");
        if (doc)
        {
            char mime[32] = {}, id[FILE_ID_CAP];
            str(member(doc, e, "mime_type"), e, mime, sizeof(mime));
            bool gif = !strcmp(mime, "image/gif");
            if (!anim && !gif)
                return nullptr; // a file of some other kind
            if (anim && str(member(anim, e, "file_id"), e, id, sizeof(id)) && id[0])
            {
                GifRef &g = gifRefs[nextGifRef++ % MAX_GIF_REFS];
                g.seq = msgTotal;
                web::scopy(g.id, id, sizeof(g.id));
            }
            long long size = number(member(doc, e, "file_size"), e);
            bool fits = size <= GIF_MAX_BYTES;
            if (gif && fits && str(member(doc, e, "file_id"), e, id, sizeof(id)) && id[0])
            {
                want(id, W_GIF);
                return "[GIF]";
            }
            //  Telegram's still first, which is small and quick; then an MP4,
            //  to move in place of it once it is decoded.  One that cannot
            //  be (not Baseline H.264, say) leaves the still where it is.
            const char *thumb = member(doc, e, "thumbnail");
            if (!thumb)
                thumb = member(doc, e, "thumb"); // what the API called it before 6.6
            if (str(member(thumb, e, "file_id"), e, id, sizeof(id)) && id[0])
                want(id, W_STILL);
            if (!gif && fits && str(member(doc, e, "file_id"), e, id, sizeof(id)) && id[0])
                want(id, W_MP4);
            return fits ? "[GIF]" : "[GIF, too big: a still of it]";
        }
        const char *sizes = member(m, e, "photo");
        if (!sizes)
            return nullptr;
        char best[128] = {}, id[128], wText[12];
        long bestW = -1;
        for (const char *ps = first(sizes, e); ps; ps = next(ps, e))
        {
            if (!str(member(ps, e, "file_id"), e, id, sizeof(id)) || !raw(member(ps, e, "width"), e, wText, sizeof(wText)))
                continue;
            long w = 0;
            for (const char *p = wText; *p >= '0' && *p <= '9'; p++)
                w = w * 10 + (*p - '0');
            bool better = bestW < 0 || (w <= 320 && (bestW > 320 || w > bestW)) || (w > 320 && bestW > 320 && w < bestW);
            if (better)
            {
                bestW = w;
                web::scopy(best, id, sizeof(best));
            }
        }
        if (best[0])
            want(best, W_STILL);
        return nullptr;
    }

    void want(const char *fileId, uint8_t kind)
    {
        if (nWanted == MAX_WANTED)
        {
            for (int i = 1; i < nWanted; i++)
                wanted[i - 1] = wanted[i];
            nWanted--;
        }
        Wanted &w = wanted[nWanted++];
        w.seq = msgTotal;
        w.kind = kind;
        web::scopy(w.fileId, fileId, sizeof(w.fileId));
        w.path[0] = 0;
    }

    //  The GIF of message seq, by file_id, or nullptr.
    const char *gifOf(int seq) const
    {
        for (const GifRef &g : gifRefs)
            if (g.seq == seq && seq >= 0)
                return g.id;
        return nullptr;
    }

    // ── Reactions ─────────────────────────────────────────────────────────────

    //  The message `id` in the chat whose number the object at `chat` has,
    //  if it is still in the ring.
    Msg *findMsg(const char *chat, const char *e, long long id)
    {
        char cid[24];
        if (!id || !tgjson::raw(tgjson::member(chat, e, "id"), e, cid, sizeof(cid)))
            return nullptr;
        int c = -1;
        for (int i = 0; i < nChats; i++)
            if (!strcmp(chats[i].id, cid))
                c = i;
        int oldest = msgTotal > MAX_MSGS ? msgTotal - MAX_MSGS : 0;
        for (int k = msgTotal - 1; c >= 0 && k >= oldest; k--)
            if (msgs[k % MAX_MSGS].chat == c && msgs[k % MAX_MSGS].id == id)
                return &msgs[k % MAX_MSGS];
        return nullptr;
    }

    //  A ReactionType object: which of REACTIONS, OTHER_REACTION for a custom
    //  emoji, a paid reaction or one not on the list.
    static int reactionOf(const char *rt, const char *e)
    {
        uint32_t cp[2];
        if (tgjson::cps(tgjson::member(rt, e, "emoji"), e, cp, 2) > 0)
            for (int i = 0; i < N_REACTIONS; i++)
                if (REACTIONS[i].cp == cp[0])
                    return i;
        return OTHER_REACTION;
    }

    //  Count `n` more (or fewer) of reaction r on m; `absolute` sets it.
    static void countReact(Msg &m, int r, int n, bool absolute)
    {
        int at = -1, spare = -1;
        for (int i = 0; i < MAX_REACTS; i++)
        {
            if (m.reacts[i].r == r)
                at = i;
            else if (m.reacts[i].r < 0 && spare < 0)
                spare = i;
        }
        if (at < 0)
        {
            if (n <= 0 || spare < 0)
                return;
            at = spare;
            m.reacts[at].r = (int8_t)r;
            m.reacts[at].n = 0;
        }
        int v = (absolute ? 0 : m.reacts[at].n) + n;
        m.reacts[at].n = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        if (!m.reacts[at].n)
            m.reacts[at].r = -1;
    }

    //  A message_reaction update: someone changed theirs.  Every reaction
    //  they took off is one fewer, every one put on one more.
    void takeReaction(const char *u, const char *e)
    {
        using namespace tgjson;
        Msg *m = findMsg(member(u, e, "chat"), e, number(member(u, e, "message_id"), e));
        if (!m)
            return;
        for (const char *r = first(member(u, e, "old_reaction"), e); r; r = next(r, e))
            countReact(*m, reactionOf(r, e), -1, false);
        for (const char *r = first(member(u, e, "new_reaction"), e); r; r = next(r, e))
            countReact(*m, reactionOf(r, e), 1, false);
        wnd->SetAttention(true);
    }

    //  message_reaction_count: the totals of anonymous reactions (channels,
    //  groups with anonymous admins), which replace what was counted.
    void takeReactionCount(const char *u, const char *e)
    {
        using namespace tgjson;
        Msg *m = findMsg(member(u, e, "chat"), e, number(member(u, e, "message_id"), e));
        if (!m)
            return;
        for (auto &r : m->reacts)
            r.r = -1;
        for (const char *r = first(member(u, e, "reactions"), e); r; r = next(r, e))
            countReact(*m, reactionOf(member(r, e, "type"), e), (int)number(member(r, e, "total_count"), e), false);
    }

    //  What is under a message with reactions, e.g. "  <3 2  +1   me: fire";
    //  false when it has none.
    static bool reactLine(const Msg &m, char *out, size_t cap)
    {
        out[0] = 0;
        for (auto &r : m.reacts)
            if (r.r >= 0)
            {
                web::scat(out, "  ", cap);
                web::scat(out, r.r < N_REACTIONS ? REACTIONS[r.r].label : "*", cap);
                if (r.n > 1)
                {
                    web::scat(out, " ", cap);
                    web::scatInt(out, r.n, cap);
                }
            }
        if (m.myReact >= 0)
        {
            web::scat(out, "   me: ", cap);
            web::scat(out, REACTIONS[m.myReact].label, cap);
        }
        return out[0] != 0;
    }

    //  Set (or with r = -1 take off) the bot's reaction on message seq.
    void react(int seq, int r)
    {
        const Msg &m = msgs[seq % MAX_MSGS];
        if (!m.id || m.chat < 0)
        {
            web::scopy(status, "That message never reached Telegram.", sizeof(status));
            wnd->Repaint();
            return;
        }
        if (nReactOut == MAX_REACT_OUT)
        {
            web::scopy(status, "Still sending the last few reactions; wait a moment.", sizeof(status));
            wnd->Repaint();
            return;
        }
        reactOut[nReactOut++] = {m.chat, m.id, (int8_t)r};
        interruptPoll();
        web::scopy(status, "Reacting...", sizeof(status));
        wnd->Repaint();
    }

    void dropReact()
    {
        for (int i = 1; i < nReactOut; i++)
            reactOut[i - 1] = reactOut[i];
        if (nReactOut)
            nReactOut--;
    }

    // ── The context menu ──────────────────────────────────────────────────────

    //  On message seq (-1: the newest in the chat on the right).
    void openMenu(int seq, bool fromKey)
    {
        if (seq < 0)
        {
            int oldest = msgTotal > MAX_MSGS ? msgTotal - MAX_MSGS : 0;
            for (int k = msgTotal - 1; k >= oldest && seq < 0; k--)
                if (msgs[k % MAX_MSGS].chat == cur)
                    seq = k;
            if (seq < 0)
            {
                web::scopy(status, "No message here to open a menu on.", sizeof(status));
                wnd->Repaint();
                return;
            }
        }
        menuSeq = seq;
        menuMoved = fromKey;
        int mr = msgs[seq % MAX_MSGS].myReact;
        menuSel = mr >= 0 ? mr : 0;
        if (!menuEnabled(menuSel))
            menuSel = MI_COPY;
        wnd->Repaint();
    }

    void closeMenu()
    {
        menuSeq = -1;
        wnd->Repaint();
    }

    //  The message before (dir -1) or after (+1) the menu's in the chat on
    //  the right: the menu moves to it.
    void menuToMessage(int dir)
    {
        int oldest = msgTotal > MAX_MSGS ? msgTotal - MAX_MSGS : 0;
        for (int k = menuSeq + dir; k >= oldest && k < msgTotal; k += dir)
            if (msgs[k % MAX_MSGS].chat == cur)
            {
                openMenu(k, true);
                return;
            }
    }

    bool menuEnabled(int i) const
    {
        const Msg &m = msgs[menuSeq % MAX_MSGS];
        if (i < N_REACTIONS)
            return m.id != 0; // only what reached Telegram has a number to react to
        if (i == MI_REMOVE)
            return m.id != 0 && m.myReact >= 0;
        if (i == MI_REPLY)
            return m.id != 0 && m.chat >= 0;
        if (i == MI_COPY_GIF)
            return gifOf(menuSeq) != nullptr;
        return i == MI_COPY;
    }

    void moveMenuSel(int dir)
    {
        for (int n = 0; n < MI_COUNT; n++)
        {
            menuSel = (menuSel + dir + MI_COUNT) % MI_COUNT;
            if (menuEnabled(menuSel))
                return;
        }
    }

    //  An item's label, with its accelerator after a '&', and what goes on
    //  the right.
    void menuLabel(int i, char *label, size_t cap, const char **keys) const
    {
        *keys = "";
        label[0] = 0;
        if (i < N_REACTIONS)
        {
            char d[4] = {'&', (char)('1' + i), ' ', 0};
            web::scopy(label, d, cap);
            web::scat(label, REACTIONS[i].label, cap);
            web::scat(label, "  ", cap);
            web::scat(label, REACTION_NAMES[i], cap);
            if (msgs[menuSeq % MAX_MSGS].myReact == i)
                *keys = "(set)";
        }
        else if (i == MI_REMOVE)
            web::scopy(label, "&0 No reaction", cap);
        else if (i == MI_REPLY)
            web::scopy(label, "&Reply", cap);
        else if (i == MI_COPY)
        {
            web::scopy(label, "&Copy text", cap);
            *keys = "Ctrl+C";
        }
        else if (i == MI_COPY_GIF)
            web::scopy(label, "Copy &GIF", cap);
    }

    void pickMenu(int i)
    {
        if (menuSeq < 0 || i < 0 || i >= MI_COUNT || !menuEnabled(i))
            return;
        int seq = menuSeq;
        menuSeq = -1;
        if (i < N_REACTIONS)
            react(seq, i);
        else if (i == MI_REMOVE)
            react(seq, -1);
        else if (i == MI_REPLY)
            replyTo(seq);
        else if (i == MI_COPY)
        {
            copyMessage(msgs[seq % MAX_MSGS]);
            wnd->Repaint();
        }
        else if (i == MI_COPY_GIF)
        {
            clipboardSetGif(gifOf(seq), "[GIF]");
            web::scopy(status, "Copied the GIF: Ctrl+V in any chat here attaches it.", sizeof(status));
            wnd->Repaint();
        }
    }

    //  What is typed next answers message seq.
    void replyTo(int seq)
    {
        const Msg &m = msgs[seq % MAX_MSGS];
        replyChat = m.chat;
        replyId = m.id;
        replySeq = seq;
        if (cur != m.chat)
        {
            cur = m.chat;
            chats[cur].unread = 0;
            scroll = 0;
        }
        web::scopy(status, "Replying: Enter sends, Esc does not reply after all.", sizeof(status));
        wnd->Repaint();
    }

    void cancelReply()
    {
        replyChat = -1;
        replyId = 0;
        replySeq = -1;
    }

    void onMenuKey(PlatformKey *key)
    {
        bool ctrl = key->isLeftControl || key->isRightControl, alt = key->isLeftAlt || key->isRightAlt;
        if (key->isEscape || key->isTab || (key->isChar && key->theChar == ' ' && (ctrl || alt)))
            closeMenu(); // the key that opened it closes it again
        else if (key->isArrowDown)
            moveMenuSel(+1), wnd->Repaint();
        else if (key->isArrowUp)
            moveMenuSel(-1), wnd->Repaint();
        else if (key->isPageUp)
            menuToMessage(-1);
        else if (key->isPageDown)
            menuToMessage(+1);
        else if (key->isEnter || (key->isChar && key->theChar == ' '))
            pickMenu(menuSel);
        else if (ctrl && key->isChar && (key->theChar == 'c' || key->theChar == 'C'))
            pickMenu(MI_COPY);
        else if (key->isChar && !ctrl)
        {
            char c = (char)key->theChar;
            if (c >= '1' && c < '1' + N_REACTIONS)
                pickMenu(c - '1');
            else if (c == '0')
                pickMenu(MI_REMOVE);
            else if (c == 'c' || c == 'C')
                pickMenu(MI_COPY);
            else if (c == 'r' || c == 'R')
                pickMenu(MI_REPLY);
            else if (c == 'g' || c == 'G')
                pickMenu(MI_COPY_GIF);
        }
    }

    //  The item at (x, y), or -1.
    int menuItemAt(double x, double y) const
    {
        if (x < menuL || x >= menuL + menuW || y < menuT + 2)
            return -1;
        int i = (int)((y - menuT - 2) / rowH);
        return i < MI_COUNT ? i : -1;
    }

    void photoNotSent()
    {
        char t[IN_CAP + 16] = "[screenshot] ";
        web::scat(t, photoCaption, sizeof(t));
        addMsg(photoChat, true, "not sent", t);
        photoQueued = false;
        photoBody.release();
    }

    void dropWanted()
    {
        for (int i = 1; i < nWanted; i++)
            wanted[i - 1] = wanted[i];
        if (nWanted)
            nWanted--;
    }

    //  The photo in, decoded, and given to its message if that is still here.
    //  The slot for message seq's picture: the one it has already (a GIF's
    //  still, when its frames come), or a free one, or the one kept longest,
    //  emptied.  -1 when the message has left the ring.
    int slotFor(int seq)
    {
        int oldest = msgTotal > MAX_MSGS ? msgTotal - MAX_MSGS : 0;
        if (seq < oldest)
            return -1;
        int have = msgs[seq % MAX_MSGS].photo;
        if (have >= 0 && photos[have].seq == seq)
            return have;
        int slot = 0;
        for (int i = 0; i < MAX_PHOTOS; i++)
        {
            if (photos[i].seq < 0)
            {
                slot = i;
                break;
            }
            if (photos[i].seq < photos[slot].seq)
                slot = i;
        }
        Photo &p = photos[slot];
        if (p.seq >= oldest && msgs[p.seq % MAX_MSGS].photo == slot)
            msgs[p.seq % MAX_MSGS].photo = -1;
        p.release();
        p.seq = -1;
        return slot;
    }

    //  Where pictures are made to fit: the width of the messages.
    int pictureMaxW() const
    {
        int maxW = (int)((msgW - 2) * pxPerUnit);
        return maxW < 16 ? 16 : maxW;
    }

    //  A picture in, decoded, and given to its message if that is still here.
    void photoArrived(const uint8_t *data, size_t len)
    {
        const Wanted &w = wanted[0];
        int slot = slotFor(w.seq);
        if (slot < 0)
            return;
        Photo &p = photos[slot];
        int colours = (int)MementoR2Impl::R2_Palette::Count();
        const char *why = nullptr;
        //  A GIF with every frame; one too big for that as its first.
        if (w.kind == W_GIF && web::isGif(data, len))
        {
            why = web::decodeAnimation(data, len, pictureMaxW(), PHOTO_MAX_H, colours, 0xFFFFFF, GIF_BUDGET, p.anim);
            if (p.anim.px)
                keepAnimations(slot);
        }
        if (!p.anim.px)
            why = web::decodePicture(data, len, pictureMaxW(), PHOTO_MAX_H, colours, 0xFFFFFF, p.pic);
        if (why)
        {
            web::scopy(status, w.kind == W_GIF ? "GIF: " : "Photo: ", sizeof(status));
            web::scat(status, why, sizeof(status));
        }
        attach(slot, w.seq);
    }

    //  The slot is message seq's, if there is anything in it.
    void attach(int slot, int seq)
    {
        Photo &p = photos[slot];
        if (!p.any())
        {
            p.seq = -1;
            return;
        }
        p.seq = seq;
        msgs[seq % MAX_MSGS].photo = (int8_t)slot;
    }

    //  An MP4 in: decoding begins, and goes on from the idle loop.  The
    //  body is taken, not copied.
    void mp4Arrived(web::Buf &body)
    {
        int oldest = msgTotal > MAX_MSGS ? msgTotal - MAX_MSGS : 0;
        if (wanted[0].seq < oldest)
            return;
        const char *why = mp4.start(body, pictureMaxW(), PHOTO_MAX_H, (int)MementoR2Impl::R2_Palette::Count(),
                                    0xFFFFFF, GIF_BUDGET);
        if (why)
        {
            web::scopy(status, "GIF: ", sizeof(status));
            web::scat(status, why, sizeof(status));
            return;
        }
        mp4Seq = wanted[0].seq;
    }

    //  Some of the MP4, and at the end its frames in place of the still.
    void decodeSome()
    {
        if (mp4.step(8))
            return;
        web::Animation a;
        const char *why = mp4.finish(a);
        int slot = why ? -1 : slotFor(mp4Seq);
        if (why)
        {
            web::scopy(status, "GIF: ", sizeof(status));
            web::scat(status, why, sizeof(status));
        }
        else if (slot >= 0)
        {
            photos[slot].anim.release();
            photos[slot].anim = a; // the frames change owners
            a = web::Animation{};
            attach(slot, mp4Seq);
            keepAnimations(slot);
        }
        a.release();
        mp4Seq = -1;
        //  A picture waiting behind this one waited for a long poll.
        if (nWanted)
            interruptPoll();
        wnd->Repaint();
    }

    //  No more than MAX_ANIMATED GIFs keep their frames: beyond that, the
    //  oldest (but not `keep`, which has just come) is shown still --- by
    //  Telegram's still, or its own first frame when it has none.
    void keepAnimations(int keep)
    {
        for (;;)
        {
            int n = 0, oldest = -1;
            for (int i = 0; i < MAX_PHOTOS; i++)
                if (photos[i].anim.px)
                {
                    n++;
                    if (i != keep && (oldest < 0 || photos[i].seq < photos[oldest].seq))
                        oldest = i;
                }
            if (n <= MAX_ANIMATED || oldest < 0)
                return;
            Photo &p = photos[oldest];
            if (!p.pic.px)
            {
                size_t fb = (size_t)p.anim.w * p.anim.h;
                p.pic.px = (uint8_t *)web::big_alloc(fb);
                if (p.pic.px)
                {
                    memcpy(p.pic.px, p.anim.frame(0), fb);
                    p.pic.w = p.anim.w;
                    p.pic.h = p.anim.h;
                }
            }
            p.anim.release();
            p.shown = -1;
            if (!p.pic.px)
                p.seq = -1; // nothing left to show: a free slot
        }
    }

    //  What the bot's messages --- what is typed here --- are signed with on
    //  this side: the system user, or "me" when there is none.
    static const char *myName()
    {
        const char *u = systemUser();
        return u[0] ? u : "me";
    }

    bool usernameIs(const char *from, const char *e)
    {
        char u[40];
        return tgjson::str(tgjson::member(from, e, "username"), e, u, sizeof(u)) && !strcmp(u, botName);
    }

    // ── Requests ──────────────────────────────────────────────────────────────

    void idle(bool on)
    {
        if (on == immediate || !wnd)
            return;
        wnd->SetImmediateMode(on);
        immediate = on;
    }

    void begin(Request r, const char *method, const char *query, const uint8_t *body = nullptr, size_t len = 0,
               const char *contentType = nullptr)
    {
        web::Url u;
        u.https = true;
        u.port = 443;
        web::scopy(u.host, "api.telegram.org", sizeof(u.host));
        web::scopy(u.path, "/bot", sizeof(u.path));
        web::scat(u.path, token, sizeof(u.path));
        web::scat(u.path, "/", sizeof(u.path));
        web::scat(u.path, method, sizeof(u.path));
        if (query)
            web::scat(u.path, query, sizeof(u.path));
        request = r;
        loader.start(u, false, body, len, contentType);
    }

    //  A file from Telegram's file store, where getFile said it is.
    void beginFile(const char *path)
    {
        web::Url u;
        u.https = true;
        u.port = 443;
        web::scopy(u.host, "api.telegram.org", sizeof(u.host));
        web::scopy(u.path, "/file/bot", sizeof(u.path));
        web::scat(u.path, token, sizeof(u.path));
        web::scat(u.path, "/", sizeof(u.path));
        web::scat(u.path, path, sizeof(u.path));
        request = RQ_FILE;
        loader.start(u, false);
    }

    //  Form encoding: everything but letters and digits as %XX.  Typed text
    //  is ASCII, which UTF-8 leaves as it is; anything above is the font's
    //  code page and goes as '?', unless the text is `utf8` already.
    static void formEncode(web::Buf &b, const char *s, bool utf8 = false)
    {
        static const char hx[] = "0123456789ABCDEF";
        for (; *s; s++)
        {
            unsigned char c = (unsigned char)*s;
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
                b.push(c);
            else if (c < 0x80 || utf8)
            {
                b.push('%');
                b.push((uint8_t)hx[c >> 4]);
                b.push((uint8_t)hx[c & 15]);
            }
            else
                b.appendStr("%3F");
        }
    }

    void startNext()
    {
        if (!botName[0])
        {
            begin(RQ_GETME, "getMe", nullptr);
            return;
        }
        if (nOut)
        {
            //  Text, or a GIF by its file_id with the text as its caption.
            const Out &o = outbox[0];
            bool gif = o.gif[0] != 0;
            web::Buf body;
            body.appendStr("chat_id=");
            body.appendStr(chats[o.chat].id);
            char plain[IN_CAP + 1], ents[256];
            codeEntities(o.text, plain, ents, sizeof(ents));
            if (gif)
            {
                body.appendStr("&animation=");
                formEncode(body, o.gif);
            }
            if (!gif || plain[0])
            {
                body.appendStr(gif ? "&caption=" : "&text=");
                formEncode(body, plain);
            }
            if (ents[0])
            {
                body.appendStr(gif ? "&caption_entities=" : "&entities=");
                formEncode(body, ents);
            }
            appendReply(body, o.replyTo);
            if (body.failed)
            {
                web::scopy(status, "No memory for the message.", sizeof(status));
                dropOut();
                return;
            }
            begin(RQ_SEND, gif ? "sendAnimation" : "sendMessage", nullptr, body.data, body.len);
            return;
        }
        if (nReactOut)
        {
            const React &r = reactOut[0];
            char json[64] = "[";
            if (r.r >= 0)
            {
                web::scat(json, "{\"type\":\"emoji\",\"emoji\":\"", sizeof(json));
                web::scat(json, REACTIONS[r.r].utf8, sizeof(json));
                web::scat(json, "\"}", sizeof(json));
            }
            web::scat(json, "]", sizeof(json));
            char mid[24] = {};
            web::scatInt(mid, (long)r.msgId, sizeof(mid));
            web::Buf body;
            body.appendStr("chat_id=");
            body.appendStr(chats[r.chat].id);
            body.appendStr("&message_id=");
            body.appendStr(mid);
            body.appendStr("&reaction=");
            formEncode(body, json, true);
            if (body.failed)
            {
                web::scopy(status, "No memory for the reaction.", sizeof(status));
                dropReact();
                return;
            }
            begin(RQ_REACT, "setMessageReaction", nullptr, body.data, body.len);
            return;
        }
        if (photoQueued)
        {
            char ct[80] = "multipart/form-data; boundary=";
            web::scat(ct, BOUNDARY, sizeof(ct));
            begin(RQ_SENDPHOTO, "sendPhoto", nullptr, photoBody.data, photoBody.len, ct);
            web::scopy(status, "Sending the screenshot...", sizeof(status));
            return;
        }
        //  An MP4 waits while another is decoded: two at once would be two
        //  decoders' worth of memory.
        if (nWanted && !(wanted[0].kind == W_MP4 && mp4.busy()))
        {
            if (wanted[0].path[0])
                beginFile(wanted[0].path);
            else
            {
                web::Buf q;
                q.appendStr("?file_id=");
                formEncode(q, wanted[0].fileId);
                begin(RQ_FILEINFO, "getFile", q.cstr());
            }
            return;
        }
        //  Reactions are delivered only when asked for by name, and naming
        //  them replaces the default list, so what else is read goes too.
        char q[200] = "?timeout=20&limit=20&allowed_updates=%5B%22message%22%2C%22channel_post%22%2C"
                      "%22message_reaction%22%2C%22message_reaction_count%22%5D&offset=";
        web::scatInt(q, (long)offset, sizeof(q));
        begin(RQ_POLL, "getUpdates", q);
    }

    //  reply_parameters, form encoded: the message answered.  Sent even when
    //  it has been deleted meanwhile, as a message that answers nothing.
    static void appendReply(web::Buf &body, long long id)
    {
        if (!id)
            return;
        char json[80] = "{\"message_id\":";
        web::scatInt(json, (long)id, sizeof(json));
        web::scat(json, ",\"allow_sending_without_reply\":true}", sizeof(json));
        body.appendStr("&reply_parameters=");
        formEncode(body, json);
    }

    //  Backticks in what is typed, as Telegram's apps read them: `inline
    //  code` and ```a code block```.  `plain` is the text without them,
    //  `ents` the JSON array of entities for it ("" for none).  The text is
    //  ASCII by then (see formEncode), so a byte is a UTF-16 unit.
    static void codeEntities(const char *in, char *plain, char *ents, size_t entsCap)
    {
        int at = 0, n = 0;
        ents[0] = 0;
        auto entity = [&](const char *type, int off, int len) {
            if (len <= 0)
                return;
            char one[64] = "{\"type\":\"";
            web::scat(one, type, sizeof(one));
            web::scat(one, "\",\"offset\":", sizeof(one));
            web::scatInt(one, off, sizeof(one));
            web::scat(one, ",\"length\":", sizeof(one));
            web::scatInt(one, len, sizeof(one));
            web::scat(one, "}", sizeof(one));
            if (strlen(ents) + strlen(one) + 3 >= entsCap)
                return;
            web::scat(ents, n++ ? "," : "[", entsCap);
            web::scat(ents, one, entsCap);
        };
        for (const char *p = in; *p;)
        {
            bool fence = !strncmp(p, "```", 3);
            const char *close = fence ? strstr(p + 3, "```") : *p == '`' ? strchr(p + 1, '`') : nullptr;
            if (close && close > p + (fence ? 3 : 1))
            {
                const char *s = p + (fence ? 3 : 1), *e = close;
                if (fence && *s == '\n')
                    s++;
                if (fence && e > s && e[-1] == '\n')
                    e--;
                int start = at;
                for (; s < e; s++)
                    plain[at++] = *s;
                entity(fence ? "pre" : "code", start, at - start);
                p = close + (fence ? 3 : 1);
                continue;
            }
            plain[at++] = *p++;
        }
        plain[at] = 0;
        if (n)
            web::scat(ents, "]", entsCap);
    }

    //  A long poll would hold what is to go out back for up to 20 s.
    void interruptPoll()
    {
        if (request == RQ_POLL)
        {
            loader.cancel();
            request = RQ_NONE;
        }
        retryAt = 0;
    }

    void outNotSent()
    {
        char t[IN_CAP + 8];
        web::scopy(t, outbox[0].gif[0] ? "[GIF] " : "", sizeof(t));
        web::scat(t, outbox[0].text, sizeof(t));
        addMsg(outbox[0].chat, true, "not sent", t);
        dropOut();
    }

    void dropOut()
    {
        for (int i = 1; i < nOut; i++)
            outbox[i - 1] = outbox[i];
        if (nOut)
            nOut--;
    }

    //  A GIF on show whose next frame is due: paint again.
    void animate()
    {
        uint64_t now = web::now_ms();
        for (const Photo &p : photos)
            if (p.shown >= 0 && p.anim.frames > 1 && p.anim.frameAt(now) != p.shown)
            {
                wnd->Repaint();
                return;
            }
    }

    void onIdle()
    {
        if (setup)
            return;
        animate();
        if (mp4.busy())
            decodeSome();
        if (loader.busy())
        {
            loader.step();
            if (!loader.busy())
                finished();
            return;
        }
        if (web::now_ms() < retryAt)
        {
            web::r2Net().poll();
            return;
        }
        startNext();
    }

    void finished()
    {
        Request was = request;
        request = RQ_NONE;
        web::HttpResponse &r = loader.response();

        if (loader.phase() == web::Loader::FAILED)
        {
            //  A poll given up for a message to send: nothing went wrong.
            if (was == RQ_POLL && !strcmp(loader.error(), "Stopped"))
                return;
            web::scopy(status, loader.error(), sizeof(status));
            if (was == RQ_FILEINFO || was == RQ_FILE)
                dropWanted(); // a photo is not worth trying for ever
            if (was == RQ_SENDPHOTO)
                photoNotSent();
            if (was == RQ_SEND)
                outNotSent();
            if (was == RQ_REACT)
                dropReact();
            retryAt = web::now_ms() + 5000;
            wnd->Repaint();
            return;
        }

        if (was == RQ_FILE)
        {
            if (r.status == 200 && r.body.len && !r.truncated)
            {
                if (wanted[0].kind == W_MP4)
                    mp4Arrived(r.body);
                else
                    photoArrived(r.body.data, r.body.len);
            }
            else
                web::scopy(status, "A photo could not be fetched.", sizeof(status));
            r.body.release();
            dropWanted();
            wnd->Repaint();
            return;
        }

        const char *b = r.body.cstr();
        const char *e = b + (b == (const char *)r.body.data ? r.body.len : 0); // "" when out of memory
        char ok[8] = {};
        tgjson::raw(tgjson::member(b, e, "ok"), e, ok, sizeof(ok));
        if (strcmp(ok, "true"))
        {
            char why[80] = {};
            tgjson::str(tgjson::member(b, e, "description"), e, why, sizeof(why));
            if (r.status == 401 || r.status == 404)
            {
                backToSetup("Telegram does not know that token. Check it and press Enter.");
                return;
            }
            web::scopy(status, r.status == 409 ? "Another program is reading this bot's updates: " : "Telegram: ",
                       sizeof(status));
            web::scat(status, why[0] ? why : r.reason, sizeof(status));
            if (was == RQ_SEND)
                outNotSent();
            if (was == RQ_FILEINFO)
                dropWanted();
            if (was == RQ_SENDPHOTO)
                photoNotSent();
            if (was == RQ_REACT)
            {
                //  REACTION_INVALID, mostly: a group that allows only some.
                web::scopy(status, "Reaction refused: ", sizeof(status));
                web::scat(status, why[0] ? why : r.reason, sizeof(status));
                dropReact();
            }
            retryAt = web::now_ms() + (r.status == 429 || r.status == 409 ? 15000 : 5000);
            wnd->Repaint();
            return;
        }

        const char *result = tgjson::member(b, e, "result");
        if (was == RQ_GETME)
        {
            tgjson::str(tgjson::member(result, e, "username"), e, botName, sizeof(botName));
            if (!botName[0])
                web::scopy(botName, "bot", sizeof(botName));
            web::scopy(status, "@", sizeof(status));
            web::scat(status, botName, sizeof(status));
            web::scat(status, nChats ? "" : " - waiting for someone to write to it", sizeof(status));
        }
        else if (was == RQ_SEND)
        {
            //  The answer is the message as sent: shown the same way as any.
            dropOut();
            takeMessage(result, e);
        }
        else if (was == RQ_REACT)
        {
            //  Telegram does not tell a bot about its own reactions: kept here.
            const React &rq = reactOut[0];
            int oldest = msgTotal > MAX_MSGS ? msgTotal - MAX_MSGS : 0;
            for (int k = msgTotal - 1; k >= oldest; k--)
                if (msgs[k % MAX_MSGS].chat == rq.chat && msgs[k % MAX_MSGS].id == rq.msgId)
                {
                    msgs[k % MAX_MSGS].myReact = rq.r;
                    break;
                }
            web::scopy(status, rq.r >= 0 ? "Reacted." : "Reaction taken off.", sizeof(status));
            dropReact();
        }
        else if (was == RQ_SENDPHOTO)
        {
            //  The answer is the message with the photo in it: shown, and its
            //  picture fetched back, the same way as anyone's.
            photoQueued = false;
            photoBody.release();
            web::scopy(status, "Screenshot sent.", sizeof(status));
            takeMessage(result, e);
        }
        else if (was == RQ_FILEINFO)
        {
            if (!nWanted || !tgjson::str(tgjson::member(result, e, "file_path"), e, wanted[0].path,
                                         sizeof(wanted[0].path)) || !wanted[0].path[0])
                dropWanted();
        }
        else if (was == RQ_POLL)
        {
            for (const char *u = tgjson::first(result, e); u; u = tgjson::next(u, e))
            {
                char idText[24];
                if (tgjson::raw(tgjson::member(u, e, "update_id"), e, idText, sizeof(idText)))
                {
                    long long id = 0;
                    for (const char *p = idText; *p >= '0' && *p <= '9'; p++)
                        id = id * 10 + (*p - '0');
                    if (id + 1 > offset)
                        offset = id + 1;
                }
                const char *m = tgjson::member(u, e, "message");
                if (!m)
                    m = tgjson::member(u, e, "channel_post");
                if (m)
                    takeMessage(m, e);
                else if (const char *mr = tgjson::member(u, e, "message_reaction"))
                    takeReaction(mr, e);
                else if (const char *mc = tgjson::member(u, e, "message_reaction_count"))
                    takeReactionCount(mc, e);
            }
            if (botName[0])
            {
                web::scopy(status, "@", sizeof(status));
                web::scat(status, botName, sizeof(status));
            }
        }
        wnd->Repaint();
    }

    // ── Keys ──────────────────────────────────────────────────────────────────

    void onKey(PlatformKey *key)
    {
        bool ctrl = key->isLeftControl || key->isRightControl;
        bool shift = key->isLeftShift || key->isRightShift;
        bool alt = key->isLeftAlt || key->isRightAlt;

        if (menuSeq >= 0)
        {
            onMenuKey(key);
            return;
        }
        if (!setup && key->isChar && key->theChar == ' ' && (ctrl || alt))
        {
            openMenu(-1, true);
            return;
        }
        if (key->isEscape)
        {
            if (!setup && replyId)
            {
                cancelReply();
                web::scopy(status, "Not replying.", sizeof(status));
                wnd->Repaint();
                return;
            }
            idle(false);
            wnd->Close();
            return;
        }
        if (key->isBackspace)
        {
            if (inputLen)
                input[--inputLen] = 0;
            else if (attachment.len)
            {
                attachment.release();
                web::scopy(status, "Screenshot taken off.", sizeof(status));
            }
            else if (attachGif[0])
            {
                attachGif[0] = 0;
                web::scopy(status, "GIF taken off.", sizeof(status));
            }
            wnd->Repaint();
            return;
        }
        if (ctrl && key->isChar && (key->theChar == 'v' || key->theChar == 'V'))
        {
            paste();
            return;
        }
        if (ctrl && key->isChar && (key->theChar == 'c' || key->theChar == 'C'))
        {
            copy();
            return;
        }
        if (setup)
        {
            if (key->isEnter)
            {
                if (!validToken(input))
                {
                    web::scopy(status, "That does not look like a token (digits, ':', then 35 more).",
                               sizeof(status));
                    wnd->Repaint();
                    return;
                }
                web::scopy(token, input, sizeof(token));
                saveToken();
                inputLen = 0;
                input[0] = 0;
                startChatting();
                return;
            }
        }
        else
        {
            if (ctrl && key->isChar && (key->theChar == 't' || key->theChar == 'T'))
            {
                backToSetup("Type the new token and press Enter.");
                return;
            }
            if (key->isTab && nChats)
            {
                cancelReply(); // it was to someone in the chat being left
                cur = ((cur < 0 ? 0 : cur) + (shift ? nChats - 1 : 1)) % nChats;
                chats[cur].unread = 0;
                scroll = 0;
                wnd->Repaint();
                return;
            }
            if (key->isArrowUp || key->isPageUp)
            {
                scroll += key->isPageUp ? visibleRows - 1 : 1;
                wnd->Repaint(); // clamped when painted: only then are the lines known
                return;
            }
            if (key->isArrowDown || key->isPageDown)
            {
                scroll -= key->isPageDown ? visibleRows - 1 : 1;
                if (scroll < 0)
                    scroll = 0;
                wnd->Repaint();
                return;
            }
            if (key->isEnter && shift)
            {
                //  A line break, for a ```code block``` of more than one line.
                if (inputLen < IN_CAP)
                {
                    input[inputLen++] = '\n';
                    input[inputLen] = 0;
                }
                wnd->Repaint();
                return;
            }
            if (key->isEnter)
            {
                send();
                return;
            }
        }
        if (key->isChar && !ctrl && inputLen < (setup ? TOKEN_CAP - 1 : IN_CAP))
        {
            input[inputLen++] = (char)key->theChar;
            input[inputLen] = 0;
            wnd->Repaint();
        }
    }

    void paste()
    {
        if (!setup && clipboardHasImage())
        {
            attachScreenshot();
            return;
        }
        if (!setup && clipboardGif())
        {
            web::scopy(attachGif, clipboardGif(), sizeof(attachGif));
            attachment.release(); // one thing attached at a time
            web::scopy(status, "GIF attached: Enter sends it, with what is typed as its caption.", sizeof(status));
            wnd->Repaint();
            return;
        }
        int cap = setup ? TOKEN_CAP - 1 : IN_CAP;
        for (const char *p = clipboardGet(); *p && inputLen < cap; p++)
        {
            //  A token copied from somewhere else often brings a space along.
            if (setup && (*p == ' ' || *p == '\t'))
                continue;
            if ((unsigned char)*p >= ' ')
                input[inputLen++] = *p;
        }
        input[inputLen] = 0;
        wnd->Repaint();
    }

    //  The clipboard's PrintScreen as a PNG, to go out with the next Enter.
    void attachScreenshot()
    {
        attachGif[0] = 0;
        if (!web::encodePng(g_clipImage, g_clipImageW, g_clipImageH, g_clipPalette, g_clipColours, attachment))
        {
            attachment.release();
            web::scopy(status, "No memory for the screenshot.", sizeof(status));
            wnd->Repaint();
            return;
        }
        attachW = g_clipImageW;
        attachH = g_clipImageH;
        char s[96] = "Screenshot attached (";
        web::scatInt(s, attachW, sizeof(s));
        web::scat(s, "x", sizeof(s));
        web::scatInt(s, attachH, sizeof(s));
        web::scat(s, ", ", sizeof(s));
        web::scatInt(s, (long)((attachment.len + 1023) / 1024), sizeof(s));
        web::scat(s, " KiB): Enter sends it", sizeof(s));
        web::scopy(status, s, sizeof(status));
        wnd->Repaint();
    }

    //  sendPhoto's body: the chat, the caption and the PNG, as
    //  multipart/form-data.
    bool buildPhotoBody(const char *chatId, const char *caption, long long reply)
    {
        web::Buf &b = photoBody;
        b.clear();
        auto part = [&](const char *name, const char *extra) {
            b.appendStr("--");
            b.appendStr(BOUNDARY);
            b.appendStr("\r\nContent-Disposition: form-data; name=\"");
            b.appendStr(name);
            b.appendStr("\"");
            b.appendStr(extra);
            b.appendStr("\r\n\r\n");
        };
        part("chat_id", "");
        b.appendStr(chatId);
        b.appendStr("\r\n");
        if (caption[0])
        {
            part("caption", "");
            b.appendStr(caption);
            b.appendStr("\r\n");
        }
        if (reply)
        {
            part("reply_parameters", "");
            b.appendStr("{\"message_id\":");
            char mid[24] = {};
            web::scatInt(mid, (long)reply, sizeof(mid));
            b.appendStr(mid);
            b.appendStr(",\"allow_sending_without_reply\":true}\r\n");
        }
        part("photo", "; filename=\"screenshot.png\"\r\nContent-Type: image/png");
        b.append(attachment.data, attachment.len);
        b.appendStr("\r\n--");
        b.appendStr(BOUNDARY);
        b.appendStr("--\r\n");
        return !b.failed;
    }

    void copy()
    {
        if (inputLen)
        {
            clipboardSet(input);
            web::scopy(status, "Copied what is typed.", sizeof(status));
        }
        else if (!setup && cur >= 0)
        {
            //  The newest message in the chat on the right, without the
            //  "Name: " in front of it.
            int oldest = msgTotal > MAX_MSGS ? msgTotal - MAX_MSGS : 0;
            const Msg *m = nullptr;
            for (int k = msgTotal - 1; k >= oldest && !m; k--)
                if (msgs[k % MAX_MSGS].chat == cur)
                    m = &msgs[k % MAX_MSGS];
            if (!m)
                return;
            copyMessage(*m);
        }
        else
            return;
        wnd->Repaint();
    }

    //  A message's text past the line it quotes, if it is a reply.
    static const char *afterQuote(const char *t)
    {
        if (*t != QUOTE)
            return t;
        const char *nl = strchr(t, '\n');
        return nl ? nl + 1 : t + strlen(t);
    }

    //  A message to the clipboard: without the "Name: " in front of it or
    //  the bytes that mark its code.
    void copyMessage(const Msg &m)
    {
        const char *s = strstr(afterQuote(m.text), ": ");
        s = s ? s + 2 : afterQuote(m.text);
        char plain[TEXT_CAP];
        int n = 0;
        for (; *s; s++)
            if (*s != PRE_ON && *s != PRE_OFF && *s != CODE)
                plain[n++] = *s;
        plain[n] = 0;
        clipboardSet(plain);
        web::scopy(status, "Copied the message.", sizeof(status));
    }

    void send()
    {
        if (!inputLen && !attachment.len && !attachGif[0])
            return;
        if (cur < 0)
        {
            web::scopy(status, "No one to write to yet: a chat appears once someone writes to the bot.",
                       sizeof(status));
            wnd->Repaint();
            return;
        }
        if (attachment.len)
        {
            //  A screenshot, with what is typed as its caption.
            if (photoQueued)
            {
                web::scopy(status, "Still sending the last screenshot; wait a moment.", sizeof(status));
                wnd->Repaint();
                return;
            }
            if (!buildPhotoBody(chats[cur].id, input, replyChat == cur ? replyId : 0))
            {
                photoBody.release();
                web::scopy(status, "No memory to send the screenshot.", sizeof(status));
                wnd->Repaint();
                return;
            }
            attachment.release();
            cancelReply();
            photoQueued = true;
            photoChat = cur;
            web::scopy(photoCaption, input, sizeof(photoCaption));
            inputLen = 0;
            input[0] = 0;
            interruptPoll();
            web::scopy(status, "Sending the screenshot...", sizeof(status));
            wnd->Repaint();
            return;
        }
        if (nOut == MAX_OUT)
        {
            web::scopy(status, "Still sending the last few; wait a moment.", sizeof(status));
            wnd->Repaint();
            return;
        }
        Out &o = outbox[nOut++];
        o.chat = cur;
        o.replyTo = replyChat == cur ? replyId : 0;
        web::scopy(o.gif, attachGif, sizeof(o.gif));
        web::scopy(o.text, input, sizeof(o.text));
        attachGif[0] = 0;
        cancelReply();
        inputLen = 0;
        input[0] = 0;
        interruptPoll();
        web::scopy(status, "Sending...", sizeof(status));
        wnd->Repaint();
    }

    //  The message painted at (x, y), or -1.
    int messageAt(double x, double y) const
    {
        if (x < msgX || x >= msgX + msgW || y < msgTop)
            return -1;
        int row = (int)((y - msgTop) / rowH);
        return row < visibleRows && row < ROWS_CAP ? rowSeq[row] : -1;
    }

    void onClick(double x, double y, bool right)
    {
        if (setup)
            return;
        if (menuSeq >= 0)
        {
            //  On an item, that; anywhere else, the menu goes away (and a
            //  right click on another message brings it up there).
            int i = menuItemAt(x, y);
            if (i >= 0)
            {
                if (!right)
                    pickMenu(i);
                return;
            }
            closeMenu();
        }
        if (right)
        {
            int seq = messageAt(x, y);
            if (seq >= 0)
                openMenu(seq, false);
            return;
        }
        if (x >= CHATS_W)
            return;
        int row = (int)((y - 3) / rowH);
        if (row >= 0 && row < nChats)
        {
            if (row != cur)
                cancelReply();
            cur = row;
            chats[cur].unread = 0;
            scroll = 0;
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
        case PlatformWindowInputEventType::OnMouseWheel:
            if (menuSeq >= 0)
                return;
            scroll += data->Data.OnMouseWheel.up ? 3 : -3;
            if (scroll < 0)
                scroll = 0;
            wnd->Repaint();
            return;
        case PlatformWindowInputEventType::OnMouseClick:
            if (data->Data.OnMouseClick.state == PlatformWindowButtonState::Pressed)
            {
                Coord mx = data->Data.OnMouseClick.mouseX, my = data->Data.OnMouseClick.mouseY;
                onClick(COORD_VAL(mx), COORD_VAL(my),
                        data->Data.OnMouseClick.button == PlatformWindowMouseButton::Right);
            }
            return;
        case PlatformWindowInputEventType::OnKeyEvent:
            if (data->Data.OnKeyEvent.key->isKeyDown)
                onKey(data->Data.OnKeyEvent.key);
            return;
        default:
            return;
        }
    }

    // ── Paint ─────────────────────────────────────────────────────────────────

    void makeResources(PlatformDrawingContext *dc)
    {
        if (cBg)
            return;
        cBg = dc->CreateColor(0xFFFFFFFF, nullptr, nullptr);
        cText = dc->CreateColor(0xFF000000, nullptr, nullptr);
        cMine = dc->CreateColor(0xFF0000AA, nullptr, nullptr);
        cPane = dc->CreateColor(0xFFAAAAAA, nullptr, nullptr);
        cSel = dc->CreateColor(0xFF0000AA, nullptr, nullptr);
        cSelText = dc->CreateColor(0xFFFFFFFF, nullptr, nullptr);
        cRule = dc->CreateColor(0xFF555555, nullptr, nullptr);
        cFaint = dc->CreateColor(0xFF555555, nullptr, nullptr);
        cCode = dc->CreateColor(0xFFAAAAAA, nullptr, nullptr);
        cPick = dc->CreateColor(0xFFFFFF55, nullptr, nullptr);
        font = dc->CreateFont(6, nullptr, false, false, false, nullptr, nullptr);
        if (font)
        {
            Coord w, h;
            if (font->GetDrawnTextSize("MMMMMMMMMM", w, h))
            {
                cw = COORD_VAL(w) / 10;
                ch = COORD_VAL(h);
            }
        }
        if (cw <= 0)
            cw = 3;
        if (ch <= 0)
            ch = 6;
        rowH = ch + 1;
    }

    void text(PlatformBitmap *t, double x, double y, double w, const char *s, PlatformColor *c)
    {
        PlatformDrawTextOptions o{};
        o.font = font;
        o.foreground = c;
        o.horizontalAlign = PlatformAlign::Begin;
        o.verticalAlign = PlatformAlign::Begin;
        t->DrawText(Coord(x), Coord(y), Coord(w), Coord(rowH), (const mchar *)s, &o, false);
    }

    //  The end of s that fits in `cells`, for an input box.
    static const char *tail(const char *s, int cells)
    {
        int l = (int)strlen(s);
        return l > cells ? s + (l - cells) : s;
    }

    //  A message cut into the lines it is drawn as: its text word-wrapped
    //  to `cols`, broken where it has line breaks, and its code blocks on
    //  lines of their own, one cell in from each side and wrapped where they
    //  reach it rather than at a space.
    struct Line
    {
        const char *s;
        int len;     // bytes, CODE marks included
        bool block;  // a line of a code block
        bool codeOn; // inside inline code where it starts
        bool quote;  // a line of what a reply answers
    };

    static int layout(const char *text, int cols, Line *out, int cap)
    {
        int n = 0;
        bool pre = false, code = false, quote = false;
        const char *s = text;
        while (*s && n < cap)
        {
            if (*s == QUOTE)
            {
                quote = true;
                s++;
                continue;
            }
            if (*s == PRE_ON || *s == PRE_OFF)
            {
                pre = *s++ == PRE_ON;
                code = false;
                if (!pre && *s == '\n')
                    s++; // the block's end is a line break already
                continue;
            }
            int width = pre ? cols - 2 : cols;
            if (width < 4)
                width = 4;
            bool startCode = code;
            const char *p = s, *space = nullptr;
            bool codeAtSpace = false;
            int vis = 0;
            while (*p && *p != '\n' && *p != PRE_ON && *p != PRE_OFF && vis < width)
            {
                if (*p == CODE)
                    code = !code;
                else
                {
                    if (*p == ' ' && vis > width / 2)
                        space = p, codeAtSpace = code;
                    vis++;
                }
                p++;
            }
            const char *next = p;
            if (vis == width && !pre && *p && *p != ' ' && *p != '\n' && *p != PRE_ON && *p != PRE_OFF && space)
            {
                p = space; // the word that did not fit goes to the next line
                next = space;
                code = codeAtSpace;
            }
            out[n++] = {s, (int)(p - s), pre, startCode, quote};
            s = next;
            if (*s == '\n')
                s++, quote = false;
            else if (!pre)
                while (*s == ' ')
                    s++;
        }
        return n;
    }

    //  One line of a message: a code block's on its box, the rest in runs
    //  with inline code on a box of its own.
    void drawLine(PlatformBitmap *t, double x, double y, double w, const Line &l, PlatformColor *fg)
    {
        char buf[TEXT_CAP];
        if (l.quote)
        {
            t->FillRect(Coord(x), Coord(y), Coord(1), Coord(rowH), cMine, false);
            memcpy(buf, l.s, (size_t)l.len);
            buf[l.len] = 0;
            text(t, x + cw, y, w - cw, buf, cFaint);
            return;
        }
        if (l.block)
        {
            t->FillRect(Coord(x), Coord(y), Coord(w), Coord(rowH), cCode, false);
            t->FillRect(Coord(x), Coord(y), Coord(1), Coord(rowH), cRule, false);
            memcpy(buf, l.s, (size_t)l.len);
            buf[l.len] = 0;
            text(t, x + cw, y, w - 2 * cw, buf, fg);
            return;
        }
        bool code = l.codeOn;
        int col = 0;
        for (const char *p = l.s, *end = l.s + l.len; p < end;)
        {
            int n = 0;
            for (; p < end && *p != CODE; p++)
                buf[n++] = *p;
            buf[n] = 0;
            if (n)
            {
                if (code)
                    t->FillRect(Coord(x + col * cw), Coord(y), Coord(n * cw), Coord(rowH), cCode, false);
                text(t, x + col * cw, y, w - col * cw, buf, fg);
                col += n;
            }
            if (p < end)
            {
                code = !code;
                p++;
            }
        }
    }

    void paintSetup(PlatformBitmap *t, double Wd, double Hd)
    {
        static const char *const lines[] = {
            "Telegram, through a bot of your own:",
            "",
            "1. In Telegram, write /newbot to @BotFather.",
            "2. Type the token it answers with here and press",
            "   Enter (or ship it in /mnt/tar/opt/memento/telegram.txt).",
            "3. Write to your bot from your phone: the chat",
            "   shows up here, and you answer as the bot.",
            "",
            "In a group the bot sees only /commands and replies",
            "to it, unless @BotFather's /setprivacy says otherwise.",
        };
        double y = 4;
        for (const char *l : lines)
        {
            text(t, 6, y, Wd - 12, l, cText);
            y += rowH;
        }
        y += 4;
        t->FillRect(6, Coord(y - 1), Coord(Wd - 12), Coord(rowH + 2), cRule, false);
        t->FillRect(Coord(6.5), Coord(y - 0.5), Coord(Wd - 13), Coord(rowH + 1), cBg, false);
        char shown[TOKEN_CAP + 2];
        web::scopy(shown, input, sizeof(shown));
        web::scat(shown, "_", sizeof(shown));
        text(t, 8, y, Wd - 16, tail(shown, (int)((Wd - 18) / cw)), cText);
        if (status[0])
            text(t, 6, Hd - rowH - 2, Wd - 12, status, cMine);
    }

    void paintChat(PlatformBitmap *t, double Wd, double Hd)
    {
        //  The chats down the left.
        t->FillRect(0, 0, Coord(CHATS_W), Coord(Hd), cPane, false);
        for (int i = 0; i < nChats; i++)
        {
            double y = 3 + i * rowH;
            char label[40];
            web::scopy(label, chats[i].unread ? "* " : "", sizeof(label));
            web::scat(label, chats[i].name, sizeof(label));
            if (i == cur)
                t->FillRect(1, Coord(y), Coord(CHATS_W - 2), Coord(rowH), cSel, false);
            text(t, 3, y, CHATS_W - 5, label, i == cur ? cSelText : cText);
        }
        if (!nChats)
            text(t, 3, 3, CHATS_W - 5, "(no chats)", cFaint);

        //  The status line and the input box along the bottom.
        double mx = CHATS_W + 3, mw = Wd - mx - 3;
        double statusY = Hd - rowH - 1;
        double inputY = statusY - rowH - 3;
        text(t, mx, statusY, mw, status, cFaint);
        //  What is being answered, over the input box.
        bool replying = replyId && replyChat == cur;
        if (replying)
        {
            double ry = inputY - rowH - 2;
            char r[80] = "Reply to ";
            if (replySeq >= 0)
            {
                const char *s = afterQuote(msgs[replySeq % MAX_MSGS].text);
                int n = (int)strlen(r);
                for (; *s && n < (int)sizeof(r) - 1; s++)
                    if (*s != PRE_ON && *s != PRE_OFF && *s != CODE)
                        r[n++] = *s == '\n' ? ' ' : *s;
                r[n] = 0;
            }
            else
                web::scat(r, "a message no longer shown", sizeof(r));
            t->FillRect(Coord(mx - 1), Coord(ry), Coord(1), Coord(rowH), cMine, false);
            text(t, mx + 1, ry, mw - 2, r, cMine);
        }
        t->FillRect(Coord(mx - 1), Coord(inputY - 1), Coord(mw + 2), Coord(rowH + 2), cRule, false);
        t->FillRect(Coord(mx - 0.5), Coord(inputY - 0.5), Coord(mw + 1), Coord(rowH + 1), cBg, false);
        char shown[IN_CAP + 20];
        web::scopy(shown, attachment.len ? "[screenshot] " : attachGif[0] ? "[GIF] " : "", sizeof(shown));
        web::scat(shown, input, sizeof(shown));
        web::scat(shown, "_", sizeof(shown));
        for (char *p = shown; *p; p++)
            if (*p == '\n')
                *p = '\x14'; // the font's pilcrow
        text(t, mx + 1, inputY, mw - 2, tail(shown, (int)((mw - 3) / cw)), cText);

        //  The messages of the chat on the right, wrapped, newest at the
        //  bottom.  Walked from the newest back, a message at a time, until
        //  the lines above the scroll position and the window are filled.
        //  A message is its lines, then its photo, then its reactions.
        double top = 3, bottom = inputY - 3 - (replying ? rowH + 2 : 0);
        visibleRows = (int)((bottom - top) / rowH);
        if (visibleRows < 1)
            visibleRows = 1;
        int cols = (int)(mw / cw);
        if (cols < 10)
            cols = 10;
        int oldest = msgTotal > MAX_MSGS ? msgTotal - MAX_MSGS : 0;
        msgW = mw;
        msgX = mx;
        msgTop = top;
        //  Pixels a unit from the DPI, not from the bitmap, which Memento
        //  allocates in steps of 150 pixels and so is often wider than the
        //  window (see the browser's paintPage).
        auto *bm = static_cast<MementoR2Impl::R2_BitmapImpl *>(t);
        if (wnd->GetEffectiveDPI() > 0)
            pxPerUnit = wnd->GetEffectiveDPI() / 96.0;
        int rowPx = (int)(rowH * pxPerUnit + 0.5);
        Line lines[TEXT_CAP];
        char reacts[64];

        //  How many lines there are, to keep the scroll in range, and where
        //  the menu's message is, to bring it into view.
        int total = 0, selFrom = -1, selTo = -1;
        for (int k = msgTotal - 1; k >= oldest; k--)
        {
            const Msg &m = msgs[k % MAX_MSGS];
            if (m.chat != cur)
                continue;
            int h = layout(m.text, cols, lines, TEXT_CAP) + photoRows(m, rowPx) + (reactLine(m, reacts, sizeof(reacts)) ? 1 : 0);
            if (k == menuSeq)
                selFrom = total, selTo = total + h;
            total += h;
        }
        if (menuMoved && selFrom >= 0)
        {
            if (selTo > scroll + visibleRows)
                scroll = selTo - visibleRows;
            if (selFrom < scroll)
                scroll = selFrom;
            menuMoved = false;
        }
        int maxScroll = total > visibleRows ? total - visibleRows : 0;
        if (scroll > maxScroll)
            scroll = maxScroll;

        for (int r = 0; r < ROWS_CAP; r++)
            rowSeq[r] = -1;
        for (Photo &p : photos)
            p.shown = -1;
        menuAnchorY = -1;
        t->SetClip(Coord(mx), Coord(top), Coord(mw), Coord(bottom - top), false);
        int line = 0; // lines from the bottom, counting down the screen
        for (int k = msgTotal - 1; k >= oldest && line < scroll + visibleRows; k--)
        {
            const Msg &m = msgs[k % MAX_MSGS];
            if (m.chat != cur)
                continue;
            int n = layout(m.text, cols, lines, TEXT_CAP);
            int pr = photoRows(m, rowPx);
            bool rr = reactLine(m, reacts, sizeof(reacts));
            int h = n + pr + (rr ? 1 : 0);
            int rowBottom = visibleRows - 1 - (line - scroll);
            int rowTop = rowBottom - h + 1;
            line += h;
            if (rowBottom < 0)
                continue;
            for (int r = rowTop < 0 ? 0 : rowTop; r <= rowBottom && r < ROWS_CAP; r++)
                rowSeq[r] = k;
            if (k == menuSeq)
            {
                t->FillRect(Coord(mx), Coord(top + rowTop * rowH), Coord(mw), Coord(h * rowH), cPick, false);
                menuAnchorY = top + (rowTop < 0 ? 0 : rowTop) * rowH;
            }
            for (int j = 0; j < n; j++)
            {
                int row = rowTop + j;
                if (row >= 0 && row < visibleRows)
                    drawLine(t, mx, top + row * rowH, mw, lines[j], m.mine ? cMine : cText);
            }
            //  Its photo under the text, drawn where its rows fall.
            if (pr)
                paintPhoto(bm, photos[m.photo], mx, top + (rowTop + n) * rowH, top, bottom, mx + mw);
            if (rr)
                text(t, mx, top + rowBottom * rowH, mw, reacts, cFaint);
        }
        t->ClearClip();
        if (scroll)
        {
            char more[16] = "^ ";
            web::scatInt(more, scroll, sizeof(more));
            PlatformDrawTextOptions o{};
            o.font = font;
            o.foreground = cFaint;
            o.horizontalAlign = PlatformAlign::End;
            o.verticalAlign = PlatformAlign::Begin;
            t->DrawText(Coord(mx), Coord(top), Coord(mw), Coord(rowH), (const mchar *)more, &o, false);
        }
        if (menuSeq >= 0)
            paintMenu(t, Wd, Hd);
    }

    //  The menu, beside the top of its message (or the top of the messages
    //  when that is scrolled off), kept inside the window.
    void paintMenu(PlatformBitmap *t, double Wd, double Hd)
    {
        int widest = 0;
        char label[48];
        const char *keys;
        for (int i = 0; i < MI_COUNT; i++)
        {
            menuLabel(i, label, sizeof(label), &keys);
            int l = (int)strlen(label) - 1 + (keys[0] ? (int)strlen(keys) + 2 : 0);
            if (l > widest)
                widest = l;
        }
        menuW = widest * cw + 8;
        double h = MI_COUNT * rowH + 4;
        menuL = msgX + 12;
        menuT = menuAnchorY >= 0 ? menuAnchorY : msgTop;
        if (menuL + menuW > Wd - 2)
            menuL = Wd - 2 - menuW;
        if (menuT + h > Hd - 2)
            menuT = Hd - 2 - h;
        if (menuL < 1)
            menuL = 1;
        if (menuT < 1)
            menuT = 1;
        //  A shadow, a border, then the items, as the browser's.
        t->FillRect(Coord(menuL + 1.5), Coord(menuT + 1.5), Coord(menuW), Coord(h), cRule, false);
        t->FillRect(Coord(menuL), Coord(menuT), Coord(menuW), Coord(h), cText, false);
        t->FillRect(Coord(menuL + 0.5), Coord(menuT + 0.5), Coord(menuW - 1), Coord(h - 1), cBg, false);
        double iy = menuT + 2;
        for (int i = 0; i < MI_COUNT; i++, iy += rowH)
        {
            if (i == MI_SEP)
            {
                t->FillRect(Coord(menuL + 2), Coord(iy + rowH / 2), Coord(menuW - 4), Coord(0.5), cPane, false);
                continue;
            }
            menuLabel(i, label, sizeof(label), &keys);
            bool on = menuEnabled(i), sel = i == menuSel;
            if (sel)
                t->FillRect(Coord(menuL + 1), Coord(iy), Coord(menuW - 2), Coord(rowH), cSel, false);
            PlatformColor *c = !on ? cFaint : sel ? cSelText : cText;
            char shown[48];
            int ai = -1, n = 0;
            for (const char *p = label; *p && n < (int)sizeof(shown) - 1; p++)
            {
                if (*p == '&' && ai < 0)
                {
                    ai = n;
                    continue;
                }
                shown[n++] = *p;
            }
            shown[n] = 0;
            double tx = menuL + 4;
            text(t, tx, iy + 0.5, menuW - 8, shown, c);
            if (ai >= 0 && on)
                t->FillRect(Coord(tx + ai * cw), Coord(iy + 0.5 + ch - 0.5), Coord(cw - 0.5), Coord(0.5), c, false);
            if (keys[0])
                text(t, menuL + menuW - 4 - strlen(keys) * cw, iy + 0.5, menuW, keys, sel ? cSelText : cFaint);
        }
    }

    //  The size a photo is drawn at: as it is, or smaller to fit the width
    //  of the messages (it was made to fit when it came, but the window may
    //  have been made narrower since).
    void photoSize(const Photo &p, int &w, int &h) const
    {
        int avail = (int)(msgW * pxPerUnit);
        w = p.w();
        h = p.h();
        if (avail > 0 && w > avail)
        {
            h = (int)((long long)h * avail / w);
            w = avail;
        }
        if (h < 1)
            h = 1;
    }

    int photoRows(const Msg &m, int rowPx) const
    {
        if (m.photo < 0 || !photos[m.photo].any() || rowPx <= 0)
            return 0;
        int w, h;
        photoSize(photos[m.photo], w, h);
        return (h + 2 + rowPx - 1) / rowPx; // with a little room under it
    }

    //  A photo straight into the window's pixels, anchored at (x, y) in
    //  units, scaled to photoSize and cut to the message area: rows
    //  [clipTop, clipBottom), columns up to clipRight.  A GIF's frame is the
    //  one for the time it is now: every GIF on show keeps to the clock, and
    //  the idle loop paints again when one of them has moved on.
    void paintPhoto(MementoR2Impl::R2_BitmapImpl *bm, Photo &ph, double x, double y, double clipTop,
                    double clipBottom, double clipRight)
    {
        uint8 *px = bm->GetPixels();
        int bw = bm->GetRealWidth().intValue(), bh = bm->GetRealHeight().intValue();
        if (!px || !ph.any())
            return;
        struct
        {
            int w, h;
            const uint8_t *px;
        } p = {ph.w(), ph.h(), ph.pic.px};
        if (ph.anim.px)
        {
            ph.shown = ph.anim.frameAt(web::now_ms());
            p.px = ph.anim.frame(ph.shown);
        }
        int w, h;
        photoSize(ph, w, h);
        int x0 = (int)(x * pxPerUnit), y0 = (int)(y * pxPerUnit);
        int top = (int)(clipTop * pxPerUnit), bottom = (int)(clipBottom * pxPerUnit);
        int right = (int)(clipRight * pxPerUnit);
        if (bottom > bh)
            bottom = bh;
        if (right > bw)
            right = bw;
        for (int dy = 0; dy < h; dy++)
        {
            int py = y0 + dy;
            if (py < top)
                continue;
            if (py >= bottom)
                break;
            const uint8_t *src = p.px + (size_t)(dy * p.h / h) * p.w;
            uint8 *dst = px + (size_t)py * bw;
            for (int dx = 0; dx < w && x0 + dx < right; dx++)
                if (x0 + dx >= 0)
                    dst[x0 + dx] = src[w == p.w ? dx : dx * p.w / w];
        }
    }

    void OnPaint(PlatformDrawingContext *dc, PlatformBitmap *target)
    {
        if (!target)
            return;
        makeResources(dc);
        if (!cBg || !font)
            return;
        Coord Wc = target->GetWidth(), Hc = target->GetHeight();
        double Wd = COORD_VAL(Wc), Hd = COORD_VAL(Hc);
        target->FillRect(0, 0, Coord(Wd), Coord(Hd), cBg, false);
        if (setup)
            paintSetup(target, Wd, Hd);
        else
            paintChat(target, Wd, Hd);
    }
};
