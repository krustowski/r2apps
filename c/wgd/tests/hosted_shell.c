/* Simulate Memento's ShellWindow rings, exercising a real hosted sh.elf. */
#include "syscall.h"
#include "mem.h"
#include "../../r2sh/host.h"
static void debug(uint8_t c) { write_port(0xe9,c); }
int main(void) {
    static uint8_t cfg[2049], args[128];
    for (unsigned i=0;i<2;++i) {
        const uint8_t *from=(const uint8_t *)(i ? "/mnt/fat/BAD.CFG":"/mnt/fat/WGD.CFG");
        const uint8_t *to=(const uint8_t *)(i ? "/mnt/tmp/BAD.CFG":"/mnt/tmp/WGD.CFG");
        int64_t n=read_file_at(from,cfg,0,sizeof(cfg)-1);
        if(n>0 && write_file_at(to,cfg,0,(uint64_t)n)!=n) return 1;
    }
    ShHostBlock_T *b=malloc(sizeof(*b)); if(!b) return 1;
    for(unsigned i=0;i<sizeof(*b);++i) ((uint8_t *)b)[i]=0;
    b->magic=SH_HOST_MAGIC; b->version=SH_HOST_VERSION; b->hostBeat=1;
    const char *start="sh --host 0x"; unsigned at=0;
    while(*start) args[at++]=(uint8_t)*start++;
    uint64_t addr=(uint64_t)b;
    for(int shift=60;shift>=0;shift-=4) args[at++]=(uint8_t)"0123456789abcdef"[(addr>>shift)&15];
    const char *end=" --run /mnt/fat/TEST.BSH";
    while(*end) args[at++]=(uint8_t)*end++; args[at]=0;
    uint8_t pid; if(!run_elf((const uint8_t *)"sh",args,&pid)) { free(b); return 1; }
    const char *marker="__WGD_TEST_DONE__"; unsigned matched=0; int sent_exit=0;
    uint64_t deadline=get_ticks()+100000;
    for (;;) {
        ++b->hostBeat;
        while(b->outTail!=b->outHead) {
            uint8_t c=b->out[b->outTail%SH_OUT_SIZE]; ++b->outTail; debug(c);
            if(!sent_exit) {
                matched=c==(uint8_t)marker[matched] ? matched+1 : 0;
                if(!marker[matched]) {
                    const char *exit_command="exit\n";
                    while(*exit_command) { b->in[b->inHead%SH_IN_SIZE]=(uint8_t)*exit_command++; ++b->inHead; }
                    sent_exit=1;
                }
            }
        }
        if(b->exited) break;
        if(get_ticks()>deadline) { b->quit=1; return 1; }
        sleep_ms(5);
    }
    const char *done="HOSTED SHELL COMPLETE\n"; while(*done) debug((uint8_t)*done++);
    free(b); return 0;
}
