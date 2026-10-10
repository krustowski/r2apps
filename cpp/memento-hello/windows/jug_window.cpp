// Jug runs in jug.elf; Memento owns the shared block and its lifetime.
#include "../../jug/host.h"
class JugHostWindow : public HostedWindow {
public:
    JugHostWindow() : HostedWindow("jug.elf", "Jug", jughost::Magic, jughost::PortBase) {}
protected:
    void onAttention(uint32_t attention) override
    {
        if (!(attention & jughost::UpdateNotification)) return;
        uint32_t count=attention & ~jughost::UpdateNotification;
        if (!count) return;
        char message[96]="Jug: ";
        web::scatInt(message,count,sizeof(message));
        web::scat(message,count==1 ? " new update available." : " new updates available.",sizeof(message));
        MementoR2Impl::R2_Notify(message,6000);
    }
};
