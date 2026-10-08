// Jug runs in jug.elf; Memento owns the shared block and its lifetime.
#include "../../jug/host.h"
class JugHostWindow : public HostedWindow {
public:
    JugHostWindow() : HostedWindow("jug.elf", "Jug", jughost::Magic, jughost::PortBase) {}
};
