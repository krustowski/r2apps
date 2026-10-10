// c8 uses the raylib software backend over Memento's shared-frame host.
class C8Window : public HostedWindow {
public:
    static constexpr int W = 300, H = 150;
    explicit C8Window(const char *rom)
        : HostedWindow("c8.elf", "C8", 0x38433252u, 0, rom) {}
protected:
    void prepareKey(r2web::Command &command, PlatformKey *key) override {
        // extra is the raw physical key for C8, so releases still match
        // their presses if Shift/Caps changes in between.
        command.extra = key->scancode | (key->scancodeExtended ? 0x100u : 0u);
    }
};
