// Telegram runs in telegram.elf (go/telegram); HostedWindow carries its frames
// and input, and on a Ctrl+V the clipboard's picture, for screenshots it sends.
#include "../../../go/telegram/host.h"
class TelegramWindow : public HostedWindow {
public:
    static const int W = 300, H = 170;
    TelegramWindow() : HostedWindow("telegram.elf", "Telegram", tghost::Magic, 0, nullptr, true, true) {}
};
