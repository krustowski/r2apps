// The browser runs in r2web.elf; HostedWindow carries its frames and input.
class BrowserWindow : public HostedWindow {
public:
    explicit BrowserWindow(const char *url = nullptr)
        : HostedWindow("r2web.elf", "Web", r2web::Magic, 0, url) {}
};
